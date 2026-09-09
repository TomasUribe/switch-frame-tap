/*
 * applet-mitm - Track B / M2 diagnostic D2
 *
 * Empty command list (zero intercepts) but ShouldMitm() returns TRUE for game
 * clients, so sm actually routes the game's appletOE session through us and
 * every command auto-forwards. Purpose: determine whether mitm'ing appletOE at
 * all is what breaks games, independent of any handler we write.
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_APPLET_MITM_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAppletMitmInterface, AMS_APPLET_MITM_INTERFACE_INFO, 0x2AB1E010)

namespace ams::mitm::applet {

    class AppletMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info);
    };
    static_assert(IsIAppletMitmInterface<AppletMitmService>);

}
