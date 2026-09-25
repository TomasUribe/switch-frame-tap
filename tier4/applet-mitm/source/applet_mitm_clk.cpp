/*
 * applet-mitm - M76: engine clocks. See applet_mitm_clk.hpp for the why.
 *
 * Both services are driven with hand-rolled IPC and explicit command ids, the
 * same way this module talks to nvdrv, so nothing depends on libnx's version
 * switches:
 *
 *   clkrst (8.0.0+)     0  OpenSession {u32 module, u32 unk} -> IClkrstSession
 *   IClkrstSession      8  GetClockRate -> u32 hz
 *
 *   mm:u (2.0.0+)       4  InitializeWithId {u32 module, u32 unk, u32 autoclear} -> u32 id
 *                       5  FinalizeWithId   {u32 id}
 *                       6  SetAndWaitWithId {u32 id, u32 hz, s32 timeout}
 *                       7  GetWithId        {u32 id} -> u32 hz
 *
 * Nothing here opens a channel or submits anything. The worst an mm:u request
 * can do is raise an idle engine's clock, which every video player does.
 */
#include "applet_mitm_clk.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include <cstdio>
#include <cstring>
#include <atomic>

namespace ams::mitm::applet {

    constinit bool g_clk_armed = false;

    namespace {

        struct Engine { const char *name; u32 pcv_module; };
        constexpr Engine SurveyEngines[] = {
            { "VIC",    PcvModule_VIC   },   /* the control: nvnflinger keeps it running */
            { "NVENC",  PcvModule_NVENC },
            { "NVJPG",  PcvModule_NVJPG },
            { "NVDEC",  PcvModule_NVDEC },
            { "HOST1X", 0x4000002C      },
        };

        struct MmCandidate { u32 id; const char *label; };
        constexpr MmCandidate MmCandidates[] = {
            { 5, "mm id 5 (libnx: NVENC, nvtegra: NVDEC)" },
            { 6, "mm id 6 (libnx: NVDEC)" },
            { 7, "mm id 7 (NVJPG)" },
        };
        constexpr size_t NumMm = sizeof(MmCandidates) / sizeof(MmCandidates[0]);

        /* Official code passes INT_MAX kHz, multiplied to Hz in a u32; this is
         * the value that produces (per nvtegra). The service clamps it to the
         * highest rate the current performance mode allows. */
        constexpr u32 MaxRequestHz = static_cast<u32>((UINT64_C(1) << 32) - 1000);

        constinit os::SdkMutex g_clk_lock;
        constinit bool    g_mm_open = false;
        constinit bool    g_holding = false;
        constinit Service g_mm_srv  = {};
        constinit u32     g_req_id[NumMm]  = {};
        constinit bool    g_req_ok[NumMm]  = {};

        /* ---- clkrst ---- */
        ::Result ClkrstRate(Service *clk, u32 module, u32 *out_hz) {
            *out_hz = 0;
            const struct { u32 module; u32 unk; } in = { module, 3 };   /* 3: what sys-clk passes */
            Service sess = {};
            ::Result rc = serviceDispatchIn(clk, 0, in,
                .out_num_objects = 1,
                .out_objects     = std::addressof(sess));
            if (R_FAILED(rc)) { return rc; }
            rc = serviceDispatchOut(std::addressof(sess), 8, *out_hz);
            serviceClose(std::addressof(sess));
            return rc;
        }

        /* ---- mm:u ---- */
        ::Result MmInitialize(u32 module, u32 *out_id) {
            const struct { u32 module; u32 unk; u32 autoclear; } in = { module, 8, 0 };
            return serviceDispatchInOut(std::addressof(g_mm_srv), 4, in, *out_id);
        }
        ::Result MmFinalize(u32 id)                    { return serviceDispatchIn(std::addressof(g_mm_srv), 5, id); }
        ::Result MmSetAndWait(u32 id, u32 hz, s32 to)  {
            const struct { u32 id; u32 hz; s32 timeout; } in = { id, hz, to };
            return serviceDispatchIn(std::addressof(g_mm_srv), 6, in);
        }
        ::Result MmGet(u32 id, u32 *out_hz)            { return serviceDispatchInOut(std::addressof(g_mm_srv), 7, id, *out_hz); }

