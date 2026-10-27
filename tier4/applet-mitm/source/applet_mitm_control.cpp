#include "applet_mitm_uvc.hpp"
#include "applet_mitm_net.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_donor.hpp"
#include <cstdio>
#include "applet_mitm_nv.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_uisource.hpp"
#include "applet_mitm_overlay.hpp"

namespace ams::mitm::applet {

    namespace {

        constexpr const char OffDir[]  = "sdmc:/config/switch-frame-tap";
        constexpr const char OffFile[] = "sdmc:/config/switch-frame-tap/stream-off";
        constexpr const char ExclFile[] = "sdmc:/config/switch-frame-tap/excluded.txt";

        /* the never-attach list: program ids, one per line in hex */
        constexpr size_t MaxExcluded = 64;
        constinit os::SdkMutex g_excl_lock;
        constinit u64 g_excl[MaxExcluded] = {};
        constinit size_t g_excl_n = 0;

        void SaveExcludedLocked() {
            char buf[MaxExcluded * 17 + 1];
            size_t n = 0;
            for (size_t i = 0; i < g_excl_n; ++i) { n += std::snprintf(buf + n, sizeof(buf) - n, "%016llx\n", static_cast<unsigned long long>(g_excl[i])); }
            static_cast<void>(fs::DeleteFile(ExclFile));
            if (n == 0) { return; }
            static_cast<void>(fs::CreateDirectory(OffDir));
            if (R_FAILED(fs::CreateFile(ExclFile, static_cast<s64>(n)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), ExclFile, fs::OpenMode_Write))) { return; }
            static_cast<void>(fs::WriteFile(f, 0, buf, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }

        class StreamControl {
            public:
                Result GetStatus(sf::Out<StreamStatus> out) {
                    StreamStatus st = {};
                    st.version      = 4;
                    st.reserved3    = (g_uvc_mode ? 1u : 0u) | (g_net_armed ? 2u : 0u) | (NetClientPresent() ? 4u : 0u) | (g_uvc_raw ? 8u : 0u) | (g_ui_stream_armed ? 16u : 0u) | (OverlaysArmed() ? 32u : 0u)
                        | (HeadroomNeeded() ? kReserved3NeedHeadroom : 0u);
                    st.reserved     = (g_audio_enabled.load(std::memory_order_relaxed) ? 0x100u : 0u) | g_audio_state.load(std::memory_order_relaxed);   /* v4: audio */
                    st.state        = g_live_state.load(std::memory_order_relaxed);
                    st.enabled      = g_stream_enabled.load(std::memory_order_relaxed) ? 1 : 0;
                    st.width        = g_live_w.load(std::memory_order_relaxed);
                    st.height       = g_live_h.load(std::memory_order_relaxed);
                    st.fps_x10      = g_live_fps_x10.load(std::memory_order_relaxed);
                    st.game_fps_x10 = g_live_game_fps_x10.load(std::memory_order_relaxed);
                    st.sessions     = g_live_sessions.load(std::memory_order_relaxed);
                    st.shot_enabled = g_shot_enabled.load(std::memory_order_relaxed) ? 1 : 0;
                    st.shots        = g_shot_count.load(std::memory_order_relaxed);
                    /* While NeedHeadroom is set, surface the requested MB here so
                     * SwitchU does not need an extra round-trip (GetDonorCap still works). */
                    st.shot_fails   = HeadroomNeeded() ? HeadroomNeedMb() : g_shot_fail.load(std::memory_order_relaxed);
                    st.app_tid      = g_app_tid.load(std::memory_order_relaxed);
                    st.app_excluded = g_app_excluded.load(std::memory_order_relaxed) ? 1 : 0;
                    out.SetValue(st);
                    R_SUCCEED();
                }

                Result SetEnabled(u8 on) {
                    const bool en = on != 0;
                    if (g_stream_enabled.exchange(en, std::memory_order_relaxed) == en) { R_SUCCEED(); }
                    /* remembered across boots: the file's presence means off */
                    /* M98: the rcs are logged - Run Z left stream-off on the card
                     * after an ON, and this boot had said "on at boot" */
                    Result r1 = ResultSuccess(), r2 = ResultSuccess();
                    if (en) {
                        r1 = fs::DeleteFile(OffFile);
                    } else {
                        r1 = fs::CreateDirectory(OffDir);
                        r2 = fs::CreateFile(OffFile, 0);
                    }
                    LogLine("sftap: stream turned %s (%s rc=0x%x%s)", en ? "ON" : "OFF", en ? "delete stream-off" : "mkdir",
                            r1.GetValue(), en ? "" : (R_SUCCEEDED(r2) ? ", create ok" : ", create FAILED"));
                    if (!en && R_FAILED(r2)) { LogLine("sftap: create stream-off rc=0x%x", r2.GetValue()); }
                    R_SUCCEED();
                }

                Result SetScreenOff(u8 on) {
                    SetScreenOffNow(on != 0);
                    LogLine("sftap: screen off while streaming %s (overlay)", on ? "ON" : "OFF");
                    R_SUCCEED();
                }

                Result ReloadConfig() {
                    ReloadReleaseConfig();
                    LoadExcluded();
                    R_SUCCEED();
                }

                /* v0.7.10: the memory donor (applet_mitm_donor.hpp) */
                Result Donate(u64 size, sf::CopyHandle &&tmem) {
                    DonorAccept(size, tmem.GetOsHandle());
                    tmem.Detach();
                    R_SUCCEED();
                }
                /* v0.7.13: and the nvdrv session's transfer memory, from the
                 * same Applet-pool heap - not 256 KB of our System group */
                Result Donate2(u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem) {
                    DonorAcceptNv(nv_size, nv_tmem.GetOsHandle());
                    nv_tmem.Detach();
                    DonorAccept(size, tmem.GetOsHandle());
                    tmem.Detach();
                    R_SUCCEED();
                }
                Result DonateFailed(u32 rc) {
                    DonorFailed(rc);
                    R_SUCCEED();
                }
                /* v0.7.14: the most the donor may take (MB) - less than the usual
                 * 42 while the HOME menu streams beside a big applet (Settings) */
                Result GetDonorCap(sf::Out<u32> out_mb) {
                    out_mb.SetValue(DonorCapMb());
                    R_SUCCEED();
                }
                Result AckHeadroom(u32 free_kb) {
                    HeadroomAck(free_kb);
                    R_SUCCEED();
                }
                Result SetAppExcluded(u64 program_id, u8 excluded) {
                    SetExcluded(program_id, excluded != 0);
                    R_SUCCEED();
                }
        };
        static_assert(IsIStreamControl<StreamControl>);

        using ControlServerOptions = sf::hipc::DefaultServerManagerOptions;
        constexpr sm::ServiceName ControlServiceName = sm::ServiceName::Encode("sftap");
        constexpr size_t ControlMaxSessions = 4;   /* app, overlay, donor, SwitchU */

        sf::hipc::ServerManager<1, ControlServerOptions, ControlMaxSessions> g_control_manager;
        constinit sf::UnmanagedServiceObject<IStreamControl, StreamControl> g_control_object;

        alignas(os::ThreadStackAlignment) constinit u8 g_control_stack[8_KB];   /* v0.7.13: 16, 2 used */
        constinit os::ThreadType g_control_thread;

        void ControlThread(void *) {
            LoopProcessSafely(g_control_manager, "sftap");
        }

    }

