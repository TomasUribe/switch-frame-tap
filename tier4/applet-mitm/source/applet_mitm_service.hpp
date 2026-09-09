/*
 * applet-mitm - Track B / M1 (v2)
 *
 * Passive mitm of "appletOE". We intercept NOTHING: an empty command list means
 * every call (including OpenApplicationProxy, which carries the game's process
 * handle) forwards at the raw HIPC level with handles intact. Observation is
 * done in ShouldMitm, which sm calls once per connecting client.
 *
 * M1 v1 tried to intercept OpenApplicationProxy and forward with
 * ResultShouldForwardToSession(); that consumes the copy-handle parameter, so
 * the replayed request reached am with no process handle and games failed to
 * launch. M2 will forward it manually instead.
 */
#pragma once
#include <stratosphere.hpp>

/* No intercepted commands - pure transparent passthrough. */
#define AMS_APPLET_MITM_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAppletMitmInterface, AMS_APPLET_MITM_INTERFACE_INFO, 0x11ABE701)

namespace ams::mitm::applet {

    class AppletMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info);
    };
    static_assert(IsIAppletMitmInterface<AppletMitmService>);

}