        void ReleaseLocked() {
            if (!g_mm_open) { return; }
            for (size_t i = 0; i < NumMm; ++i) {
                if (g_req_ok[i]) {
                    const ::Result rc = MmFinalize(g_req_id[i]);
                    LogLine("   clk: release %s rc=0x%x", MmCandidates[i].label, rc);
                    g_req_ok[i] = false;
                }
            }
            serviceClose(std::addressof(g_mm_srv));
            g_mm_open = false;
            g_holding = false;
        }

        alignas(os::ThreadStackAlignment) constinit u8 g_clk_stack[16_KB];
        constinit os::ThreadType g_clk_thread;
        constinit bool g_keep_holding = false;
        constinit std::atomic<bool> g_watch_stop{false};

        const char *EngineName(u32 pcv_module) {
            for (const auto &e : SurveyEngines) { if (e.pcv_module == pcv_module) { return e.name; } }
            return "?";
        }

        /* Which mm:u candidates drive which engine. 7 is NVJPG in every
         * source; 5 and 6 are NVENC/NVDEC in some order (M76). */
        bool MmDrives(size_t i, u32 pcv_module) {
            const u32 id = MmCandidates[i].id;
            if (pcv_module == PcvModule_NVJPG) { return id == 7; }
            if (pcv_module == PcvModule_NVENC || pcv_module == PcvModule_NVDEC) { return id == 5 || id == 6; }
            return false;
        }

        /* M77: the clock watch. M76 Run B saw NVJPG (and VIC with it - they
         * share a bus, per Run A's release) fall to their idle rates somewhere
         * in a ten-second gap that held a node survey (open+close of every
         * engine node, NVJPG's included), a VIC channel open and nothing
         * else. Sampling every 200 ms and logging only on change pins the
         * drop to one of those, or shows it is a timer. */
        void WatchClocks() {
            Service clk = {};
            if (R_FAILED(smGetService(std::addressof(clk), "clkrst"))) {
                LogLine("   clk-watch: clkrst unavailable - not watching");
                return;
            }
            static constexpr u32 Mods[3] = { PcvModule_NVJPG, PcvModule_VIC, PcvModule_NVDEC };
            u32 last[3] = { ~0u, ~0u, ~0u };
            const u64 t0 = armTicksToNs(armGetSystemTick());
            constexpr u64 LimitNs = UINT64_C(45) * 1000000000;
            LogLine("   clk-watch: sampling NVJPG/VIC/NVDEC every 200 ms, logging changes only");
            while (!g_watch_stop.load() && armTicksToNs(armGetSystemTick()) - t0 < LimitNs) {
                u32 cur[3];
                for (u32 k = 0; k < 3; ++k) {
                    if (R_FAILED(ClkrstRate(std::addressof(clk), Mods[k], std::addressof(cur[k])))) { cur[k] = ~0u - 1; }
                }
                if (cur[0] != last[0] || cur[1] != last[1] || cur[2] != last[2]) {
                    LogLine("   clk-watch  NVJPG=%u.%01u  VIC=%u.%01u  NVDEC=%u.%01u MHz",
                            cur[0] / 1000000, (cur[0] / 100000) % 10, cur[1] / 1000000, (cur[1] / 100000) % 10,
                            cur[2] / 1000000, (cur[2] / 100000) % 10);
                    std::memcpy(last, cur, sizeof(last));
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(200));
            }
            serviceClose(std::addressof(clk));
            LogLine("   clk-watch: stopped (%s)", g_watch_stop.load() ? "engine probe finished" : "45 s limit");
        }

