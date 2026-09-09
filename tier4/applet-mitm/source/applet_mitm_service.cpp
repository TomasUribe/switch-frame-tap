#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    bool AppletMitmService::ShouldMitm(const sm::MitmProcessInfo &client_info) {
        const bool is_app = ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
        if (is_app) {
            LogLine("appletOE client: program=%016llx  (application)",
                    static_cast<unsigned long long>(client_info.program_id.value));
        }
        /* Returning false = we don't mitm this client at all, cleanest possible.
         * M1 only needs the observation above; flip to `is_app` in M2 when we
         * actually wrap the proxy chain. */
        return false;
    }

}
