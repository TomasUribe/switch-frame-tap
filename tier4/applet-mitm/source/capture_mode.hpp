/*
 * v0.7.15: capture modes and the per-title overrides in
 * sdmc:/config/switch-frame-tap/capture.ini.
 *
 * The same parser is used by the sysmodule (C++17, no libnx) and by the
 * manager/overlay (host builds). It is deliberately free of std::string and
 * exceptions: a malformed line must leave the defaults, never arm anything
 * else.
 *
 *   capture_mode = auto | standard | composite | layer     (config.ini)
 *   01006c300e9f0000 = composite                           (capture.ini)
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sftcapture {

    enum Mode : uint32_t {
        Auto      = 0,
        Standard  = 1,
        Composite = 2,
        Layer     = 3,
    };

    constexpr size_t MaxOverrides = 64;
    constexpr const char *IniPath = "sdmc:/config/switch-frame-tap/capture.ini";

    struct Override {
        uint64_t tid;
        uint32_t mode;
    };

    inline const char *Key(uint32_t mode) {
        switch (mode) {
            case Auto:      return "auto";
            case Standard:  return "standard";
            case Composite: return "composite";
            case Layer:     return "layer";
            default:        return "auto";
        }
    }

    inline const char *Label(uint32_t mode) {
        switch (mode) {
            case Auto:      return "Auto";
            case Standard:  return "Standard";
            case Composite: return "Compositor";
            case Layer:     return "Layer";
            default:        return "Auto";
        }
    }

    inline bool Parse(const char *s, size_t n, uint32_t *out) {
        if (s == nullptr || n == 0 || out == nullptr) { return false; }
        struct Name { const char *text; uint32_t mode; };
        static const Name names[] = {
            { "auto", Auto }, { "standard", Standard }, { "composite", Composite }, { "layer", Layer },
            { "compositor", Composite }, { "layers", Layer },
        };
        for (const Name &name : names) {
            const size_t k = std::strlen(name.text);
            if (n == k && std::memcmp(s, name.text, k) == 0) { *out = name.mode; return true; }
        }
        return false;
    }

    inline int HexDigit(char c) {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    }

    inline bool ParseTid(const char *s, size_t n, uint64_t *out) {
        if (s == nullptr || out == nullptr) { return false; }
        if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
        if (n != 16) { return false; }
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            const int h = HexDigit(s[i]);
            if (h < 0) { return false; }
            v = (v << 4) | static_cast<uint64_t>(h);
        }
        if (v == 0) { return false; }
        *out = v;
        return true;
    }

    inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    /* Parse "tid = mode" lines. Comments ('#' or ';') and [sections] are
     * ignored. Later lines win for the same title, as in the ini convention. */
    inline size_t ParseIni(const char *text, size_t len, Override *out, size_t cap) {
        if (text == nullptr || out == nullptr) { return 0; }
        size_t n = 0;
        for (size_t i = 0; i < len; ) {
            size_t e = i;
            while (e < len && text[e] != '\n' && text[e] != '\0') { ++e; }
            size_t a = i;
            while (a < e && IsSpace(text[a])) { ++a; }
            size_t b = e;
            while (b > a && IsSpace(text[b - 1])) { --b; }
            if (a < b && text[a] != '#' && text[a] != ';' && text[a] != '[') {
                size_t eq = a;
                while (eq < b && text[eq] != '=') { ++eq; }
                if (eq < b) {
                    size_t ke = eq;
                    while (ke > a && IsSpace(text[ke - 1])) { --ke; }
                    size_t v = eq + 1;
                    while (v < b && IsSpace(text[v])) { ++v; }
                    size_t ve = b;
                    while (ve > v && IsSpace(text[ve - 1])) { --ve; }
                    uint64_t tid = 0;
                    uint32_t mode = 0;
                    if (ParseTid(text + a, ke - a, &tid) && Parse(text + v, ve - v, &mode)) {
                        size_t k = 0;
                        while (k < n && out[k].tid != tid) { ++k; }
                        if (k == n && n < cap) { ++n; }
                        if (k < n) { out[k].tid = tid; out[k].mode = mode; }
                    }
                }
            }
            if (e < len && text[e] == '\0') { break; }
            i = e + 1;
        }
        return n;
    }

    inline uint32_t Resolve(uint64_t tid, uint32_t def, const Override *overrides, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (overrides[i].tid == tid) { return overrides[i].mode; }
        }
        return def;
    }

    inline size_t FormatLine(char *out, size_t cap, uint64_t tid, uint32_t mode) {
        if (out == nullptr || cap == 0) { return 0; }
        const int n = std::snprintf(out, cap, "%016llx = %s\n", static_cast<unsigned long long>(tid), Key(mode));
        return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
    }

    /* v0.7.15: the Auto probe's conservative test. A candidate is "live" when
     * its signature changed at least three times; "dead" otherwise. A game is
     * multi-surface when at least one layer changes and at least two stay
     * static/empty. A normal triple buffer (three live) and a still picture
     * (zero live) both stay on Standard. */
    inline void ClassifyCandidates(const uint32_t *changed, const uint32_t *seen, size_t n, uint32_t frames,
                                   uint32_t *live, uint32_t *dead, uint32_t *best) {
        uint32_t l = 0, d = 0, b = 0, bc = 0;
        for (size_t c = 0; c < n; ++c) {
            if (seen[c] < frames / 2) { continue; }
            if (changed[c] >= 3) {
                ++l;
                if (changed[c] > bc) { bc = changed[c]; b = static_cast<uint32_t>(c); }
            } else {
                ++d;
            }
        }
        if (live != nullptr) { *live = l; }
        if (dead != nullptr) { *dead = d; }
        if (best != nullptr) { *best = b; }
    }

    inline bool IsMultiSurface(uint32_t live, uint32_t dead) {
        return live >= 1 && dead >= 2;
    }

}