        void ClockThread(void *) {
            /* The engine probes fire at system uptime >= wait. Survey ~10 s
             * earlier, so "before" really is before anything raised a clock. */
            const u64 target_s = (g_probe_delay_s > 20) ? g_probe_delay_s - 10 : g_probe_delay_s;
            while (armTicksToNs(armGetSystemTick()) / UINT64_C(1000000000) < target_s) {
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
            }
            LogMark("clk:1_survey");
            LogLine("---- M76 CLOCK SURVEY (no engine contact) ----");
            ClockSurvey("before");

            /* Is grc holding NVENC? Sample a few times before we change
             * anything: a steady non-zero NVENC clock during gameplay means
             * something already keeps it running, and then the clock is NOT
             * what stalled M68-M71. */
            for (u32 i = 0; i < 3; ++i) {
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
                ClockSurvey("before+");
            }

            LogMark("clk:2_hold");
            const bool held = ClocksHoldForEngines("survey");
            os::SleepThread(TimeSpan::FromMilliSeconds(100));
            ClockSurvey(held ? "held" : "hold FAILED");
            os::SleepThread(TimeSpan::FromSeconds(2));
            ClockSurvey(held ? "held+2s" : "hold FAILED+2s");

            if (!g_keep_holding) {
                LogMark("clk:3_release");
                {
                    std::scoped_lock lk(g_clk_lock);
                    ReleaseLocked();
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
                ClockSurvey("released");
                LogLine("---- M76 CLOCK SURVEY done ----");
            } else {
                LogLine("   clk: holding for the engine probes armed in this run");
                LogLine("---- M76 CLOCK SURVEY done ----");
                LogMark("clk:watch");
                WatchClocks();
            }
            LogMark("clk:done");
        }

    }

    bool ClockRateOf(u32 pcv_module, u32 *out_hz) {
        *out_hz = 0;
        Service clk = {};
        if (R_FAILED(smGetService(std::addressof(clk), "clkrst"))) { return false; }
        const ::Result rc = ClkrstRate(std::addressof(clk), pcv_module, out_hz);
        serviceClose(std::addressof(clk));
        return R_SUCCEEDED(rc);
    }

    void ClockSurvey(const char *label) {
        Service clk = {};
        const ::Result src = smGetService(std::addressof(clk), "clkrst");
        if (R_FAILED(src)) {
            LogLine("   clk[%s]: clkrst unavailable rc=0x%x", label, src);
            return;
        }
        char line[256];
        int n = std::snprintf(line, sizeof(line), "   clk[%-12s]", label);
        for (const auto &e : SurveyEngines) {
            u32 hz = 0;
            const ::Result rc = ClkrstRate(std::addressof(clk), e.pcv_module, std::addressof(hz));
            if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
                if (R_SUCCEEDED(rc)) {
                    n += std::snprintf(line + n, sizeof(line) - n, "  %s=%u.%01u MHz", e.name, hz / 1000000, (hz / 100000) % 10);
                } else {
                    n += std::snprintf(line + n, sizeof(line) - n, "  %s=rc:0x%x", e.name, rc);
                }
            }
        }
        serviceClose(std::addressof(clk));
        LogLine("%s", line);
    }

