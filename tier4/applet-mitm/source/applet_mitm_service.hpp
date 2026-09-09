/*
 * applet-mitm - Track B / M2 step 3: capture layer_id + aruid
 *
 *   vi:u.GetDisplayService(0)  -> wrap IApplicationDisplayService
 *     .OpenDisplay(1010)  -> log display name          (trivial sig, bisect probe)
 *     .OpenLayer(2020)    -> log layer_id + aruid      (the prize)
 *
 * Everything else auto-forwards, thanks to the libstratosphere patch that gives
 * non-domain mitm sub-objects a forward service (patch_libstrat.py).
 *
 * OpenLayer signature notes (this is what aborted the first attempt):
 *   raw = DisplayName(0x40) + u64 layer_id + u64 aruid = 0x50 bytes.
 *   The command has a PID descriptor, but send_pid lives in the HIPC header,
 *   NOT in raw data - libnx adds no placeholder. libstratosphere's
 *   sf::ClientProcessId *does* consume 8 raw bytes, so declaring it would make
 *   InDataSize 0x58 vs the actual 0x50 and mis-parse. We omit it; the program id
 *   is already in m_client_info.
 *   The out buffer is type-0x6 = MapAlias|Out = sf::OutBuffer.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {
    struct DisplayName { char data[0x40]; };
    static_assert(sizeof(DisplayName) == 0x40);
}

/* ---- IApplicationDisplayService wrapper ------------------------------- */
#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                                                                            \
    AMS_SF_METHOD_INFO(C, H, 1010, Result, OpenDisplay, (const ams::mitm::applet::DisplayName &display_name, sf::Out<u64> out_display_id), (display_name, out_display_id))                                                                                                      \
    AMS_SF_METHOD_INFO(C, H, 2020, Result, OpenLayer,   (const ams::mitm::applet::DisplayName &display_name, u64 layer_id, u64 aruid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size), (display_name, layer_id, aruid, native_window, out_native_window_size))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E121)

/* ---- vi:u root wrapper --------------------------------------------- */
#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E122)

namespace ams::mitm::applet {

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id);
            Result OpenLayer(const DisplayName &display_name, u64 layer_id, u64 aruid, const sf::OutBuffer &native_window, sf::Out<u64> out_native_window_size);
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
