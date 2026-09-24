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

}
