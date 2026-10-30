#include "applet_mitm_donor.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_uisource.hpp"

namespace ams::mitm::applet {

    namespace {

        constexpr u64 DonorProgramId = 0x0100000000000C21ull;

        constinit os::SdkMutex g_lock;
        /* 0 waiting, 1 memory handed over, 2 the donor could not get any */
        constinit std::atomic<int> g_state{0};
        constinit os::NativeHandle g_handle = os::InvalidNativeHandle;
        constinit u64 g_size = 0;
        constinit os::NativeHandle g_nv_handle = os::InvalidNativeHandle;   /* v0.7.13 */
        constinit u64 g_nv_size = 0;
        constinit u32 g_fail_rc = 0;
        constinit u64 g_pid = 0;
        constinit os::TransferMemoryType g_tmem = {};
        constinit bool g_mapped = false;
        constinit void *g_addr = nullptr;

        /* v0.7.10: pm:shell stays open while the donor lives - opening it
         * to terminate took 180 ms of test18's race against the browser, and
         * the donor owns the memory until it exits */
        constinit bool g_pmshell_open = false;

        void TerminateDonor() {
            /* an nvdrv transfer memory the engines never took */
            if (g_nv_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(g_nv_handle); g_nv_handle = os::InvalidNativeHandle; }
            if (g_pid == 0) { return; }
            ::Result rc = 0;
            if (g_pmshell_open) {
                rc = ::pmshellTerminateProcess(g_pid);
            } else {
                SmUse sm_pm;
                const ::Result r0 = ::pmshellInitialize();
                rc = R_SUCCEEDED(r0) ? ::pmshellTerminateProcess(g_pid) : r0;
                if (R_SUCCEEDED(r0)) { ::pmshellExit(); }
            }
            if (g_pmshell_open) { ::pmshellExit(); g_pmshell_open = false; }
            LogLine("donor: pid %llu terminated rc=0x%x", static_cast<unsigned long long>(g_pid), rc);
            g_pid = 0;
        }

    }

    constinit std::atomic<bool> g_applet_needs_memory{false};

