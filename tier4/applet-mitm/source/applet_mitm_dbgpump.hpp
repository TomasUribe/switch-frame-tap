/*
 * applet-mitm - M84: keeping an attached game running.
 *
 * Run I's stream froze the game at a loading screen, and the reason is in
 * mesosphere (kern_k_debug_base.cpp, ProcessDebugEvent): while a debugger is
 * attached, every thread start and thread exit in the game pushes a debug
 * event, and pushing one suspends ALL of the game's threads until the
 * debugger calls ContinueDebugEvent. We drained and continued once, at
 * attach, and never again. Mid-race no thread comes or goes, so Run H and
 * the M67 stream never saw it; a loading screen starts and stops workers.
 *
 * The pump is what dmnt's cheat engine does for the same problem
 * (dmnt_cheat_api.cpp DebugEventsThread): a thread blocks on the debug
 * handle, which the kernel signals while events are pending, drains them,
 * and continues. Like dmnt it continues on the core of the thread that was
 * just created (dmnt_cheat_debug_events_manager.cpp: "prevents a kernel
 * deadlock"), which is why the NPDM now allows cores 0-2 for three small
 * helper threads. Everything else in this process stays on core 3.
 *
 * Exceptions are NOT reported to us: we never set EnableExceptionEvent, so a
 * crash in the game stays a crash (ProcessDebugEvent returns NotHandled).
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct DebugPumpStats {
        u32 wakes;              /* times the handle was signalled */
        u32 events;             /* debug events drained, all types */
        u32 created;            /* DebugEvent_CreateThread */
        u32 exited;             /* DebugEvent_ExitThread */
        u32 other;              /* CreateProcess / ExitProcess / Exception */
        u32 continues;          /* successful ContinueDebugEvent calls */
        u32 busy;               /* ContinueDebugEvent said Busy: an event came in between */
        u32 failed;             /* any other ContinueDebugEvent failure */
        u32 last_fail_rc;
        u32 per_core[4];        /* which core each continue ran on */
        u64 max_hold_ns;        /* longest wake -> continued, i.e. our share of a break */
        bool gone;              /* the game exited or we were detached */
    };

    /* Starts pumping `dbg`, which must already be drained and continued once
     * (the attach burst). Call from the thread that owns the handle. */
    void DebugPumpStart(::ams::svc::Handle dbg);

    /* Stops pumping and returns once the pump no longer touches the handle
     * (at most ~50 ms). Must run BEFORE the handle is closed. No-op if the
     * pump was never started. */
    void DebugPumpStop();

    /* True once the game has exited (or the handle stopped working). A read
     * failing after this is the game closing, not a fault. */
    bool DebugPumpTargetGone();

    void DebugPumpGetStats(DebugPumpStats *out);

    /* One line: "debug events N (+created/-exited/other), continued C ..." */
    void DebugPumpLogStats(const char *who);

}
