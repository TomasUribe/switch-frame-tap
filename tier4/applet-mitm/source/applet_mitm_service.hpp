/*
 * applet-mitm - Track B / M2 (via vi:u)
 *
 * appletOE is unmitm-able (one-session-only). vi:u tolerates mitm (D3), and
 * IApplicationDisplayService::OpenLayer (cmd 2020) carries BOTH the game's
 * LayerId and its AppletResourceUserId - and returns only a parcel buffer, no
 * sub-object. So:
 *
 *   vi:u.GetDisplayService(0)            -> wrap IApplicationDisplayService
 *     .OpenLayer(2020)  -> log { display_name, layer_id, aruid }, then forward
 *
 * Only GetDisplayService needs wrapping (u32 in, object out, no pid). OpenLayer
 * is read-then-ResultShouldForwardToSession(). Everything else auto-forwards.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {
    struct DisplayName { char data[0x40]; };
    static_assert(sizeof(DisplayName) == 0x40);
}

/* ---- IApplicationDisplayService wrapper (global scope) ------------------ */
#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 2020, Result, OpenLayer, (const ams::mitm::applet::DisplayName &display_name, u64 layer_id, u64 aruid, const sf::ClientProcessId &client_pid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size), (display_name, layer_id, aruid, client_pid, native_window, out_native_window_size))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E101)

/* ---- vi:u root wrapper --------------------------------------------- */
#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E102)

namespace ams::mitm::applet {

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result OpenLayer(const DisplayName &display_name, u64 layer_id, u64 aruid, const sf::ClientProcessId &client_pid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size);
    };
    static_assert(IsIViDisplaySvcMitm<ViDisplaySvcMitm>);

    class ViRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
            }
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode);
    };
    static_assert(IsIViRootMitm<ViRootMitm>);

}
