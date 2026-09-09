#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    /* ---- IApplicationDisplayService::OpenLayer (read + forward) --------- */

    Result ViDisplaySvcMitm::OpenLayer(const DisplayName &display_name, u64 layer_id, u64 aruid, const sf::ClientProcessId &client_pid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size) {
        AMS_UNUSED(native_window, out_native_window_size);
        LogLine("*** OpenLayer  program=%016llx  pid=%llu  display=\"%.16s\"  layer_id=%016llx  aruid=%016llx",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                static_cast<unsigned long long>(client_pid.GetValue().value),
                display_name.data,
                static_cast<unsigned long long>(layer_id),
                static_cast<unsigned long long>(aruid));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* ---- vi:u root::GetDisplayService (forward + wrap) ---------------- */

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        LogLine("GetDisplayService ENTER program=%016llx mode=%u",
                static_cast<unsigned long long>(m_client_info.program_id.value), mode);

        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(m_forward_service.get(), 0, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("GetDisplayService fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };
        auto shared_srv = std::make_shared<::Service>(disp_svc);
        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::move(shared_srv), m_client_info), target_object_id);
        LogLine("GetDisplayService: wrapped IApplicationDisplayService (obj_id=%llu)",
                static_cast<unsigned long long>(target_object_id.value));
        R_SUCCEED();
    }

}
