/*
 * applet-mitm - arm file token parsing.
 *
 * Pure functions over a buffer, no libstratosphere, so the host test in
 * tier4/applet-mitm/test/ compiles exactly this code.
 *
 * M75 made ArmFileContains match whole tokens after `strstr` armed the grc
 * interceptor from "grcscan". ArmFileNumber kept `strstr`, so the same class
 * survived there: "sweep stream sw=768" read `sw` out of "sweep", found no
 * digits, and silently fell back to the default. Both now go through one
 * tokenizer: tokens are whitespace-separated, and "key=value" names "key".
 */
#pragma once
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace ams::mitm::applet::armfile {

    inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    /* Finds the token named `key`. On success *val points just past '=' (or at
     * the token end when there is no '='), and *val_len is the value length. */
    inline bool FindToken(const char *buf, size_t n, const char *key, const char **val, size_t *val_len) {
        const size_t klen = std::strlen(key);
        if (klen == 0) { return false; }
        for (size_t i = 0; i < n; ) {
            while (i < n && IsSpace(buf[i])) { ++i; }
            size_t j = i;
            while (j < n && !IsSpace(buf[j]) && buf[j] != '\0') { ++j; }
            if (j > i) {
                size_t name_len = j - i;
                for (size_t k = i; k < j; ++k) { if (buf[k] == '=') { name_len = k - i; break; } }
                if (name_len == klen && std::memcmp(buf + i, key, klen) == 0) {
                    const size_t v = (i + name_len < j) ? i + name_len + 1 : j;
                    if (val != nullptr)     { *val = buf + v; }
                    if (val_len != nullptr) { *val_len = j - v; }
                    return true;
                }
            }
            if (j < n && buf[j] == '\0') { break; }
            i = j;
        }
        return false;
    }

    inline bool Contains(const char *buf, size_t n, const char *key) {
        return FindToken(buf, n, key, nullptr, nullptr);
    }

    /* "key=123" -> 123. A bare "key", a non-numeric value or a missing token
     * all return `def`: a malformed number must not arm anything by accident. */
    inline unsigned Number(const char *buf, size_t n, const char *key, unsigned def) {
        const char *v = nullptr;
        size_t len = 0;
        if (!FindToken(buf, n, key, &v, &len) || len == 0) { return def; }
        unsigned out = 0;
        for (size_t i = 0; i < len; ++i) {
            if (v[i] < '0' || v[i] > '9') { return def; }
            out = out * 10 + static_cast<unsigned>(v[i] - '0');
        }
        return out;
    }


    /* ---- M98: the release config -------------------------------------------
     *
     * A release install has no arm file; the module reads
     * sdmc:/config/switch-frame-tap/config.ini instead - "key = value" lines,
     * '#' or ';' comments, written by the manager app or by hand - and turns
     * it into the same flag tokens the arm file uses, so everything after
     * this point is shared with test runs:
     *
     *   quality           = high | medium | low     -> nvqp=20 | 24 | 28
     *   qp                = 10..40                   (advanced; beats quality)
     *   keyframe_interval = frames between IDRs      -> nvgop= (default 60)
     *   max_resolution    = 1080 | 720               -> cap720 at 720
     *   start_delay       = seconds after boot       -> wait= (default 20)
     *   allow_untested_firmware = 1                  -> anyfw
     *   screenshot        = 1                        -> shot     (M99)
     *   screenshot_buttons = 0..3                    -> shotkey= (the combo preset)
     *   audio             = 1 | 0 (default 1)        -> audio    (v0.3: game audio from grc:d)
     *   usb_mode          = viewer | webcam          -> uvc      (v0.4: a UVC camera, read at boot)
     *
     * Unknown keys and malformed values are ignored: a bad config file must
     * leave the defaults, never arm something else. */
    inline bool IniValue(const char *ini, size_t n, const char *key, char *out, size_t cap) {
        const size_t klen = std::strlen(key);
        for (size_t i = 0; i < n; ) {
            size_t e = i;
            while (e < n && ini[e] != '\n' && ini[e] != '\0') { ++e; }
            size_t a = i;
            while (a < e && IsSpace(ini[a])) { ++a; }
            if (a < e && ini[a] != '#' && ini[a] != ';' && ini[a] != '[') {
                size_t eq = a;
                while (eq < e && ini[eq] != '=') { ++eq; }
                size_t kend = eq;
                while (kend > a && IsSpace(ini[kend - 1])) { --kend; }
                if (eq < e && kend - a == klen && std::memcmp(ini + a, key, klen) == 0) {
                    size_t v = eq + 1, ve = e;
                    while (v < ve && IsSpace(ini[v])) { ++v; }
                    while (ve > v && IsSpace(ini[ve - 1])) { --ve; }
                    if (ve - v >= cap) { return false; }
                    std::memcpy(out, ini + v, ve - v);
                    out[ve - v] = '\0';
                    return true;
                }
            }
            if (e < n && ini[e] == '\0') { break; }
            i = e + 1;
        }
        return false;
    }

    inline bool ParseUnsigned(const char *s, unsigned *out) {
        if (*s == '\0') { return false; }
        unsigned v = 0;
        for (; *s != '\0'; ++s) {
            if (*s < '0' || *s > '9' || v > 100000u) { return false; }
            v = v * 10 + static_cast<unsigned>(*s - '0');
        }
        *out = v;
        return true;
    }

    /* the arm tokens for a release install; returns the length written */
    inline size_t BuildReleaseArm(const char *ini, size_t n, char *out, size_t cap) {
        unsigned qp = 20, gop = 60, wait = 20, u = 0;
        unsigned shotkey = 0;
        bool cap720 = false, anyfw = false, shot = false, audio = true, uvc = false;
        char v[32];
        if (IniValue(ini, n, "quality", v, sizeof(v))) {
            if (std::strcmp(v, "medium") == 0) { qp = 24; }
            else if (std::strcmp(v, "low") == 0) { qp = 28; }
        }
        if (IniValue(ini, n, "qp", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 10 && u <= 40) { qp = u; }
        if (IniValue(ini, n, "keyframe_interval", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 1 && u <= 250) { gop = u; }
        if (IniValue(ini, n, "max_resolution", v, sizeof(v)) && std::strcmp(v, "720") == 0) { cap720 = true; }
        if (IniValue(ini, n, "start_delay", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 600) { wait = u; }
        if (IniValue(ini, n, "allow_untested_firmware", v, sizeof(v)) && std::strcmp(v, "1") == 0) { anyfw = true; }
        if (IniValue(ini, n, "screenshot", v, sizeof(v)) && std::strcmp(v, "1") == 0) { shot = true; }
        if (IniValue(ini, n, "screenshot_buttons", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 3) { shotkey = u; }
        if (IniValue(ini, n, "audio", v, sizeof(v)) && std::strcmp(v, "0") == 0) { audio = false; }
        if (IniValue(ini, n, "usb_mode", v, sizeof(v)) && std::strcmp(v, "webcam") == 0) { uvc = true; }
        char shotbuf[24] = "";
        if (shot) { std::snprintf(shotbuf, sizeof(shotbuf), " shot shotkey=%u", shotkey); }
        const int w = std::snprintf(out, cap, "vic exec dbg usb live nvqp=%u nvgop=%u wait=%u%s%s%s%s%s",
                                    qp, gop, wait, cap720 ? " cap720" : "", anyfw ? " anyfw" : "", shotbuf, audio ? " audio" : "", uvc ? " uvc" : "");
        return (w < 0 || static_cast<size_t>(w) >= cap) ? 0 : static_cast<size_t>(w);
    }

}
