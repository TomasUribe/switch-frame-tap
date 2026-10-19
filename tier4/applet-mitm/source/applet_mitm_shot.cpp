#include "applet_mitm_shot.hpp"
#include "applet_mitm_png.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include <cstdio>
#include "shot_buttons.hpp"

namespace ams::mitm::applet {

    constinit std::atomic<bool> g_shot_enabled{false};
    constinit std::atomic<u32>  g_shot_combo{0};
    constinit std::atomic<u32>  g_shot_mask{0};
    constinit std::atomic<bool> g_shot_capture{false};
    constinit std::atomic<bool> g_shot_album{false};
    constinit std::atomic<u32>  g_shot_count{0}, g_shot_fail{0};

    namespace {

        constinit std::atomic<u64> g_shot_request_tick{0};
        /* arm tick of the last HOME-button rising edge (0 = none this boot) */
        constinit std::atomic<u64> g_home_button_tick{0};

        constexpr const char ShotDir[] = "sdmc:/switch/switch-frame-tap/screenshots";

        u64 ComboMask(u32 combo) {
            switch (combo) {
                case 4:  return g_shot_mask.load(std::memory_order_relaxed);
                case 1:  return HidNpadButton_L | HidNpadButton_R | HidNpadButton_Down;
                case 2:  return HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Down;
                case 3:  return HidNpadButton_Minus | HidNpadButton_Down;
                default: return HidNpadButton_StickL | HidNpadButton_StickR;
            }
        }

        /* v0.7.11: the Capture button is not an Npad button; hid keeps its own
         * state in the same shared memory (read only - no hid:sys call: M99b's
         * EnableAppletToGetInput crashed the HOME menu's controller settings) */
        struct CaptureReader {
            u64 last_sampling = 0;
            bool said_live = false, said_dead = false;
            u64 t0 = 0;
            bool Held() {
                const auto *shm = static_cast<const HidSharedMemory *>(hidGetSharedmemAddr());
                if (shm == nullptr) { return false; }
                const HidCaptureButtonLifo &lifo = shm->capture_button.lifo;
                const u64 tail = lifo.header.tail;
                if (tail >= 17) { return false; }
                const HidCaptureButtonState st = lifo.storage[tail].state;
                if (t0 == 0) { t0 = armTicksToNs(armGetSystemTick()); last_sampling = st.sampling_number; }
                if (st.sampling_number != last_sampling && !said_live) {
                    said_live = true;
                    LogLine("shot: the Capture button is readable");
                }
                if (!said_live && !said_dead && armTicksToNs(armGetSystemTick()) - t0 > UINT64_C(60000000000)) {
                    said_dead = true;
                    LogLine("shot: the Capture button's state never changed in 60 s - a combo with Capture may not work on this console");
                }
                last_sampling = st.sampling_number;
                return st.buttons != 0;
            }
        };

        /* HOME is also not an Npad bit - same shm layout as Capture.
         * Read-only: no hid:sys (ActivateHomeButton / EnableAppletToGetInput).
         * hid:sys is not in this module's access list, and an earlier hid:sys
         * call from here crashed the HOME menu's controller settings (M99b). */
        struct HomeButtonReader {
            u64 last_buttons = 0;
            bool said_live = false, said_dead = false;
            u64 t0 = 0;

            bool PressedEdge() {
                const auto *shm = static_cast<const HidSharedMemory *>(hidGetSharedmemAddr());
                if (shm == nullptr) { return false; }
                const HidHomeButtonLifo &lifo = shm->home_button.lifo;
                const u64 tail = lifo.header.tail;
                if (tail >= 17) { return false; }
                const HidHomeButtonState st = lifo.storage[tail].state;
                if (t0 == 0) { t0 = armTicksToNs(armGetSystemTick()); }
                if (st.sampling_number != 0 && st.buttons != last_buttons && !said_live) {
                    said_live = true;
                    LogLine("homebtn: HOME button shm is live");
                }
                if (!said_live && !said_dead && armTicksToNs(armGetSystemTick()) - t0 > UINT64_C(60000000000)) {
                    said_dead = true;
                    LogLine("homebtn: HOME shm never changed in 60 s - detach will use Album/fallback only");
                }
                const bool held = st.buttons != 0;
                const bool edge = held && last_buttons == 0;
                last_buttons = st.buttons;
                return edge;
            }
        };

