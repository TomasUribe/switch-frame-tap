#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        LogMark("GetDisplayService:enter");
        LogLine("   program=%016llx mode=%u",
                static_cast<unsigned long long>(m_client_info.program_id.value), mode);

        LogMark("GetDisplayService:forwarding");
        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(m_forward_service.get(), 0, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        LogMark("GetDisplayService:wrapping");
        auto shared_srv = std::make_shared<::Service>(disp_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };

        /* tier4 libstratosphere patch: hand the wrapper's session its forward
         * service, so undeclared IApplicationDisplayService commands auto-forward
         * even though this is a NON-domain session (upstream registers such
         * sub-objects with plain RegisterSession -> IsMitmSession() false ->
         * ForwardRequest aborts). */
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;

        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetDisplayService:done");
        LogLine("   wrapped obj_id=%llu, forward armed - undeclared cmds should now forward",
                static_cast<unsigned long long>(target_object_id.value));
        R_SUCCEED();
    }

}
