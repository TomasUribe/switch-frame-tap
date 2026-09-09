/*
 * applet-mitm - Track B / M2 step 4: discriminate forward mechanism vs signature
 *
 * Step 3 result: OpenDisplay:enter fired and parsed correctly, then the game
 * failed to launch and OpenLayer never fired. Two candidate causes:
 *   (a) ResultShouldForwardToSession() (raw replay) is broken on our patched
 *       non-domain sub-session, so OpenDisplay's own forward failed; or
 *   (b) OpenLayer's dispatch rejects the message (signature), so the game got
 *       an error creating its layer.
 *
 * This build separates them:
 *   OpenDisplay(1010)     -> forwarded MANUALLY (serviceDispatchInOut). No pid,
 *                            no buffers, so a manual forward is exact.
 *   OpenLayer(2020)       -> log, then replay-forward. Must be replay: the
 *                            command carries a kernel-attested PID descriptor
 *                            that a manual forward would re-attribute to us.
 *   CreateStrayLayer(2030)-> log, then replay-forward (games with aruid==0 use
 *                            this instead of OpenLayer).
 *
 * Read of the outcome:
 *   OpenDisplay + OpenLayer both log, game runs  -> done, we have the aruid.
 *   OpenDisplay logs, game runs on, OpenLayer logs but game fails -> replay
 *                                                 forward is the broken part.
 *   OpenDisplay logs, game fails, no OpenLayer   -> OpenLayer signature.
 *
 * Signature notes:
 *   OpenLayer raw = DisplayName(0x40) + u64 layer_id + u64 aruid = 0x50.
 *   libstratosphere sorts raw args by alignment ASCENDING (verified in
 *   RawDataOffsetCalculator), so align-1 DisplayName lands at offset 0 and the
 *   u64s at 0x40/0x48 - matching libnx's wire struct exactly.
 *   sf::ClientProcessId is ArgumentType::InData (needs a raw u64 placeholder)
 *   which this command does NOT have, so it is deliberately omitted.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {
    struct DisplayName { char data[0x40]; };
    static_assert(sizeof(DisplayName) == 0x40);
}

/* ---- IApplicationDisplayService wrapper ------------------------------- */
#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                                                                                       \
    AMS_SF_METHOD_INFO(C, H, 1010, Result, OpenDisplay,      (const ams::mitm::applet::DisplayName &display_name, sf::Out<u64> out_display_id), (display_name, out_display_id))                                                                                                            \
    AMS_SF_METHOD_INFO(C, H, 2020, Result, OpenLayer,        (const ams::mitm::applet::DisplayName &display_name, u64 layer_id, u64 aruid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size), (display_name, layer_id, aruid, native_window, out_native_window_size)) \
    AMS_SF_METHOD_INFO(C, H, 2030, Result, CreateStrayLayer, (u32 layer_flags, u64 display_id, sf::Out<u64> out_layer_id, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size), (layer_flags, display_id, out_layer_id, native_window, out_native_window_size))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E131)

/* ---- vi:u root wrapper --------------------------------------------- */
#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E132)

namespace ams::mitm::applet {

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id);
            Result OpenLayer(const DisplayName &display_name, u64 layer_id, u64 aruid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size);
            Result CreateStrayLayer(u32 layer_flags, u64 display_id, sf::Out<u64> out_layer_id, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size);
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