               alignas(os::ThreadStackAlignment) constinit u8 g_input_stack[8_KB];   /* v0.7.13: 16, 3 used */
        constinit os::ThreadType g_input_thread;

        void InputThread(void *) {
            bool ready = false, prev = false, time_ok = false, time_tried = false;
            bool hid_failed = false;
            PadState pad;
            CaptureReader capture;
            HomeButtonReader home;
            for (;;) {
                if (!ready && !hid_failed) {
                    /* READ-ONLY pad/shm. No hid:sys. Do NOT open time here:
                     * holding time from this sysmodule blanks the HOME clock
                     * and breaks timezone / network sync. timeInitialize stays
                     * gated to screenshots (same as upstream). */
                    SmUse sm_hid;
                    const ::Result rh = hidInitialize();
                    LogLine("input: hid up (read-only) rc=0x%x - HOME watch always on (shm only, 100 ms poll when shot off)", rh);
                    if (R_FAILED(rh)) {
                        hid_failed = true;
                        LogLine("input: no hid - HOME-button detach confirm unavailable this boot");
                    } else {
                        padInitializeAny(&pad);
                        ready = true;
                    }
                }
                const bool shot_on = g_shot_enabled.load(std::memory_order_relaxed);
                if (ready) {
                    if (home.PressedEdge()) {
                        g_home_button_tick.store(armGetSystemTick(), std::memory_order_relaxed);
                        LogLine("homebtn: HOME pressed");
                    }
                    if (shot_on) {
                        /* Filename timestamps only - not needed for HOME confirm. */
                        if (!time_tried) {
                            time_tried = true;
                            time_ok = R_SUCCEEDED(timeInitialize());
                            LogLine("input: time %s (screenshot filenames)", time_ok ? "ok" : "unavailable");
                        }
                        padUpdate(&pad);
                        const u32 combo = g_shot_combo.load(std::memory_order_relaxed);
                        const u64 mask = ComboMask(combo);
                        const bool want_capture = combo == 4 && g_shot_capture.load(std::memory_order_relaxed);
                        const bool cap_held = (want_capture || combo == 4) ? capture.Held() : false;
                        const bool now = (mask != 0 || want_capture) && (padGetButtons(&pad) & mask) == mask && (!want_capture || cap_held);
                        if (now && !prev) {
                            g_shot_request_tick.store(armGetSystemTick(), std::memory_order_relaxed);
                            LogLine("shot: combo %u pressed", combo);
                        }
                        prev = now;
                    } else {
                        prev = false;
                    }
                }
                /* HOME-only: 100 ms is enough for a button edge (confirm window
                 * is seconds). Screenshots keep 25 ms so combos stay snappy.
                 * Core 3 is shared with the display-IPC thread. */
                os::SleepThread(TimeSpan::FromMilliSeconds(shot_on ? 25 : 100));
            }
        }

        /* v0.7.11: where the system keeps its screenshots - sdmc:/Nintendo/Album,
         * or under emuMMC the redirected Nintendo folder (emummc.ini
         * nintendo_path, e.g. sdmc:/emuMMC/RAW1/Nintendo/Album) */
        const char *AlbumRoot() {
            static char root[0x100] = "";
            if (root[0] == 0) {
                const char *nd = ::ams::emummc::IsActive() ? ::ams::emummc::GetNintendoDirPath() : nullptr;
                if (nd != nullptr && nd[0] != 0) {
                    std::snprintf(root, sizeof(root), "sdmc:%s%s/Album", nd[0] == '/' ? "" : "/", nd);
                } else {
                    std::snprintf(root, sizeof(root), "sdmc:/Nintendo/Album");
                }
            }
            return root;
        }

