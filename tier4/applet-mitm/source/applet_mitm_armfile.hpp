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
#include "shot_buttons.hpp"

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
     *   usb_keyframe_interval = 0..250               -> usbgop= (live over USB: an IDR every N
     *                                                  frames + on request, when longer than
     *                                                  keyframe_interval; default 240, 0 off)
     *   max_resolution    = 1080 | 720               -> cap720 at 720
     *   start_delay       = seconds after boot       -> wait= (default 20)
     *   allow_untested_firmware = 1                  -> anyfw
     *   screenshot        = 1                        -> shot     (M99)
     *   screenshot_buttons = 0..3                    -> shotkey= (the combo preset)
     *   screenshot_buttons = 4 + screenshot_custom = ZR+Down+Capture
     *                                                -> shotkey=4 shotmask=N [shotcap] (v0.7.11)
     *   screenshot_location = album                  -> shotalbum (v0.7.11: next to the
     *                                                  system's screenshots, Album/YYYY/MM/DD)
     *   audio             = 1 | 0 (default 1)        -> audio    (v0.3: game audio from grc:d)
     *   usb_mode          = viewer | webcam          -> uvc      (v0.4: a UVC camera, read at boot)
     *   encoder_test      = 1                        -> nvab     (v0.6: the bitrate experiment; not in the manager)
     *   connection        = usb | webcam | network   -> usb / usb uvc / net (v0.7: one at a time, read at
     *                                                  boot; network leaves USB alone entirely)
     *                       | webcam-any             -> usb uvc uvcraw (v0.7.7: uncompressed NV12, any camera app)
     *   (usb_mode = webcam and network = 1, the v0.4-v0.7 test keys, still mean webcam / network)
     *   detach_on_home    = 0                        -> nodethome (keep session through HOME;
     *                                                  default is detach + free donor memory)
     *   screen_off        = 1                        -> screenoff (v0.7.12: the console's screen
     *                                                  dark while a stream runs)
     *   early_collect     = 0 | 1 | ab (default 1)   -> earlyoff / earlyab (v0.7.12, PR #12: send a
     *                                                  finished 1080p encode while waiting for the next frame)
     *   home_menu         = 1                        -> homemenu (v0.7.14: stream the HOME menu and the
     *                                                  system applets when no game is on screen; beta)
     *   overlays          = 1                        -> overlays (v0.7.14: the volume bar, quick settings,
     *                                                  power menu and Tesla/Ultrahand in the stream; beta)
     *   homebrew_docked   = 1                        -> hbdocked (v0.7.18: homebrew started from the
     *                                                  Homebrew Menu is told the console is docked - 1080p)
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
        bool cap720 = false, anyfw = false, shot = false, audio = true, uvc = false, uvcraw = false, nvab = false, net = false, nohb = false, nodethome = false, shotalbum = false, screenoff = false, earlyoff = false, earlyab = false, homemenu = false, overlays = false, hbdocked = false;
        char v[32];
        if (IniValue(ini, n, "quality", v, sizeof(v))) {
            if (std::strcmp(v, "medium") == 0) { qp = 24; }
            else if (std::strcmp(v, "low") == 0) { qp = 28; }
        }
        if (IniValue(ini, n, "qp", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 10 && u <= 40) { qp = u; }
        if (IniValue(ini, n, "keyframe_interval", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 1 && u <= 250) { gop = u; }
        int usbgop = -1;               /* not set: the module's default (applet_mitm_gop.hpp) */
        if (IniValue(ini, n, "usb_keyframe_interval", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 250) { usbgop = static_cast<int>(u); }
        if (IniValue(ini, n, "max_resolution", v, sizeof(v)) && std::strcmp(v, "720") == 0) { cap720 = true; }
        if (IniValue(ini, n, "start_delay", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 600) { wait = u; }
        if (IniValue(ini, n, "allow_untested_firmware", v, sizeof(v)) && std::strcmp(v, "1") == 0) { anyfw = true; }
        if (IniValue(ini, n, "screenshot", v, sizeof(v)) && std::strcmp(v, "1") == 0) { shot = true; }
        if (IniValue(ini, n, "screenshot_buttons", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 4) { shotkey = u; }
        unsigned shotmask = 0;
        bool shotcap = false;
        {
            char cv[128];
            if (IniValue(ini, n, "screenshot_custom", cv, sizeof(cv))) { shotmask = sft::shotbuttons::Parse(cv, &shotcap); }
        }
        if (shotkey == 4 && shotmask == 0 && !shotcap) { shotkey = 0; }   /* custom, but nothing recorded */
        if (IniValue(ini, n, "screenshot_location", v, sizeof(v)) && std::strcmp(v, "album") == 0) { shotalbum = true; }
        if (IniValue(ini, n, "audio", v, sizeof(v)) && std::strcmp(v, "0") == 0) { audio = false; }
        if (IniValue(ini, n, "usb_mode", v, sizeof(v)) && std::strcmp(v, "webcam") == 0) { uvc = true; }
        if (IniValue(ini, n, "encoder_test", v, sizeof(v)) && std::strcmp(v, "1") == 0) { nvab = true; }
        if (IniValue(ini, n, "network", v, sizeof(v)) && std::strcmp(v, "1") == 0) { net = true; uvc = false; }
        if (IniValue(ini, n, "homebrew", v, sizeof(v)) && std::strcmp(v, "0") == 0) { nohb = true; }
        /* default on in code; only detach_on_home=0 emits nodethome */
        if (IniValue(ini, n, "detach_on_home", v, sizeof(v)) && std::strcmp(v, "0") == 0) { nodethome = true; }
        if (IniValue(ini, n, "screen_off", v, sizeof(v)) && std::strcmp(v, "1") == 0) { screenoff = true; }
        if (IniValue(ini, n, "home_menu", v, sizeof(v)) && std::strcmp(v, "1") == 0) { homemenu = true; }
        if (IniValue(ini, n, "overlays", v, sizeof(v)) && std::strcmp(v, "1") == 0) { overlays = true; }
        if (IniValue(ini, n, "homebrew_docked", v, sizeof(v)) && std::strcmp(v, "1") == 0) { hbdocked = true; }
        if (IniValue(ini, n, "early_collect", v, sizeof(v))) {
            if (std::strcmp(v, "0") == 0) { earlyoff = true; }
            else if (std::strcmp(v, "ab") == 0) { earlyab = true; }
        }
        if (IniValue(ini, n, "connection", v, sizeof(v))) {
            if (std::strcmp(v, "network") == 0) { net = true; uvc = uvcraw = false; }
            else if (std::strcmp(v, "webcam") == 0) { uvc = true; uvcraw = false; net = false; }
            else if (std::strcmp(v, "webcam-any") == 0) { uvc = true; uvcraw = true; net = false; }
            else if (std::strcmp(v, "usb") == 0) { uvc = uvcraw = false; net = false; }
        }
        char shotbuf[72] = "";
        if (shot) {
            char custom[40] = "";
            if (shotkey == 4) { std::snprintf(custom, sizeof(custom), " shotmask=%u%s", shotmask, shotcap ? " shotcap" : ""); }
            std::snprintf(shotbuf, sizeof(shotbuf), " shot shotkey=%u%s%s", shotkey, custom, shotalbum ? " shotalbum" : "");
        }
        char usbgopbuf[24] = "";
        if (usbgop >= 0) { std::snprintf(usbgopbuf, sizeof(usbgopbuf), " usbgop=%d", usbgop); }
        /* one transport: "usb" (the viewer or, with uvc, the webcam) or "net" */
        const int w = std::snprintf(out, cap, "vic exec dbg %s live nvqp=%u nvgop=%u wait=%u%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                                    net ? "net" : "usb", qp, gop, wait, cap720 ? " cap720" : "", anyfw ? " anyfw" : "", shotbuf, audio ? " audio" : "",
                                    uvc ? " uvc" : "", uvcraw ? " uvcraw" : "", nvab ? " nvab" : "", nohb ? " nohb" : "",
                                    nodethome ? " nodethome" : "", screenoff ? " screenoff" : "",
                                    earlyoff ? " earlyoff" : "", earlyab ? " earlyab" : "", homemenu ? " homemenu" : "", overlays ? " overlays" : "",
                                    hbdocked ? " hbdocked" : "", usbgopbuf);
        return (w < 0 || static_cast<size_t>(w) >= cap) ? 0 : static_cast<size_t>(w);
    }

}
