/*
 * applet-mitm - read the game through dmnt:cht when someone else debugs it.
 *
 * The kernel allows ONE debugger per process. Our stream attaches with
 * svcDebugActiveProcess itself, so when another debugger (Atmosphere's
 * cheat engine, once cheats are on for the title) holds the game, our
 * attach fails and the stream got no video. dmnt:cht already exports what
 * reads need - ReadCheatProcessMemory, QueryCheatProcessMemory (65102 and
 * 65104, the same SVCs underneath, with dmnt's own event pump keeping the
 * game running) - so in that case dmnt stays the single debugger, cheats
 * keep working, and we just read through it. Nothing is ever ForceOpened:
 * a game without cheats keeps running debugger-free, exactly as before.
 *
 * Backend choice is per-attach and automatic: direct attach first, dmnt
 * only when it fails because dmnt already holds the game. Callers thread
 * one Handle through the existing capture code unchanged: a dmnt session
 * is the kDmntDbg sentinel, a direct one a real handle. The grc observer
 * paths keep real handles - dmnt only ever tracks the application,
 * never grc.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    /* Sentinel for "this session reads through dmnt:cht". A real kernel
     * handle never looks like this; never pass it to svcCloseHandle. */
    constexpr svc::Handle kDmntDbg = static_cast<svc::Handle>(0x444D4E54u);

    inline bool DbgIsDmnt(svc::Handle dbg) { return dbg == kDmntDbg; }

    /* Direct attach first; dmnt:cht only when our attach fails because dmnt
     * already holds the game. Never ForceOpens: no debugger is attached to
     * a game that ran debugger-free. Logs which backend won. */
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
