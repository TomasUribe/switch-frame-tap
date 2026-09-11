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
#include <cstring>
#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"

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

        constexpr size_t MallocBufferSize = 1_MB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        enum PortIndex {
            PortIndex_AppletMitm,
            PortIndex_Count,
        };

        constexpr sm::ServiceName AppletMitmServiceName = sm::ServiceName::Encode("vi:u");

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        constexpr size_t MaxSessions = 8;

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_server_manager;

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
        alignas(os::ThreadStackAlignment) constinit u8 g_hb_stack[16_KB];
        constinit os::ThreadType g_hb_thread;

        void HeartbeatThread(void *) {
            for (u32 i = 1; ; i++) {
                os::SleepThread(TimeSpan::FromSeconds(3));
                char b[96];
                const auto &st = mitm::applet::g_stats;
                std::snprintf(b, sizeof(b), "hb:%u sess=%u getdisp=%u relay=%u txn=%u vic=%s",
                              i, st.sessions.load(), st.getdisp.load(),
                              st.relay.load(), st.txns.load(),
                              mitm::applet::g_vic_stage.load(std::memory_order_relaxed));
                mitm::applet::LogMark(b);
            }
        }

        /* ---- opt-in arm file ----------------------------------------------
         * Nothing touches nvdrv/VIC unless sdmc:/applet-mitm.armed exists and
         * contains "vic". Default is a pure observer, i.e. M7d behaviour. */
        bool ArmFileContains(const char *keyword) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/applet-mitm.armed", fs::OpenMode_Read))) {
                return false;
            }
            char buf[256] = {};
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return false; }
            size_t n = static_cast<size_t>(fsz);
            if (n > sizeof(buf) - 1) { n = sizeof(buf) - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return false; }
            buf[n] = '\0';
            return std::strstr(buf, keyword) != nullptr;
        }

        /* Numeric option out of the arm file: "wait=180" delays the probe so
         * there is time to actually get into a race before it fires. M39 probed
         * at ~50 s, which is still the title screen. */
        u32 ArmFileNumber(const char *key, u32 def) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/applet-mitm.armed", fs::OpenMode_Read))) {
                return def;
            }
            char buf[256] = {};
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return def; }
            size_t n = static_cast<size_t>(fsz);
            if (n > sizeof(buf) - 1) { n = sizeof(buf) - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return def; }
            buf[n] = '\0';

            const char *p = std::strstr(buf, key);
            if (p == nullptr) { return def; }
            p += std::strlen(key);
            while (*p == '=' || *p == ' ') { ++p; }
            u32 v = 0;
            bool any = false;
            while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<u32>(*p - '0'); ++p; any = true; }
            return any ? v : def;
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
            mitm::applet::LogLine("   THIS PROCESS (pool_partition 2, at boot): total=%llu KB used=%llu KB free=%lld KB",
                                  static_cast<unsigned long long>(ptot / 1024),
                                  static_cast<unsigned long long>(pused / 1024),
                                  static_cast<long long>((static_cast<s64>(ptot) - static_cast<s64>(pused)) / 1024));
            mitm::applet::LogLine("   for scale: one 1080p frame = 7913 KB, one block-row strip = 960 KB");
        }

        /* M49 answered it: SetMemoryHeapSize(8 MB) succeeds at BOOT, first try,
         * leaving ~3.2 MB spare of a 13,076 KB process. The 2 MB ceiling was
         * never a hard limit - it was an artefact of asking LATE, once a game
         * was resident and the transfer memory committed. So the fix is timing,
         * not pool_partition: take the heap at startup and hold it.
         *
         * A whole 1080p frame is 7913 KB and now fits, which makes the
         * strip-wise design a choice rather than a constraint and gives NVENC
         * room for an input surface and a bitstream buffer.
         *
         * THE RISK THIS RUN RETIRES: M49 grabbed 8 MB and released it
         * immediately. HOLDING it through a game launch is a different
         * proposition - the System pool has only 14 MB free in total, and M27
         * proved that starving it fatals a DIFFERENT sysmodule with
         * LimitReached. If the console fails to launch MK8 after this, that is
         * the cause, and recovery is deleting
         * atmosphere/contents/0100000000000C20 or booting with Volume Up. */
        void GrabHeapAtBoot() {
            if (!mitm::applet::g_vic_armed) {
                mitm::applet::LogLine("   [boot] not armed - heap not taken");
                return;
            }
            u64 t0 = 0, u0 = 0;
            svc::GetInfo(std::addressof(t0), svc::InfoType_TotalMemorySize, svc::PseudoHandle::CurrentProcess, 0);
            svc::GetInfo(std::addressof(u0), svc::InfoType_UsedMemorySize,  svc::PseudoHandle::CurrentProcess, 0);
            mitm::applet::LogLine("   [boot] before grab: total=%llu KB used=%llu KB",
                                  static_cast<unsigned long long>(t0 / 1024),
                                  static_cast<unsigned long long>(u0 / 1024));

            const bool ok = mitm::applet::AllocVicHeapAtBoot();

            u64 t1 = 0, u1 = 0, st = 0, su = 0;
            svc::GetInfo(std::addressof(t1), svc::InfoType_TotalMemorySize, svc::PseudoHandle::CurrentProcess, 0);
            svc::GetInfo(std::addressof(u1), svc::InfoType_UsedMemorySize,  svc::PseudoHandle::CurrentProcess, 0);
            svc::GetSystemInfo(std::addressof(st), svc::SystemInfoType_TotalPhysicalMemorySize, svc::InvalidHandle, 2);
            svc::GetSystemInfo(std::addressof(su), svc::SystemInfoType_UsedPhysicalMemorySize,  svc::InvalidHandle, 2);
            mitm::applet::LogLine("   [boot] after grab : total=%llu KB used=%llu KB  -> %s",
                                  static_cast<unsigned long long>(t1 / 1024),
                                  static_cast<unsigned long long>(u1 / 1024),
                                  ok ? "*** HEAP HELD FROM BOOT ***" : "FAILED - probe will retry late");
            mitm::applet::LogLine("   [boot] System pool now: used=%llu KB free=%lld KB  (we took %llu KB of it)",
                                  static_cast<unsigned long long>(su / 1024),
                                  static_cast<long long>((static_cast<s64>(st) - static_cast<s64>(su)) / 1024),
                                  static_cast<unsigned long long>((u1 - u0) / 1024));
            mitm::applet::LogLine("   [boot] a 1080p frame is 7913 KB - %s",
                                  ok ? "a WHOLE FRAME now fits in one piece" : "still strip-wise only");
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
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

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

            ams::CheckApiVersion();
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void Main() {
        os::SetThreadNamePointer(os::GetCurrentThread(), "applet-mitm.Main");

        mitm::applet::LogInit();
        mitm::applet::LogLine("applet-mitm M50: up. 8 MB heap taken at boot - a whole 1080p frame now fits.");

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::g_dbg_armed   = ArmFileContains("dbg");
        mitm::applet::g_dump_armed  = ArmFileContains("dump");
        mitm::applet::g_probe_delay_s = ArmFileNumber("wait", 120);
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
        LogMemoryPools();
        GrabHeapAtBoot();

        mitm::applet::LogLine("probe fires at t=%u s; frame dump to SD: %s",
                              mitm::applet::g_probe_delay_s,
                              mitm::applet::g_dump_armed ? "ARMED (freezes the game ~0.3-2.5 s)" : "off");
        mitm::applet::LogLine("debug-capture route: %s   pmdmntInitialize rc=0x%x",
                              mitm::applet::g_dbg_armed ? "ARMED" : "off (add \"dbg\" to the arm file)",
                              mitm::applet::g_pmdmnt_rc);

        /* start the heartbeat before anything that can block */
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_hb_thread), HeartbeatThread, nullptr,
                                        g_hb_stack, sizeof(g_hb_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_hb_thread), "applet-mitm.HB");
        os::StartThread(std::addressof(g_hb_thread));
        mitm::applet::LogMark("main:heartbeat_started");

        mitm::applet::StartVicWorker();

        R_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");

        mitm::applet::LogMark("main:LoopProcess");
        g_server_manager.LoopProcess();
        mitm::applet::LogMark("main:LoopProcess_RETURNED");
    }

}
