/* log.h - dead-simple line logger for the Phase 0 recon sysmodule (renamed from logf: clashes with math.h).
 * Writes to sdmc:/tier4-recon.log (append, flushed every line) and also to
 * svcOutputDebugString so it shows up in the Atmosphere log if enabled. */
#ifndef TIER4_LOG_H
#define TIER4_LOG_H

void log_init(void);
void log_exit(void);
void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* hexdump `len` bytes of `p` with a label, 16 bytes/line */
void log_hex(const char *label, const void *p, unsigned len);

#endif
