#include "applet_mitm_service.hpp"
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_nv.hpp"
#include <atomic>

namespace ams::mitm::applet {

    namespace {

        /* queueBuffer runs at up to 60 Hz; one open/append/close per line would
         * hammer the SD card. Log the first few of each transaction code, then
         * only a periodic heartbeat. */
        constinit std::atomic<u32> g_txn_total{0};
        constinit std::atomic<u32> g_txn_per_code[16] = {};
        /* the VIC blit fires exactly once, ever - never per-frame */
        constinit std::atomic<bool> g_blit_attempted{false};

        const char *TxnName(u32 code) {
            switch (code) {
                case 1:  return "requestBuffer";
                case 2:  return "setBufferCount";
                case 3:  return "dequeueBuffer";
                case 4:  return "detachBuffer";
                case 5:  return "detachNextBuffer";
                case 6:  return "attachBuffer";
                case 7:  return "queueBuffer";
                case 8:  return "cancelBuffer";
                case 9:  return "query";
                case 10: return "connect";
                case 11: return "disconnect";
                case 12: return "setSidebandStream";
                case 13: return "allocateBuffers";
                case 14: return "setPreallocatedBuffer";
                default: return "?";
            }
        }

    }

    /* ---- IHOSBinderDriver: the frame pipeline -------------------------- */

    Result BinderMitm::TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out) {
        const u32 total = g_txn_total.fetch_add(1) + 1;
        const u32 per   = (code < 16) ? (g_txn_per_code[code].fetch_add(1) + 1) : 0;
        g_stats.txns.store(total, std::memory_order_relaxed);

        /* first 3 of each code, then a heartbeat every 600 transactions */
        if (per <= 3 || (total % 600) == 0) {
            LogLine("*** binder txn #%u  program=%016llx  session=%d  code=%u(%s) x%u  flags=0x%x  in=%zu out=%zu",
                    total,
                    static_cast<unsigned long long>(m_client_info.program_id.value),
                    session_id, code, TxnName(code), per, flags,
                    parcel_in.GetSize(), parcel_out.GetSize());
        }
        if (total == 1) { LogMark("binder:first_txn"); }

        /* SET_PREALLOCATED_BUFFER (14) carries a flattened NvGraphicBuffer in
         * its INPUT parcel - the full description of one frame's memory.
         * Games register one per swapchain slot at startup (MK8: 3 = triple
         * buffered). This is the descriptor we need to import and read pixels. */
        if (code == 14 && per <= 8) {
            LogMark("binder:parse_preallocated");
            if (const auto *gb = FindGraphicBuffer(parcel_in.GetPointer(), parcel_in.GetSize()); gb != nullptr) {
                char tag[48];
                std::snprintf(tag, sizeof(tag), "setPreallocatedBuffer#%u", per);
                LogGraphicBuffer(tag, gb);
                CaptureGameSurface(gb, per);
            } else {
                LogLine("    (no NvGraphicBuffer magic found in %zu-byte parcel)", parcel_in.GetSize());
            }
        }

        /* queueBuffer (7): the frame is now rendered and its swapchain slot is
         * the first int32 of the parcel payload. Once the game has presented a
         * few hundred frames (real content on screen), run the one-shot VIC
         * blit against that slot. */
        if (code == 7 && g_vic_armed && total > 300 && g_game_surface.armed) {
            bool ex = false;
            if (g_blit_attempted.compare_exchange_strong(ex, true)) {
                s32 qslot = 0;
                const auto *p = static_cast<const u8 *>(parcel_in.GetPointer());
                const size_t psz = parcel_in.GetSize();
                if (p != nullptr && psz >= 24) {
                    u32 data_off = 0;
                    std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
                    if (data_off + 4 <= psz) {
                        std::memcpy(std::addressof(qslot), p + data_off, sizeof(qslot));
                    }
                }
                LogMark("binder:vic_blit_trigger");
                LogLine("   queueBuffer txn#%u parcel=%zu slot=%d", total, psz, qslot);
                TryVicBlit(qslot);
            }
        }

        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* ---- IApplicationDisplayService ----------------------------------- */

    Result ViDisplaySvcMitm::GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out) {
        g_stats.relay.fetch_add(1);
        LogMark("GetRelayService:enter");

        ::Service binder_svc = {};
        const Result rc = serviceDispatch(m_forward_service.get(), 100,
            .out_num_objects = 1,
            .out_objects     = std::addressof(binder_svc),
        );
        if (R_FAILED(rc)) {
            LogMark("GetRelayService:forward_FAILED");
            LogLine("   rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        auto shared_srv = std::make_shared<::Service>(binder_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(binder_svc)) };

        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IBinderMitm, BinderMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetRelayService:wrapped");
        R_SUCCEED();
    }

    Result ViDisplaySvcMitm::OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id) {
        LogMark("OpenDisplay:enter");
        u64 display_id = 0;
        const Result rc = serviceDispatchInOut(m_forward_service.get(), 1010, display_name, display_id);
        if (R_SUCCEEDED(rc)) {
            out_display_id.SetValue(display_id);
            LogLine("   OpenDisplay \"%.32s\" -> display_id=%llu",
                    display_name.data, static_cast<unsigned long long>(display_id));
        } else {
            LogMark("OpenDisplay:forward_FAILED");
            LogLine("   rc=0x%x", rc.GetValue());
        }
        R_RETURN(rc);
    }

    /* ---- vi:u root ---------------------------------------------------- */

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        g_stats.getdisp.fetch_add(1);
        LogMark("GetDisplayService:enter");
        LogLine("   program=%016llx mode=%u",
                static_cast<unsigned long long>(m_client_info.program_id.value), mode);

        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(m_forward_service.get(), 0, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        auto shared_srv = std::make_shared<::Service>(disp_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };

        /* tier4 libstratosphere patch: give the wrapper's session a forward
         * service so undeclared commands auto-forward on this NON-domain
         * session. */
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetDisplayService:done");
        R_SUCCEED();
    }

}
