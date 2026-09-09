/*
 * applet-mitm - Track B / M2 diagnostic D3
 *
 * appletOE mitm is dead (one-session-only service; transparent mitm still
 * breaks game launch). Next candidate: vi:u, which games use for layer setup
 * (OpenLayer carries layer_id + AppletResourceUserId) and which is NOT
 * session-limited. D3: transparent mitm of vi:u for game clients - do games
 * still launch?
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_APPLET_MITM_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAppletMitmInterface, AMS_APPLET_MITM_INTERFACE_INFO, 0x2AB1E020)

namespace ams::mitm::applet {

    class AppletMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info);
    };
    static_assert(IsIAppletMitmInterface<AppletMitmService>);

}
