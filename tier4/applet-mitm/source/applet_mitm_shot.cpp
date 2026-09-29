#include "applet_mitm_shot.hpp"
#include "applet_mitm_png.hpp"
#include "applet_mitm_log.hpp"
#include <cstdio>

namespace ams::mitm::applet {

    constinit std::atomic<bool> g_shot_enabled{false};
    constinit std::atomic<u32>  g_shot_combo{0};
    constinit std::atomic<u32>  g_shot_count{0}, g_shot_fail{0};

    namespace {

        constinit std::atomic<u64> g_shot_request_tick{0};

        constexpr const char ShotDir[] = "sdmc:/switch/switch-frame-tap/screenshots";

        u64 ComboMask(u32 combo) {
            switch (combo) {
                case 1:  return HidNpadButton_L | HidNpadButton_R | HidNpadButton_Down;
                case 2:  return HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Down;
                case 3:  return HidNpadButton_Minus | HidNpadButton_Down;
                default: return HidNpadButton_StickL | HidNpadButton_StickR;
            }
        }

               alignas(os::ThreadStackAlignment) constinit u8 g_input_stack[16_KB];
        constinit os::ThreadType g_input_thread;

        void InputThread(void *) {
            bool ready = false, prev = false, time_ok = false;
            PadState pad;
            for (;;) {
                if (!g_shot_enabled.load(std::memory_order_relaxed)) {
                    prev = false;
                    os::SleepThread(TimeSpan::FromMilliSeconds(300));
                    continue;
                }
                if (!ready) {
                    /* M99b: READ-ONLY. M99 also called padConfigureInput and
                     * hid:sys EnableAppletToGetInput(aruid 0), as libtesla
                     * does; in Run AB the HOME menu then crashed opening the
                     * controller settings (2144-0001). Nothing here changes
                     * any controller configuration now: hidInitialize maps
                     * aruid 0's shared memory, padUpdate only reads it. */
                    const ::Result rh = hidInitialize();
                    time_ok = R_SUCCEEDED(timeInitialize());
                    LogLine("shot: input up (read-only) - hid rc=0x%x, time %s",
                            rh, time_ok ? "ok" : "unavailable (file names use the uptime)");
                    if (R_FAILED(rh)) {
                        g_shot_enabled.store(false, std::memory_order_relaxed);
                        LogLine("shot: no controller access - screenshots stay off this boot");
                        continue;
                    }
                    padInitializeAny(&pad);
                    ready = true;
                }
                padUpdate(&pad);
                const u64 mask = ComboMask(g_shot_combo.load(std::memory_order_relaxed));
                const bool now = (padGetButtons(&pad) & mask) == mask;
                if (now && !prev) {
                    g_shot_request_tick.store(armGetSystemTick(), std::memory_order_relaxed);
                    LogLine("shot: combo %u pressed", g_shot_combo.load(std::memory_order_relaxed));
                }
                prev = now;
                os::SleepThread(TimeSpan::FromMilliSeconds(25));
            }
        }

        void MakeShotPath(char *out, size_t cap) {
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

        /* the PNG goes out through this, 128 KB at a time */
        constexpr size_t OutBufSize = 128_KB;
        alignas(64) constinit u8 g_out_buf[OutBufSize];
        alignas(64) constinit u8 g_row_buf[1 + 1920 * 3];

    }

    void StartShotInput() {
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_input_thread), InputThread, nullptr,
                                        g_input_stack, sizeof(g_input_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_input_thread), "applet-mitm.ShotInput");
        os::StartThread(std::addressof(g_input_thread));
    }

    bool TakeShotRequest() {
        const u64 t = g_shot_request_tick.exchange(0, std::memory_order_relaxed);
        if (t == 0) { return false; }
        const u64 age_ms = armTicksToNs(armGetSystemTick() - t) / 1000000;
        if (age_ms > 3000) { LogLine("shot: a request %llu ms old - dropped", static_cast<unsigned long long>(age_ms)); return false; }
        return true;
    }

    bool WriteShot(const u8 *src, size_t src_size, u32 w, u32 h, u32 stride_bytes, u32 bh_log2) {
        if (w == 0 || h == 0 || w > 1920 || h > 1088) { ++g_shot_fail; LogLine("shot: %ux%u is not a size this writes", w, h); return false; }
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
            io_ok = R_SUCCEEDED(fs::WriteFile(f, off, g_out_buf, fill, fs::WriteOption::None));
            off += static_cast<s64>(fill);
            fill = 0;
            return io_ok;
        };
        const bool ok = png::WritePng(w, h, g_row_buf,
            [&](const uint8_t *d, size_t n) {
                while (n > 0) {
                    const size_t k = (OutBufSize - fill) < n ? (OutBufSize - fill) : n;
                    std::memcpy(g_out_buf + fill, d, k);
                    fill += k; d += k; n -= k;
                    if (fill == OutBufSize && !flush()) { return false; }
                }
                return true;
            },
            [&](uint32_t y, uint8_t *rgb) { png::DeswizzleRowRgb(src, src_size, y, w, stride_bytes, bh_log2, rgb); }) && flush();
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

}
