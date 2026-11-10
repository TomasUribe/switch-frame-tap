#include "applet_mitm_am.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    namespace {

        /* Forwarded objects are closed with the wrapper: a domain object of
         * ours must close its id in am's domain too (libstratosphere: "Mitm
         * objects are required to close their associated domain id"). */
        std::shared_ptr<::Service> MakeForward(const ::Service &s) {
            return std::shared_ptr<::Service>(new ::Service(s), [](::Service *p) { serviceClose(p); delete p; });
        }

        /* Out-objects on a NON-domain session need the tier4 libstratosphere
         * patch to get their forward service (applet_mitm_service.cpp, vi);
         * on a domain the object id does it. Only set the side channel when
         * it will be consumed, so a stale one never reaches a later object. */
        void PendForward(const std::shared_ptr<::Service> &fwd) {
            if (!serviceIsDomain(fwd.get()) && !serviceIsDomainSubservice(fwd.get())) {
                ::ams::sf::impl::g_tier4_pending_mitm_forward = fwd;
            }
        }

        /* Library applets that are the console's browser: they need nearly
         * the whole Applet pool (libnx AppletId) */
        bool IsBrowserApplet(u32 applet_id) {
            switch (applet_id) {
                case 0x13: /* Web */
                case 0x14: /* Shop (eShop) */
                case 0x17: /* OfflineWeb: manuals, news */
                case 0x18: /* LoginShare */
                case 0x19: /* WifiWebAuth */
                case 0x1A: /* MyPage */
                    return true;
                default:
                    return false;
            }
        }

        enum PortIndex {
            PortIndex_AppletOe,
            PortIndex_Count,
        };

        constexpr sm::ServiceName AppletOeServiceName = sm::ServiceName::Encode("appletOE");

        /* am converts its sessions to domains, and a mitm domain must reserve
         * the very object ids am hands out - from am's one table for every
         * client, so they run high: 32 entries aborted the module (a fatal) as
         * the first game converted (test1). The ids are probed at
         * registration; past half of this, am is left alone. The pointer
         * buffer must be at least am's (smaller aborts too). */
        struct AmServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 8;   /* v0.7.13: 4 - two apps' worth of headroom more */
            static constexpr size_t MaxDomainObjects    = 0x200;   /* v0.7.13: 0x1000 - am's ids ran 34-47 over 15 apps (test9) */
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };
        constexpr size_t AmMaxSessions = 8;   /* v0.7.13: was 4, with no room past it (see LoopProcessSafely) */

        class AmServerManager final : public sf::hipc::ServerManager<PortIndex_Count, AmServerOptions, AmMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        AmServerManager g_am_server_manager;

        /* two threads: CreateLibraryApplet may wait ~0.1-1.5 s for the
         * stream to stop, and the game's other am calls must not queue behind it */
        constexpr size_t AmThreads = 2;
        alignas(os::ThreadStackAlignment) constinit u8 g_am_stacks[AmThreads][12_KB];   /* v0.7.13: 16, 4 used (test9) */
        constinit os::ThreadType g_am_threads[AmThreads];
        void AmLoopThread(void *) { LoopProcessSafely(g_am_server_manager, "am"); }

        Result AmServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            AMS_ABORT_UNLESS(port_index == PortIndex_AppletOe);
            /* v0.7.13: one line an app start - a slow leak would show here */
            static u32 s_accepted = 0;
            LogLine("am: appletOE of program %016llx wrapped (%u this boot)", static_cast<unsigned long long>(client_info.program_id.value), ++s_accepted);
            R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<IAmOeMitm, AmOeMitm>(decltype(fsrv)(fsrv), client_info), fsrv));
        }

    }

    Result AmOeMitm::OpenApplicationProxy(sf::Out<sf::SharedPointer<IAmProxyMitm>> out, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process) {
        ::Service proxy = {};
        const u64 reserved = 0;
        R_TRY(serviceMitmDispatchIn(m_forward_service.get(), 0, reserved,
            .in_send_pid     = true,
            .in_num_handles  = 1,
            .in_handles      = { process.GetOsHandle() },
            .out_num_objects = 1,
            .out_objects     = std::addressof(proxy),
            .override_pid    = client_pid.GetValue().value,
        ));
        const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(proxy))};
        LogLine("am: the app's proxy is am's object %u (our table %zu)", static_cast<unsigned>(target.value), AmServerOptions::MaxDomainObjects);   /* v0.7.13: how high am's ids run */
        auto fwd = MakeForward(proxy);
        PendForward(fwd);
        out.SetValue(sf::CreateSharedObjectEmplaced<IAmProxyMitm, AmProxyMitm>(decltype(fwd)(fwd), m_client_info), target);
        R_SUCCEED();
    }

    constinit std::atomic<bool> g_hb_docked{false};

    /* v0.7.18: homebrew (hbl, v0.7.18.1 a forwarder too) with homebrew_docked
     * on gets the docked answers; anything else has its state getter go to am
     * untouched */
    Result AmProxyMitm::GetCommonStateGetter(sf::Out<sf::SharedPointer<IAmStateMitm>> out) {
        if (!IsHomebrewApp(m_client_info) || !g_hb_docked.load(std::memory_order_relaxed)) {
            R_THROW(sm::mitm::ResultShouldForwardToSession());
        }
        ::Service getter = {};
        R_TRY(serviceDispatch(m_forward_service.get(), 0,
            .out_num_objects = 1,
            .out_objects     = std::addressof(getter),
        ));
        const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(getter))};
        auto fwd = MakeForward(getter);
        PendForward(fwd);
        out.SetValue(sf::CreateSharedObjectEmplaced<IAmStateMitm, AmStateMitm>(decltype(fwd)(fwd), m_client_info), target);
        LogLine("am: homebrew in program %016llx is told the console is docked (1920x1080)", static_cast<unsigned long long>(m_client_info.program_id.value));
        R_SUCCEED();
    }

    /* AppletOperationMode_Console, ApmPerformanceMode_Boost, the TV's 1080p */
    Result AmStateMitm::GetOperationMode(sf::Out<u8> out) { out.SetValue(1); R_SUCCEED(); }
    Result AmStateMitm::GetPerformanceMode(sf::Out<u32> out) { out.SetValue(1); R_SUCCEED(); }
    Result AmStateMitm::GetDefaultDisplayResolution(sf::Out<s32> out_w, sf::Out<s32> out_h) { out_w.SetValue(1920); out_h.SetValue(1080); R_SUCCEED(); }

    Result AmProxyMitm::GetLibraryAppletCreator(sf::Out<sf::SharedPointer<IAmCreatorMitm>> out) {
        ::Service creator = {};
        R_TRY(serviceDispatch(m_forward_service.get(), 11,
            .out_num_objects = 1,
            .out_objects     = std::addressof(creator),
        ));
        const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(creator))};
        auto fwd = MakeForward(creator);
        PendForward(fwd);
        out.SetValue(sf::CreateSharedObjectEmplaced<IAmCreatorMitm, AmCreatorMitm>(decltype(fwd)(fwd), m_client_info), target);
        R_SUCCEED();
    }

    Result AmCreatorMitm::CreateLibraryApplet(u32 applet_id, u32 mode) {
        AMS_UNUSED(mode);
        /* the stream stops and the donor's memory is back before am starts it */
        if (IsBrowserApplet(applet_id)) { PrepareForApplet(applet_id); }
        /* the request itself goes to am unchanged (from the saved message) */
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    void StartAmMitm() {
        /* am's pointer buffer must fit in ours, or accepting a session aborts */
        {
            os::NativeHandle h = os::InvalidNativeHandle;
            if (const Result rc = sm::GetServiceHandle(std::addressof(h), AppletOeServiceName); R_FAILED(rc)) {
                LogLine("am: appletOE not reachable rc=0x%x - no browser guard while streaming", rc.GetValue());
                return;
            }
            ::Service s = {};
            serviceCreate(std::addressof(s), h);
            const u16 pbs = s.pointer_buffer_size;
            const Result crc = serviceConvertToDomain(std::addressof(s));
            const u32 probe_id = R_SUCCEEDED(crc) ? s.object_id : 0;
            serviceClose(std::addressof(s));
            LogLine("am: appletOE pointer buffer %#x (ours %#zx); a new domain gets object id %u (convert rc=0x%x, our table %zu)",
                    pbs, AmServerOptions::PointerBufferSize, probe_id, crc.GetValue(), AmServerOptions::MaxDomainObjects);
            if (pbs > AmServerOptions::PointerBufferSize) { LogLine("am: pointer buffer too large - not wrapping appletOE"); return; }
            if (R_FAILED(crc) || probe_id == 0 || probe_id > AmServerOptions::MaxDomainObjects / 2) {
                LogLine("am: am's object ids do not fit our domain table - not wrapping appletOE");
                return;
            }
        }
        if (const Result rc = g_am_server_manager.RegisterMitmServer<AmOeMitm>(PortIndex_AppletOe, AppletOeServiceName); R_FAILED(rc)) {
            LogLine("am: appletOE mitm not registered rc=0x%x", rc.GetValue());
            return;
        }
        const s32 prio = ServerThreadPriority;
        for (size_t i = 0; i < AmThreads; ++i) {
            if (R_FAILED(os::CreateThread(std::addressof(g_am_threads[i]), AmLoopThread, nullptr, g_am_stacks[i], sizeof(g_am_stacks[i]), prio))) {
                LogLine("am: server thread %zu not created", i);
                continue;
            }
            os::SetThreadNamePointer(std::addressof(g_am_threads[i]), "applet-mitm.Am");
            os::StartThread(std::addressof(g_am_threads[i]));
            NoteThread(std::addressof(g_am_threads[i]));
        }
        LogLine("am: appletOE wrapped - a browser applet stops the stream before it starts");
    }

}
