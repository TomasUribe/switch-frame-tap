#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    /* ---- IWindowController (not reached this iteration) ---------------- */

    Result WindowControllerMitm::GetAppletResourceUserId(sf::Out<u64> out_aruid) {
        AMS_UNUSED(out_aruid);
        LogLine("GetAppletResourceUserId ENTER program=%016llx",
                static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* ---- IApplicationProxy (not reached this iteration) --------------- */

    Result ApplicationProxyMitm::GetWindowController(sf::Out<sf::SharedPointer<IWindowControllerMitm>> out) {
        AMS_UNUSED(out);
        LogLine("GetWindowController ENTER program=%016llx",
                static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* ---- appletOE root ---------------------------------------------- */
    /* M2 diag: prove the interception + raw-replay forward works and games
     * still launch. No manual forward, no wrapping yet. */

    Result AppletMitmService::OpenApplicationProxy(sf::Out<sf::SharedPointer<IApplicationProxyMitm>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle) {
        AMS_UNUSED(out, reserved);
        LogLine("OpenApplicationProxy ENTER: program=%016llx pid=%llu",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                static_cast<unsigned long long>(client_pid.GetValue().value));
        /* Let libstratosphere replay the original message (pid + handle intact)
         * to the real appletOE and return its IApplicationProxy to the game. */
        AMS_UNUSED(process_handle);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

}