    namespace {
        /* v0.7.14: the applets that end up needing (nearly) the whole Applet
         * pool are the web ones - the browser, the eShop, manuals and news,
         * the Nintendo Account and Wi-Fi login pages */
        bool WebAppletRunning() {
            /* firmware 22.0 replaced the shop, web, login and auth applets
             * with systemWeb (0x1042) and openWeb (0x1043); the old ids are
             * stubs there (switchbrew 22.0.0) */
            static constexpr u64 Web[] = { 0x010000000000100Aull, 0x010000000000100Bull, 0x010000000000100Full, 0x0100000000001010ull, 0x0100000000001011ull,
                                           0x0100000000001042ull, 0x0100000000001043ull };
            for (const u64 id : Web) {
                os::ProcessId pid{};
                if (R_SUCCEEDED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{id})) && pid.value != 0) { return true; }
            }
            return false;
        }

        /* v0.7.14: what the Applet pool says about the stream. Only a web
         * applet (it may want the whole pool) or a pool within 48 MB of full
         * stops it. The HOME menu used this from beta1 (the user picker took
         * the pool to 270 MB of 500 with our 41, test2; System Settings to
         * 454); games since beta2: they stopped whenever the pool passed
         * 160 MB, and Super Mario 3D All-Stars runs its games with the pool
         * at 290-340 MB - the stream waited the whole time (issue #20).
         * Library applets a game opens still get the memory back first
         * (PrepareForApplet). */
        constexpr u64 TightMb = 48;
        /* v0.7.16: with a web applet open, this much more must stay free
         * beside us. Super Mario 3D All-Stars runs each game with OfflineWeb
         * (0x17) open the whole time and the pool at ~287 of 500 MB: "a web
         * applet runs" alone kept the stream off for the whole game. The
         * browser and the eShop take (nearly) the whole pool, so they still
         * keep it off; the pool watch stops it if one grows into this. */
        constexpr u64 WebRoomMb = 96;
        /* v0.7.14: the smallest donor that streams 720p (capture region
         * >= ~22 MB plus the engine buffers) and the usual one */
        constexpr u32 MinDonorMb = 30, FullDonorMb = 42;
        constinit std::atomic<u32> g_cap_mb{FullDonorMb};

        /* v0.7.15: firmware 23 made the Applet pool 33 MB smaller (500 ->
         * 467 MB), and the applets kept their sizes, so beside the two big
         * ones the 48 MB margin no longer leaves room for a donor:
         *  - the Album (photoViewer - not the homebrew menu in its place) holds
         *    a constant 324 MB for its whole session (the pool at 421 MB with
         *    HOME in every 22.5 run and on 23.0.1), 46 MB left;
         *  - System Settings adds 316 MB (the pool at 413 MB with HOME on 23.0.1,
         *    412 on 22.5), 54 MB left. On a retail console it is the HOME menu
         *    itself: the set applet (0x100E) is not on retail (switchbrew), and
         *    no thread was created when it opened (test2: the Applet group's
         *    threads stayed at 30) - qlaunch grows by the 316 MB. It opens
         *    other applets (the keyboard, connection tests, web pages) - web
         *    ones are held by the systemWeb hook; any new process beside it
         *    stops the stream at once (below).
         * Each has a margin kept free when the donor is taken, and a smaller
         * one under which the stream stops, so a MB of drift never makes it
         * stop and start again (logs/v0715-album-fw23, logs/v0715-test1). */
        struct Beside { u64 take_mb, stop_mb; const char *what; };
        constexpr Beside BesideOther    = { TightMb, TightMb, nullptr };
        constexpr Beside BesideAlbum    = { 12, 8, "the Album" };
        constexpr Beside BesideSettings = { 20, 14, "System Settings" };

        bool Running(u64 program_id, os::ProcessId *out_pid = nullptr) {
            os::ProcessId pid{};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id})) || pid.value == 0) { return false; }
            if (out_pid != nullptr) { *out_pid = pid; }
            return true;
        }

        /* Any 100D: stock photoViewer, hbmenu, or SwitchU (HBL title replace).
         * v0.7.15 only counted non-HBL Album so hbmenu's variable size did not
         * use the tight Album margin. SwitchU coexistence needs that margin -
         * without it BesideOther (48 MB) never clears AppletBusy while SwitchU
         * holds ~456 of 467 MB, and NeedHeadroom never runs (DonorObtain is
         * past the busy wait). */
        bool AlbumOrSwitchURunning() {
            return Running(0x010000000000100Dull);
        }

        /* the HOME menu's own memory: idle it is part of the 97 MB the Applet
         * group holds; with System Settings open it holds 316 MB more */
        constexpr u64 HomeWithSettingsMb = 200;
        u64 HomeMenuMb() {
            os::ProcessId pid{};
            if (!Running(0x0100000000001000ull, std::addressof(pid))) { return 0; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), pid))) { return 0; }
            u64 used = 0;
            const bool ok = R_SUCCEEDED(::svcGetInfo(&used, InfoType_UsedMemorySize, h, 0));
            os::CloseNativeHandle(h);
            return ok ? used >> 20 : 0;
        }

        /* what is open beside the stream: asked of pm only when the usual
         * margin is in the way, and once per pool size (the watch looks every
         * 20 ms, and the pool does not move while an applet just sits open) */
        constinit std::atomic<u64> g_beside_free{~u64(0)};
        const Beside &BesideNow(u64 free_mb) {
            static constinit std::atomic<u64> &s_free = g_beside_free;
            static constinit std::atomic<int> s_kind{0};
            /* the pool back near idle: the applet closed - ask again next time */
            if (free_mb >= TightMb + FullDonorMb) { s_free.store(~u64(0), std::memory_order_relaxed); return BesideOther; }
            if (s_free.load(std::memory_order_relaxed) != free_mb) {
                const u64 home_mb = HomeMenuMb();
                const int kind = AlbumOrSwitchURunning() ? 1 : home_mb >= HomeWithSettingsMb ? 2 : 0;
                if (s_kind.exchange(kind, std::memory_order_relaxed) != kind || s_free.load(std::memory_order_relaxed) == ~u64(0)) {
                    LogLine("donor: %llu MB free in the Applet pool - %s (the HOME menu holds %llu MB)", static_cast<unsigned long long>(free_mb),
                            kind == 1 ? "the Album / SwitchU is open" : kind == 2 ? "System Settings is open" : "no Album or System Settings",
                            static_cast<unsigned long long>(home_mb));
                }
                s_free.store(free_mb, std::memory_order_relaxed);
            }
            const int k = s_kind.load(std::memory_order_relaxed);
            return k == 1 ? BesideAlbum : k == 2 ? BesideSettings : BesideOther;
        }

        /* beside a big applet the margin is small: a process created there
         * (an applet it opens) stops the stream before it asks for memory -
         * it shows in the process list while its code still loads */
        constinit u64 g_beside_max_pid = 0;   /* 0: no baseline (not beside a big applet) */
        bool NewProcessBeside(u64 donor_pid, u64 *out_pid) {
            u64 &s_max_pid = g_beside_max_pid;
            u64 pids[96];
            s32 n = 0;
            if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return false; }
            u64 top = 0;
            for (s32 i = 0; i < n; ++i) { if (pids[i] != donor_pid && pids[i] > top) { top = pids[i]; } }
            const bool fresh = s_max_pid != 0 && top > s_max_pid;
            s_max_pid = top;
            *out_pid = top;
            return fresh;
        }
        u64 ProgramIdOf(u64 pid) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return 0; }
            os::CloseNativeHandle(h);
            return loc.program_id.value;
        }
        u64 AppletFreeMb() {
            u64 cur = 0, lim = 0;
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return 0; }
            return (lim - cur) >> 20;
        }
    }

    u32 DonorCapMb() { return g_cap_mb.load(std::memory_order_relaxed); }

    namespace {
        alignas(os::ThreadStackAlignment) constinit u8 g_watch_stack[8_KB];
        constinit os::ThreadType g_watch_thread;
        constinit u64 g_trigger_ns = 0;
        constinit std::atomic<u64> g_hold_until_ns{0};
        /* v0.7.17: when the Applet pool last jumped by 16 MB or more without
         * us (OfflineWeb starting, its menu, going back to the title list:
         * 287 -> 457 -> 297 within a second, twice over on the way out), and
         * when our own donor last came or went */
        constinit std::atomic<u64> g_pool_moved_ns{0};
        constinit std::atomic<u64> g_donor_change_ns{0};
        constexpr u64 PoolCalmNs = 2'000'000'000ull;

        /* v0.7.14: systemWeb (firmware 22+: the eShop, news pages, logins)
         * asks for its whole memory as it starts - faster than any poll: with
         * the HOME menu streaming, the eShop opened from HOME aborted with
         * 2210-1024 and the Applet pool never moved (logs/v0714-p0test1).
         * While the donor holds Applet memory, pm holds systemWeb back once it
         * is created (pm:dmnt HookToCreateProcess); the stream stops, the
         * memory goes back, and only then does it start. One hook slot in pm:
         * if another tool holds it, the pool watch below is all there is. */
        constexpr u64 SystemWebProgramId = 0x0100000000001042ull;
        constexpr u64 WebHookMaxHoldNs = 1'500'000'000ull;
        constinit os::NativeHandle g_web_hook = os::InvalidNativeHandle;
        constinit u64 g_web_hook_fired_ns = 0;   /* 0: armed, not fired */

        void StartHookedWeb(const char *memory) {
            os::ProcessId pid{};
            ::Result rc = ::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{SystemWebProgramId}).GetValue();
            if (R_SUCCEEDED(rc)) { rc = ::ams::pm::dmnt::StartProcess(pid).GetValue(); }
            LogLine("donor: systemWeb started (pid %llu) rc=0x%x - memory %s after %llu ms", static_cast<unsigned long long>(pid.value), rc, memory,
                    static_cast<unsigned long long>(g_web_hook_fired_ns != 0 ? (armTicksToNs(armGetSystemTick()) - g_web_hook_fired_ns) / 1000000 : 0));
        }

        void CloseWebHook() {
            os::CloseNativeHandle(g_web_hook);
            g_web_hook = os::InvalidNativeHandle;
            g_web_hook_fired_ns = 0;
        }

        void WebHookStep() {
            if (g_web_hook == os::InvalidNativeHandle) {
                if (!g_mapped || g_pmdmnt_rc != 0) { return; }
                static bool said = false;
                os::NativeHandle h = os::InvalidNativeHandle;
                const ::Result rc = ::ams::pm::dmnt::HookToCreateProcess(std::addressof(h), ncm::ProgramId{SystemWebProgramId}).GetValue();
                if (R_FAILED(rc)) {
                    if (!said) { LogLine("donor: no systemWeb launch hook (rc=0x%x, another tool holds pm's hook) - the pool watch only", rc); said = true; }
                    return;
                }
                g_web_hook = h;
                g_web_hook_fired_ns = 0;
                return;
            }
            const u64 now = armTicksToNs(armGetSystemTick());
            if (g_web_hook_fired_ns == 0) {
                if (R_SUCCEEDED(::svcWaitSynchronizationSingle(g_web_hook, 0))) {
                    static_cast<void>(::svcResetSignal(g_web_hook));
                    g_web_hook_fired_ns = now;
                    if (g_mapped) {
                        LogDefer(true);   /* written once the memory is back (DonorRelease) */
                        LogLine("donor: systemWeb (eShop, news, a login page) is starting - held until the stream gives the memory back");
                        g_hold_until_ns.store(now + 4'000'000'000ull, std::memory_order_relaxed);
                        g_trigger_ns = now;
                        g_applet_needs_memory.store(true, std::memory_order_relaxed);
                    }
                } else if (!g_mapped) {
                    /* the memory went back for another reason: let pm start
                     * systemWeb by itself again - unless it fired just now */
                    static_cast<void>(::pmdmntClearHook(1));
                    if (R_SUCCEEDED(::svcWaitSynchronizationSingle(g_web_hook, 0))) {
                        static_cast<void>(::svcResetSignal(g_web_hook));
                        StartHookedWeb("not held");
                    }
                    CloseWebHook();
                }
                return;
            }
            /* fired: start it once the memory is back - and never hold it
             * longer than 1.5 s, whatever the stream does */
            if (!g_mapped || now - g_web_hook_fired_ns >= WebHookMaxHoldNs) {
                if (!g_mapped) { os::SleepThread(TimeSpan::FromMilliSeconds(15)); }   /* the donor frees its heap ~5 ms after the unmap */
                StartHookedWeb(g_mapped ? "STILL HELD" : "given back");
                CloseWebHook();   /* pm cleared the hook when it fired */
            }
        }

        /* Every 100 ms while the donor's memory is mapped: the Applet group
         * past idle plus the donor means an applet started. Test16's browser
         * went 96 -> 288 MB in its first 250 ms sample and asked for the rest
         * ~0.9 s later. */
        void AppletWatchThread(void *) {
            u64 checked_mb = 0, base_mb = 0;   /* base_mb: the pool when our memory came (v0.7.17) */
            bool was_mapped = false;
            u64 calm_prev_mb = 0;
            for (u32 tick = 0;; ++tick) {
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
                WebHookStep();
                /* v0.7.17: every 100 ms, mapped or not: is the pool moving? */
                if ((tick % 5) == 0) {
                    u64 c = 0, l = 0;
                    if (R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(c), std::addressof(l), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) {
                        const u64 m = c >> 20, now = armTicksToNs(armGetSystemTick());
                        const u64 d = m > calm_prev_mb ? m - calm_prev_mb : calm_prev_mb - m;
                        if (calm_prev_mb != 0 && d >= 16 && now - g_donor_change_ns.load(std::memory_order_relaxed) >= 1'500'000'000ull) {
                            g_pool_moved_ns.store(now, std::memory_order_relaxed);
                        }
                        calm_prev_mb = m;
                    }
                }
                if (g_applet_needs_memory.load(std::memory_order_relaxed)) {
                    /* the teardown never reached DonorRelease: write the log anyway */
                    if (armTicksToNs(armGetSystemTick()) - g_trigger_ns > 3'000'000'000ull) { LogDefer(false); }
                    continue;
                }
                if (!g_mapped) { was_mapped = false; g_beside_max_pid = 0; continue; }
                u64 cur = 0, lim = 0;
                if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { continue; }
                const u64 mb = cur >> 20, donor_mb = g_size >> 20;
                /* the pool grew 32 MB since the last look: is it a web applet? */
                if (!was_mapped) { was_mapped = true; checked_mb = base_mb = mb; }
                if (mb < checked_mb) { checked_mb = mb; }
                if (mb < base_mb) { base_mb = mb; }
                /* v0.7.15: beside the Album or System Settings the margins are small */
                const u64 free_mb = (lim >> 20) - mb;
                const Beside &beside = free_mb < TightMb ? BesideNow(free_mb) : BesideOther;
                const bool tight = free_mb < beside.stop_mb;
                if (beside.what == nullptr) { g_beside_max_pid = 0; }
                u64 new_pid = 0;
                const bool spawned = beside.what != nullptr && NewProcessBeside(g_pid, std::addressof(new_pid));
                bool web = false;
                /* v0.7.16: a web applet beside us only matters once it leaves less than WebRoomMb.
                 * v0.7.17: or once it grows 24 MB past where it was when our
                 * memory came - Super Mario 3D All-Stars' "-" menu (OfflineWeb,
                 * 342 -> 384 MB) over Super Mario Sunshine, which draws on
                 * under it: no stall to stop the stream, and OfflineWeb ran
                 * out (2003-0008) on the next step */
                if (mb >= checked_mb + 32 || mb >= base_mb + 24 || tight) {
                    checked_mb = mb;
                    web = (free_mb < WebRoomMb || mb >= base_mb + 24) && WebAppletRunning();
                }
                const bool trigger = web || tight || spawned;
                if (trigger) {
                    LogDefer(true);   /* written once the memory is back (DonorRelease) */
                    g_trigger_ns = armTicksToNs(armGetSystemTick());
                    g_applet_needs_memory.store(true, std::memory_order_relaxed);
                    LogLine("donor: an applet is starting (Applet group %llu MB, %llu of them ours%s%s) - stopping the stream to give the memory back",
                            static_cast<unsigned long long>(mb), static_cast<unsigned long long>(donor_mb),
                            spawned ? "; a new process beside " : "", spawned ? beside.what : "");
                    if (spawned) { LogLine("donor: the new process: pid %llu, program %016llx", static_cast<unsigned long long>(new_pid), static_cast<unsigned long long>(ProgramIdOf(new_pid))); }
                }
            }
        }
    }

    /* v0.7.17: a hold ends early once the Applet pool has stopped growing -
     * Super Mario 3D All-Stars' OfflineWeb peaks ~1 s after it starts and
     * settles ~0.5 s later; the full 4-5 s was the longest part of the wait
     * for the picture after each game started */
    bool AppletHoldActive() {
        const u64 now = armTicksToNs(armGetSystemTick());
        const u64 until = g_hold_until_ns.load(std::memory_order_relaxed);
        if (now >= until) { return false; }
        /* counted from the first look at this hold - later, never sooner */
        static u64 s_until = 0, s_start = 0, s_prev_mb = 0, s_stable_ns = 0;
        if (until != s_until) { s_until = until; s_start = now; s_prev_mb = 0; s_stable_ns = 0; }
        constexpr u64 MinHoldNs = 2'000'000'000ull, SettledNs = 750'000'000ull;
        const u64 start = s_start;
        if (now - start < MinHoldNs) { return true; }
        /* v0.7.17: not between two games - the donor launched while
         * Super Mario 3D All-Stars' title list was being started (back from
         * Super Mario 64) and the title list never came up; the full hold
         * kept that apart before */
        {
            os::ProcessId app{};
            if (g_pmdmnt_rc != 0 || R_FAILED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(app)))) { return true; }
        }
        {
            const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
            if (moved != 0 && now - moved < PoolCalmNs) { s_stable_ns = 0; return true; }
        }
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return true; }
        const u64 mb = cur >> 20;
        /* the applet's jumps came and went 2 s ago: that is the settling */
        const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
        const bool calm_after_move = moved != 0 && moved + 1'000'000'000ull >= start;
        if (s_stable_ns == 0 || mb > s_prev_mb + 4) { s_stable_ns = now; }
        s_prev_mb = mb;
        if (!calm_after_move && now - s_stable_ns < SettledNs) { return true; }
        g_hold_until_ns.store(0, std::memory_order_relaxed);
        LogLine("donor: the Applet pool settled at %llu MB after %llu ms - no more waiting", static_cast<unsigned long long>(mb),
                static_cast<unsigned long long>((now - start) / 1000000));
        return false;
    }

    void PrepareForApplet(u32 applet_id) {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        /* the applet takes its memory over its first seconds: no donor until then */
        g_hold_until_ns.store(t0 + 4'000'000'000ull, std::memory_order_relaxed);
        if (!g_mapped) { return; }
        /* no SD writes on this thread now: the game waits on it */
        LogDefer(true);
        LogLine("am: the game opens library applet %#x - stopping the stream to give the memory back first", applet_id);
        g_trigger_ns = t0;
        g_applet_needs_memory.store(true, std::memory_order_relaxed);
        while (g_mapped && armTicksToNs(armGetSystemTick()) - t0 < 1'500'000'000ull) { os::SleepThread(TimeSpan::FromMilliSeconds(5)); }
        /* the donor frees its heap ~5 ms after the unmap */
        os::SleepThread(TimeSpan::FromMilliSeconds(15));
        LogLine("am: memory %s after %llu ms - letting the applet start", g_mapped ? "STILL HELD" : "given back",
                static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000000));
    }

    void PrepareForHomeMenu() {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        /* Keep engines from remounting the donor while SwitchU / Album inits.
         * Viewer may still be connected; without this hold RunVicBlit sets up
         * again immediately and the first Album launch OOMs. */
        g_hold_until_ns.store(t0 + 5'000'000'000ull, std::memory_order_relaxed);
        g_trigger_ns = t0;
        g_applet_needs_memory.store(true, std::memory_order_relaxed);
        LogLine("home: detach-on-home - release capture memory for home menu (no engine re-setup for 5 s)");
    }

    void StartAppletWatch() {
        const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
        if (R_FAILED(os::CreateThread(std::addressof(g_watch_thread), AppletWatchThread, nullptr, g_watch_stack, sizeof(g_watch_stack), prio))) {
            LogLine("donor: applet watch thread not created - opening a manual while streaming will crash it");
            return;
        }
        os::SetThreadNamePointer(std::addressof(g_watch_thread), "applet-mitm.AppletWatch");
        os::StartThread(std::addressof(g_watch_thread));
        NoteThread(std::addressof(g_watch_thread));
    }

    void DonorAccept(u64 size, os::NativeHandle tmem) {
        if (g_state.load() != 0 || g_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(tmem); return; }
        g_handle = tmem;
        g_size = size;
        g_state.store(1);
    }

    void DonorAcceptNv(u64 size, os::NativeHandle tmem) {
        if (g_nv_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(g_nv_handle); }
        g_nv_handle = tmem;
        g_nv_size = size;
    }

    bool DonorTakeNvTmem(os::NativeHandle *out, u64 *out_size) {
        std::scoped_lock lk(g_lock);
        if (g_nv_handle == os::InvalidNativeHandle) { return false; }
        *out = g_nv_handle;
        *out_size = g_nv_size;
        g_nv_handle = os::InvalidNativeHandle;
        return true;
    }

    void DonorFailed(u32 rc) {
        g_fail_rc = rc;
        g_state.store(2);
    }

    bool DonorActive() { return g_mapped; }

    bool WebAppletOpen() { return WebAppletRunning(); }

    bool AlbumOrSwitchUOpen() { return AlbumOrSwitchURunning(); }

    bool AppletBusy(u64 *out_mb) {
        /* idle (home menu and friends) ~96 MB; the browser 290-494 MB of 500.
         * Launching the donor then took 42 MB from under it (test15: a
         * screenshot inside a manual). v0.7.14: busy only with a web applet
         * open or no room for the smallest donor and the margin */
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return false; }
        const u64 mb = cur >> 20;
        if (out_mb != nullptr) { *out_mb = mb; }
        const u64 free_mb = (lim >> 20) - mb;
        /* v0.7.15: the margin beside the Album or System Settings is smaller */
        if (free_mb < MinDonorMb + BesideNow(free_mb).take_mb) { return true; }
        if (!WebAppletRunning()) { return false; }
        /* v0.7.17: a web applet and a pool that moved in the last 2 s - between
         * two of OfflineWeb's peaks the pool looks like its quiet level
         * (287 vs ~300 MB), and the donor taken there left it 2 MB short
         * of its next peak (2003-0008) */
        const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
        if (moved != 0 && armTicksToNs(armGetSystemTick()) - moved < PoolCalmNs) { return true; }
        /* v0.7.16: a web applet beside the stream needs WebRoomMb more
         * (Super Mario 3D All-Stars runs every game with OfflineWeb open) */
        return free_mb < MinDonorMb + TightMb + WebRoomMb;
    }

    bool KeyboardHoldsStream() { return !WebAppletRunning() && Running(0x0100000000001008ull); }

    /* v0.7.18: beside the Album or System Settings the donor is taken smaller
     * (29-33 MB), and it stayed that size until the engines closed for some
     * other reason - a docked game then streamed at 720p (Smash after Settings,
     * logs/v0716-pr38test2). Room for the full donor again: what is free plus
     * ours, with the usual 48 MB left beside it, and no web applet. */
    bool DonorCanGrow() {
        if (!g_mapped || g_cap_mb.load(std::memory_order_relaxed) >= FullDonorMb) { return false; }
        static u64 s_last_ns = 0;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (s_last_ns != 0 && now - s_last_ns < UINT64_C(60000000000)) { return false; }
        const u64 free_mb = AppletFreeMb(), ours_mb = g_size >> 20;
        if (free_mb + ours_mb < TightMb + FullDonorMb || WebAppletRunning()) { return false; }
        s_last_ns = now;
        LogLine("donor: the Applet pool has room again (%llu MB free beside our %llu) - the full %u MB for a 1080p picture",
                static_cast<unsigned long long>(free_mb), static_cast<unsigned long long>(ours_mb), FullDonorMb);
        return true;
    }

    bool HomeDetachShouldFire(u64 stall_age_ms, u32 probe_ms, u32 fallback_ms, const char **out_reason) {
        if (probe_ms < 50) { probe_ms = 50; }
        if (fallback_ms < probe_ms) { fallback_ms = probe_ms; }

        /* Explicit HOME button (hid shm) - snappy path. Need a tiny present
         * gap so we do not tear down mid-frame; loads never press HOME. */
        if (HomeButtonRecent(3000) && stall_age_ms >= 50) {
            if (out_reason != nullptr) { *out_reason = "HOME button"; }
            return true;
        }

        if (stall_age_ms < probe_ms) { return false; }

        if (g_applet_needs_memory.load(std::memory_order_relaxed)) {
            if (out_reason != nullptr) { *out_reason = "applet needs memory"; }
            return true;
        }
        /* review (v0.7.18): the stream loop asks on every pass while a game
         * draws nothing - pm is asked at most every 100 ms. Not AppletBusy():
         * Super Mario 3D All-Stars keeps OfflineWeb open the whole game, the
         * pool always looked busy, and every loading pause would end the
         * session again (what #38 fixed); the pool watch catches an applet that
         * really needs the memory (g_applet_needs_memory, above). */
        static u64 s_asked_ns = 0;
        static bool s_hold = false, s_album = false;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now - s_asked_ns >= UINT64_C(100000000)) {
            s_asked_ns = now;
            s_hold = AppletHoldActive();
            /* Stock Album / SwitchU replace qlaunch as 010000000000100D (HBL too). */
            s_album = Running(0x010000000000100Dull);
        }
        if (s_hold) {
            if (out_reason != nullptr) { *out_reason = "applet hold active"; }
            return true;
        }
        if (s_album) {
            if (out_reason != nullptr) { *out_reason = "Album/SwitchU 100D up"; }
            return true;
        }
        /* Stock HOME with no Album yet / HOME shm dead: wait out load gaps */
        if (stall_age_ms >= fallback_ms) {
            if (out_reason != nullptr) { *out_reason = "present stall fallback"; }
            return true;
        }
        return false;
    }

    bool DonorObtain(uintptr_t *out_addr, size_t *out_size) {
        g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);   /* its heap comes before the map */
        /* Cap + SwitchU headroom wait outside g_lock (can sleep many seconds). */
        u32 ask_cap = FullDonorMb;
        {
            const u64 free_mb = AppletFreeMb();
            const Beside &beside = BesideNow(free_mb);
            const u64 room = free_mb > beside.take_mb ? free_mb - beside.take_mb : 0;
            ask_cap = room >= FullDonorMb ? FullDonorMb : (room >= MinDonorMb ? static_cast<u32>(room) : MinDonorMb);
        }
        bool need_headroom = false;
        {
            std::scoped_lock peek(g_lock);
            need_headroom = !g_mapped;
        }
        if (need_headroom) {
            if (AlbumOrSwitchUOpen()) {
                /* Boot / first HOME: do not launch on a 2 s timeout while SwitchU
                 * still fills the Applet pool (soak: "launching donor anyway" with
                 * 456 MB used). Re-ask until the pool has room or we defer. */
                constexpr u32 kTotalMs = 15000;
                constexpr u32 kRoundMs = 2500;
                u32 elapsed = 0;
                bool room = false;
                while (elapsed < kTotalMs) {
                    HeadroomAskAndWait(ask_cap, kRoundMs);
                    u64 used_mb = 0;
                    if (!AppletBusy(std::addressof(used_mb))) { room = true; break; }
                    HeadroomClear();
                    LogLine("donor: SwitchU still holds the Applet pool (%llu MB used, need ~%u MB free) - waiting, not launching",
                            static_cast<unsigned long long>(used_mb), ask_cap);
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                    elapsed += kRoundMs + 500;
                }
                if (!room) {
                    LogLine("donor: no Applet-pool headroom after %u ms beside SwitchU - deferring donor (engines will retry)",
                            kTotalMs);
                    HeadroomClear();
                    return false;
                }
                /* Pool has room - brief re-assert so SwitchU stays light through launch */
                HeadroomAskAndWait(ask_cap, 500);
            } else if (HeadroomEverAcked()) {
                /* SwitchU was present this boot - short ask before remount.
                 * Stock (never ACK'd): no wait (Tomas #56). */
                HeadroomAskAndWait(ask_cap, 2000);
            }
        }
        std::scoped_lock lk(g_lock);
        if (!g_mapped) {
            g_state.store(0);
            /* Last check: SwitchU may have filled the pool again between wait and lock */
            if (AlbumOrSwitchUOpen()) {
                u64 used_mb = 0;
                if (AppletBusy(std::addressof(used_mb))) {
                    LogLine("donor: Applet pool filled again beside SwitchU (%llu MB) - deferring launch",
                            static_cast<unsigned long long>(used_mb));
                    HeadroomClear();
                    return false;
                }
            }
            /* v0.7.14: beside a big applet (System Settings: the pool at 413
             * of 500 MB without us) the HOME menu streams on a smaller donor,
             * leaving the margin free (test5: 41 MB left 46 - the stream stopped) */
            u32 cap = FullDonorMb;
            {
                const u64 free_mb = AppletFreeMb();
                const Beside &beside = BesideNow(free_mb);
                const u64 room = free_mb > beside.take_mb ? free_mb - beside.take_mb : 0;
                cap = room >= FullDonorMb ? FullDonorMb : (room >= MinDonorMb ? static_cast<u32>(room) : MinDonorMb);
                if (cap < FullDonorMb) {
                    if (beside.what != nullptr) {
                        LogLine("donor: the Applet pool has %llu MB free - taking at most %u MB (%s is open: %llu MB are left beside it, the stream stops under %llu)",
                                static_cast<unsigned long long>(free_mb), cap, beside.what,
                                static_cast<unsigned long long>(beside.take_mb), static_cast<unsigned long long>(beside.stop_mb));
                    } else {
                        LogLine("donor: the Applet pool has %llu MB free - taking at most %u MB", static_cast<unsigned long long>(free_mb), cap);
                    }
                }
            }
            g_cap_mb.store(cap, std::memory_order_relaxed);
            {
                SmUse sm_pm;
                ::Result rc = ::pmshellInitialize();
                if (R_FAILED(rc)) { LogLine("donor: pm:shell rc=0x%x", rc); HeadroomClear(); return false; }
                const NcmProgramLocation loc = { .program_id = DonorProgramId, .storageID = NcmStorageId_None, .pad = {} };
                u64 pid = 0;
                rc = ::pmshellLaunchProgram(0, std::addressof(loc), std::addressof(pid));
                LogLine("donor: launch rc=0x%x pid %llu", rc, static_cast<unsigned long long>(pid));
                if (R_FAILED(rc)) { ::pmshellExit(); HeadroomClear(); return false; }
                g_pmshell_open = true;   /* kept for TerminateDonor */
                g_pid = pid;
            }
            for (int i = 0; i < 500 && g_state.load() == 0; ++i) { os::SleepThread(TimeSpan::FromMilliSeconds(10)); }
            if (g_state.load() != 1) {
                LogLine("donor: no memory (%s, rc=0x%x)", g_state.load() == 2 ? "the Applet pool refused every size" : "no answer in 5 s", g_fail_rc);
                TerminateDonor();
                HeadroomClear();
                return false;
            }
            os::AttachTransferMemory(std::addressof(g_tmem), g_size, g_handle, true);
            g_handle = os::InvalidNativeHandle;
            if (const Result rc = os::MapTransferMemory(std::addressof(g_addr), std::addressof(g_tmem), os::MemoryPermission_None); R_FAILED(rc)) {
                LogLine("donor: map %llu MB rc=0x%x", static_cast<unsigned long long>(g_size >> 20), rc.GetValue());
                os::DestroyTransferMemory(std::addressof(g_tmem));
                TerminateDonor();
                HeadroomClear();
                return false;
            }
            g_mapped = true;
            g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
            LogLine("donor: %llu MB from the Applet pool mapped at %p", static_cast<unsigned long long>(g_size >> 20), g_addr);
            HeadroomClear();
        }
        *out_addr = reinterpret_cast<uintptr_t>(g_addr);
        *out_size = g_size;
        return true;
    }

    void DonorRelease() {
        std::scoped_lock lk(g_lock);
        if (g_mapped) {
            /* Not os::UnmapTransferMemory: it aborts when the kernel refuses,
             * and it refuses while a device still maps the pages - nvservices
             * drops its pins only once it sees our nvdrv session closed, and
             * test13's unmap came the same millisecond (std::abort, fatal). */
            /* the engine buffers were made uncached (nvmapOwn), and the kernel
             * unmaps only memory with no attribute left - test14 was refused
             * with 0xd401 and attribute 0x8 (Uncached), no device map */
            const Result arc = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(g_addr), g_size, svc::MemoryAttribute_Uncached, 0);
            if (R_FAILED(arc)) { LogLine("donor: clearing Uncached rc=0x%x", arc.GetValue()); }
            Result rc = ResultSuccess();
            int tries = 0;
            for (; tries < 100; ++tries) {
                rc = svc::UnmapTransferMemory(g_tmem.handle, reinterpret_cast<uintptr_t>(g_addr), g_size);
                if (R_SUCCEEDED(rc)) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(50));
            }
            if (R_FAILED(rc)) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                static_cast<void>(svc::QueryMemory(std::addressof(mi), std::addressof(pi), reinterpret_cast<uintptr_t>(g_addr)));
                LogLine("donor: unmap refused for 5 s rc=0x%x (state %u attr %#x device maps %u ipc %u) - keeping the memory for the next stream",
                        rc.GetValue(), static_cast<unsigned>(mi.state), static_cast<unsigned>(mi.attribute),
                        static_cast<unsigned>(mi.device_count), static_cast<unsigned>(mi.ipc_count));
                LogDefer(false);
                return;
            }
            g_tmem.state = os::TransferMemoryType::State_Created;
            g_tmem.address = nullptr;
            g_tmem.allocated = false;
            os::DestroyTransferMemory(std::addressof(g_tmem));
            g_mapped = false;
            g_addr = nullptr;
            LogLine("donor: memory unmapped%s", tries > 0 ? " (after waiting for nvservices)" : "");
            if (tries > 0) { LogLine("donor: waited %d ms", tries * 50); }
        }
        TerminateDonor();
        g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
        LogDefer(false);
    }

}
