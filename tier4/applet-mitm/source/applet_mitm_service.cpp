#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    Result AppletMitmService::OpenApplicationProxy(sf::Out<sf::SharedPointer<IApplicationProxyStub>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle) {
        AMS_UNUSED(out, reserved, process_handle);

        LogLine("OpenApplicationProxy: program=%016llx  pid=%llu",
                static_cast<unsigned long long>(m_client_info.program_id.value),
                static_cast<unsigned long long>(client_pid.GetValue().value));

        /* M1: forward untouched. libstratosphere replays the request to the real
         * appletOE and returns its IApplicationProxy handle to the client. */
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

}
