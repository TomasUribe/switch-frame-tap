#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    namespace {

        template<typename Wrapper, typename Out>
        void WrapForwarded(Out &out, ::Service &fwd, const sm::MitmProcessInfo &info) {
            const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(fwd)) };
            auto shared_srv = std::make_shared<::Service>(fwd);
            out.SetValue(sf::CreateSharedObjectEmplaced<typename Out::Interface, Wrapper>(std::move(shared_srv), info), target_object_id);
        }

    }

    /* ---- IWindowController ---------------------------------------------- */

    Result WindowControllerMitm::GetAppletResourceUserId(sf::Out<u64> out_aruid) {
        u64 aruid = 0;
        const Result rc = serviceDispatchOut(m_forward_service.get(), 1, aruid);
        if (R_SUCCEEDED(rc)) {
            out_aruid.SetValue(aruid);
            LogLine("*** ARUID  program=%016llx  aruid=%016llx",
                    static_cast<unsigned long long>(m_client_info.program_id.value),
                    static_cast<unsigned long long>(aruid));
        } else {
            LogLine("GetAppletResourceUserId fwd rc=0x%x", rc.GetValue());
        }
        R_RETURN(rc);
    }

    /* ---- IApplicationProxy -------------------------------------------- */

    Result ApplicationProxyMitm::GetWindowController(sf::Out<sf::SharedPointer<IWindowControllerMitm>> out) {
        ::Service wc = {};
        const Result rc = serviceDispatch(m_forward_service.get(), 2,
            .out_num_objects = 1,
            .out_objects     = std::addressof(wc),
        );
        if (R_FAILED(rc)) {
            LogLine("GetWindowController fwd rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }
        LogLine("GetWindowController: program=%016llx -> wrapped",
                static_cast<unsigned long long>(m_client_info.program_id.value));
        WrapForwarded<WindowControllerMitm>(out, wc, m_client_info);
        R_SUCCEED();
    }

    /* ---- appletOE root ---------------------------------------------- */

    Result AppletMitmService::OpenApplicationProxy(sf::Out<sf::SharedPointer<IApplicationProxyMitm>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle) {
        ::Service app_proxy = {};
        const u64 in = reserved;
        const Result rc = serviceDispatchIn(m_forward_service.get(), 0, in,
            .in_send_pid     = true,
            .in_num_handles  = 1,
            .in_handles      = { process_handle.GetOsHandle() },
            .out_num_objects = 1,
            .out_objects     = std::addressof(app_proxy),
        );
        if (R_FAILED(rc)) {
            LogLine("OpenApplicationProxy fwd FAILED rc=0x%x program=%016llx",
                    rc.GetValue(), static_cast<unsigned long long>(m_client_info.program_id.value));
            R_RETURN(rc);
        }
        LogLine("OpenApplicationProxy: program=%016llx pid=%llu -> wrapped",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                static_cast<unsigned long long>(client_pid.GetValue().value));
        WrapForwarded<ApplicationProxyMitm>(out, app_proxy, m_client_info);
        R_SUCCEED();
    }

}
