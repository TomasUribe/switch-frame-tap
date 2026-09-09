/*
 * applet-mitm - Track B / M2 (ARUID-only)
 *
 *   appletOE.OpenApplicationProxy(0)         -> wrap IApplicationProxy
 *     IApplicationProxy.GetWindowController(2) -> wrap IWindowController
 *       IWindowController.GetAppletResourceUserId(1) -> log the u64
 *
 * Everything not listed is auto-forwarded by libstratosphere's domain mitm
 * path. Each interception hand-forwards and wraps via the fs_mitm SetValue
 * pattern:  out.SetValue(wrapper, DomainObjectId{ serviceGetObjectId(&fwd) }).
 */
#pragma once
#include <stratosphere.hpp>

/* ---- IWindowController wrapper (define at global scope) ------------------ */
#define AMS_AM_WINDOWCONTROLLER_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetAppletResourceUserId, (sf::Out<u64> out_aruid), (out_aruid))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IWindowControllerMitm, AMS_AM_WINDOWCONTROLLER_MITM_INTERFACE_INFO, 0x2AB1E001)

/* ---- IApplicationProxy wrapper ---------------------------------------- */
#define AMS_AM_APPPROXY_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetWindowController, (sf::Out<sf::SharedPointer<ams::mitm::applet::IWindowControllerMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IApplicationProxyMitm, AMS_AM_APPPROXY_MITM_INTERFACE_INFO, 0x2AB1E002)

/* ---- appletOE root wrapper ------------------------------------------- */
#define AMS_APPLETOE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, OpenApplicationProxy, (sf::Out<sf::SharedPointer<ams::mitm::applet::IApplicationProxyMitm>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle), (out, reserved, client_pid, std::move(process_handle)))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAppletMitmInterface, AMS_APPLETOE_MITM_INTERFACE_INFO, 0x2AB1E003)

namespace ams::mitm::applet {

    class WindowControllerMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetAppletResourceUserId(sf::Out<u64> out_aruid);
    };
    static_assert(IsIWindowControllerMitm<WindowControllerMitm>);

    class ApplicationProxyMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetWindowController(sf::Out<sf::SharedPointer<IWindowControllerMitm>> out);
    };
    static_assert(IsIApplicationProxyMitm<ApplicationProxyMitm>);

    class AppletMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
            }
        public:
            Result OpenApplicationProxy(sf::Out<sf::SharedPointer<IApplicationProxyMitm>> out, u64 reserved, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process_handle);
    };
    static_assert(IsIAppletMitmInterface<AppletMitmService>);

}
