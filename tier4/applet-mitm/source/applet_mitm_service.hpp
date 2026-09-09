/*
 * applet-mitm - Track B / M1
 *
 * Passive mitm of "appletOE" (nn::am::service::IApplicationProxyService).
 * appletOE has a single command, OpenApplicationProxy (cmd 0). For M1 we do NOT
 * wrap the returned IApplicationProxy - we just log that a game opened it and
 * forward the request untouched (sm::mitm::ResultShouldForwardToSession()).
 *
 * M2 will wrap the proxy -> ISelfController / IWindowController to capture the
 * ARUID and the separable recording LayerId.
 */
#pragma once
#include <stratosphere.hpp>

/* Minimal stand-in for nn::am::service::IApplicationProxy. Zero intercepted
 * commands => every call forwards. We only need the type so OpenApplicationProxy
 * has a return type for codegen; M1 forwards before it is ever constructed. */
#define AMS_APPLET_MITM_APP_PROXY_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IApplicationProxyStub, AMS_APPLET_MITM_APP_PROXY_INTERFACE_INFO, 0x11ABE700)

#define AMS_APPLET_MITM_INTERFACE_INFO(C, H)                                                                                     \
    AMS_SF_METHOD_INFO(C, H, 0, Result, OpenApplicationProxy, (sf::Out<sf::SharedPointer<ams::mitm::applet::IApplicationProxyStub>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle), (out, reserved, client_pid, std::move(process_handle)))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAppletMitmInterface, AMS_APPLET_MITM_INTERFACE_INFO, 0x11ABE701)

namespace ams::mitm::applet {

    class AppletMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* Only games, and not HBL running under an application id. */
                return ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
            }
        public:
            Result OpenApplicationProxy(sf::Out<sf::SharedPointer<IApplicationProxyStub>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle);
    };
    static_assert(IsIAppletMitmInterface<AppletMitmService>);

}
