#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    bool AppletMitmService::ShouldMitm(const sm::MitmProcessInfo &client_info) {
        const bool is_app = ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
        if (is_app) {
            LogLine("D2: MITM appletOE for program=%016llx (empty iface, all forward)",
                    static_cast<unsigned long long>(client_info.program_id.value));
        }
        return is_app;
    }

}
