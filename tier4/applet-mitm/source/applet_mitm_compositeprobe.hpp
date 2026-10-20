/*
 * v0.7.15 composite feasibility probe.
 *
 * Opt-in with "compositeprobe" in the arm file. It answers the questions the
 * layer fallback cannot:
 *   1. while a game is in gameplay, do any of vi's 7 system-shared-buffer
 *      slots hold the full composited picture?
 *   2. how many independent layer sessions does the game present?
 *   3. what do the official vi:m readback commands answer (8297/8293)?
 *
 * Read-only: it attaches to vi as a debugger, reads, and detaches. It never
 * calls caps:sc Attach/Capture and never submits an engine job. Raw slots are
 * written to sdmc:/config/switch-frame-tap/probe/ for deswizzle.py.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    extern bool g_composite_probe_armed;

    /* start the probe thread (main, after pm/dmnt and the log are up) */
    void StartCompositeProbe();

    /* binder-thread bookkeeping: setPreallocatedBuffer (14) / queueBuffer (7) */
    void CompositeProbeNoteTxn(u64 program_id, s32 session_id, u32 code, s32 slot);

}
