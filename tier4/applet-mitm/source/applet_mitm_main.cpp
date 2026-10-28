/*
 * applet-mitm - Track B / M1
 *
 * Standalone libstratosphere sysmodule that mitms "appletOE" and logs when a
 * game opens its application proxy. Forwards everything untouched.
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000C20/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000C20/flags/boot2.flag   (empty)
 * Log: sdmc:/applet-mitm.log
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "applet_mitm_service.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_am.hpp"
#include "applet_mitm_audout.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_nvdrv.hpp"
#include "applet_mitm_nvjpg.hpp"
#include "applet_mitm_armfile.hpp"
#include "applet_mitm_gop.hpp"
#include "applet_mitm_clk.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_uvc.hpp"
#include "applet_mitm_net.hpp"
#include "applet_mitm_uibinder.hpp"
#include "applet_mitm_uisource.hpp"
#include "applet_mitm_soundprobe.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"
#include "applet_mitm_featmem.hpp"

/* Force libnx's nv layer to use "nvdrv:s" instead of picking a service via
 * appletGetAppletType() - that call is meaningless here and is what made a
 * plain sysmodule fatal on nvInitialize() during Phase 0. Also shrink the
 * nvdrv transfer memory: we only issue tiny nvmap ioctls, and libnx's 3 MB
 * default would not fit our allocator. */
extern "C" {
    NvServiceType __nx_nv_service_type    = NvServiceType_System;
    u32           __nx_nv_transfermem_size = 0x40000;
}

namespace ams {

    namespace {

        /* v0.7.13: 96 KB, was 1 MB - every custom sysmodule shares a few MB
         * of the System group with am (#15), and the most ever in use was
         * 17 KB (Run v0713-ballast4-late: a game, the HOME menu, applets),
         * plus SyncSoundPatches' 44 KB for a moment at boot */
        constexpr size_t MallocBufferSize = 96_KB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        enum PortIndex {
            PortIndex_AppletMitm,
            PortIndex_ViManagerMitm,   /* v0.1.1: homebrew applications present through vi:m */
            PortIndex_Count,
        };

        constexpr sm::ServiceName AppletMitmServiceName = sm::ServiceName::Encode("vi:u");
        constexpr sm::ServiceName ViManagerMitmServiceName = sm::ServiceName::Encode("vi:m");

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        /* v0.7.1: 16 (8 was tight with a game, a homebrew app and their sub-objects) */
        constexpr size_t MaxSessions = 16;

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_server_manager;

        /* v0.7.1: the vi server on several threads. With one, a forwarded call
         * that blocks inside vi - a homebrew app's dequeueBuffer while the
         * on-screen keyboard takes over the screen - held the only thread, and
         * the keyboard applet's own vi:m connection waited on our ShouldMitm
         * answer forever: the keyboard never opened (kingboxnt on GBAtemp;
         * sphaira here, the binder traffic stopped dead at the key press). The
         * same "one blocked call ties up one thread of several" design the grc
         * server below uses. */
        constexpr size_t ViThreads = 4;     /* this one and three more */
        alignas(os::ThreadStackAlignment) constinit u8 g_vi_stacks[ViThreads - 1][16_KB];   /* v0.7.13: 32, 5 used */
        constinit os::ThreadType g_vi_threads[ViThreads - 1];
        void ViLoopThread(void *) { mitm::applet::LoopProcessSafely(g_server_manager, "vi"); }

        /* ---- v0.7.14 probe2: dispdrv, for the HOME menu's and applets' frames
         * Research only (probe-ui.txt). Its own server: an applet's
         * dequeueBuffer blocks inside nvnflinger until a buffer is free, so
         * it must not hold a vi thread, and domains are allowed in case an
         * applet converts its session (vi:u's server has none). */
        enum UiPortIndex {
            UiPortIndex_Dispdrv,
            UiPortIndex_ViSystem,    /* probe4: vi:s, library applets only */
            UiPortIndex_Count,
        };
        constexpr sm::ServiceName DispdrvServiceName = sm::ServiceName::Encode("dispdrv");
        constexpr sm::ServiceName ViSystemServiceName = sm::ServiceName::Encode("vi:s");
        struct UiServerOptions {
            static constexpr size_t PointerBufferSize   = 0x3000;   /* probe2: dispdrv's is 0x3000 */
            static constexpr size_t MaxDomains          = 4;
            static constexpr size_t MaxDomainObjects    = 16;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };
        /* probe4: an applet's vi:s root, display service, system display
         * service, relay. Without the probe only the Album's dispdrv session
         * is served; each session here costs its 12 KB pointer buffer in
         * memory, with home_menu off too (v0.7.14: 12 -> 4, one fewer thread) */
#if SFT_UI_PROBE
        constexpr size_t UiMaxSessions = 12;
        constexpr size_t UiThreads = 3;
#else
        constexpr size_t UiMaxSessions = 4;
        constexpr size_t UiThreads = 2;
#endif
        class UiServerManager final : public sf::hipc::ServerManager<UiPortIndex_Count, UiServerOptions, UiMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };
        /* v0.7.14: made only with home_menu on (or the probe), in memory
         * taken then (52 KB, most of it the sessions' pointer buffers -
         * static, it cost everyone; applet_mitm_featmem.hpp) */
        constinit UiServerManager *g_ui_server_manager = nullptr;
        UiServerManager *UiServer() {
            static_assert(alignof(UiServerManager) <= os::MemoryPageSize);
            if (g_ui_server_manager == nullptr) {
                if (void *mem = mitm::applet::FeatureMemory(sizeof(UiServerManager), "home menu: the dispdrv server"); mem != nullptr) {
                    g_ui_server_manager = std::construct_at(static_cast<UiServerManager *>(mem));
                }
            }
            return g_ui_server_manager;
        }
        alignas(os::ThreadStackAlignment) constinit u8 g_ui_stacks[UiThreads][12_KB];
        constinit os::ThreadType g_ui_threads[UiThreads];
        void UiLoopThread(void *) { mitm::applet::LoopProcessSafely(*g_ui_server_manager, "dispdrv"); }

        /* our own dispdrv session first (taken before the mitm exists, so it
         * is the real service), and its pointer buffer: a mitm session's
         * must be at least as large or libstratosphere aborts on accept */
        /* GCC 15 guesses the vi server's AllocateServer here (speculative
         * devirtualization) and, the UI server being the smaller of the two,
         * warns about a path that never runs */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        void StartUiBinderMitm() {
            /* v0.7.14: dispdrv also for the Album with home_menu on */
            if (!mitm::applet::UiProbeOn() && !mitm::applet::g_ui_stream_armed) { return; }
            ::Service dd = {};
            ::Result rc = 0;
            {
                mitm::applet::SmUse sm_dd;
                rc = ::smGetService(std::addressof(dd), "dispdrv");
            }
            if (R_FAILED(rc)) { mitm::applet::LogLine("home menu: dispdrv not opened rc=0x%x - the Album is not streamed", rc); return; }
            mitm::applet::LogLine("home menu: dispdrv's pointer buffer %#x (ours %#zx)", static_cast<unsigned>(dd.pointer_buffer_size), UiServerOptions::PointerBufferSize);
            mitm::applet::UiProbeSetDispdrv(dd);
            bool any = false;
            const bool probe_dd = mitm::applet::UiProbeWants("dispdrv");
            if (!probe_dd && !mitm::applet::g_ui_stream_armed) {
                mitm::applet::LogLine("uiprobe: dispdrv mitm off (nodispdrv)");
            } else if (dd.pointer_buffer_size > UiServerOptions::PointerBufferSize) {
                mitm::applet::LogLine("home menu: dispdrv's pointer buffer is larger than ours - the Album is not streamed");
            } else if (UiServer() == nullptr) {
                mitm::applet::LogLine("home menu: the Album is not streamed (no memory for its server)");
            } else {
                mitm::applet::g_ui_dispdrv_probe.store(probe_dd);
                SFT_ABORT_UNLESS(UiServer()->RegisterMitmServer<mitm::applet::UiBinderMitm>(UiPortIndex_Dispdrv, DispdrvServiceName));
                mitm::applet::LogLine("dispdrv wrapped for %s", probe_dd ? "the probe (HOME-menu processes) and the Album" : "the Album (home_menu)");
                any = true;
            }
            /* probe4: the library applets' vi (vi:s here, vi:m in the vi
             * server) - their shared-layer presents name the slot each frame */
            if (mitm::applet::UiProbeWants("vi")) {
                ::Service vs = {};
                ::Result vrc = 0;
                {
                    mitm::applet::SmUse sm_vs;
                    vrc = ::smGetService(std::addressof(vs), "vi:s");
                }
                if (R_FAILED(vrc)) {
                    mitm::applet::LogLine("uiprobe: vi:s not opened rc=0x%x - applets not wrapped", vrc);
                } else {
                    const u32 pbs = static_cast<u32>(vs.pointer_buffer_size);
                    ::serviceClose(std::addressof(vs));
                    mitm::applet::LogLine("uiprobe: vi:s pointer buffer %#x (ours %#zx, vi server %#zx)", pbs, UiServerOptions::PointerBufferSize, ServerOptions::PointerBufferSize);
                    if (pbs <= UiServerOptions::PointerBufferSize && pbs <= ServerOptions::PointerBufferSize && UiServer() != nullptr) {
                        mitm::applet::g_ui_vi_mitm.store(true);
                        SFT_ABORT_UNLESS(UiServer()->RegisterMitmServer<mitm::applet::ViSystemRootMitm>(UiPortIndex_ViSystem, ViSystemServiceName));
                        mitm::applet::LogLine("uiprobe: library applets' vi:s and vi:m wrapped (shared-layer presents logged)");
                        any = true;
                    } else {
                        mitm::applet::LogLine("uiprobe: vi:s pointer buffer too large - applets not wrapped");
                    }
                }
            }
            if (!any) { return; }
            const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
            for (size_t i = 0; i < UiThreads; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_ui_threads[i]), UiLoopThread, nullptr, g_ui_stacks[i], sizeof(g_ui_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_ui_threads[i]), "applet-mitm.Ui");
                os::StartThread(std::addressof(g_ui_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_ui_threads[i]));
            }
            mitm::applet::LogLine("home menu: dispdrv server up, %zu threads", UiThreads);
        }
#pragma GCC diagnostic pop

        /* ---- M72: the grc recorder's own server --------------------------
         * Separate from vi:u on purpose. Anything forwarded for grc can block
         * inside nvservices - a syncpoint wait, an event wait - and on a shared
         * LoopProcess thread that would stall the game's binder traffic, i.e.
         * freeze the picture. Here a blocked grc call ties up one of four
         * threads that serve grc and nobody else. */
        enum NvPortIndex {
            NvPortIndex_Nvdrv,
            NvPortIndex_Count,
        };

        constexpr sm::ServiceName NvdrvMitmServiceName = sm::ServiceName::Encode("nvdrv:s");
        constexpr size_t NvMaxSessions = 16;
        constexpr size_t NvThreads     = 4;

        /* v0.7.10: the grc IPC interceptor (M72-M76 research, never armed in
         * a release) cost 1.4 MB of .bss - a 1 MB server manager, 128 KB of
         * stacks, NvDrvMitm's 256 KB record - in the Applet pool the browser
         * applets need nearly all of (2210-1024). Built only with
         * SFT_GRC_RESEARCH=1. */
