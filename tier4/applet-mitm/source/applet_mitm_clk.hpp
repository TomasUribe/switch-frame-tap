/*
 * applet-mitm - M76: engine clocks.
 *
 * Every engine this project has driven that is NOT the VIC has done exactly
 * nothing: NVENC (M68-M71) and NVJPG (M73-M74) both accept a submit, issue a
 * fence, and never execute - NVJPG's status buffer reports cycles=0, i.e. the
 * engine did not run for a single clock.
 *
 * What nobody here had ever done is ask for those engines' clocks. On Horizon
 * that is not nvservices' job: a client asks the multimedia service, "mm:u",
 * for a frequency on the engine it is about to use. Both open-source drivers
 * known to work on this console do it before touching the engine:
 *
 *   - averne's oss-nvjpg (NVJPG decode):   mmuRequestInitialize(Nvjpg)
 *   - averne's FFmpeg nvtegra (NVDEC/NVJPG): mmuRequestInitialize, then
 *     mmuRequestSetAndWait(max) - "reproduces official code"
 *
 * The VIC never needed it because nvnflinger keeps the VIC clocked from boot.
 * That is the one difference M73 identified between the engine that works and
 * the two that do not, and this is the mechanism behind it.
 *
 * Two parts, both without any engine contact:
 *   ClockSurvey          - reads the real module clocks through clkrst.
 *   ClocksHoldForEngines - raises the mm:u requests for NVENC, NVDEC and NVJPG
 *                          and keeps them for the life of the process.
 *
 * The mm:u module ids are ambiguous in the sources: libnx names 5 NVENC and
 * 6 NVDEC, while nvtegra (by the same author, and newer) passes 5 for NVDEC.
 * So all of 5, 6 and 7 are requested; clocking an idle decoder too costs a
 * little power and nothing else. The clkrst survey says which id moved which
 * engine.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    extern bool g_clk_armed;

    /* pcv module ids, as clkrst takes them (libnx pcv.h) */
    constexpr u32 PcvModule_VIC   = 0x4000002F;
    constexpr u32 PcvModule_NVENC = 0x40000030;
    constexpr u32 PcvModule_NVJPG = 0x40000031;
    constexpr u32 PcvModule_NVDEC = 0x40000032;

    /* The clock clkrst reports for one module. False if it could not be read. */
    bool ClockRateOf(u32 pcv_module, u32 *out_hz);

    /* One log line per engine: the clock clkrst reports right now. */
    void ClockSurvey(const char *label);

    /* Creates the mm:u requests once (maximum rate on every candidate engine
     * id) and keeps them. Returns true if any request succeeded. Safe to call
     * from any thread.
     *
     * M77: "held" means the requests EXIST, not that any clock is running
     * now. M76 Run B watched NVJPG go from 652.8 MHz to 0 while its request
     * was never released, and jpgdec trusted this call's `true`. Before a
     * submit, use ClockEnsure. */
    bool ClocksHoldForEngines(const char *who);

    /* M77: make sure `pcv_module` is clocked RIGHT NOW. Re-applies the mm:u
     * request for that engine and reads the clock back through clkrst,
     * escalating until it is non-zero or the budget is spent:
     *   1. SetAndWait(max) on the existing request
     *   2. SetAndWait(0) then SetAndWait(max) - defeats a cached "no change"
     *   3. Finalize, fresh InitializeWithId, SetAndWait(max)
     * Which step brings it back is itself evidence of how the clock was lost.
     * Returns the rate clkrst reports, 0 if it never came up. */
    u32 ClockEnsure(u32 pcv_module, const char *who);

    /* Ends the clock watch the survey thread runs while it holds clocks. */
    void ClockWatchStop();

    /* Starts the survey thread when "clk" is armed: at t=wait it surveys,
     * holds, surveys again, and (if no engine probe is armed) releases. */
    void StartClockProbe(bool keep_holding);

}
