#include "applet_mitm_capture.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    namespace {

        constexpr size_t MaxAutoHints = 8;
        constexpr const char *ConfigDir = "sdmc:/config/switch-frame-tap";

        constinit os::SdkMutex g_cap_lock;
        constinit u32 g_capture_default = sftcapture::Auto;
        constinit sftcapture::Override g_capture_overrides[sftcapture::MaxOverrides] = {};
        constinit size_t g_capture_override_n = 0;
        struct AutoHint { u64 tid; u32 mode; };
        constinit AutoHint g_capture_hints[MaxAutoHints] = {};
        constinit size_t g_capture_hint_n = 0;

        size_t ReadFile(const char *path, char *buf, size_t cap) {
            if (buf == nullptr || cap == 0) { return 0; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read))) { return 0; }
            s64 sz = 0;
            size_t n = 0;
            if (R_SUCCEEDED(fs::GetFileSize(std::addressof(sz), f)) && sz > 0) {
                n = static_cast<size_t>(sz) < cap - 1 ? static_cast<size_t>(sz) : cap - 1;
                if (R_FAILED(fs::ReadFile(f, 0, buf, n))) { n = 0; }
            }
            fs::CloseFile(f);
            buf[n] = '\0';
            return n;
        }

        bool SaveOverridesLocked() {
            static_cast<void>(fs::CreateDirectory(ConfigDir));
            if (g_capture_override_n == 0) {
                static_cast<void>(fs::DeleteFile(sftcapture::IniPath));
                return true;
            }
            char buf[sftcapture::MaxOverrides * 40 + 160];
            size_t n = 0;
            const int h = std::snprintf(buf, sizeof(buf),
                                        "# Switch Frame Tap per-title capture modes - written by the overlay/sysmodule\n"
                                        "# <title id> = auto | standard | composite | layer\n");
            if (h > 0) { n = static_cast<size_t>(h); }
            for (size_t i = 0; i < g_capture_override_n && n < sizeof(buf); ++i) {
                n += sftcapture::FormatLine(buf + n, sizeof(buf) - n, g_capture_overrides[i].tid, g_capture_overrides[i].mode);
            }
            static_cast<void>(fs::DeleteFile(sftcapture::IniPath));
            if (R_FAILED(fs::CreateFile(sftcapture::IniPath, static_cast<s64>(n)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), sftcapture::IniPath, fs::OpenMode_Write))) { return false; }
            const bool ok = R_SUCCEEDED(fs::WriteFile(f, 0, buf, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
            return ok;
        }

        u32 HintFor(u64 tid) {
            for (size_t i = 0; i < g_capture_hint_n; ++i) {
                if (g_capture_hints[i].tid == tid) { return g_capture_hints[i].mode; }
            }
            return sftcapture::Auto;
        }

        void SetHintLocked(u64 tid, u32 mode) {
            size_t i = 0;
            while (i < g_capture_hint_n && g_capture_hints[i].tid != tid) { ++i; }
            if (i == g_capture_hint_n && g_capture_hint_n < MaxAutoHints) { ++g_capture_hint_n; }
            if (i < g_capture_hint_n) { g_capture_hints[i].tid = tid; g_capture_hints[i].mode = mode; }
        }

    }

    constinit std::atomic<u32> g_capture_mode_active{0};
    constinit std::atomic<u32> g_capture_mode_requested{0};
    constinit std::atomic<u32> g_capture_ncand{0};
    constinit std::atomic<u32> g_capture_live_cands{0};
    constinit std::atomic<u32> g_capture_dead_cands{0};
    constinit std::atomic<u64> g_capture_tid{0};
    constinit std::atomic<bool> g_capture_reconfig{false};
    constinit std::atomic<bool> g_capture_auto_probe{false};
    constinit std::atomic<u32> g_capture_layer_cand{0xFF};
    constinit std::atomic<u64> g_capture_layer_addr{0};
    constinit std::atomic<u64> g_capture_layer_tid{0};

    void SetCaptureDefault(u32 mode) {
        if (mode > sftcapture::Layer) { mode = sftcapture::Auto; }
        std::scoped_lock lk(g_cap_lock);
        g_capture_default = mode;
        LogLine("capture: global mode %s", sftcapture::Key(mode));
    }

    u32 CaptureDefault() {
        std::scoped_lock lk(g_cap_lock);
        return g_capture_default;
    }

    void LoadCaptureConfig() {
        static char text[4096];
        const size_t n = ReadFile(sftcapture::IniPath, text, sizeof(text));
        sftcapture::Override tmp[sftcapture::MaxOverrides] = {};
        const size_t count = sftcapture::ParseIni(text, n, tmp, sftcapture::MaxOverrides);
        std::scoped_lock lk(g_cap_lock);
        for (size_t i = 0; i < count; ++i) { g_capture_overrides[i] = tmp[i]; }
        g_capture_override_n = count;
        LogLine("capture: %zu per-title override(s) from %s", count, n != 0 ? sftcapture::IniPath : "(none)");
    }

    u32 ResolveCaptureMode(u64 tid) {
        std::scoped_lock lk(g_cap_lock);
        for (size_t i = 0; i < g_capture_override_n; ++i) {
            if (g_capture_overrides[i].tid == tid) {
                /* a per-title auto means "follow the global setting"; when the
                 * global is Auto it probes again instead of using the hint */
                if (g_capture_overrides[i].mode == sftcapture::Auto) { return g_capture_default; }
                return g_capture_overrides[i].mode;
            }
        }
        u32 mode = g_capture_default;
        if (mode == sftcapture::Auto && tid != 0) {
            const u32 hint = HintFor(tid);
            if (hint != sftcapture::Auto) { mode = hint; }
        }
        return mode;
    }

    void SetCaptureMode(u64 tid, u32 mode) {
        if (tid == 0 || mode > sftcapture::Layer) { return; }
        std::scoped_lock lk(g_cap_lock);
        size_t i = 0;
        while (i < g_capture_override_n && g_capture_overrides[i].tid != tid) { ++i; }
        if (mode == sftcapture::Auto) {
            /* Auto means "follow the global setting": remove the override and
             * forget the hint, so the next session probes again. */
            if (i < g_capture_override_n) {
                for (size_t k = i; k + 1 < g_capture_override_n; ++k) { g_capture_overrides[k] = g_capture_overrides[k + 1]; }
                --g_capture_override_n;
            }
            SetHintLocked(tid, sftcapture::Auto);
        } else {
            if (i == g_capture_override_n && g_capture_override_n < sftcapture::MaxOverrides) { ++g_capture_override_n; }
            if (i < g_capture_override_n) { g_capture_overrides[i].tid = tid; g_capture_overrides[i].mode = mode; }
        }
        const bool saved = SaveOverridesLocked();
        LogLine("capture: %016llx -> %s (%s)", static_cast<unsigned long long>(tid), sftcapture::Key(mode),
                saved ? "saved to capture.ini" : "NOT saved");
        g_capture_reconfig.store(true, std::memory_order_relaxed);
    }

    size_t CaptureOverrides(sftcapture::Override *out, size_t cap) {
        std::scoped_lock lk(g_cap_lock);
        const size_t n = g_capture_override_n < cap ? g_capture_override_n : cap;
        for (size_t i = 0; i < n; ++i) { out[i] = g_capture_overrides[i]; }
        return n;
    }

    void CaptureAutoDetected(u64 tid, u32 mode, const char *why) {
        {
            std::scoped_lock lk(g_cap_lock);
            SetHintLocked(tid, mode);
        }
        LogLine("capture: auto -> %s for %016llx: %s", sftcapture::Key(mode),
                static_cast<unsigned long long>(tid), why != nullptr ? why : "multi-surface game");
        g_capture_reconfig.store(true, std::memory_order_relaxed);
    }

    bool CompositeCaptureAvailable() {
        /* v0.7.15: the compositor route is a prototype; it is armed only when
         * the build sets the token, and false means Auto falls back to Layer. */
        return false;
    }

}
