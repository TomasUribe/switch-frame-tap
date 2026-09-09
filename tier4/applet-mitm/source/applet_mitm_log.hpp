/*
 * applet-mitm - Track B
 * SD-card line logger (libstratosphere fs) + a crash-durable breadcrumb.
 *
 *   sdmc:/applet-mitm.log   append, one open/write/close per line
 *   sdmc:/applet-mitm.last  single line: the step currently in flight
 *
 * A hard fatal can lose the tail of the .log before FAT commits; .last is
 * rewritten+closed on every mark, so it survives.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    /* Live counters. The heartbeat snapshots these into .last, so even a run
     * that ends in a forced power-off (which can lose the .log tail before FAT
     * commits) still tells us how far the module got: did it ever get a
     * session, did the wrapper chain run, were frames flowing. */
    struct Stats {
        std::atomic<u32> sessions;   /* OnNeedsToAccept calls   */
        std::atomic<u32> getdisp;    /* GetDisplayService calls */
        std::atomic<u32> relay;      /* GetRelayService calls   */
        std::atomic<u32> txns;       /* binder transactions     */
    };
    extern Stats g_stats;

    void LogInit();
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    /* breadcrumb: overwrite sdmc:/applet-mitm.last, then LogLine("-> what") */
    void LogMark(const char *what);

}
