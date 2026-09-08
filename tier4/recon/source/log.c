#include "log.h"

#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "sdmc:/tier4-recon.log"

static FILE *g_f;
static char  g_buf[1024];

void log_init(void)
{
    g_f = fopen(LOG_PATH, "ab");
    logf("");
    logf("============================================================");
}

void log_exit(void)
{
    if (g_f) { fflush(g_f); fclose(g_f); g_f = NULL; }
}

void logf(const char *fmt, ...)
{
    u64 t = armGetSystemTick();
    u64 ms = armTicksToNs(t) / 1000000ULL;

    int n = snprintf(g_buf, sizeof(g_buf), "[%6llu.%03llu] ",
                     (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));

    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(g_buf + n, sizeof(g_buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((unsigned)n > sizeof(g_buf) - 2) n = sizeof(g_buf) - 2;
    g_buf[n++] = '\n';
    g_buf[n] = 0;

    svcOutputDebugString(g_buf, n);
    if (g_f) { fwrite(g_buf, 1, n, g_f); fflush(g_f); }
}

void log_hex(const char *label, const void *p, unsigned len)
{
    const unsigned char *b = p;
    char line[3 * 16 + 16];
    logf("%s (%u bytes):", label, len);
    for (unsigned i = 0; i < len; i += 16) {
        int k = 0;
        k += snprintf(line + k, sizeof(line) - k, "  +%04x  ", i);
        for (unsigned j = 0; j < 16 && i + j < len; j++)
            k += snprintf(line + k, sizeof(line) - k, "%02x ", b[i + j]);
        logf("%s", line);
    }
}