        /* Album/YYYY/MM/DD/YYYYMMDDHHMMSS00-SwitchFrameTap.png: the system's
         * date folders and the start of its names, so a PC sorts them with the
         * console's own screenshots; the Album app skips names it cannot
         * parse (its own are 32 hex digits) and shows only its signed JPEGs */
        bool MakeAlbumPath(char *out, size_t cap, const TimeCalendarTime &ct) {
            char dir[0x140];
            std::snprintf(dir, sizeof(dir), "%s", AlbumRoot());
            static_cast<void>(fs::CreateDirectory(dir));
            const size_t n = std::strlen(dir);
            std::snprintf(dir + n, sizeof(dir) - n, "/%04u", ct.year);   static_cast<void>(fs::CreateDirectory(dir));
            std::snprintf(dir + n + 5, sizeof(dir) - n - 5, "/%02u", ct.month); static_cast<void>(fs::CreateDirectory(dir));
            std::snprintf(dir + n + 8, sizeof(dir) - n - 8, "/%02u", ct.day);   static_cast<void>(fs::CreateDirectory(dir));
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), dir))) { return false; }
            for (u32 k = 0; k < 100; ++k) {
                std::snprintf(out, cap, "%s/%04u%02u%02u%02u%02u%02u%02u-SwitchFrameTap.png", dir, ct.year, ct.month, ct.day, ct.hour, ct.minute, ct.second, k);
                if (R_FAILED(fs::GetEntryType(std::addressof(t), out))) { return true; }
            }
            return false;
        }

        void MakeShotPath(char *out, size_t cap) {
            if (g_shot_album.load(std::memory_order_relaxed)) {
                u64 posix = 0;
                TimeCalendarTime ct = {};
                if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &posix)) &&
                    R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &ct, nullptr)) && MakeAlbumPath(out, cap, ct)) {
                    return;
                }
                LogLine("shot: the album folder is not usable - saving to %s instead", ShotDir);
            }
            static_cast<void>(fs::CreateDirectory("sdmc:/switch"));
            static_cast<void>(fs::CreateDirectory("sdmc:/switch/switch-frame-tap"));
            static_cast<void>(fs::CreateDirectory(ShotDir));
            u64 posix = 0;
            TimeCalendarTime ct = {};
            if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &posix)) &&
                R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &ct, nullptr))) {
                std::snprintf(out, cap, "%s/%04u-%02u-%02u_%02u-%02u-%02u.png", ShotDir, ct.year, ct.month, ct.day, ct.hour, ct.minute, ct.second);
            } else {
                std::snprintf(out, cap, "%s/shot-%llu.png", ShotDir, static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1000000));
            }
            /* two in one second: add a suffix */
            fs::DirectoryEntryType t;
            if (R_SUCCEEDED(fs::GetEntryType(std::addressof(t), out))) {
                const size_t n = std::strlen(out);
                for (u32 k = 2; k < 100; ++k) {
                    std::snprintf(out + n - 4, cap - (n - 4), "-%u.png", k);
                    if (R_FAILED(fs::GetEntryType(std::addressof(t), out))) { break; }
                }
            }
        }

        /* M99c: a toast in the top-left corner through Ultrahand, the overlay
         * menu: it shows every "<APP>-*.notify" JSON file in its notifications
         * folder (libultrahand tesla.hpp: "text" required; "title",
         * "duration" in ms, "font_size", "priority" optional) and deletes it
         * when the toast ends. Only if that folder exists - nothing is created
         * for users of another overlay menu. Written under a temporary name
         * and renamed, so it is never read half-written. */
        void Notify(const char *text) {
            constexpr const char Dir[] = "sdmc:/config/ultrahand/notifications";
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), Dir)) || t != fs::DirectoryEntryType_Directory) { return; }
            const unsigned long long ms = armTicksToNs(armGetSystemTick()) / 1000000;
            char tmp[128], dst[128], json[256];
            std::snprintf(tmp, sizeof(tmp), "%s/SwitchFrameTap-%llu.tmp", Dir, ms);
            std::snprintf(dst, sizeof(dst), "%s/SwitchFrameTap-%llu.notify", Dir, ms);
            const int n = std::snprintf(json, sizeof(json), "{\"title\":\"Switch Frame Tap\",\"text\":\"%s\",\"duration\":3000}", text);
            if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) { return; }
            if (R_FAILED(fs::CreateFile(tmp, n))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), tmp, fs::OpenMode_Write))) { static_cast<void>(fs::DeleteFile(tmp)); return; }
            const bool ok = R_SUCCEEDED(fs::WriteFile(f, 0, json, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
            if (!ok || R_FAILED(fs::RenameFile(tmp, dst))) { static_cast<void>(fs::DeleteFile(tmp)); }
        }

        alignas(64) constinit u8 g_row_buf[1 + 1920 * 3];

    }

    void NotifyShot(const char *text) { Notify(text); }

    void StartShotInput() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_input_thread), InputThread, nullptr,
                                        g_input_stack, sizeof(g_input_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_input_thread), "applet-mitm.ShotInput");
        os::StartThread(std::addressof(g_input_thread));
        NoteThread(std::addressof(g_input_thread));
    }

    bool HomeButtonRecent(u32 within_ms) {
        const u64 t = g_home_button_tick.load(std::memory_order_relaxed);
        if (t == 0 || within_ms == 0) { return false; }
        const u64 age_ms = armTicksToNs(armGetSystemTick() - t) / UINT64_C(1000000);
        return age_ms <= within_ms;
    }

    bool ShotRequestPending() { return g_shot_request_tick.load(std::memory_order_relaxed) != 0; }

    bool TakeShotRequest() {
        const u64 t = g_shot_request_tick.exchange(0, std::memory_order_relaxed);
        if (t == 0) { return false; }
        const u64 age_ms = armTicksToNs(armGetSystemTick() - t) / 1000000;
        if (age_ms > 3000) { LogLine("shot: a request %llu ms old - dropped", static_cast<unsigned long long>(age_ms)); return false; }
        return true;
    }

    bool WriteShot(u8 *out, const u8 *src, size_t src_size, u32 w, u32 h, u32 stride_bytes, u32 bh_log2, bool bgra, u32 flip) {
        if (w == 0 || h == 0 || w > 1920 || h > 1088) { ++g_shot_fail; LogLine("shot: %ux%u is not a size this writes", w, h); return false; }
        if (out == nullptr) { ++g_shot_fail; LogLine("shot: no capture memory for the PNG"); return false; }
        const u64 t0 = armTicksToNs(armGetSystemTick());
        char path[128];
        MakeShotPath(path, sizeof(path));
        /* M99b: created at its final size - one allocation instead of 48 extensions */
        if (R_FAILED(fs::CreateFile(path, static_cast<s64>(png::PngSize(w, h))))) { ++g_shot_fail; LogLine("shot: cannot create %s (SD full?)", path); return false; }
        fs::FileHandle f;
        if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) {
            ++g_shot_fail; LogLine("shot: cannot open %s", path); return false;
        }
        s64 off = 0;
        size_t fill = 0;
        bool io_ok = true;
        auto flush = [&]() {
            if (fill == 0 || !io_ok) { return io_ok; }
            io_ok = R_SUCCEEDED(fs::WriteFile(f, off, out, fill, fs::WriteOption::None));
            off += static_cast<s64>(fill);
            fill = 0;
            return io_ok;
        };
        const bool ok = png::WritePng(w, h, g_row_buf,
            [&](const uint8_t *d, size_t n) {
                while (n > 0) {
                    const size_t k = (ShotOutBufSize - fill) < n ? (ShotOutBufSize - fill) : n;
                    std::memcpy(out + fill, d, k);
                    fill += k; d += k; n -= k;
                    if (fill == ShotOutBufSize && !flush()) { return false; }
                }
                return true;
            },
            [&](uint32_t y, uint8_t *rgb) {
                png::DeswizzleRowRgb(src, src_size, (flip & 2) ? h - 1 - y : y, w, stride_bytes, bh_log2, rgb, bgra);
                if (flip & 1) { png::MirrorRowRgb(rgb, w); }
            }) && flush();
        static_cast<void>(fs::FlushFile(f));
        fs::CloseFile(f);
        const u64 ms = (armTicksToNs(armGetSystemTick()) - t0) / 1000000;
        if (!ok) {
            ++g_shot_fail;
            static_cast<void>(fs::DeleteFile(path));
            LogLine("shot: writing %s FAILED after %llu ms (SD full?)", path, static_cast<unsigned long long>(ms));
            Notify("Screenshot failed - is the SD card full?");
            return false;
        }
        ++g_shot_count;
        LogLine("shot: %ux%u -> %s (%lld KB, %llu ms)", w, h, path, static_cast<long long>(off / 1024), static_cast<unsigned long long>(ms));
        {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Screenshot saved (%ux%u)", w, h);
            Notify(msg);
        }
        return true;
    }

    void NoteAlbumPath() {
        constexpr const char *P = "sdmc:/config/switch-frame-tap/album_path.txt";
        char line[0x110];
        const int n = std::snprintf(line, sizeof(line), "%s\n", AlbumRoot());
        if (n <= 0) { return; }
        static_cast<void>(fs::DeleteFile(P));
        if (R_FAILED(fs::CreateFile(P, n))) { return; }
        fs::FileHandle f;
        if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), P, fs::OpenMode_Write))) {
            static_cast<void>(fs::WriteFile(f, 0, line, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }
        LogLine("shot: the system's screenshots are in %s%s", AlbumRoot(), g_shot_album.load() ? " - saving there (screenshot_location = album)" : "");
    }

}
