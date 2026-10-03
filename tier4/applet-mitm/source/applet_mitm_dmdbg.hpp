/*
 * applet-mitm - Option A: read the game through dmnt:cht instead of holding
 * our own debug handle.
 *
 * The kernel allows ONE debugger per process. Our stream used to attach with
 * svcDebugActiveProcess itself, so any other debugger (Atmosphere's cheat
 * engine, once cheats are on for the title) won the race and the stream got
 * no video. dmnt:cht already exports what we need - ForceOpenCheatProcess,
 * ReadCheatProcessMemory, QueryCheatProcessMemory (65102/65104, the same
 * SVCs underneath, with dmnt's own event pump keeping the game running) -
 * and ForceOpen attaches to ANY application, cheats or not. So dmnt stays
 * the single debugger, cheats keep working, and we just read through it.
 *
 * Backend choice is per-attach and automatic: dmnt first, direct SVC
 * fallback (service missing, or dmnt refused). Callers thread one Handle
 * through the existing capture code unchanged: a dmnt session is the
 * kDmntDbg sentinel, a direct one a real handle. The grc observer paths
 * keep real handles - dmnt only ever tracks the application, never grc.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    /* Sentinel for "this session reads through dmnt:cht". A real kernel
     * handle never looks like this; never pass it to svcCloseHandle. */
    constexpr svc::Handle kDmntDbg = static_cast<svc::Handle>(0x444D4E54u);

    inline bool DbgIsDmnt(svc::Handle dbg) { return dbg == kDmntDbg; }

    /* dmnt first, direct DebugActiveProcess fallback. Logs which won. */
    bool DmAttach(u64 pid, svc::Handle *out_dbg);

    /* Drop-in routed reads: dmnt IPC or the debug SVC, by handle kind. */
    bool DbgRead(svc::Handle dbg, void *dst, u64 va, size_t len);
    bool DbgQuery(svc::Handle dbg, svc::MemoryInfo *mi, svc::PageInfo *pi, u64 addr);

    /* Drain the attach burst and resume. No-op for dmnt (it pumps its own
     * handle; the game never stopped). Returns the continue rc. */
    Result DbgResume(svc::Handle dbg, u32 *out_nev);

    /* End the session. No-op for dmnt: the handle is dmnt's, and closing it
     * would detach the cheat engine too. dmnt drops it when the game exits. */
    void DbgDetach(svc::Handle dbg);

    /* Per-piece read timing for the dmnt backend (diagnostic): reset on
     * attach, one summary line per live session. */
    void DmStatsReset();
    void DmStatsLog(u32 session);

}
