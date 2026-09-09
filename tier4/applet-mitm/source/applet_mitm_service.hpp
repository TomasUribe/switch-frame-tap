/*
 * applet-mitm - Track B / M2 (via vi:u) - step 1: wrap only
 *
 * M2-full aborted (std::abort 0xffe) before our handler -> OpenLayer's
 * signature broke libstratosphere command metadata. This step declares
 * IViDisplaySvcMitm with ZERO commands: just wrap GetDisplayService(0) and let
 * every display-service command auto-forward. Proves the wrap + domain
 * auto-forward hold on vi:u. OpenLayer interception is added next.
 */
#pragma once
#include <stratosphere.hpp>

/* ---- IApplicationDisplayService wrapper: no intercepts yet ------------- */
#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E111)

/* ---- vi:u root wrapper --------------------------------------------- */
#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E112)

namespace ams::mitm::applet {

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
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
