#include "applet_mitm_service.hpp"
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_control.hpp"
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

        /* queueBuffer's parcel begins with writeInterfaceToken, not with the
         * arguments: [u32 strict_mode_policy][u32 len][UTF-16 name, len+1 units,
         * padded to 4]. Reading the first word gave 0x100 - that is
         * STRICT_MODE_PENALTY_GATHER, which is why the slot came back as 256.
         * For "android.gui.IGraphicBufferProducer" (34 units) the slot lands at
         * payload+80, i.e. parcel offset 96. Parsed rather than hardcoded. */
        s32 ParseQueueBufferSlot(const u8 *p, size_t sz) {
            if (p == nullptr || sz < 24) { return -1; }
            u32 data_off = 0;
            std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
            if (static_cast<size_t>(data_off) + 8 > sz) { return -1; }

            u32 len = 0;
            std::memcpy(std::addressof(len), p + data_off + 4, sizeof(len));
            if (len == 0 || len > 128) { return -1; }

            size_t off = static_cast<size_t>(data_off) + 8 + (static_cast<size_t>(len) + 1) * 2;
            off = (off + 3) & ~static_cast<size_t>(3);
            if (off + 4 > sz) { return -1; }

            s32 slot = 0;
            std::memcpy(std::addressof(slot), p + off, sizeof(slot));
            return slot;
        }

        /* M88: queueBuffer's input after the slot is a flattened
         * BqBufferInput (libnx buffer_producer.h): int32 length, int32 fd
         * count, then { s64 timestamp; s32 isAutoTimestamp; rect crop;
         * s32 scalingMode; u32 transform, stickyTransform, unk, swapInterval;
         * NvMultiFence fence } - the fence at +48: u32 count, then up to four
         * { u32 syncpt id; u32 value }. */
        bool ParseQueueBufferFence(const u8 *p, size_t sz, u32 *n, u64 out[4]) {
            if (p == nullptr || sz < 24) { return false; }
            u32 data_off = 0, len = 0;
            std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
            if (static_cast<size_t>(data_off) + 8 > sz) { return false; }
            std::memcpy(std::addressof(len), p + data_off + 4, sizeof(len));
            if (len == 0 || len > 128) { return false; }
            size_t off = static_cast<size_t>(data_off) + 8 + (static_cast<size_t>(len) + 1) * 2;
            off = (off + 3) & ~static_cast<size_t>(3);
            off += 4;                                   /* the slot */
            if (off + 8 + 84 > sz) { return false; }
            u32 flen = 0;
            std::memcpy(std::addressof(flen), p + off, sizeof(flen));
            if (flen < 84 || flen > 0x100) { return false; }
            const u8 *in = p + off + 8;
            u32 cnt = 0;
            std::memcpy(std::addressof(cnt), in + 48, sizeof(cnt));
            if (cnt > 4) { return false; }
            for (u32 k = 0; k < 4; ++k) {
                u32 id = 0, v = 0;
                std::memcpy(std::addressof(id), in + 52 + 8 * k, 4);
                std::memcpy(std::addressof(v), in + 56 + 8 * k, 4);
                out[k] = (static_cast<u64>(id) << 32) | v;
            }
            *n = cnt;
            return true;
        }

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

        /* v0.1.1: only the running application's frames feed the capture.
         * With vi:m wrapped too, anything else that slipped through is passed
         * on untouched (0 = the watcher has not seen an application yet). */
        {
            const u64 ap = g_app_pid.load(std::memory_order_relaxed);
            if (ap != 0 && ap != m_client_info.process_id.value) { R_RETURN(sm::mitm::ResultShouldForwardToSession()); }
        }

        /* SET_PREALLOCATED_BUFFER (14) carries a flattened NvGraphicBuffer in
         * its INPUT parcel - the full description of one frame's memory.
         * Games register one per swapchain slot at startup (MK8: 3 = triple
         * buffered). This is the descriptor we need to import and read pixels. */
        /* M87: every registration, not just the first 8 of the boot (a third
         * game launch registered nothing before), with the slot the parcel
         * names - the int32 after the interface token, as in queueBuffer */
        if (code == 14) {
            LogMark("binder:parse_preallocated");
            if (const auto *gb = FindGraphicBuffer(parcel_in.GetPointer(), parcel_in.GetSize()); gb != nullptr) {
                const s32 pslot = ParseQueueBufferSlot(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize());
                char tag[48];
                std::snprintf(tag, sizeof(tag), "setPreallocatedBuffer#%u slot %d", per, pslot);
                LogGraphicBuffer(tag, gb);
                CaptureGameSurface(gb, pslot);
            } else {
                LogLine("    (no NvGraphicBuffer magic found in %zu-byte parcel)", parcel_in.GetSize());
            }
        }

        /* queueBuffer (7): the frame is now rendered and its swapchain slot is
         * the first int32 of the parcel payload. Once the game has presented a
         * few hundred frames (real content on screen), run the one-shot VIC
         * blit against that slot. */
        /* Per-frame bookkeeping for the capture loop: a cheap parse and two
         * relaxed stores. The worker thread polls these to know when a new frame
         * has been presented and which slot holds it. */
        if (code == 7) {
            const s32 qs = ParseQueueBufferSlot(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize());
            u32 fn = 0;
            u64 fv[4] = {};
            const bool have_fence = ParseQueueBufferFence(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize(), std::addressof(fn), fv);
            /* M89: seqlock - odd while slot, fence, tick and count change
             * together, so the capture never pairs one present's slot with
             * another's fence */
            g_queue_seq.fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            if (qs >= 0 && qs < 8) { g_queue_slot.store(qs, std::memory_order_relaxed); }
            if (have_fence) {
                for (u32 k = 0; k < 4; ++k) { g_queue_fence[k].store(fv[k], std::memory_order_relaxed); }
                g_queue_fence_n.store(fn, std::memory_order_relaxed);
            } else {
                g_queue_fence_n.store(0, std::memory_order_relaxed);
            }
            const u64 qtick = armGetSystemTick();
            g_queue_tick.store(qtick, std::memory_order_relaxed);
            {
                /* M96: this present's record, complete before the count moves */
                const u32 c = g_queue_count.load(std::memory_order_relaxed) + 1;
                PresentRec &r = g_present_ring[c % PresentRingSize];
                r.seq.fetch_add(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                r.count.store(c, std::memory_order_relaxed);
                r.slot.store((qs >= 0 && qs < 8) ? qs : -1, std::memory_order_relaxed);
                r.fence_n.store(have_fence ? fn : 0, std::memory_order_relaxed);
                for (u32 k = 0; k < 4; ++k) { r.fence[k].store(have_fence ? fv[k] : 0, std::memory_order_relaxed); }
                r.tick.store(qtick, std::memory_order_relaxed);
                r.seq.fetch_add(1, std::memory_order_release);
            }
            g_queue_count.fetch_add(1, std::memory_order_release);
            g_queue_seq.fetch_add(1, std::memory_order_release);
        }

        /* Fire on elapsed time, not transaction count. "total > 300" landed at
         * ~50 s, which is still the title screen - too early to be holding a
         * controller in a race. g_probe_delay_s comes from "wait=N" in the arm
         * file and defaults to 120 s. */
        const u64 uptime_s = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000000);
        if (code == 7 && g_vic_armed && uptime_s >= g_probe_delay_s && g_game_surface.armed) {
            bool ex = false;
            if (g_blit_attempted.compare_exchange_strong(ex, true)) {
                const auto *p  = static_cast<const u8 *>(parcel_in.GetPointer());
                const size_t psz = parcel_in.GetSize();
                const s32 qslot  = ParseQueueBufferSlot(p, psz);
                LogMark("binder:vic_blit_requested");
                LogLine("   queueBuffer txn#%u parcel=%zu slot=%d -> handed to VIC worker", total, psz, qslot);
                RequestVicBlit(qslot);
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

        /* ---- indirect-layer recon (one-shot, read-only) --------------------
         * Reading the game's swapchain is structurally impossible for us, and
         * the display-controller nodes only PROGRAM the display - there is no
         * readback ioctl anywhere in nvdrv. But vi has the mechanism Nintendo
         * actually uses for "one process reads another's layer": indirect
         * layers. GetIndirectLayerImageMap (2450) writes into a type-0x46
         * buffer, i.e. memory WE supply - which is precisely what our process
         * isolation requires.
         *
         * Two cheap forward calls decide whether that route is open from here:
         *   2460 GetIndirectLayerImageRequiredMemoryInfo - no PID descriptor,
         *        just two s64 in / two s64 out. A sane size for 1280x720 means
         *        the indirect-layer machinery answers us at all.
         *   102  GetManagerDisplayService - gates CreateIndirectLayer (2050),
         *        which is how a consumer handle gets made without AM. */
        {
            static std::atomic<bool> probed{false};
            bool ex = false;
            if (probed.compare_exchange_strong(ex, true)) {
                LogMark("indirect:probe");
                struct { s64 w; s64 h; } in  = { 1280, 720 };
                struct { s64 size; s64 align; } out = {};
                const Result r1 = serviceDispatchInOut(m_forward_service.get(), 2460, in, out);
                LogLine("   2460 RequiredMemoryInfo(1280x720) rc=0x%x -> size=%lld align=%lld",
                        r1.GetValue(), static_cast<long long>(out.size), static_cast<long long>(out.align));

                ::Service mgr = {};
                const Result r2 = serviceDispatch(m_forward_service.get(), 102,
                    .out_num_objects = 1, .out_objects = std::addressof(mgr));
                LogLine("   102 GetManagerDisplayService rc=0x%x -> %s",
                        r2.GetValue(), R_SUCCEEDED(r2) ? "GOT IT (CreateIndirectLayer reachable)"
                                                       : "denied on a vi:u session");
                if (R_SUCCEEDED(r2)) { serviceClose(std::addressof(mgr)); }
                LogMark("indirect:probe_done");
            }
        }

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

    constinit std::atomic<u64> g_app_pid{0};

    namespace {
        alignas(os::ThreadStackAlignment) constinit u8 g_app_watch_stack[8_KB];
        constinit os::ThreadType g_app_watch_thread;

        void AppWatchThread(void *) {
            u64 last = ~UINT64_C(0);
            const bool info_ok = R_SUCCEEDED(::pminfoInitialize());
            for (;;) {
                os::ProcessId pid{};
                const u64 now = R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid))) ? pid.value : 0;
                g_app_pid.store(now, std::memory_order_relaxed);
                if (now != last) {
                    /* v0.1.1b: which program it is, and whether it is excluded */
                    u64 tid = 0;
                    if (now != 0 && info_ok && R_FAILED(::pminfoGetProgramId(&tid, now))) { tid = 0; }
                    g_app_tid.store(tid, std::memory_order_relaxed);
                    g_app_excluded.store(tid != 0 && IsExcluded(tid), std::memory_order_relaxed);
                    LogLine("app watch: application pid %llu, program %016llx%s", static_cast<unsigned long long>(now),
                            static_cast<unsigned long long>(tid), g_app_excluded.load() ? " (EXCLUDED)" : "");
                    last = now;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
        }
    }

    void StartAppWatch() {
        if (g_pmdmnt_rc != 0) { LogLine("app watch: pm:dmnt unavailable - homebrew applications need an application-range program id"); return; }
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_app_watch_thread), AppWatchThread, nullptr,
                                        g_app_watch_stack, sizeof(g_app_watch_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_app_watch_thread), "applet-mitm.AppWatch");
        os::StartThread(std::addressof(g_app_watch_thread));
    }

    Result ForwardGetDisplayService(::Service *fwd, u32 cmd, u32 mode, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        g_stats.getdisp.fetch_add(1);
        LogMark("GetDisplayService:enter");
        LogLine("   program=%016llx cmd %u mode=%u%s", static_cast<unsigned long long>(ci.program_id.value), cmd, mode,
                cmd == 2 ? " (vi:m - a homebrew application)" : "");

        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(fwd, cmd, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }
        WrapDisplayService(disp_svc, ci, out);
        LogMark("GetDisplayService:done");
        R_SUCCEED();
    }

    void WrapDisplayService(::Service disp_svc, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        auto shared_srv = std::make_shared<::Service>(disp_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };

        /* tier4 libstratosphere patch: give the wrapper's session a forward
         * service so undeclared commands auto-forward on this NON-domain
         * session. */
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), ci), target_object_id);
    }

    /* msg: the client's request, copied from TLS before any IPC of ours */
    Result ForwardProxyNameExchange(::Service *fwd, u32 cmd, const u8 *msg, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        g_stats.getdisp.fetch_add(1);
        LogMark("GetDisplayServiceWithProxyNameExchange:enter");

        const HipcParsedRequest req = hipcParseRequest(const_cast<u8 *>(msg));
        const uintptr_t words = reinterpret_cast<uintptr_t>(req.data.data_words);
        const uintptr_t raw   = (words + 0xF) & ~static_cast<uintptr_t>(0xF);
        const size_t total    = static_cast<size_t>(req.meta.num_data_words) * 4;
        const CmifInHeader *hdr = reinterpret_cast<const CmifInHeader *>(raw);
        /* the client sized its data words as header + 0x10 alignment slack +
         * arguments (libnx cmifMakeRequest); what follows the header, less the
         * slack actually used, is the arguments with their zero padding */
        size_t args = 0;
        const bool sane = total >= sizeof(CmifInHeader) + (raw - words) && raw + sizeof(CmifInHeader) <= reinterpret_cast<uintptr_t>(msg) + 0x100
                       && hdr->magic == CMIF_IN_HEADER_MAGIC && hdr->command_id == cmd;
        if (sane) {
            args = total - (raw - words) - sizeof(CmifInHeader);
            if (args > 0x40) { args = 0x40; }
        }
        const u8 *a = reinterpret_cast<const u8 *>(raw + sizeof(CmifInHeader));
        LogLine("   program=%016llx cmd %u (ProxyNameExchange): %zu argument byte(s)%s; data words %u, pid %s",
                static_cast<unsigned long long>(ci.program_id.value), cmd, args, sane ? "" : " (UNPARSED)",
                req.meta.num_data_words, req.meta.send_pid ? "sent" : "none");
        if (sane && args != 0) {
            char hex[3 * 0x40 + 1] = {};
            for (size_t i = 0; i < args; ++i) { std::snprintf(hex + 3 * i, 4, "%02x ", a[i]); }
            LogLine("   args: %s", hex);
        }
        if (!sane) {
            /* could not read the request: let the real service answer it as
             * before (unwrapped), rather than guess its arguments */
            LogLine("   not wrapping: forwarding untouched is no longer possible here - using GetDisplayService instead");
        }

        ::Service disp_svc = {};
        ::Result rc = 0;
        if (sane) {
            SfDispatchParams disp = {};
            disp.out_num_objects = 1;
            disp.out_objects     = std::addressof(disp_svc);
            rc = serviceDispatchImpl(fwd, cmd, a, static_cast<u32>(args), nullptr, 0, disp);
        } else {
            /* the plain GetDisplayService of the same root: 0 on vi:u, 2 on vi:m */
            const u32 policy = cmd == 3 ? 1 : 0;
            rc = serviceDispatchIn(fwd, cmd - 1, policy,
                                   .out_num_objects = 1, .out_objects = std::addressof(disp_svc));
        }
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc);
            R_RETURN(::ams::Result(rc));
        }
        WrapDisplayService(disp_svc, ci, out);
        LogMark("GetDisplayServiceWithProxyNameExchange:done");
        R_SUCCEED();
    }

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 0, mode, m_client_info, out));
    }

    void ViRootMitm::Wrap(::Service disp_svc, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        WrapDisplayService(disp_svc, m_client_info, out);
    }

    Result ViRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out) {
        /* FIRST, before any IPC of ours (logging included) reuses the TLS
         * message buffer: copy the game's request as it arrived. */
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 1, msg, m_client_info, out));
    }

    Result ViManagerRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 2, mode, m_client_info, out));
    }

    Result ViManagerRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out) {
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 3, msg, m_client_info, out));
    }

}