    void LoadExcluded() {
        char buf[MaxExcluded * 18];
        size_t len = 0;
        fs::FileHandle f;
        if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), ExclFile, fs::OpenMode_Read))) {
            s64 sz = 0;
            if (R_SUCCEEDED(fs::GetFileSize(std::addressof(sz), f)) && sz > 0) {
                len = static_cast<size_t>(sz) < sizeof(buf) - 1 ? static_cast<size_t>(sz) : sizeof(buf) - 1;
                if (R_FAILED(fs::ReadFile(f, 0, buf, len))) { len = 0; }
            }
            fs::CloseFile(f);
        }
        buf[len] = '\0';
        std::scoped_lock lk(g_excl_lock);
        g_excl_n = 0;
        for (size_t i = 0; i < len && g_excl_n < MaxExcluded; ) {
            u64 v = 0;
            size_t d = 0;
            for (; i < len; ++i, ++d) {
                const char c = buf[i];
                const int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (h < 0) { break; }
                v = (v << 4) | static_cast<u64>(h);
            }
            if (d == 16 && v != 0) { g_excl[g_excl_n++] = v; }
            while (i < len && buf[i] != '\n') { ++i; }
            ++i;
        }
        const u64 cur = g_app_tid.load(std::memory_order_relaxed);
        bool ex = false;
        for (size_t k = 0; k < g_excl_n; ++k) { if (g_excl[k] == cur) { ex = true; } }
        g_app_excluded.store(cur != 0 && ex, std::memory_order_relaxed);
        LogLine("sftap: %zu app(s) excluded (never attached)", g_excl_n);
    }

    void SetExcluded(u64 program_id, bool excluded) {
        if (program_id == 0) { return; }
        {
            std::scoped_lock lk(g_excl_lock);
            size_t i = 0;
            while (i < g_excl_n && g_excl[i] != program_id) { ++i; }
            if (excluded && i == g_excl_n && g_excl_n < MaxExcluded) { g_excl[g_excl_n++] = program_id; }
            if (!excluded && i < g_excl_n) { g_excl[i] = g_excl[--g_excl_n]; }
            SaveExcludedLocked();
        }
        if (g_app_tid.load(std::memory_order_relaxed) == program_id) { g_app_excluded.store(excluded, std::memory_order_relaxed); }
        LogLine("sftap: %016llx %s", static_cast<unsigned long long>(program_id), excluded ? "EXCLUDED - never attached" : "streamed again");
    }

    bool IsExcluded(u64 program_id) {
        std::scoped_lock lk(g_excl_lock);
        for (size_t k = 0; k < g_excl_n; ++k) { if (g_excl[k] == program_id) { return true; } }
        return false;
    }

    void LoadStreamEnabled() {
        fs::DirectoryEntryType t;
        const Result r = fs::GetEntryType(std::addressof(t), OffFile);
        const bool off = R_SUCCEEDED(r);
        g_stream_enabled.store(!off, std::memory_order_relaxed);
        LogLine("sftap: stream %s at boot (stream-off: rc=0x%x)%s", off ? "OFF" : "on", r.GetValue(),
                off ? " - turn it on from the overlay or the manager" : "");
    }

    void StartControlService() {
        const Result r = g_control_manager.RegisterObjectForServer(g_control_object.GetShared(), ControlServiceName, ControlMaxSessions);
        LogLine("sftap: control service rc=0x%x", r.GetValue());
        if (R_FAILED(r)) { return; }
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_control_thread), ControlThread, nullptr,
                                        g_control_stack, sizeof(g_control_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_control_thread), "applet-mitm.Control");
        os::StartThread(std::addressof(g_control_thread));
        NoteThread(std::addressof(g_control_thread));
    }

    namespace {
        constexpr const char NeedHeadroomFile[] = "sdmc:/config/switch-frame-tap/need_headroom";
        constexpr const char HeadroomReadyFile[] = "sdmc:/config/SwitchU/ft_headroom_ready";
        constinit std::atomic<bool> g_need_headroom{false};
        constinit std::atomic<bool> g_headroom_acked{false};
        constinit std::atomic<u32> g_need_headroom_mb{0};
        constinit std::atomic<u32> g_headroom_free_kb{0};

        void WriteNeedHeadroomFile(u32 need_mb) {
            char buf[16];
            const int n = std::snprintf(buf, sizeof(buf), "%u\n", need_mb);
            if (n <= 0) { return; }
            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap"));
            static_cast<void>(fs::DeleteFile(NeedHeadroomFile));
            if (R_FAILED(fs::CreateFile(NeedHeadroomFile, static_cast<s64>(n)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), NeedHeadroomFile, fs::OpenMode_Write))) { return; }
            static_cast<void>(fs::WriteFile(f, 0, buf, static_cast<size_t>(n), fs::WriteOption::Flush));
            fs::CloseFile(f);
        }

        void ClearNeedHeadroomFile() {
            static_cast<void>(fs::DeleteFile(NeedHeadroomFile));
            static_cast<void>(fs::DeleteFile(HeadroomReadyFile));
        }

        bool HeadroomReadyFilePresent() {
            fs::DirectoryEntryType t;
            return R_SUCCEEDED(fs::GetEntryType(std::addressof(t), HeadroomReadyFile));
        }
    }

    bool HeadroomNeeded() {
        return g_need_headroom.load(std::memory_order_relaxed);
    }

    u32 HeadroomNeedMb() {
        return g_need_headroom_mb.load(std::memory_order_relaxed);
    }

    void HeadroomClear() {
        g_need_headroom.store(false, std::memory_order_relaxed);
        g_headroom_acked.store(false, std::memory_order_relaxed);
        g_need_headroom_mb.store(0, std::memory_order_relaxed);
        ClearNeedHeadroomFile();
    }

    void HeadroomAck(u32 free_kb) {
        g_headroom_free_kb.store(free_kb, std::memory_order_relaxed);
        g_headroom_acked.store(true, std::memory_order_release);
        LogLine("sftap: SwitchU AckHeadroom free=%u KB", free_kb);
    }

    void HeadroomAskAndWait(u32 need_mb, u32 timeout_ms) {
        if (need_mb == 0) { need_mb = 30; }
        g_headroom_acked.store(false, std::memory_order_relaxed);
        g_need_headroom_mb.store(need_mb, std::memory_order_relaxed);
        g_need_headroom.store(true, std::memory_order_release);
        WriteNeedHeadroomFile(need_mb);
        LogLine("sftap: NeedHeadroom %u MB - waiting up to %u ms for SwitchU", need_mb, timeout_ms);

        const u32 step = 50;
        u32 waited = 0;
        while (waited < timeout_ms) {
            if (g_headroom_acked.load(std::memory_order_acquire) || HeadroomReadyFilePresent()) {
                LogLine("sftap: NeedHeadroom acknowledged after %u ms (free=%u KB)",
                        waited, g_headroom_free_kb.load(std::memory_order_relaxed));
                break;
            }
            os::SleepThread(TimeSpan::FromMilliSeconds(step));
            waited += step;
        }
        if (waited >= timeout_ms) {
            LogLine("sftap: NeedHeadroom timed out after %u ms - launching donor anyway", timeout_ms);
        }
        /* Keep the bit set until remount finishes so late SwitchU polls still reclaim. */
    }

}