#if SFT_GRC_RESEARCH
        /* M72b: a mitm session's pointer buffer must be at least as large as the
         * real service's (sf_hipc_server_session_manager.cpp:148 aborts
         * otherwise). M72 reused vi:u's 0x1000, and the first grc session
         * fataled the module the instant Mario Kart launched. nvservices'
         * size is reported through QueryPointerBufferSize as a u16, so
         * 0x10000 covers any value it can have. 16 sessions x 64 KB = 1 MB of
         * .bss, from the Applet pool that had 509 MB free at boot. */
        struct NvServerOptions {
            static constexpr size_t PointerBufferSize   = 0x10000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        class NvServerManager final : public sf::hipc::ServerManager<NvPortIndex_Count, NvServerOptions, NvMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        NvServerManager g_nv_server_manager;

        alignas(os::ThreadStackAlignment) constinit u8 g_nv_stacks[NvThreads][32_KB];
        constinit os::ThreadType g_nv_threads[NvThreads];

        void NvLoopThread(void *) { g_nv_server_manager.LoopProcess(); }

#endif

        /* Registered before anything else in main: mitm.lst has boot2 declare
         * this mitm in advance, so every nvdrv:s client in the system is waiting
         * on us until this returns. */
#if !SFT_GRC_RESEARCH
        /* Release builds: nothing to register. A mitm.lst left on the card
         * (M72-M75 test builds) still has boot2 declare a future nvdrv:s mitm,
         * and every nvdrv:s client - vi among them - would wait on us forever:
         * withdraw the declaration and delete the file. */
        void StartGrcRecorder() {
            constexpr const char *Lst = "sdmc:/atmosphere/contents/0100000000000C20/mitm.lst";
            fs::DirectoryEntryType t;
            if (mitm::applet::g_grc_armed) { mitm::applet::LogLine("grc IPC interceptor: armed, but not in this build (SFT_GRC_RESEARCH)"); }
            if (R_FAILED(fs::GetEntryType(std::addressof(t), Lst))) { mitm::applet::LogLine("grc IPC interceptor: off"); return; }
            const Result crc = sm::mitm::ClearFutureMitm(NvdrvMitmServiceName);
            const Result drc = fs::DeleteFile(Lst);
            mitm::applet::LogLine("grc IPC interceptor: off - stale mitm.lst found: future nvdrv:s mitm cleared rc=0x%x, file deleted rc=0x%x", crc.GetValue(), drc.GetValue());
        }
#else
        void StartGrcRecorder() {
            /* M73: opt-in. M72 registered this unconditionally, which meant
             * every run carried the recorder's risk even when the run had
             * nothing to do with grc. Without "grc" in the arm file nothing
             * here is registered at all, so nvdrv:s is untouched - unless
             * mitm.lst is on the card, in which case see M76 below. */
            bool have_lst = false;
            {
                fs::FileHandle lf;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(lf), "sdmc:/atmosphere/contents/0100000000000C20/mitm.lst", fs::OpenMode_Read))) {
                    fs::CloseFile(lf);
                    have_lst = true;
                }
            }
            if (!mitm::applet::g_grc_armed) {
                if (!have_lst) {
                    mitm::applet::LogLine("grc IPC interceptor: off");
                    return;
                }
                /* M76: mitm.lst left on the card from M72-M75 and "grc" not
                 * armed. boot2 has already declared a future nvdrv:s mitm, so
                 * if nothing registers, every nvdrv:s client - vi among them -
                 * waits forever and the console never reaches the home menu.
                 * Register anyway: ShouldMitm is false for everyone while grc
                 * is off, so every client gets the real service and none of
                 * the recorder's handlers can run. */
                mitm::applet::LogLine("grc IPC interceptor: off, but mitm.lst is PRESENT - registering as a pass-through "
                                      "(accepts nobody) so nvdrv:s clients are not blocked. Delete mitm.lst.");
            } else if (!have_lst) {
                /* M75: refuse without mitm.lst. boot2 only declares a future mitm
                 * for services listed there; registering one late means clients
                 * that already have sessions bypass us, and the ones that do not
                 * can block. Checking is cheap and the failure mode is a console
                 * that will not launch a game. */
                mitm::applet::LogLine("grc IPC interceptor: ARMED but mitm.lst is MISSING - refusing to register");
                return;
            } else {
                mitm::applet::LogLine("grc IPC interceptor: ARMED (this handler has hung the console before)");
            }
            SFT_ABORT_UNLESS(g_nv_server_manager.RegisterMitmServer<mitm::applet::NvDrvMitm>(NvPortIndex_Nvdrv, NvdrvMitmServiceName));
            const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
            for (size_t i = 0; i < NvThreads; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_nv_threads[i]), NvLoopThread, nullptr,
                                                g_nv_stacks[i], sizeof(g_nv_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_nv_threads[i]), "applet-mitm.NvDrv");
                os::StartThread(std::addressof(g_nv_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_nv_threads[i]));
            }
        }

