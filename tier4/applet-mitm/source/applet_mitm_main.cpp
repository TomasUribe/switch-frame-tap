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
#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

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

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));

            switch (port_index) {
                case PortIndex_AppletMitm:
                    R_RETURN(this->AcceptMitmImpl(server,
                        sf::CreateSharedObjectEmplaced<mitm::applet::IViRootMitm, mitm::applet::ViRootMitm>(decltype(fsrv)(fsrv), client_info),
                        fsrv));
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
        mitm::applet::LogLine("applet-mitm M8: up. VIC CHANNEL_SUBMIT blit (320x180 crop, one-shot on queueBuffer).");

        R_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");

        g_server_manager.LoopProcess();
    }

}
