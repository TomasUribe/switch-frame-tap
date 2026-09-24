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
            } else {
                LogLine("   clk: holding for the engine probes armed in this run");
            }
            LogLine("---- M76 CLOCK SURVEY done ----");
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