#endif

        /* ---- liveness heartbeat -------------------------------------------
         * Two runs in a row went silent after "registered mitm server", with
         * the system wedging on vi. That is ambiguous: the process may have
         * died, or it may be alive but never reaching our handlers (sm blocks
         * every vi:u open on the mitm query port, so a dead LoopProcess wedges
         * the system exactly the same way).
         *
         * This thread is independent of the dispatch path. LogMark rewrites
         * sdmc:/applet-mitm.last whole, so even after a forced power-off the
         * last committed beat tells us how long the process lived. */
        alignas(os::ThreadStackAlignment) constinit u8 g_hb_stack[8_KB];   /* v0.7.13: 16, 4 used */
        constinit os::ThreadType g_hb_thread;
        constinit std::atomic<bool> g_screen_off{false};   /* v0.7.12: config screen_off = 1 */

        /* v0.7.10 diagnostic: the Applet resource-limit group (ours, shared with
         * every library applet) - current/limit of memory, threads, events,
         * transfer memories and sessions, every 250 ms, logged when one moves.
         * SooraMaru: the web applet crashes with the module installed even
         * with 4 MB held; the line before the crash names the limit. */
        void LogAppletLimits(const char *tag = nullptr) {
            if (mitm::applet::g_pmdmnt_rc != 0) { return; }
            static s64 last[5] = { -1, -1, -1, -1, -1 };
            static const svc::LimitableResource res[5] = {
                svc::LimitableResource_PhysicalMemoryMax, svc::LimitableResource_ThreadCountMax, svc::LimitableResource_EventCountMax,
                svc::LimitableResource_TransferMemoryCountMax, svc::LimitableResource_SessionCountMax,
            };
            u64 cur[5] = {}, lim[5] = {};
            bool moved = false;
            for (u32 k = 0; k < 5; ++k) {
                if (R_FAILED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur[k]), std::addressof(lim[k]), pm::ResourceLimitGroup_Applet, res[k]))) { return; }
                const s64 v = k == 0 ? static_cast<s64>(cur[k] >> 20) : static_cast<s64>(cur[k]);   /* memory in MB */
                if (v != last[k]) { moved = true; last[k] = v; }
            }
            /* v0.7.13: and the System group's memory - every custom sysmodule,
             * am among the system's, shares it; when it runs out am fatals with
             * 2001-0132 at boot (#15, GBAtemp). In KB: it is a few MB. */
            u64 scur = 0, slim = 0;
            static s64 last_sys = -1;
            if (R_SUCCEEDED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(scur), std::addressof(slim), pm::ResourceLimitGroup_System, svc::LimitableResource_PhysicalMemoryMax))) {
                const s64 v = static_cast<s64>(scur >> 18);   /* 256 KB steps */
                if (v != last_sys) { moved = true; last_sys = v; }
            }
            /* v0.7.13 (#15): and its sessions, threads and events - a count
             * that climbs with each app would end in a hang at the same app
             * every time, with no memory moving */
            u64 sc[3] = {}, sl[3] = {};
            static s64 last_cnt[3] = { -1, -1, -1 };
            static const svc::LimitableResource cres[3] = { svc::LimitableResource_SessionCountMax, svc::LimitableResource_ThreadCountMax, svc::LimitableResource_EventCountMax };
            for (u32 k = 0; k < 3; ++k) {
                if (R_SUCCEEDED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(sc[k]), std::addressof(sl[k]), pm::ResourceLimitGroup_System, cres[k]))) {
                    if (static_cast<s64>(sc[k]) != last_cnt[k]) { moved = true; last_cnt[k] = static_cast<s64>(sc[k]); }
                }
            }
            /* v0.7.13: the most of the 1 MB malloc buffer ever in use, in 16 KB
             * steps - how small it can be made */
            {
                static size_t low = ~size_t(0);
                const size_t fr = init::GetAllocator()->GetTotalFreeSize();
                if (fr + 16_KB <= low || (tag != nullptr && fr < low)) {
                    low = fr;
                    mitm::applet::LogLine("malloc: at most %zu KB of %zu KB in use so far", (MallocBufferSize - low) / 1024, MallocBufferSize / 1024);
                }
            }
            if (!moved && tag == nullptr) { return; }
            if (tag != nullptr) { mitm::applet::LogLine("applet group @ %s:", tag); }
            mitm::applet::LogLine("system group: memory %llu/%llu KB used (%lld KB free), sessions %llu/%llu, threads %llu/%llu, events %llu/%llu; sftap:s sessions made %u",
                                  static_cast<unsigned long long>(scur >> 10), static_cast<unsigned long long>(slim >> 10),
                                  static_cast<long long>((static_cast<s64>(slim) - static_cast<s64>(scur)) / 1024),
                                  static_cast<unsigned long long>(sc[0]), static_cast<unsigned long long>(sl[0]),
                                  static_cast<unsigned long long>(sc[1]), static_cast<unsigned long long>(sl[1]),
                                  static_cast<unsigned long long>(sc[2]), static_cast<unsigned long long>(sl[2]),
                                  mitm::applet::SessionsMadeViaSm());
            mitm::applet::LogLine("applet group: memory %llu/%llu MB, threads %llu/%llu, events %llu/%llu, transfer memories %llu/%llu, sessions %llu/%llu",
                                  static_cast<unsigned long long>(cur[0] >> 20), static_cast<unsigned long long>(lim[0] >> 20),
                                  static_cast<unsigned long long>(cur[1]), static_cast<unsigned long long>(lim[1]),
                                  static_cast<unsigned long long>(cur[2]), static_cast<unsigned long long>(lim[2]),
                                  static_cast<unsigned long long>(cur[3]), static_cast<unsigned long long>(lim[3]),
                                  static_cast<unsigned long long>(cur[4]), static_cast<unsigned long long>(lim[4]));
        }

        /* v0.7.12: "Screen off while streaming" - the console's backlight off
         * (lbl, as the system's own screen-off does) while a stream runs, on
         * again the moment it stops: the viewer closed, HOME pressed, the
         * stream switched off. Only this thread touches it. */
        void ScreenFollowStream() {
            static bool dark = false, tried = false, lbl_ok = false;
            /* and the game is drawing: HOME with detach_on_home = 0 keeps the
             * session, but the screen should come back for the HOME menu.
             * v0.7.17: 2.5 s without a frame, not 1 - since v0.7.16 a session
             * outlives a game's short pauses, and Final Fantasy Resonance's
             * battle transitions (just over 1 s) flashed the screen on and off
             * (#20) */
            const u64 qt = mitm::applet::g_queue_tick.load(std::memory_order_relaxed);
            const bool drawing = qt != 0 && armTicksToNs(armGetSystemTick() - qt) < UINT64_C(2500000000);
            const bool want = g_screen_off.load(std::memory_order_relaxed) && drawing &&
                              mitm::applet::g_streaming_now.load(std::memory_order_relaxed);
            if (want == dark) { return; }
            if (!tried) {
                tried = true;
                mitm::applet::SmUse sm;
                const Result r = lblInitialize();
                lbl_ok = R_SUCCEEDED(r);
                mitm::applet::LogLine("screen: lbl %s (rc=0x%x)", lbl_ok ? "ready" : "unavailable", r.GetValue());
            }
            if (!lbl_ok) { return; }
            const Result r = want ? lblSwitchBacklightOff(0) : lblSwitchBacklightOn(0);
            dark = want;
            mitm::applet::LogLine("screen: %s (rc=0x%x)", want ? "off while streaming" : "on", r.GetValue());
        }

        /* v0.7.13 test: config/switch-frame-tap/ballast.txt = "N [S]" holds N MB
         * (even) more of the System group, from boot or from S seconds after
         * boot, to reproduce a console with less memory to spare (#15) and to
         * find when am needs it. Never in a normal install. */
        constinit size_t g_ballast_mb = 0;
        constinit u32 g_ballast_delay_s = 0;
        void BallastRead() {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/config/switch-frame-tap/ballast.txt", fs::OpenMode_Read))) { return; }
            char b[16] = {};
            size_t n = 0;
            static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
            fs::CloseFile(f);
            char *e = nullptr;
            g_ballast_mb = static_cast<size_t>(std::strtoul(b, std::addressof(e), 10)) & ~size_t(1);
            g_ballast_delay_s = static_cast<u32>(std::strtoul(e, nullptr, 10));
            mitm::applet::LogLine("TEST ballast: %zu MB, taken %u s after boot", g_ballast_mb, g_ballast_delay_s);
        }
        void BallastTake() {
            if (g_ballast_mb == 0) { return; }
            uintptr_t addr = 0;
            const Result r1 = os::SetMemoryHeapSize(g_ballast_mb << 20);
            const Result r2 = R_SUCCEEDED(r1) ? os::AllocateMemoryBlock(std::addressof(addr), g_ballast_mb << 20) : r1;
            if (addr != 0) { std::memset(reinterpret_cast<void *>(addr), 0, g_ballast_mb << 20); }
            mitm::applet::LogLine("TEST ballast: %zu MB held now (heap rc=0x%x, block rc=0x%x)", g_ballast_mb, r1.GetValue(), r2.GetValue());
            g_ballast_mb = 0;
            LogAppletLimits("ballast");
        }

        /* v0.7.13: the launcher (0100000000000C21, launcher.c) holds the
         * boot2.flag and starts the module once the system has booted - at
         * boot, the module's memory came out of the System group the system
         * still needed ~7.2 MB of, and am fataled (2001-0132, #15). An older
         * install's own boot2.flag is moved to the launcher; started by it in
         * the first minute, the module exits at once (its memory goes back
         * before the system needs it) and the launcher starts it later. */
        bool StartedInTime() {
            constexpr const char *OwnFlag = "sdmc:/atmosphere/contents/0100000000000C20/flags/boot2.flag";
            constexpr const char *LauncherFlag = "sdmc:/atmosphere/contents/0100000000000C21/flags/boot2.flag";
            constexpr const char *Go = "sdmc:/config/switch-frame-tap/launch.txt";
            fs::DirectoryEntryType t;
            if (R_SUCCEEDED(fs::GetEntryType(std::addressof(t), OwnFlag))) {
                static_cast<void>(fs::CreateDirectory("sdmc:/atmosphere/contents/0100000000000C21/flags"));
                static_cast<void>(fs::CreateFile(LauncherFlag, 0));
                static_cast<void>(fs::DeleteFile(OwnFlag));
                mitm::applet::LogLine("boot2.flag moved to the launcher (0100000000000C21): it starts Switch Frame Tap after the system has booted");
            }
            /* sysmodule managers list the launcher's toolbox.json now */
            static_cast<void>(fs::DeleteFile("sdmc:/atmosphere/contents/0100000000000C20/toolbox.json"));
            u64 at = 0;
            {
                fs::FileHandle f;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), Go, fs::OpenMode_Read))) {
                    char b[24] = {};
                    size_t n = 0;
                    static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
                    fs::CloseFile(f);
                    static_cast<void>(fs::DeleteFile(Go));
                    at = std::strtoull(b, nullptr, 10);
                }
            }
            const u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
            const bool by_launcher = at != 0 && now >= at && now - at < 15000;
            if (!by_launcher && now < 60000) {
                mitm::applet::LogLine("started at %llu.%03llu s, before the system has booted (an older install's boot2.flag) - exiting; the launcher starts it once the HOME menu is up",
                                      static_cast<unsigned long long>(now / 1000), static_cast<unsigned long long>(now % 1000));
                return false;
            }
            /* what the launcher saw - in this log, for reports */
            fs::FileHandle f;
            if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), "sdmc:/config/switch-frame-tap/launcher.txt", fs::OpenMode_Read))) {
                char b[768] = {};
                size_t n = 0;
                static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
                fs::CloseFile(f);
                for (char *line = b; line != nullptr && *line != 0; ) {
                    char *nl = std::strchr(line, '\n');
                    if (nl != nullptr) { *nl = 0; }
                    if (*line != 0) { mitm::applet::LogLine("launcher: %s", line); }
                    line = nl != nullptr ? nl + 1 : nullptr;
                }
            }
            if (!by_launcher) { mitm::applet::LogLine("started by hand, %llu s after boot", static_cast<unsigned long long>(now / 1000)); }
            return true;
        }

        void HeartbeatThread(void *) {
            for (u32 i = 1; ; i++) {
                for (u32 t = 0; t < 12; ++t) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    if (g_ballast_mb != 0 && armTicksToNs(armGetSystemTick()) >= static_cast<u64>(g_ballast_delay_s) * UINT64_C(1000000000)) { BallastTake(); }
                    /* v0.7.13: the stacks' high-water marks, 2 and 10 minutes in, then hourly */
                    if (t == 0 && (i == 40 || i == 200 || (i > 200 && i % 1200 == 0))) { mitm::applet::LogStackUse(); }
                    LogAppletLimits();
                    mitm::applet::UiProbeStep();   /* v0.7.14 research: off without probe-ui.txt */
                    ScreenFollowStream();
                }
                /* v0.7.15 (#33): no breadcrumb while a stream runs - its SD
                 * writes every 3 s showed as a latency spike in the viewer
                 * (vbezniskotempo measured it gone without them) */
                if (mitm::applet::LogQuiet()) { continue; }
                mitm::applet::LogFlushHeld();
                char b[96];
                const auto &st = mitm::applet::g_stats;
                std::snprintf(b, sizeof(b), "hb:%u sess=%u getdisp=%u relay=%u txn=%u vic=%s",
                              i, st.sessions.load(), st.getdisp.load(),
                              st.relay.load(), st.txns.load(),
                              mitm::applet::g_vic_stage.load(std::memory_order_relaxed));
                mitm::applet::LogMark(b);
                mitm::applet::FlushNvdrvRecords();
            }
        }

        /* ---- opt-in arm file ----------------------------------------------
         * Nothing touches nvdrv/VIC unless sdmc:/applet-mitm.armed exists and
         * contains "vic". Default is a pure observer, i.e. M7d behaviour. */
        /* Called once per flag at boot; the file is a single short line. */
        /* v0.7.4: "Sound in no-record games" (common/sound_patch.hpp) is a copy
         * of the bundled dvr-patches in atmosphere/exefs_patches. While it is
         * on, refresh that copy from the bundle - an update of Switch Frame Tap
         * brings patches for a new firmware, and am reads them next boot. */
        void SyncSoundPatches() {
            constexpr const char *Bundled = "sdmc:/config/switch-frame-tap/dvr-patches";
            constexpr const char *Ours    = "sdmc:/atmosphere/exefs_patches/switch-frame-tap-sound";
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), Ours)) || t != fs::DirectoryEntryType_Directory) { return; }
            fs::DirectoryHandle d;
            if (R_FAILED(fs::OpenDirectory(std::addressof(d), Bundled, fs::OpenDirectoryMode_File))) { return; }
            /* v0.7.13: from malloc for this moment at boot, not 44 KB of .bss
             * (the System group, #15) */
            struct Scratch { fs::DirectoryEntry entries[48]; u8 a[4096], b[4096]; };
            std::unique_ptr<Scratch> scratch(new (std::nothrow) Scratch);
            if (!scratch) { fs::CloseDirectory(d); return; }
            fs::DirectoryEntry *entries = scratch->entries;
            s64 n = 0;
            const Result rr = fs::ReadDirectory(std::addressof(n), entries, d, 48);
            fs::CloseDirectory(d);
            if (R_FAILED(rr)) { return; }
            u32 updated = 0, same = 0;
            u8 *a = scratch->a, *b = scratch->b;
            auto read_all = [](const char *path, u8 *buf, size_t cap, size_t *len) -> bool {
                fs::FileHandle f;
                if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read))) { return false; }
                s64 sz = 0;
                bool ok = R_SUCCEEDED(fs::GetFileSize(std::addressof(sz), f)) && sz > 0 && static_cast<size_t>(sz) <= cap &&
                          R_SUCCEEDED(fs::ReadFile(f, 0, buf, static_cast<size_t>(sz)));
                fs::CloseFile(f);
                *len = ok ? static_cast<size_t>(sz) : 0;
                return ok;
            };
            for (s64 i = 0; i < n; ++i) {
                const char *name = entries[i].name;
                const size_t nl = std::strlen(name);
                if (nl < 5 || (std::strcmp(name + nl - 4, ".ips") != 0 && std::strcmp(name + nl - 4, ".IPS") != 0)) { continue; }
                char from[256], to[256];
                std::snprintf(from, sizeof(from), "%s/%s", Bundled, name);
                std::snprintf(to, sizeof(to), "%s/%s", Ours, name);
                size_t la = 0, lb = 0;
                if (!read_all(from, a, sizeof(scratch->a), std::addressof(la))) { continue; }
                if (read_all(to, b, sizeof(scratch->b), std::addressof(lb)) && la == lb && std::memcmp(a, b, la) == 0) { ++same; continue; }
                fs::DeleteFile(to);
                fs::FileHandle f;
                if (R_SUCCEEDED(fs::CreateFile(to, static_cast<s64>(la))) && R_SUCCEEDED(fs::OpenFile(std::addressof(f), to, fs::OpenMode_Write))) {
                    if (R_SUCCEEDED(fs::WriteFile(f, 0, a, la, fs::WriteOption::Flush))) { ++updated; }
                    fs::CloseFile(f);
                }
            }
            mitm::applet::LogLine("sound in no-record games: on (dvr-patches, %u current, %u refreshed from the bundle)", same, updated);
        }

        size_t ReadFileText(const char *path, char *buf, size_t cap) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read))) {
                return 0;
            }
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return 0; }
            size_t n = static_cast<size_t>(fsz);
            if (n > cap - 1) { n = cap - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return 0; }
            buf[n] = '\0';
            return n;
        }

        /* M75: WHOLE-TOKEN match, not substring.
         *
         * This was `strstr`, and it cost a hardware cycle and a forced
         * power-off. The arm file "vic grcscan wait=90" made
         * ArmFileContains("grc") true, because "grcscan" contains "grc" -
         * so the grc IPC interceptor registered when I believed it was off,
         * re-running a handler already known to be broken, and the game
         * could not launch. The run I thought was a read-only observer was
         * nothing of the kind.
         *
         * M76: ArmFileNumber still used `strstr` and carried the same bug:
         * "sweep stream sw=768" read `sw` out of "sweep" and fell back to 0.
         * Both now share applet_mitm_armfile.hpp, which has a host test. */
        /* M98: read once. A test install has sdmc:/applet-mitm.armed; a
         * release install has sdmc:/config/switch-frame-tap/config.ini (or
         * nothing: the defaults), turned into the same tokens. */
        constinit char g_arm_buf[512] = {};
        constinit size_t g_arm_len = 0;
        constinit bool g_arm_loaded = false;

        void LoadArm() {
            if (g_arm_loaded) { return; }
            g_arm_loaded = true;
            if (!mitm::applet::g_release_mode) {
                g_arm_len = ReadFileText("sdmc:/applet-mitm.armed", g_arm_buf, sizeof(g_arm_buf));
                return;
            }
            static char ini[4096];
            const size_t n = ReadFileText("sdmc:/config/switch-frame-tap/config.ini", ini, sizeof(ini));
            g_arm_len = mitm::applet::armfile::BuildReleaseArm(ini, n, g_arm_buf, sizeof(g_arm_buf));
        }

        bool ArmFileContains(const char *keyword) {
            LoadArm();
            return mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, keyword);
        }

        /* the per-session settings, from the (re)loaded tokens */
        void ApplySessionSettings() {
            mitm::applet::g_nvstream_qp   = mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "nvqp", 20);
            mitm::applet::g_nvstream_gop  = mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "nvgop", 0);
            mitm::applet::g_cap720_armed  = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "cap720");
            mitm::applet::g_anyfw_armed   = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "anyfw");
            mitm::applet::g_shot_enabled.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shot"));
            mitm::applet::g_shot_combo.store(std::min(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "shotkey", 0), 4u));
            mitm::applet::g_shot_mask.store(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "shotmask", 0) & sft::shotbuttons::AllButtons);
            mitm::applet::g_shot_capture.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shotcap"));
            mitm::applet::g_shot_album.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shotalbum"));
            mitm::applet::g_audio_enabled.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "audio"));
            mitm::applet::g_nvab_armed = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "nvab");
            /* on unless config sets detach_on_home=0 (nodethome) */
            mitm::applet::g_detach_on_home = !mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "nodethome");
            /* (home_menu is read at boot only: the Album's dispdrv mitm and the
             * HOME menu's start are set up then) */
            g_screen_off.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "screenoff"), std::memory_order_relaxed);
            /* v0.7.18: homebrew told the console is docked - the next one started */
            mitm::applet::g_hb_docked.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "hbdocked"), std::memory_order_relaxed);
            mitm::applet::g_early_collect = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "earlyoff") ? 0
                                          : mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "earlyab") ? 2 : 1;
            mitm::applet::g_usb_gop.store(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "usbgop", mitm::applet::keyframe::UsbGopDefault),
                                          std::memory_order_relaxed);
        }

        /* Numeric option out of the arm file: "wait=180" delays the probe so
         * there is time to actually get into a race before it fires. M39 probed
         * at ~50 s, which is still the title screen. */
        u32 ArmFileNumber(const char *key, u32 def) {
            LoadArm();
            return mitm::applet::armfile::Number(g_arm_buf, g_arm_len, key, def);
        }

        /* Ask the kernel how big each physical memory pool is, rather than
         * inferring our budget from a failed allocation.
         *
         * M47 measured this process at total=4720 KB, used=3744 KB - the whole
         * allocation is smaller than ONE 1080p frame (7913 KB). masagrator's
         * objection on GBAtemp was right, and by a wider margin than he argued.
         * The open question is whether pool_partition is the lever: we are on 2
         * (System), and if Applet or Application has meaningful headroom then a
         * one-line NPDM change might buy the room NVENC needs.
         *
         * Deliberately logged at BOOT. Every memory question then costs a ~20 s
         * boot instead of a three-minute race to reach the probe. */
        void LogMemoryPools() {
            static const char *const names[4] = { "Application", "Applet", "System", "SystemUnsafe" };
            mitm::applet::LogLine("---- physical memory pools (svcGetSystemInfo) ----");
            for (u64 pool = 0; pool < 4; ++pool) {
                u64 tot = 0, used = 0;
                const auto r1 = svc::GetSystemInfo(std::addressof(tot),  svc::SystemInfoType_TotalPhysicalMemorySize, svc::InvalidHandle, pool);
                const auto r2 = svc::GetSystemInfo(std::addressof(used), svc::SystemInfoType_UsedPhysicalMemorySize,  svc::InvalidHandle, pool);
                mitm::applet::LogLine("   pool %llu %-12s total=%8llu KB  used=%8llu KB  free=%9lld KB   rc=%x/%x",
                                      static_cast<unsigned long long>(pool), names[pool],
                                      static_cast<unsigned long long>(tot / 1024),
                                      static_cast<unsigned long long>(used / 1024),
                                      static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024),
                                      r1.GetValue(), r2.GetValue());
            }
            u64 ptot = 0, pused = 0;
            svc::GetInfo(std::addressof(ptot),  svc::InfoType_TotalMemorySize, svc::PseudoHandle::CurrentProcess, 0);
            svc::GetInfo(std::addressof(pused), svc::InfoType_UsedMemorySize,  svc::PseudoHandle::CurrentProcess, 0);
            mitm::applet::LogLine("   THIS PROCESS (pool_partition 1 = Applet, at boot): total=%llu KB used=%llu KB free=%lld KB",
                                  static_cast<unsigned long long>(ptot / 1024),
                                  static_cast<unsigned long long>(pused / 1024),
                                  static_cast<long long>((static_cast<s64>(ptot) - static_cast<s64>(pused)) / 1024));
            mitm::applet::LogLine("   for scale: one 1080p frame = 7913 KB, one block-row strip = 960 KB");
        }

        /* M50 FATALED THE CONSOLE: 2001-0132 (0x10801, kernel LimitReached) in
         * program 0100000000000023 - am, the applet manager. Not us: a DIFFERENT
         * sysmodule, killed because we took the memory it needed.
         *
         *     [boot] System pool now: used=231424 KB free=6304 KB (we took 8192 KB)
         *
         * 8 MB out of a pool with 14,496 KB free left 6,304 KB, and am could not
         * start. The allocation itself always succeeded - holding it is what
         * broke the console.
         *
         * So M49's reading was wrong, and so was mine. 8 MB was grantable at boot
         * precisely BECAUSE am had not allocated yet; that was a transient, not
         * headroom. The 2 MB seen at probe time is the System pool's honest
         * steady state once every sysmodule has claimed its share. masagrator's
         * original objection stands: on 22.5.0 a sysmodule cannot hold a 1080p
         * buffer.
         *
         * This is the second time this pool has been over-drawn - M27 did it with
         * 4 MB of .bss and fataled a sysmodule the same way. No heap is taken at
         * boot. The probe allocates 2 MB, which has been safe across ~20 runs,
         * and capture stays strip-wise - which costs nothing structurally, since
         * the 983,040 B block-row is the natural unit of the block-linear layout
         * anyway. */

        /* ---- USB device enumeration -------------------------------------
         * The transport half, and the reason it is worth a run before NVENC:
         * NVENC has a class ID now (0x21, cross-checked against VIC's 0x5D which
         * we proved on hardware in M16) but no method table anywhere local, so a
         * SETCL for it would be writing blind at an engine. USB is safe - no
         * engine, no memory grab - and it is on the critical path.
         *
         * The arithmetic that makes this the interesting direction: the VIC
         * already downscales correctly, and 480x270 raw at 30 fps is 15.5 MB/s,
         * inside USB 2.0's ~30 MB/s with no encoder at all. A working prototype
         * without NVENC.
         *
         * Sequence and descriptor values follow SysDVR's UsbComms.c, which is a
         * known-good implementation on this firmware. Endpoint buffers are
         * static 0x1000 pairs - 8 KB of .bss, not heap, so this costs nothing
         * against the 976 KB. */
        alignas(0x1000) constinit u8 g_usb_ep_in_buf[0x1000]  = {};
        alignas(0x1000) constinit u8 g_usb_ep_out_buf[0x1000] = {};
        constinit bool g_usb_armed = false;

        /* M57: the endpoint handles have to outlive TryUsbEnumerate. Until now
         * they were locals inside it and were discarded the moment it returned,
         * which is why the transport could enumerate but never transmit a byte. */
        constinit UsbDsInterface *g_usb_iface  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_in  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_out = nullptr;

        /* One bulk post per iteration. 256 KB is a multiple of 0x1000, so every
         * chunk boundary stays aligned for the next PostBufferAsync. */
        constexpr size_t UsbChunk     = 0x40000;
        constexpr u64    UsbTimeoutNs = UINT64_C(5000000000);

        void TryUsbEnumerate() {
            if (!g_usb_armed) {
                mitm::applet::LogLine("   usb: not armed (add \"usb\" to the arm file)");
                return;
            }
            /* v0.4: webcam mode describes a UVC camera instead */
            if (mitm::applet::g_uvc_mode) {
                static_cast<void>(mitm::applet::UvcSetupDevice());
                return;
            }
            mitm::applet::LogLine("---- USB device enumeration (usb:ds) ----");

            /* ::Result - libnx's u32 - NOT ams::Result, which is a class and
             * would not survive being passed through printf varargs. */
            ::Result rc = usbDsInitialize();
            mitm::applet::LogLine("   usbDsInitialize rc=0x%x %s", rc,
                                  R_SUCCEEDED(rc) ? "" : "<- could not acquire usb:ds");
            if (R_FAILED(rc)) { return; }

            u8 iMan = 0, iProd = 0, iSer = 0;
            static const u16 langs[1] = { 0x0409 };
            rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iMan),  "switch-frame-tap"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iProd), "Switch Frame Tap"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iSer),  "0001"); }
            mitm::applet::LogLine("   string descriptors rc=0x%x", rc);

            struct usb_device_descriptor dd = {
                .bLength            = USB_DT_DEVICE_SIZE,
                .bDescriptorType    = USB_DT_DEVICE,
                .bcdUSB             = 0x0110,
                .bDeviceClass       = 0x00,
                .bDeviceSubClass    = 0x00,
                .bDeviceProtocol    = 0x00,
                .bMaxPacketSize0    = 0x40,
                .idVendor           = 0x1209,   /* pid.codes open range */
                .idProduct          = 0x5F1E,
                .bcdDevice          = 0x0100,
                .iManufacturer      = iMan,
                .iProduct           = iProd,
                .iSerialNumber      = iSer,
                .bNumConfigurations = 0x01,
            };
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, std::addressof(dd));
            dd.bcdUSB = 0x0200;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, std::addressof(dd)); }
            /* M58: SuperSpeed. M52-M57 declared only Full and High, so the host
             * negotiated 480 Mbps because that is all we ever offered - the
             * ceiling was ours, not the platform's. Atmosphere's own haze
             * declares Super (usb_session.cpp:144), which proves the console
             * supports USB 3.0 device mode.
             *
             * USB 3.0 requires bcdUSB 0x0300 and bMaxPacketSize0 encoded as a
             * POWER OF TWO: 9 means 2^9 = 512, not 512 itself. */
            dd.bcdUSB          = 0x0300;
            dd.bMaxPacketSize0 = 9;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, std::addressof(dd)); }
            mitm::applet::LogLine("   device descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            /* M71: the Binary Object Store - the piece M58 was missing.
             *
             * M58 declared SuperSpeed device and endpoint descriptors and the
             * link still trained to High every time. I read that as a cable or
             * a console limit. It was neither: USB 3.0 enumeration requires a
             * BOS containing a SuperSpeed USB Device Capability descriptor, and
             * we never called usbDsSetBinaryObjectStore at all. Atmosphere's
             * haze does (usb_session.cpp:220) immediately after its Super
             * device descriptor, and haze negotiates SuperSpeed.
             *
             * wSpeedSupported 0x000c = bit2 (High) | bit3 (Super).
             * bFunctionalitySupport 3 = full functionality from High upward. */
            {
                const u8 bos[0x16] = {
                    0x05, USB_DT_BOS, 0x16, 0x00, 0x02,

                    /* USB 2.0 extension */
                    0x07, USB_DT_DEVICE_CAPABILITY, 0x02,
                    0x02, 0x00, 0x00, 0x00,

                    /* SuperSpeed USB device capability */
                    0x0a, USB_DT_DEVICE_CAPABILITY, 0x03,
                    0x00,
                    0x0c, 0x00,
                    0x03,
                    0x00,
                    0x00, 0x00,
                };
                rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
                mitm::applet::LogLine("   usbDsSetBinaryObjectStore rc=0x%x%s", rc,
                                      R_SUCCEEDED(rc) ? "" : "  <- SuperSpeed will not be offered");
                if (R_FAILED(rc)) { return; }
            }

            /* bound to the file-scope handles so the transport can post to
             * ep_in long after boot; the rest of this function is unchanged */
            UsbDsInterface *&iface  = g_usb_iface;
            UsbDsEndpoint  *&ep_in  = g_usb_ep_in;
            UsbDsEndpoint  *&ep_out = g_usb_ep_out;
            iface = nullptr; ep_in = nullptr; ep_out = nullptr;

            struct usb_interface_descriptor id = {
                .bLength            = USB_DT_INTERFACE_SIZE,
                .bDescriptorType    = USB_DT_INTERFACE,
                .bInterfaceNumber   = 4,
                .bAlternateSetting  = 0,
                .bNumEndpoints      = 2,
                .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
                .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
                .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
                .iInterface         = 0,
            };
            struct usb_endpoint_descriptor ep_i = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_IN,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x200,
                .bInterval        = 0,
            };
            struct usb_endpoint_descriptor ep_o = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_OUT,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x40,
                .bInterval        = 0,
            };

            std::memset(g_usb_ep_in_buf,  0, sizeof(g_usb_ep_in_buf));
            std::memset(g_usb_ep_out_buf, 0, sizeof(g_usb_ep_out_buf));

            rc = usbDsRegisterInterface(std::addressof(iface));
            mitm::applet::LogLine("   usbDsRegisterInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            id.bInterfaceNumber   = iface->interface_index;
            ep_i.bEndpointAddress = static_cast<u8>(ep_i.bEndpointAddress + id.bInterfaceNumber + 1);
            ep_o.bEndpointAddress = static_cast<u8>(ep_o.bEndpointAddress + id.bInterfaceNumber + 1);

            /* Full speed */
            rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(id),   USB_DT_INTERFACE_SIZE);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            /* High speed - 512 B bulk, the speed we actually expect */
            ep_i.wMaxPacketSize = 0x200;
            ep_o.wMaxPacketSize = 0x200;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(id),   USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            /* M58: SuperSpeed config - 1024 B bulk, and every SS endpoint needs a
             * companion descriptor immediately after it or the host rejects the
             * configuration. bMaxBurst 0x0f = 16 packets per burst, copied from
             * haze (usb_session.cpp:108). Order matters: iface, ep, companion,
             * ep, companion. */
            struct usb_ss_endpoint_companion_descriptor ss_comp = {
                .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
                .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
                .bMaxBurst         = 0x0f,
                .bmAttributes      = 0x00,
                .wBytesPerInterval = 0x00,
            };
            ep_i.wMaxPacketSize = 0x400;
            ep_o.wMaxPacketSize = 0x400;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(id),      USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_i),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_o),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            mitm::applet::LogLine("   configuration descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_in), ep_i.bEndpointAddress);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_out), ep_o.bEndpointAddress); }
            mitm::applet::LogLine("   endpoints IN=0x%02x OUT=0x%02x rc=0x%x", ep_i.bEndpointAddress, ep_o.bEndpointAddress, rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_EnableInterface(iface);
            mitm::applet::LogLine("   EnableInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsEnable();
            mitm::applet::LogLine("   usbDsEnable rc=0x%x", rc);
            if (R_FAILED(rc)) {
                mitm::applet::LogLine("   usbDsEnable failed - device will not appear on the bus");
                return;
            }

            /* M53: usbDsEnable returning 0x0 only means OUR side configured itself.
             * It says nothing about whether the HOST enumerated us - M52 logged
             * "USB DEVICE ENUMERATED" off this rc alone and that claim was wrong.
             * usbDsGetState reaching UsbState_Configured is the real signal: the
             * host has read our descriptors and selected a configuration.
             * Atmosphere's own haze uses exactly this test (usb_session.cpp:242).
             * Poll 10 s so a cable seated slightly late still counts. */
            static const char *const state_names[] = {
                "Detached", "Attached", "Powered", "Default", "Address", "Configured", "Suspended",
            };
            UsbState st = UsbState_Detached, best = UsbState_Detached;
            for (int i = 0; i < 100; ++i) {
                if (R_FAILED(usbDsGetState(std::addressof(st)))) { break; }
                if (static_cast<int>(st) > static_cast<int>(best)) { best = st; }
                if (st == UsbState_Configured) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
            const unsigned b = static_cast<unsigned>(best);
            mitm::applet::LogLine("   usb state high-water = %u (%s)", b,
                                  b < (sizeof(state_names) / sizeof(state_names[0])) ? state_names[b] : "?");
            /* M58: the number this whole milestone exists to produce. */
            {
                UsbDeviceSpeed neg = UsbDeviceSpeed_None;
                const ::Result srrc = usbDsGetSpeed(std::addressof(neg));
                static const char *const speed_names[] = {
                    "None", "Low(1.5Mbps)", "Full(12Mbps)", "High(480Mbps)", "SUPER(5Gbps)",
                };
                const unsigned sn = static_cast<unsigned>(neg);
                mitm::applet::LogLine("   *** NEGOTIATED SPEED = %u (%s) *** rc=0x%x", sn,
                                      sn < (sizeof(speed_names) / sizeof(speed_names[0])) ? speed_names[sn] : "?",
                                      srrc);
                mitm::applet::LogLine("   %s", neg == UsbDeviceSpeed_Super
                    ? "SuperSpeed: NV12 1080p60 (178 MiB/s) is within reach"
                    : "High Speed (USB 2.0) - all the stream needs");
            }
            mitm::applet::LogLine("   %s", best == UsbState_Configured
                ? "*** HOST CONFIGURED US - we own the bus, expect 1209:5f1e in lsusb ***"
                : "host did NOT configure us - no cable, or the PC is not the host: a USB-C to USB-C cable can make the Switch the host; use USB-C to USB-A (#22)");
        }

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            mitm::applet::g_stats.sessions.fetch_add(1);

            mitm::applet::LogLine("OnNeedsToAccept port=%d program=%016llx", port_index,
                                  static_cast<unsigned long long>(client_info.program_id.value));

            switch (port_index) {
                case PortIndex_AppletMitm:
                    {
                        const Result r = this->AcceptMitmImpl(server,
                            sf::CreateSharedObjectEmplaced<mitm::applet::IViRootMitm, mitm::applet::ViRootMitm>(decltype(fsrv)(fsrv), client_info),
                            fsrv);
                        mitm::applet::LogLine("  AcceptMitmImpl rc=0x%x", r.GetValue());
                        R_RETURN(r);
                    }
                case PortIndex_ViManagerMitm:
                    {
                        const Result r = this->AcceptMitmImpl(server,
                            sf::CreateSharedObjectEmplaced<mitm::applet::IViManagerRootMitm, mitm::applet::ViManagerRootMitm>(decltype(fsrv)(fsrv), client_info),
                            fsrv);
                        mitm::applet::LogLine("  AcceptMitmImpl (vi:m) rc=0x%x", r.GetValue());
                        R_RETURN(r);
                    }
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        Result UiServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            mitm::applet::LogLine("%s accept program=%016llx forward pointer_buffer_size=%#x", port_index == UiPortIndex_Dispdrv ? "dispdrv" : "vi:s",
                                  static_cast<unsigned long long>(client_info.program_id.value), static_cast<unsigned>(fsrv->pointer_buffer_size));
            switch (port_index) {
                case UiPortIndex_Dispdrv:
                    R_RETURN(this->AcceptMitmImpl(server,
                        sf::CreateSharedObjectEmplaced<mitm::applet::IUiBinderMitm, mitm::applet::UiBinderMitm>(decltype(fsrv)(fsrv), client_info),
                        fsrv));
                case UiPortIndex_ViSystem:
                    R_RETURN(this->AcceptMitmImpl(server,
                        sf::CreateSharedObjectEmplaced<mitm::applet::IViSystemRootMitm, mitm::applet::ViSystemRootMitm>(decltype(fsrv)(fsrv), client_info),
                        fsrv));
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

#if SFT_GRC_RESEARCH
        Result NvServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            AMS_ABORT_UNLESS(port_index == NvPortIndex_Nvdrv);
            /* logged BEFORE accepting, so a failure inside the accept still
             * leaves the number that caused it on the SD card */
            mitm::applet::LogLine("nvdrv:s accept program=%016llx forward pointer_buffer_size=%#x (ours %#zx)",
                                  static_cast<unsigned long long>(client_info.program_id.value),
                                  static_cast<unsigned>(fsrv->pointer_buffer_size), NvServerOptions::PointerBufferSize);
            R_RETURN(this->AcceptMitmImpl(server,
                sf::CreateSharedObjectEmplaced<mitm::applet::INvDrvMitm, mitm::applet::NvDrvMitm>(decltype(fsrv)(fsrv), client_info),
                fsrv));
        }
#endif

    }

    namespace init {

        void InitializeSystemModule() {
            R_ABORT_UNLESS(sm::Initialize());
            fs::InitializeForSystem();
            fs::SetEnabledAutoAbort(false);

            /* pm:dmnt does NOT self-initialise: libstratosphere's
             * pm::dmnt::GetApplicationProcessId calls straight through to libnx's
             * pmdmntGetApplicationProcessId, and dmnt / dmnt.gen2 / ams_mitm all
             * call pmdmntInitialize() here. Deliberately NOT R_ABORT_UNLESS as
             * they do - a boot-time fatal is the one failure mode this module
             * must never have. Record it and let the debug route skip instead. */
            mitm::applet::g_pmdmnt_rc = ::pmdmntInitialize();

            /* v0.7.7: no ams::CheckApiVersion() - it aborts when the running
             * Atmosphere is older than the headers this was built with (1.11.2),
             * before the log is open: masagrator on GBAtemp got a bare fatal on
             * 1.11.1. Nothing here needs 1.11.2; Main logs both versions. */
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void Main() {
        os::SetThreadNamePointer(os::GetCurrentThread(), "applet-mitm.Main");

        mitm::applet::LogInit();
        if (!StartedInTime()) { return; }
        LogAppletLimits("main start");

        /* M72: first thing, before any other work. With mitm.lst present every
         * nvdrv:s client - vi among them - is blocked until this registers. The
         * arm file is read here only for the one flag ShouldMitm needs; without
         * "grc" we still register (mitm.lst obliges us to) but accept nobody. */
        mitm::applet::g_grc_armed = ArmFileContains("grc");
        StartGrcRecorder();

        /* M76: this line used to print jpg=off before jpg was parsed, and
         * called every build a "read-only observer". The flag dump below is
         * the record of what this boot armed. */
        mitm::applet::LogLine("applet-mitm v0.7.19: up (grc IPC interceptor %s), built against Atmosphere %d.%d.%d",
                              mitm::applet::g_grc_armed ? "ARMED" : "off",
                              ATMOSPHERE_RELEASE_VERSION_MAJOR, ATMOSPHERE_RELEASE_VERSION_MINOR, ATMOSPHERE_RELEASE_VERSION_MICRO);
        {
            const auto api = exosphere::GetApiInfo();
            const u32 hv = hosversionGet();
            mitm::applet::LogLine("Atmosphere %u.%u.%u, firmware %u.%u.%u (built with Atmosphere %u.%u.%u, tested on firmware 22.5.0; 23.0.0 by a user)",
                                  api.GetMajorVersion(), api.GetMinorVersion(), api.GetMicroVersion(),
                                  HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
                                  ATMOSPHERE_RELEASE_VERSION_MAJOR, ATMOSPHERE_RELEASE_VERSION_MINOR, ATMOSPHERE_RELEASE_VERSION_MICRO);
        }
        SyncSoundPatches();

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::g_dbg_armed   = ArmFileContains("dbg");
        mitm::applet::g_dump_armed  = ArmFileContains("dump");
        g_usb_armed                 = ArmFileContains("usb");
        mitm::applet::g_uvc_mode    = ArmFileContains("uvc");
        mitm::applet::g_uvc_raw     = mitm::applet::g_uvc_mode && ArmFileContains("uvcraw");   /* v0.7.7: webcam-any */
        mitm::applet::g_net_armed   = ArmFileContains("net");
        mitm::applet::g_bench_armed   = ArmFileContains("bench");
        mitm::applet::g_nvenc_armed   = ArmFileContains("nvenc");
        mitm::applet::g_nvjpg_armed   = ArmFileContains("jpg");
        mitm::applet::g_nvjpg_quality = ArmFileNumber("q", 85);
        mitm::applet::g_nvjpg_attempts = ArmFileNumber("jat", 90);
        mitm::applet::g_sweep_armed   = ArmFileContains("sweep");
        mitm::applet::g_matrix_armed  = ArmFileContains("mtx");
        mitm::applet::g_grcscan_armed = ArmFileContains("grcscan");
        mitm::applet::g_clk_armed     = ArmFileContains("clk");
        mitm::applet::g_jpgdec_armed  = ArmFileContains("jpgdec");
        mitm::applet::g_nvgrc_armed   = ArmFileContains("nvgrc");
        mitm::applet::g_csc_armed     = ArmFileContains("csc");
        mitm::applet::g_nvframe_armed = ArmFileContains("nvframe");
        mitm::applet::g_nvframe_n     = ArmFileNumber("nvframe", 120);
        mitm::applet::g_nvstream_armed = ArmFileContains("nvstream");
        mitm::applet::g_nvstream_n    = ArmFileNumber("nvstream", 3600);
        mitm::applet::g_nvstream_qp   = ArmFileNumber("nvqp", 20);
        mitm::applet::g_nvstream_gop  = ArmFileNumber("nvgop", 0);
        mitm::applet::g_nvab_armed    = ArmFileContains("nvab");
        mitm::applet::g_live_armed    = ArmFileContains("live");
        mitm::applet::g_detach_on_home = !ArmFileContains("nodethome");
        mitm::applet::g_ui_stream_armed = ArmFileContains("homemenu");   /* v0.7.14: the HOME menu streams too */
        g_screen_off.store(ArmFileContains("screenoff"), std::memory_order_relaxed);
        mitm::applet::g_early_collect = ArmFileContains("earlyoff") ? 0 : ArmFileContains("earlyab") ? 2 : 1;
        mitm::applet::g_usb_gop.store(ArmFileNumber("usbgop", mitm::applet::keyframe::UsbGopDefault), std::memory_order_relaxed);
        mitm::applet::g_nv1080_armed  = ArmFileContains("nv1080");
        mitm::applet::g_nv1080_n      = ArmFileNumber("nv1080", 120);
        mitm::applet::g_cap720_armed  = ArmFileContains("cap720");
        mitm::applet::g_anyfw_armed   = ArmFileContains("anyfw");
        mitm::applet::g_shot_enabled.store(ArmFileContains("shot"));
        mitm::applet::g_shot_combo.store(std::min(ArmFileNumber("shotkey", 0), 4u));
        mitm::applet::g_shot_mask.store(ArmFileNumber("shotmask", 0) & sft::shotbuttons::AllButtons);
        mitm::applet::g_shot_capture.store(ArmFileContains("shotcap"));
        mitm::applet::g_shot_album.store(ArmFileContains("shotalbum"));
        mitm::applet::NoteAlbumPath();   /* v0.7.11: for the manager's gallery */
        mitm::applet::g_audio_enabled.store(ArmFileContains("audio"));
        mitm::applet::LogLine("install type from GetEntryType(sdmc:/applet-mitm.armed) rc=0x%x", mitm::applet::g_arm_probe_rc);
        mitm::applet::LogLine("%s: %.*s", mitm::applet::g_release_mode ? "release install, sdmc:/config/switch-frame-tap/config.ini ->" : "TEST install, sdmc:/applet-mitm.armed",
                              static_cast<int>(g_arm_len), g_arm_buf);
        /* live is a mode of the H.264 stream: everything nvstream sets up, it needs */
        if (mitm::applet::g_live_armed) { mitm::applet::g_nvstream_armed = true; }
        /* M97: the overlay's on/off switch, remembered on the SD */
        mitm::applet::LoadStreamEnabled();
        mitm::applet::LoadExcluded();
        if (!mitm::applet::g_live_armed) { mitm::applet::g_live_state.store(mitm::applet::LiveState_NotArmed); }
        mitm::applet::g_nvp_armed     = ArmFileContains("nvp");
        mitm::applet::g_nvp_n         = ArmFileNumber("nvp", 30);
        mitm::applet::g_matrix_mode   = ArmFileNumber("mtx", 1);
        mitm::applet::g_stream_armed  = ArmFileContains("stream");
        mitm::applet::g_stream_w      = ArmFileNumber("sw", 0);
        mitm::applet::g_stream_h      = ArmFileNumber("sh", 0);
        /* an explicit sw=/sh= overrides the link-speed picker; without one the
         * stream sizes itself to whatever the negotiated link can carry */
        mitm::applet::g_stream_auto   = (mitm::applet::g_stream_w == 0 ||
                                         mitm::applet::g_stream_h == 0);
        mitm::applet::g_stream_frames = ArmFileNumber("sframes", 600);
        mitm::applet::g_probe_delay_s = ArmFileNumber("wait", 120);
        mitm::applet::LogLine("ARMED FLAGS: vic=%d exec=%d dbg=%d dump=%d usb=%d bench=%d nvenc=%d "
                              "jpg=%d sweep=%d mtx=%d stream=%d grc=%d grcscan=%d clk=%d jpgdec=%d nvgrc=%d csc=%d nvframe=%d(%u) nvstream=%d(%u, qp %u, gop %u) nvp=%d(%u) live=%d nv1080=%d(%u) wait=%u",
                              mitm::applet::g_vic_armed, mitm::applet::g_vic_execute,
                              mitm::applet::g_dbg_armed, mitm::applet::g_dump_armed,
                              g_usb_armed, mitm::applet::g_bench_armed,
                              mitm::applet::g_nvenc_armed, mitm::applet::g_nvjpg_armed,
                              mitm::applet::g_sweep_armed, mitm::applet::g_matrix_armed,
                              mitm::applet::g_stream_armed, mitm::applet::g_grc_armed,
                              mitm::applet::g_grcscan_armed, mitm::applet::g_clk_armed,
                              mitm::applet::g_jpgdec_armed, mitm::applet::g_nvgrc_armed,
                              mitm::applet::g_csc_armed, mitm::applet::g_nvframe_armed, mitm::applet::g_nvframe_n,
                              mitm::applet::g_nvstream_armed, mitm::applet::g_nvstream_n, mitm::applet::g_nvstream_qp,
                              mitm::applet::g_nvstream_gop,
                              mitm::applet::g_nvp_armed, mitm::applet::g_nvp_n,
                              mitm::applet::g_live_armed,
                              mitm::applet::g_nv1080_armed, mitm::applet::g_nv1080_n,
                              mitm::applet::g_probe_delay_s);
        mitm::applet::LogLine("detach-on-home=%s (exp: end session when presents stall ~1s; config detach_on_home=0 to keep)",
                              mitm::applet::g_detach_on_home ? "on" : "off");
        if ((mitm::applet::g_jpgdec_armed || mitm::applet::g_nvgrc_armed ||
             mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_armed) {
            mitm::applet::LogLine("jpgdec/nvgrc/csc/nvframe/nvstream armed without vic: they run inside the VIC worker, so they will NOT run");
        }
        if ((mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed || mitm::applet::g_nvp_armed || mitm::applet::g_nv1080_armed) && !mitm::applet::g_dbg_armed) {
            mitm::applet::LogLine("nvframe/nvstream/nvp armed without dbg: they run inside the debug capture, so they will NOT run");
        }
        if ((mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_execute) {
            mitm::applet::LogLine("csc/nvframe/nvstream/nvp armed without exec: the VIC buffers are never mapped, so they will NOT run");
        }
        if (mitm::applet::g_nvstream_armed && !g_usb_armed) {
            mitm::applet::LogLine("nvstream armed without usb: the USB device is never brought up, so it will NOT stream");
        }
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
        if (mitm::applet::g_stream_armed) {
            if (mitm::applet::g_stream_auto) {
                mitm::applet::LogLine("stream ARMED: size AUTO (from the negotiated link speed), %u frames",
                                      mitm::applet::g_stream_frames);
            } else {
                mitm::applet::LogLine("stream ARMED: %ux%u packed-420, %u frames (%u B/frame); 60 fps needs <=16667 us/frame",
                                      mitm::applet::g_stream_w, mitm::applet::g_stream_h,
                                      mitm::applet::g_stream_frames,
                                      mitm::applet::g_stream_w * mitm::applet::g_stream_h * 3 / 2);
            }
        }
        LogMemoryPools();
        BallastRead();
        if (g_ballast_delay_s == 0) { BallastTake(); }
        /* v0.7.11: with streaming switched off, the USB port is left to other
         * apps (masagrator) - taken when it is switched on (UsbFollowStreamSwitch) */
        if (mitm::applet::g_stream_enabled.load(std::memory_order_relaxed)) {
            TryUsbEnumerate();
        } else {
            mitm::applet::NoteUsbLeftAlone();
        }

        mitm::applet::LogLine("probe fires at t=%u s; frame dump to SD: %s",
                              mitm::applet::g_probe_delay_s,
                              mitm::applet::g_dump_armed ? "ARMED (freezes the game ~0.3-2.5 s)" : "off");
        mitm::applet::LogLine("debug-capture route: %s   pmdmntInitialize rc=0x%x",
                              mitm::applet::g_dbg_armed ? "ARMED" : "off (add \"dbg\" to the arm file)",
                              mitm::applet::g_pmdmnt_rc);

        /* start the heartbeat before anything that can block */
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_hb_thread), HeartbeatThread, nullptr,
                                        g_hb_stack, sizeof(g_hb_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_hb_thread), "applet-mitm.HB");
        os::StartThread(std::addressof(g_hb_thread));
        mitm::applet::NoteThread(std::addressof(g_hb_thread));
        mitm::applet::LogMark("main:heartbeat_started");

        mitm::applet::StartControlService();
        mitm::applet::StartAppWatch();
        mitm::applet::StartShotInput();   /* idle, and hid untouched, until screenshots are on */
        mitm::applet::StartAudio();
        mitm::applet::SoundProbeStart();   /* v0.7.14 research: off without probe-sound.txt */
        mitm::applet::OvlProbeStart();     /* v0.7.14 research: off without probe-ovl.txt */
        mitm::applet::StartNet();         /* v0.7: only with "net"; waits for the console to be online */       /* idle, and grc:d untouched, until audio is on */
        mitm::applet::StartVicWorker();
        mitm::applet::StartAppletWatch();   /* v0.7.10: an applet starting while we stream */
        /* M76: its own thread, so an mm:u or clkrst call that blocks can never
         * hold up vi:u registration below. Holds the clocks past the survey
         * only when an engine probe in this run needs them. */
        mitm::applet::StartClockProbe(mitm::applet::g_jpgdec_armed || mitm::applet::g_nvenc_armed ||
                                      mitm::applet::g_nvjpg_armed || mitm::applet::g_nvgrc_armed ||
                                      mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
                                      mitm::applet::g_nvp_armed);

        /* v0.7.1: the sessions behind wrapped objects come from sm, so they do
         * not use up the applets' session limit (the keyboard applet) */
        ::ams::sf::impl::g_tier4_create_session = mitm::applet::CreateSessionViaSm;
        /* v0.7.1: the first pair through sm, made here and thrown away. In
         * every boot of the first sm build, the first app to open the display
         * died right after its session was made (Mario Kart, Minecraft and
         * sphaira alike: nn::vi got nothing back, then aborted) and every
         * later one was fine; that first pair is also the one that registers
         * sftap:s. Now no game ever gets it. */
        {
            os::NativeHandle ws = os::InvalidNativeHandle, wc = os::InvalidNativeHandle;
            const Result wrc = mitm::applet::CreateSessionViaSm(std::addressof(ws), std::addressof(wc));
            mitm::applet::LogLine("sessions: warm-up pair rc=0x%x (server 0x%x, client 0x%x) - closed", wrc.GetValue(), ws, wc);
            if (R_SUCCEEDED(wrc)) { os::CloseNativeHandle(wc); os::CloseNativeHandle(ws); }
        }
        SFT_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");
        /* v0.7.10: the game's am, so a browser applet stops the stream first;
         * "noam" in config/arm leaves am alone (escape hatch) */
        /* v0.7.18: before am's mitm: it decides which apps to wrap */
        mitm::applet::g_hb_docked.store(ArmFileContains("hbdocked"), std::memory_order_relaxed);
        if (mitm::applet::g_hb_docked.load()) { mitm::applet::LogLine("am: homebrew from the Homebrew Menu is told the console is docked (homebrew_docked)"); }
        if (ArmFileContains("noam")) { mitm::applet::LogLine("am: not wrapped (noam)"); } else { mitm::applet::StartAmMitm(); }
        /* v0.7.17: a game's audout:u - its own PCM for the stream when nothing else carries its sound (PokeMMO) */
        if (ArmFileContains("noaudout")) { mitm::applet::LogLine("audout: not wrapped (noaudout)"); } else { mitm::applet::StartAudOutMitm(); }
        /* v0.1.1: after vi:u, and wrapping only the running application's own
         * sessions (ShouldMitm asks pm:dmnt) */
        /* v0.7.1: "nohb" (config homebrew = 0, not in the manager) leaves vi:m
         * alone - homebrew apps are then never streamed. Added while chasing
         * the keyboard bug; kept as an escape hatch should wrapping homebrew
         * ever break one. */
        mitm::applet::OverlayArm(ArmFileContains("overlays"));    /* v0.7.14: the overlays (config overlays = 1, or overlays.txt; starts their thread), and Tesla's vi:m with them or the probe */
        if (ArmFileContains("nohb")) {
            mitm::applet::LogLine("vi:m NOT wrapped (homebrew = 0): homebrew apps are not streamed this boot");
        } else {
            SFT_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViManagerRootMitm>(PortIndex_ViManagerMitm, ViManagerMitmServiceName));
            mitm::applet::LogLine("registered mitm server for vi:m (homebrew applications only)");
        }

        {
            const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
            for (size_t i = 0; i < ViThreads - 1; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_vi_threads[i]), ViLoopThread, nullptr,
                                                g_vi_stacks[i], sizeof(g_vi_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_vi_threads[i]), "applet-mitm.Vi");
                os::StartThread(std::addressof(g_vi_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_vi_threads[i]));
            }
            mitm::applet::LogLine("vi server: %zu threads, %zu sessions", ViThreads, MaxSessions);
        }
        /* v0.7.14: with home_menu on (or the research probe) */
        StartUiBinderMitm();
        /* v0.7.10: drop the boot reference to sm (sm::Initialize). Our own sm
         * connection is charged to the Applet group and cost the applets one
         * of their 6 sessions - the web applet's (2210-1024). Every later use
         * holds it only while needed (SmUse, applet_mitm_sm.hpp). */
        {
            const Result src = mitm::applet::SmReleaseBoot();
            mitm::applet::LogLine("sm: boot session detached (rc=0x%x) and released - sm is now opened only while a service is fetched", src.GetValue());
        }
        LogAppletLimits("after sm released");
        mitm::applet::LogMark("main:LoopProcess");
        mitm::applet::LoopProcessSafely(g_server_manager, "vi");
        mitm::applet::LogMark("main:LoopProcess_RETURNED");
    }

}

/* ---- M57: USB bulk transport ------------------------------------------------
 * Defined out here, not in the anonymous namespace above, because
 * applet_mitm_nv.cpp calls these and internal linkage would not reach it.
 * The anonymous namespace's members are still visible from this block: an
 * unnamed namespace inside `namespace ams` injects its names into `ams`, so
 * ::ams::g_usb_ep_in resolves within this translation unit.
 *
 * Every usbDs and event call returns libnx's ::Result - a bare u32, NOT
 * ams::Result. Calling .GetValue() on one does not compile, which has already
 * cost this project a build cycle once. They are printed with %x directly.
 *
 * The sequence follows Atmosphere's own haze (troposphere/haze/source/
 * usb_session.cpp:250,256-258): PostBufferAsync -> wait CompletionEvent ->
 * eventClear -> GetReportData -> ParseReportData. */
namespace ams::mitm::applet {

    /* M98: sftap ReloadConfig. A release install only: a test install's arm
     * file is read once at boot, as it always was. The stream thread reads
     * these globals at the start of a session; the request makes it end the
     * current one (StreamEnd::Reconfigure) so the next starts with them. */
    void ReloadReleaseConfig() {
        if (!g_release_mode) { LogLine("sftap: ReloadConfig ignored - test install (arm file)"); return; }
        ::ams::g_arm_loaded = false;
        ::ams::LoadArm();
        ::ams::ApplySessionSettings();
        LogLine("sftap: config reloaded -> %.*s", static_cast<int>(::ams::g_arm_len), ::ams::g_arm_buf);
        g_reconfig_request.store(true, std::memory_order_relaxed);
    }

    /* M71: the negotiated link speed, asked of the kernel rather than
     * assumed. The stream picks its resolution from this: SuperSpeed lifts
     * the ~37 MB/s wall M70 measured, and there is no point aiming a
     * 129 MB/s frame rate at a High-Speed link. */
    bool UsbIsSuperSpeed() {
        UsbDeviceSpeed sp = UsbDeviceSpeed_None;
        if (R_FAILED(usbDsGetSpeed(std::addressof(sp)))) { return false; }
        return sp == UsbDeviceSpeed_Super;
    }

    void UsbCancelIn();

    namespace {
        constinit std::atomic<u32> g_viewer_reason{0};

        bool UsbPostWait(void *buf, u32 size, u32 timeout_ms) {
            u32 urb = 0;
            if (R_FAILED(usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in, buf, size, std::addressof(urb)))) { return false; }
            if (R_FAILED(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), static_cast<u64>(timeout_ms) * 1000000))) {
                UsbCancelIn();
                return false;
            }
            eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
            return true;
        }

        /* The status packet: a header flagged audio + status (16) with rate 0
         * and 0 channels - viewers before v0.7.10 drop audio that is not
         * 48 kHz stereo - then the reason and two lines of text, each its own
         * transfer as the video's header and payload are. */
        void UsbSendStatus(u32 reason, bool net = false) {
            struct Status { u32 version; u32 reason; char head[64]; char detail[120]; };
            static_assert(sizeof(Status) == 192);
            static const char *const text[][2] = {
                { "", "" },
                { "Connected - no game is running", "Start a game and the picture appears here." },
                { "Connected - the game is not on screen", "The HOME menu and system screens can't be streamed. Go back to the game." },
                { "Paused - a manual, the eShop or another applet is open", "The stream comes back a few seconds after you close it." },
                { "Streaming is turned off", "Turn it on in the Switch Frame Tap overlay or manager app." },
                { "This app is excluded from streaming", "Remove it from the excluded apps in the manager app." },
                { "This firmware is untested", "Set allow_untested_firmware = 1 in config.ini to stream anyway." },
                { "The video engine stopped responding", "Restart the console to stream again." },
                { "Not enough memory to stream right now", "It tries again every few seconds." },
                { "This game's picture was not found", "Restart the game. If it keeps happening, please send the log." },
                { "Starting the stream...", "" },
                { "Paused - the keyboard is open", "There is no memory left to stream while it is open. The stream comes back when you close it." },
            };
            if (reason >= sizeof(text) / sizeof(text[0])) { return; }
            alignas(0x1000) static u8 hdr_buf[0x1000];
            alignas(0x1000) static u8 body_buf[0x1000];
            const u32 h[8] = { 0x52544653u, 2u | ((8u | 16u) << 16), 0, 0, 0, static_cast<u32>(sizeof(Status)), 0, 0 };
            std::memcpy(hdr_buf, h, sizeof(h));
            Status st = {};
            st.version = 1;
            st.reason = reason;
            std::snprintf(st.head, sizeof(st.head), "%s", text[reason][0]);
            std::snprintf(st.detail, sizeof(st.detail), "%s", text[reason][1]);
            std::memcpy(body_buf, std::addressof(st), sizeof(st));
            if (net) {   /* v0.7.17: a network viewer (one thread sends - this one) */
                if (NetSendBuffer(hdr_buf, sizeof(h))) { static_cast<void>(NetSendBuffer(body_buf, sizeof(st))); }
                return;
            }
            if (UsbPostWait(hdr_buf, sizeof(h), 300)) { static_cast<void>(UsbPostWait(body_buf, sizeof(st), 300)); }
        }
    }

    void SetViewerReason(ViewerReason r) { g_viewer_reason.store(r, std::memory_order_relaxed); }

    namespace {
        constinit bool g_usb_released = false;
    }

    void NoteUsbLeftAlone() {
        g_usb_released = true;
        LogLine("usb: streaming is off - the USB port is left to other apps until it is switched on");
    }

    /* v0.7.11 (masagrator): streaming off = usb:ds given up, so other USB apps
     * (installers, MTP) can use the port; on = enumerated again. Only ever
     * called on the VIC worker thread, the one thread that sends over USB, and
     * never during a stream session. Webcam mode keeps its camera. */
    void UsbFollowStreamSwitch() {
        if (!::ams::g_usb_armed || g_uvc_mode) { return; }
        const bool on = g_stream_enabled.load(std::memory_order_relaxed);
        if (!on && !g_usb_released && ::ams::g_usb_ep_in != nullptr) {
            static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_in));
            UsbByeStop();
            if (::ams::g_usb_ep_out != nullptr) { static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_out)); }
            usbDsExit();
            ::ams::g_usb_iface = nullptr;
            ::ams::g_usb_ep_in = nullptr;
            ::ams::g_usb_ep_out = nullptr;
            g_usb_released = true;
            LogLine("usb: streaming switched off - USB port released for other apps");
        } else if (on && g_usb_released) {
            g_usb_released = false;
            LogLine("usb: streaming switched on - taking the USB port again");
            SmUse sm_usb;
            ::ams::TryUsbEnumerate();
        }
    }

    bool UsbViewerPresent(u32 timeout_ms) {
        /* v0.7: a viewer connected over the network counts, and goes first */
        if (NetClientPresent()) {
            /* v0.7.17: the status goes over the network too, about once a
             * second - the viewer says why there is no picture, and the silence
             * after a game stops drawing no longer looks like a dead connection
             * (the viewer reconnects after 5 s without data, PR #23) */
            static u64 s_status_ns = 0;
            const u64 now = armTicksToNs(armGetSystemTick());
            if (now - s_status_ns >= UINT64_C(1000000000)) {
                s_status_ns = now;
                if (const u32 r = g_viewer_reason.load(std::memory_order_relaxed); r != ViewerReason_None) { UsbSendStatus(r, true); }
                else {
                    /* nothing to say: an empty header, which every viewer skips */
                    static constexpr u32 Hello[8] = { 0x52544653u, 2u, 0, 0, 0, 0, 0, 0 };
                    static_cast<void>(NetSendBuffer(Hello, sizeof(Hello)));
                }
            }
            return true;
        }
        if (!UsbReady()) { return false; }
        /* v0.4: a webcam's "viewer" is a host that committed a stream */
        if (g_uvc_mode) {
            for (u32 t = 0; !g_uvc_streaming.load(std::memory_order_relaxed); t += 50) {
                if (t >= timeout_ms) { return false; }
                os::SleepThread(TimeSpan::FromMilliSeconds(50));
            }
            /* committed - but after a stall, only once the host reads again */
            return !UvcHostStalled() || UvcHostReading(timeout_ms);
        }
        alignas(0x1000) static u8 hello[0x1000];
        const u32 h[8] = { 0x52544653u, 2u, 0, 0, 0, 0, 0, 0 };   /* "SFTR", v2, all else 0: length 0 */
        std::memcpy(hello, h, sizeof(h));
        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in, hello, sizeof(h), std::addressof(urb)))) { return false; }
        if (R_FAILED(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), static_cast<u64>(timeout_ms) * 1000000))) {
            UsbCancelIn();
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
        UsbDsReportData report = {};
        u32 transferred = 0;
        if (R_FAILED(usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report)))) { return false; }
        if (R_FAILED(usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred)))) { return false; }
        if (transferred != sizeof(h)) { return false; }
        if (const u32 r = g_viewer_reason.load(std::memory_order_relaxed); r != ViewerReason_None) { UsbSendStatus(r); }
        return true;
    }

    /* v0.7.17: between two sessions (a game rebuilding its buffers, up to ~8 s)
     * the stream sends nothing; viewers with PR #23 take 5 s without data as a
     * dead connection and reconnected each time (logs/v0717-test1). About once
     * a second: the USB probe's empty header, or the network status. */
    void ViewerKeepAlive() {
        if (g_uvc_mode) { return; }
        static u64 s_ns = 0;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now - s_ns < UINT64_C(1000000000)) { return; }
        s_ns = now;
        static_cast<void>(UsbViewerPresent(100));
    }

    bool UsbReady() {
        if (::ams::g_usb_ep_in == nullptr && !g_uvc_mode) { return false; }
        UsbState st = UsbState_Detached;
        if (R_FAILED(usbDsGetState(std::addressof(st)))) { return false; }
        return st == UsbState_Configured;
    }

    bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb) {
        if (out_urb != nullptr) { *out_urb = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {        /* v0.7: this stream is on the network */
            if (NetPostAsync(buf, len)) { return true; }
            NetDropClient("send failed");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        u32 urb_id = 0;
        const ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                                const_cast<void *>(buf), static_cast<u32>(len),
                                std::addressof(urb_id));
        if (R_FAILED(rc)) {
            LogLine("   usb: PostBufferAsync(%zu B) rc=0x%x", len, rc);
            return false;
        }
        if (out_urb != nullptr) { *out_urb = urb_id; }
        return true;
    }

    /* M86: a transfer the host never takes stays posted. Left there, its
     * bytes go to whichever program reads the endpoint next - a new viewer
     * would start mid-frame. Cancel it and let the cancellation complete
     * (SysDVR's UsbComms.c does the same). */
    void UsbCancelIn() {
        if (::ams::g_usb_ep_in == nullptr) { return; }
        static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_in));
        static_cast<void>(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), UINT64_C(1000000000)));
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
    }

    bool UsbWaitAsync(u32 urb, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {
            if (NetWaitAsync(out_sent)) { return true; }
            NetDropClient("the connection dropped");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        ::Result rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
        if (R_FAILED(rc)) {
            LogLine("   usb: async completion timed out rc=0x%x (host not draining?) - cancelled", rc);
            UsbCancelIn();
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

        UsbDsReportData report = {};
        rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
        if (R_FAILED(rc)) { LogLine("   usb: async GetReportData rc=0x%x", rc); return false; }

        u32 transferred = 0;
        rc = usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred));
        if (R_FAILED(rc)) { LogLine("   usb: async ParseReportData rc=0x%x", rc); return false; }

        if (out_sent != nullptr) { *out_sent = transferred; }
        return transferred > 0;
    }

    void SetScreenOffNow(bool on) { ::ams::g_screen_off.store(on, std::memory_order_relaxed); }

    namespace { bool g_bye_posted = false; u32 g_bye_urb = 0; }

    bool UsbViewerSaidBye() {
        if (g_tx_net.load(std::memory_order_relaxed) || g_uvc_mode || ::ams::g_usb_ep_out == nullptr) { return false; }
        UsbDsEndpoint *ep = ::ams::g_usb_ep_out;
        if (!g_bye_posted) {
            eventClear(std::addressof(ep->CompletionEvent));
            std::memset(::ams::g_usb_ep_out_buf, 0, 16);
            if (R_FAILED(usbDsEndpoint_PostBufferAsync(ep, ::ams::g_usb_ep_out_buf, sizeof(::ams::g_usb_ep_out_buf), std::addressof(g_bye_urb)))) { return false; }
            g_bye_posted = true;
            return false;
        }
        if (R_FAILED(eventWait(std::addressof(ep->CompletionEvent), 0))) { return false; }   /* nothing yet */
        eventClear(std::addressof(ep->CompletionEvent));
        g_bye_posted = false;
        UsbDsReportData report = {};
        u32 got = 0;
        if (R_FAILED(usbDsEndpoint_GetReportData(ep, std::addressof(report))) ||
            R_FAILED(usbDsParseReportData(std::addressof(report), g_bye_urb, nullptr, std::addressof(got)))) { return false; }
        /* v0.7.16: "SFTC" after the SFTK - this viewer draws crops itself */
        for (u32 k = 0; k + 4 <= got && k + 4 <= sizeof(::ams::g_usb_ep_out_buf); k += 4) {
            if (std::memcmp(::ams::g_usb_ep_out_buf + k, "SFTC", 4) == 0) { g_viewer_crop.store(true, std::memory_order_relaxed); }
            /* v0.7.17: "SFTD" - it records: the pictures cropped here, not in the viewer */
            if (std::memcmp(::ams::g_usb_ep_out_buf + k, "SFTD", 4) == 0) { g_viewer_crop.store(false, std::memory_order_relaxed); }
            /* v0.7.18: "SFTL" / "SFTU" - it records: one stream size until it stops (#44) */
            if (std::memcmp(::ams::g_usb_ep_out_buf + k, "SFTL", 4) == 0) { ViewerLockSize(true); }
            if (std::memcmp(::ams::g_usb_ep_out_buf + k, "SFTU", 4) == 0) { ViewerLockSize(false); }
        }
        /* v0.7.13: "SFTK" - the viewer wants a keyframe (a new decoder, a recording) */
        if (got >= 4 && std::memcmp(::ams::g_usb_ep_out_buf, "SFTK", 4) == 0) { g_viewer_key_request.store(true, std::memory_order_relaxed); return false; }
        const bool bye = got >= 4 && std::memcmp(::ams::g_usb_ep_out_buf, "SFTQ", 4) == 0;
        if (bye) { g_viewer_crop.store(false, std::memory_order_relaxed); ViewerLockSize(false); }
        return bye;
    }

    void UsbByeStop() {
        if (!g_bye_posted || ::ams::g_usb_ep_out == nullptr) { g_bye_posted = false; return; }
        static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_out));
        static_cast<void>(eventWait(std::addressof(::ams::g_usb_ep_out->CompletionEvent), UINT64_C(100000000)));
        eventClear(std::addressof(::ams::g_usb_ep_out->CompletionEvent));
        g_bye_posted = false;
    }

    bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {
            if (NetSendBuffer(buf, len)) { if (out_sent != nullptr) { *out_sent = len; } return true; }
            NetDropClient("the connection dropped");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        const u8 *p = static_cast<const u8 *>(buf);
        size_t done = 0;

        while (done < len) {
            const size_t chunk = ((len - done) < ::ams::UsbChunk) ? (len - done) : ::ams::UsbChunk;

            u32 urb_id = 0;
            ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                              const_cast<u8 *>(p + done), static_cast<u32>(chunk),
                              std::addressof(urb_id));
            if (R_FAILED(rc)) {
                LogLine("   usb: PostBufferAsync(+%zu, %zu B) rc=0x%x", done, chunk, rc);
                return false;
            }

            rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
            if (R_FAILED(rc)) {
                LogLine("   usb: completion wait timed out at +%zu rc=0x%x (host not reading?) - cancelled", done, rc);
                UsbCancelIn();
                return false;
            }
            eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

            UsbDsReportData report = {};
            rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
            if (R_FAILED(rc)) { LogLine("   usb: GetReportData rc=0x%x", rc); return false; }

            u32 transferred = 0;
            rc = usbDsParseReportData(std::addressof(report), urb_id, nullptr, std::addressof(transferred));
            if (R_FAILED(rc)) { LogLine("   usb: ParseReportData rc=0x%x", rc); return false; }

            if (transferred == 0) {
                LogLine("   usb: zero-length completion at +%zu - host is not draining", done);
                return false;
            }
            if (transferred != chunk) {
                /* A short completion would push the next post off 0x1000
                 * alignment, which usbDs rejects. Report it rather than send
                 * a silently corrupt frame. */
                done += transferred;
                if (out_sent != nullptr) { *out_sent = done; }
                LogLine("   usb: short completion %u/%zu at +%zu - stopping (would break alignment)",
                        transferred, chunk, done - transferred);
                return false;
            }

            done += transferred;
            if (out_sent != nullptr) { *out_sent = done; }
        }
        return true;
    }

}
