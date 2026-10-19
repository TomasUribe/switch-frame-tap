#include "applet_mitm_donor.hpp"
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
            static constexpr u64 Web[] = { 0x010000000000100Aull, 0x010000000000100Bull, 0x010000000000100Full, 0x0100000000001010ull, 0x0100000000001011ull };
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
        /* v0.7.14: the smallest donor that streams 720p (capture region
         * >= ~22 MB plus the engine buffers) and the usual one */
        constexpr u32 MinDonorMb = 30, FullDonorMb = 42;
        constinit std::atomic<u32> g_cap_mb{FullDonorMb};

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

        /* Every 100 ms while the donor's memory is mapped: the Applet group
         * past idle plus the donor means an applet started. Test16's browser
         * went 96 -> 288 MB in its first 250 ms sample and asked for the rest
         * ~0.9 s later. */
        void AppletWatchThread(void *) {
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
                if (g_applet_needs_memory.load(std::memory_order_relaxed)) {
                    /* the teardown never reached DonorRelease: write the log anyway */
                    if (armTicksToNs(armGetSystemTick()) - g_trigger_ns > 3'000'000'000ull) { LogDefer(false); }
                    continue;
                }
                if (!g_mapped) { continue; }
                u64 cur = 0, lim = 0;
                if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { continue; }
                const u64 mb = cur >> 20, donor_mb = g_size >> 20;
                /* the pool grew 32 MB since the last look: is it a web applet? */
                static u64 checked_mb = 0;
                if (mb < checked_mb) { checked_mb = mb; }
                const bool tight = (lim >> 20) - mb < TightMb;
                bool web = false;
                if (mb >= checked_mb + 32 || tight) { checked_mb = mb; web = WebAppletRunning(); }
                const bool trigger = web || tight;
                if (trigger) {
                    LogDefer(true);   /* written once the memory is back (DonorRelease) */
                    g_trigger_ns = armTicksToNs(armGetSystemTick());
                    g_applet_needs_memory.store(true, std::memory_order_relaxed);
                    LogLine("donor: an applet is starting (Applet group %llu MB, %llu of them ours) - stopping the stream to give the memory back",
                            static_cast<unsigned long long>(mb), static_cast<unsigned long long>(donor_mb));
                }
            }
        }
    }

    namespace {
        constinit std::atomic<u64> g_hold_until_ns{0};
        constinit u32 g_home_hold_ms_local = 2500;
    }

    bool AppletHoldActive() { return armTicksToNs(armGetSystemTick()) < g_hold_until_ns.load(std::memory_order_relaxed); }

    void SetHomeHoldMs(u32 ms) {
        if (ms < 500) { ms = 500; }
        if (ms > 15000) { ms = 15000; }
        g_home_hold_ms_local = ms;
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
         * again immediately and the first Album launch OOMs.
         * Default 2.5 s (was 5 s) - long enough for Album settle, short enough
         * that FT-off HOME stays snappy. */
        const u64 hold_ns = static_cast<u64>(g_home_hold_ms_local) * UINT64_C(1000000);
        g_hold_until_ns.store(t0 + hold_ns, std::memory_order_relaxed);
        g_trigger_ns = t0;
        g_applet_needs_memory.store(true, std::memory_order_relaxed);
        LogLine("home: detach-on-home - release capture memory for home menu (no engine re-setup for %u ms)",
                g_home_hold_ms_local);
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
        if (AppletHoldActive()) {
            if (out_reason != nullptr) { *out_reason = "applet hold active"; }
            return true;
        }
        u64 mb = 0;
        if (AppletBusy(std::addressof(mb))) {
            if (out_reason != nullptr) { *out_reason = "applet pool busy"; }
            return true;
        }
        /* Stock Album / SwitchU replace qlaunch as 010000000000100D */
        {
            static constexpr u64 AlbumProgramId = 0x010000000000100Dull;
            os::ProcessId album{};
            if (R_SUCCEEDED(::ams::pm::dmnt::GetProcessId(std::addressof(album), ncm::ProgramId{AlbumProgramId}))) {
                if (out_reason != nullptr) { *out_reason = "Album/SwitchU 100D up"; }
                return true;
            }
        }
        /* Stock HOME with no Album yet / HOME shm dead: wait out load gaps */
        if (stall_age_ms >= fallback_ms) {
            if (out_reason != nullptr) { *out_reason = "present stall fallback"; }
            return true;
        }
        return false;
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

    bool AppletBusy(u64 *out_mb) {
        /* idle (home menu and friends) ~96 MB; the browser 290-494 MB of 500.
         * Launching the donor then took 42 MB from under it (test15: a
         * screenshot inside a manual). v0.7.14: busy only with a web applet
         * open or no room for the smallest donor and the margin */
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return false; }
        const u64 mb = cur >> 20;
        if (out_mb != nullptr) { *out_mb = mb; }
        return WebAppletRunning() || (lim >> 20) - mb < MinDonorMb + TightMb;
    }

    bool DonorObtain(uintptr_t *out_addr, size_t *out_size) {
        std::scoped_lock lk(g_lock);
        if (!g_mapped) {
            g_state.store(0);
            /* v0.7.14: beside a big applet (System Settings: the pool at 413
             * of 500 MB without us) the HOME menu streams on a smaller donor,
             * leaving the margin free (test5: 41 MB left 46 - the stream stopped) */
            u32 cap = FullDonorMb;
            {
                const u64 free_mb = AppletFreeMb();
                const u64 room = free_mb > TightMb ? free_mb - TightMb : 0;
                cap = room >= FullDonorMb ? FullDonorMb : (room >= MinDonorMb ? static_cast<u32>(room) : MinDonorMb);
                if (cap < FullDonorMb) { LogLine("donor: the Applet pool has %llu MB free - taking at most %u MB", static_cast<unsigned long long>(free_mb), cap); }
            }
            g_cap_mb.store(cap, std::memory_order_relaxed);
            {
                SmUse sm_pm;
                ::Result rc = ::pmshellInitialize();
                if (R_FAILED(rc)) { LogLine("donor: pm:shell rc=0x%x", rc); return false; }
                const NcmProgramLocation loc = { .program_id = DonorProgramId, .storageID = NcmStorageId_None, .pad = {} };
                u64 pid = 0;
                rc = ::pmshellLaunchProgram(0, std::addressof(loc), std::addressof(pid));
                LogLine("donor: launch rc=0x%x pid %llu", rc, static_cast<unsigned long long>(pid));
                if (R_FAILED(rc)) { ::pmshellExit(); return false; }
                g_pmshell_open = true;   /* kept for TerminateDonor */
                g_pid = pid;
            }
            for (int i = 0; i < 500 && g_state.load() == 0; ++i) { os::SleepThread(TimeSpan::FromMilliSeconds(10)); }
            if (g_state.load() != 1) {
                LogLine("donor: no memory (%s, rc=0x%x)", g_state.load() == 2 ? "the Applet pool refused every size" : "no answer in 5 s", g_fail_rc);
                TerminateDonor();
                return false;
            }
            os::AttachTransferMemory(std::addressof(g_tmem), g_size, g_handle, true);
            g_handle = os::InvalidNativeHandle;
            if (const Result rc = os::MapTransferMemory(std::addressof(g_addr), std::addressof(g_tmem), os::MemoryPermission_None); R_FAILED(rc)) {
                LogLine("donor: map %llu MB rc=0x%x", static_cast<unsigned long long>(g_size >> 20), rc.GetValue());
                os::DestroyTransferMemory(std::addressof(g_tmem));
                TerminateDonor();
                return false;
            }
            g_mapped = true;
            LogLine("donor: %llu MB from the Applet pool mapped at %p", static_cast<unsigned long long>(g_size >> 20), g_addr);
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
        LogDefer(false);
    }

}
