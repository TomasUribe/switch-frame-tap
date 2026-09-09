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
        mitm::applet::LogLine("applet-mitm M21: up. Hunt the game aruid, adopt it with SetAruidWithoutCheck.");

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");

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
