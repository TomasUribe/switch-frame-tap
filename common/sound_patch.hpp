/*
 * "Sound in no-record games": the bundled dvr-patches (exelix11, BSD-3 -
 * third_party/dvr-patches) on or off. Shared by the manager app and the
 * overlay; plain stdio on sdmc:, which the caller must have mounted (the
 * overlay wraps calls in tsl::hlp::doWithSDCardHandle).
 *
 *   Bundled:  config/switch-frame-tap/dvr-patches/  (.ips files from the release zip; off)
 *   On:       atmosphere/exefs_patches/switch-frame-tap-sound/  (a copy; am reads it at boot)
 *
 * dvr-patches installed by hand (atmosphere/exefs_patches/am, as the v0.7.2
 * README said) count as on too; off parks that folder at
 * config/switch-frame-tap/dvr-patches-am-off, and on puts it back when nothing
 * is bundled. The sysmodule refreshes the copy from the bundle at boot.
 */
#pragma once
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sftsound {

    constexpr const char *Bundled = "sdmc:/config/switch-frame-tap/dvr-patches";
    constexpr const char *Ours    = "sdmc:/atmosphere/exefs_patches/switch-frame-tap-sound";
    constexpr const char *HandAm  = "sdmc:/atmosphere/exefs_patches/am";
    constexpr const char *HandOff = "sdmc:/config/switch-frame-tap/dvr-patches-am-off";

    enum class State { Missing, Off, On };

    inline bool IsDir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

    inline bool IsIps(const char *name) {
        const size_t n = std::strlen(name);
        return n > 4 && (std::strcmp(name + n - 4, ".ips") == 0 || std::strcmp(name + n - 4, ".IPS") == 0);
    }

    /* how many .ips files the directory holds */
    inline int CountIps(const char *dir) {
        DIR *d = opendir(dir);
        if (d == nullptr) { return 0; }
        int n = 0;
        while (const dirent *e = readdir(d)) { if (IsIps(e->d_name)) { ++n; } }
        closedir(d);
        return n;
    }

    inline State Get() {
        if (CountIps(Ours) > 0 || IsDir(HandAm)) { return State::On; }
        if (CountIps(Bundled) > 0 || IsDir(HandOff)) { return State::Off; }
        return State::Missing;
    }

    inline bool CopyFile(const char *from, const char *to) {
        FILE *in = std::fopen(from, "rb");
        if (in == nullptr) { return false; }
        FILE *out = std::fopen(to, "wb");
        if (out == nullptr) { std::fclose(in); return false; }
        char buf[1024];
        size_t n;
        bool ok = true;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) { ok &= std::fwrite(buf, 1, n, out) == n; }
        std::fclose(in);
        ok &= std::fclose(out) == 0;
        return ok;
    }

    /* every file in dir, then dir itself */
    inline void RemoveDir(const char *dir) {
        DIR *d = opendir(dir);
        if (d == nullptr) { return; }
        char p[512];
        while (const dirent *e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) { continue; }
            std::snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            std::remove(p);
        }
        closedir(d);
        rmdir(dir);
    }

    /* true when the state on the SD card is now `on` (a restart applies it) */
    inline bool Set(bool on) {
        if (on) {
            if (CountIps(Bundled) > 0) {
                mkdir("sdmc:/atmosphere/exefs_patches", 0777);
                mkdir(Ours, 0777);
                DIR *d = opendir(Bundled);
                if (d == nullptr) { return false; }
                int copied = 0;
                char from[512], to[512];
                while (const dirent *e = readdir(d)) {
                    if (!IsIps(e->d_name)) { continue; }
                    std::snprintf(from, sizeof(from), "%s/%s", Bundled, e->d_name);
                    std::snprintf(to, sizeof(to), "%s/%s", Ours, e->d_name);
                    if (CopyFile(from, to)) { ++copied; }
                }
                closedir(d);
                return copied > 0;
            }
            if (IsDir(HandOff)) { return std::rename(HandOff, HandAm) == 0; }
            return false;
        }
        RemoveDir(Ours);
        if (IsDir(HandAm)) {
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/switch-frame-tap", 0777);
            if (IsDir(HandOff)) { RemoveDir(HandOff); }
            std::rename(HandAm, HandOff);
        }
        return Get() != State::On;
    }

}
