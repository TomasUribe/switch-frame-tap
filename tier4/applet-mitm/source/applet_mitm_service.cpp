#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    /* ---- IApplicationDisplayService intercepts ------------------------ */

    /* Manual forward: no pid, no buffers, so serviceDispatchInOut is exact.
     * Tests whether manual forwarding works on the patched sub-session,
     * independent of the raw-replay path. */
    Result ViDisplaySvcMitm::OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id) {
        LogMark("OpenDisplay:enter");
        LogLine("   program=%016llx display=\"%.32s\"",
                static_cast<unsigned long long>(m_client_info.program_id.value), display_name.data);

        u64 display_id = 0;
        const Result rc = serviceDispatchInOut(m_forward_service.get(), 1010, display_name, display_id);
        if (R_SUCCEEDED(rc)) {
            out_display_id.SetValue(display_id);
            LogMark("OpenDisplay:forwarded_ok");
            LogLine("   display_id=%llu", static_cast<unsigned long long>(display_id));
        } else {
            LogMark("OpenDisplay:forward_FAILED");
            LogLine("   rc=0x%x", rc.GetValue());
        }
        R_RETURN(rc);
    }

    /* Must use raw replay: this command carries a kernel-attested PID
     * descriptor, and vi validates the aruid against it. A manual forward would
     * re-attribute the pid to our process and be rejected. */
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

    Result ViDisplaySvcMitm::CreateStrayLayer(u32 layer_flags, u64 display_id, sf::Out<u64> out_layer_id, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size) {
        LogMark("CreateStrayLayer:enter");
        LogLine("*** CreateStrayLayer  program=%016llx  flags=0x%x  display_id=%llu",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                layer_flags, static_cast<unsigned long long>(display_id));
        AMS_UNUSED(out_layer_id, native_window, out_native_window_size);
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
         * session. */
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;

        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetDisplayService:done");
        R_SUCCEED();
    }

}