    bool ClocksHoldForEngines(const char *who) {
        std::scoped_lock lk(g_clk_lock);
        if (g_holding) { return true; }

        if (!g_mm_open) {
            const ::Result rc = smGetService(std::addressof(g_mm_srv), "mm:u");
            LogLine("   clk(%s): smGetService(mm:u) rc=0x%x", who, rc);
            if (R_FAILED(rc)) { return false; }
            g_mm_open = true;
        }

        bool any = false;
        for (size_t i = 0; i < NumMm; ++i) {
            u32 id = 0, before = 0, after = 0;
            const ::Result ri = MmInitialize(MmCandidates[i].id, std::addressof(id));
            if (R_FAILED(ri)) {
                LogLine("   clk: %s InitializeWithId rc=0x%x", MmCandidates[i].label, ri);
                continue;
            }
            g_req_id[i] = id;
            g_req_ok[i] = true;
            const ::Result rg0 = MmGet(id, std::addressof(before));
            const ::Result rs  = MmSetAndWait(id, MaxRequestHz, -1);
            const ::Result rg1 = MmGet(id, std::addressof(after));
            LogLine("   clk: %s req=%u  get=%u Hz (rc=0x%x)  SetAndWait(max) rc=0x%x  -> get=%u Hz (rc=0x%x)",
                    MmCandidates[i].label, id, before, rg0, rs, after, rg1);
            any = any || R_SUCCEEDED(rs);
        }
        g_holding = any;
        if (!any) { ReleaseLocked(); }
        return any;
    }

    u32 ClockEnsure(u32 pcv_module, const char *who) {
        const char *name = EngineName(pcv_module);
        if (!ClocksHoldForEngines(who)) {
            LogLine("   clk-ensure(%s): no mm:u request could be made", who);
            return 0;
        }
        static const char *const Step[3] = { "re-set max", "set 0, then max", "fresh request" };
        constexpr u32 Attempts = 6;
        for (u32 attempt = 0; attempt < Attempts; ++attempt) {
            const u32 step = attempt < 3 ? attempt : 2;
            ::Result rc_last = 0;
            u32 touched = 0;
            {
                std::scoped_lock lk(g_clk_lock);
                for (size_t i = 0; i < NumMm; ++i) {
                    if (!MmDrives(i, pcv_module)) { continue; }
                    if (step == 2 || !g_req_ok[i]) {
                        if (g_req_ok[i]) { static_cast<void>(MmFinalize(g_req_id[i])); g_req_ok[i] = false; }
                        u32 id = 0;
                        rc_last = MmInitialize(MmCandidates[i].id, std::addressof(id));
                        if (R_FAILED(rc_last)) { continue; }
                        g_req_id[i] = id;
                        g_req_ok[i] = true;
                    } else if (step == 1) {
                        static_cast<void>(MmSetAndWait(g_req_id[i], 0, -1));
                    }
                    rc_last = MmSetAndWait(g_req_id[i], MaxRequestHz, -1);
                    ++touched;
                }
            }
            u32 hz = 0;
            const bool readable = ClockRateOf(pcv_module, std::addressof(hz));
            LogLine("   clk-ensure(%s) #%u %-15s: %u request(s), SetAndWait rc=0x%x -> clkrst %s=%u Hz%s",
                    who, attempt + 1, Step[step], touched, rc_last, name, hz, readable ? "" : " (UNREADABLE)");
            if (readable && hz != 0) { return hz; }
            os::SleepThread(TimeSpan::FromMilliSeconds(50 * (attempt + 1)));
        }
        return 0;
    }

    void ClockWatchStop() { g_watch_stop = true; }

    void StartClockProbe(bool keep_holding) {
        if (!g_clk_armed) { return; }
        g_keep_holding = keep_holding;
        /* Below main/IPC, like every worker here: all our threads share core 3,
         * and nothing on this thread may delay a binder reply. */
        const s32 prio = os::GetThreadPriority(os::GetCurrentThread()) + 4;
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_clk_thread), ClockThread, nullptr,
                                        g_clk_stack, sizeof(g_clk_stack), prio));
        os::SetThreadNamePointer(std::addressof(g_clk_thread), "applet-mitm.Clk");
        os::StartThread(std::addressof(g_clk_thread));
        LogLine("clock survey armed: fires at uptime ~%u s, %s", g_probe_delay_s > 20 ? g_probe_delay_s - 10 : g_probe_delay_s,
                keep_holding ? "then HOLDS the clocks for this run's engine probes" : "then releases");
    }

}
