#include "log.h"

#include <switch.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_PATH  "sdmc:/tier4-recon.log"
#define LAST_PATH "sdmc:/tier4-recon.last"

static char g_buf[1024];

void log_init(void)
{
    /* fresh file each boot so the log is always current and self-contained */
    FILE *f = fopen(LOG_PATH, "wb");
    if (f) fclose(f);
    f = fopen(LAST_PATH, "wb");
    if (f) { fputs("log_init\n", f); fclose(f); }

    rlog("============================================================");
    rlog("tier4 recon log (fresh this boot)");
}

void log_exit(void) { }

void rlog(const char *fmt, ...)
{
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;

    int n = snprintf(g_buf, sizeof(g_buf), "[%6llu.%03llu] ",
                     (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(g_buf + n, sizeof(g_buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if ((unsigned)n > sizeof(g_buf) - 2) n = sizeof(g_buf) - 2;
    g_buf[n++] = '\n';
    g_buf[n] = 0;

    svcOutputDebugString(g_buf, n);

    /* Open-append-close on every line. The FAT directory entry is committed on
     * fclose, so a hard fatal immediately after a line still leaves it on the
     * card. Slow, but this is a diagnostic that writes only tens of lines. */
    FILE *f = fopen(LOG_PATH, "ab");
    if (f) { fwrite(g_buf, 1, n, f); fclose(f); }
}

/* Write a single-line breadcrumb to sdmc:/tier4-recon.last (truncated each
 * time) naming the probe about to run. Survives even if the main log's tail
 * is lost to FAT damage on a hard fatal. */
void log_mark(const char *what)
{
    FILE *f = fopen(LAST_PATH, "wb");
    if (f) { fputs(what, f); fputc('\n', f); fclose(f); }
    rlog("-> %s", what);
}

void log_hex(const char *label, const void *p, unsigned len)
{
    const unsigned char *b = p;
    char line[3 * 16 + 16];
    rlog("%s (%u bytes):", label, len);
    for (unsigned i = 0; i < len; i += 16) {
        int k = 0;
        k += snprintf(line + k, sizeof(line) - k, "  +%04x  ", i);
        for (unsigned j = 0; j < 16 && i + j < len; j++)
            k += snprintf(line + k, sizeof(line) - k, "%02x ", b[i + j]);
        rlog("%s", line);
    }
}
