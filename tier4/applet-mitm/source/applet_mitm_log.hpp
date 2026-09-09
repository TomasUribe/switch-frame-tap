/*
 * applet-mitm - Track B / M1
 * Tiny SD-card line logger (libstratosphere fs).
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    void LogInit();                       /* mount sdmc, truncate the log */
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}
