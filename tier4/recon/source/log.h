/* log.h - crash-durable line logger for the Phase 0 recon sysmodule.
 * (rlog, not logf: logf clashes with the C builtin from math.h)
 *
 * sdmc:/tier4-recon.log   - fresh each boot, one open/append/close per line
 *                           so a hard fatal cannot lose committed lines
 * sdmc:/tier4-recon.last  - single line: the probe currently in flight
 * also mirrored to svcOutputDebugString (Atmosphere log). */
#ifndef TIER4_LOG_H
#define TIER4_LOG_H

void log_init(void);
void log_exit(void);
void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* breadcrumb: write `what` to sdmc:/tier4-recon.last (truncated), then rlog("-> what") */
void log_mark(const char *what);

/* hexdump `len` bytes of `p` with a label, 16 bytes/line */
void log_hex(const char *label, const void *p, unsigned len);

#endif
