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

namespace ams::mitm::applet {

    void LogInit();
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    /* breadcrumb: overwrite sdmc:/applet-mitm.last, then LogLine("-> what") */
    void LogMark(const char *what);

}
