#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    /* ---- IApplicationDisplayService intercepts ------------------------ */

    Result ViDisplaySvcMitm::OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id) {
        LogMark("OpenDisplay:enter");
        LogLine("   program=%016llx display=\"%.32s\"",
                static_cast<unsigned long long>(m_client_info.program_id.value), display_name.data);
        AMS_UNUSED(out_display_id);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result ViDisplaySvcMitm::OpenLayer(const DisplayName &display_name, u64 layer_id, u64 aruid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size) {
        LogMark("OpenLayer:enter");
        LogLine("*** OpenLayer  program=%016llx  display=\"%.32s\"  layer_id=%016llx  aruid=%016llx",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                display_name.data,
                static_cast<unsigned long long>(layer_id),
                static_cast<unsigned long long>(aruid));
        AMS_UNUSED(native_window, out_native_window_size);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* ---- vi:u root::GetDisplayService (forward + wrap) ---------------- */

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        LogMark("GetDisplayService:enter");
        LogLine("   program=%016llx mode=%u",
                static_cast<unsigned long long>(m_client_info.program_id.value), mode);

        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(m_forward_service.get(), 0, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        auto shared_srv = std::make_shared<::Service>(disp_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };

        /* tier4 libstratosphere patch: give the wrapper's session a forward
         * service so undeclared commands auto-forward on this NON-domain
         * session (upstream would RegisterSession -> IsMitmSession() false ->
         * ForwardRequest aborts). */
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;

        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetDisplayService:done");
        R_SUCCEED();
    }

}
