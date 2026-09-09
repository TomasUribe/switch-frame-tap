#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

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
        LogLine("GetDisplayService: wrapped (obj_id=%llu) - undeclared cmds should auto-forward",
                static_cast<unsigned long long>(target_object_id.value));
        R_SUCCEED();
    }

}
