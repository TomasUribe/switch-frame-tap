/*
 * applet-mitm - Track B / M3: tap the game's frame pipeline (the binder)
 *
 * Chain:
 *   vi:u.GetDisplayService(0)          -> wrap IApplicationDisplayService
 *     .OpenDisplay(1010)               -> log (manual forward; proves liveness)
 *     .GetRelayService(100)            -> wrap IHOSBinderDriver
 *       .TransactParcelAuto(3)         -> LOG every IGraphicBufferProducer txn
 *
 * Why the binder and not OpenLayer:
 *   OpenLayer was only a route to aruid+layer_id. But every frame the game
 *   presents goes through the binder as an IGraphicBufferProducer transaction
 *   (dequeueBuffer/queueBuffer), and requestBuffer replies carry the
 *   GraphicBuffer descriptor (nvmap handle, stride, format). That IS the frame
 *   data path. It also sidesteps OpenLayer's PID-descriptor problem entirely:
 *   TransactParcelAuto has no pid.
 *
 * OpenLayer/CreateStrayLayer are deliberately NOT declared - declaring
 * OpenLayer is the one thing that made games fail to launch (its dispatch
 * rejects; libstratosphere's sf::ClientProcessId needs a raw u64 placeholder
 * that this command does not have, and there is no way to express
 * "send_pid without placeholder").
 *
 * ABI notes (verified against libnx):
 *   GetRelayService: cmd 100, no input, one out object.
 *   TransactParcel:  fw >= 3.0.0 uses cmd 3 (Auto) with AutoSelect buffers;
 *                    cmd 0 (MapAlias) is the pre-3.0.0 form. We are on 22.5.0,
 *                    so we declare cmd 3 only.
 *                    raw in = { s32 session_id, u32 code, u32 flags } = 12B.
 *   IGraphicBufferProducer codes: 1=requestBuffer 2=setBufferCount
 *     3=dequeueBuffer 4=detachBuffer 5=detachNextBuffer 6=attachBuffer
 *     7=queueBuffer 8=cancelBuffer 9=query 10=connect 11=disconnect
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {
    struct DisplayName { char data[0x40]; };
    static_assert(sizeof(DisplayName) == 0x40);
}

/* ---- IHOSBinderDriver wrapper (innermost - declare first) -------------- */
#define AMS_VI_BINDER_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (session_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IBinderMitm, AMS_VI_BINDER_MITM_INTERFACE_INFO, 0x2AB1E141)

/* ---- IApplicationDisplayService wrapper ------------------------------- */
#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H)                                                                                                                            \
    AMS_SF_METHOD_INFO(C, H, 100,  Result, GetRelayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IBinderMitm>> out), (out))                                            \
    AMS_SF_METHOD_INFO(C, H, 1010, Result, OpenDisplay,     (const ams::mitm::applet::DisplayName &display_name, sf::Out<u64> out_display_id), (display_name, out_display_id))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E142)

/* ---- vi:u root wrapper --------------------------------------------- */
/* M87: command 1, GetDisplayServiceWithProxyNameExchange, is how some games
 * (BOTW in Run M) open the display service; left undeclared it auto-forwards
 * and the game's binder never passes through us. Its arguments differ between
 * sources (switchbrew: a u32; SwIPC's vi:s/vi:m variants: 8-byte ProxyName +
 * u32), so it is declared with NO typed input: the handler copies the game's
 * raw request and forwards those bytes unchanged. */
#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E143)

namespace ams::mitm::applet {

    class BinderMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out);
    };
    static_assert(IsIBinderMitm<BinderMitm>);

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out);
            Result OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id);
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
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out);
        private:
            void Wrap(::Service disp_svc, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out);
    };
    static_assert(IsIViRootMitm<ViRootMitm>);

}
