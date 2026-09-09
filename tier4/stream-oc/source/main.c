/* stream-oc - charger-gated overclock companion sysmodule.
 *
 * Runs alongside stock SysDVR. While the console is on a strong-enough charger
 * (and, by default, a game is running) it raises CPU/GPU/EMC to docked/boost
 * clocks so the game doesn't lose frames to the video encoder. On unplug it
 * restores handheld defaults. A reboot resets all clocks regardless.
 *
 * OPT-IN: does nothing unless sdmc:/config/stream-oc/config.ini has enable=1.
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000C10/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000C10/flags/boot2.flag   (empty)
 * Log: sdmc:/stream-oc.log
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "oc.h"

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x40000
char   __nx_inner_heap[INNER_HEAP_SIZE];
size_t __nx_inner_heap_size = INNER_HEAP_SIZE;
void __libnx_initheap(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = __nx_inner_heap;
    fake_heap_end   = __nx_inner_heap + INNER_HEAP_SIZE;
}

static bool g_have_pmdmnt;

void __appInit(void)
{
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(1, 1));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(1, 2));
    fsdevMountSdmc();
    psmInitialize();
    g_have_pmdmnt = R_SUCCEEDED(pmdmntInitialize());
}

void __appExit(void)
{
    if (g_have_pmdmnt) pmdmntExit();
    psmExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

/* ------------------------------------------------------------------ log -- */

#define LOG_PATH "sdmc:/stream-oc.log"
static void lg(const char *fmt, ...)
{
    char b[512];
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    int n = snprintf(b, sizeof(b), "[%6llu.%03llu] ",
                     (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - n - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((unsigned)n > sizeof(b) - 2) n = sizeof(b) - 2;
    b[n++] = '\n'; b[n] = 0;
    svcOutputDebugString(b, n);
    FILE *f = fopen(LOG_PATH, "ab");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}

/* ------------------------------------------------------------------ cfg -- */

struct cfg {
    int enable;
    int require_game;
    int charger_any;          /* 1 = any charger; 0 = EnoughPower only */
    u32 poll_ms;
    oc_profile_t boost;
    oc_profile_t handheld;
};

static void cfg_defaults(struct cfg *c)
{
    c->enable = 0;
    c->require_game = 1;
    c->charger_any = 0;
    c->poll_ms = 2000;
    c->boost    = (oc_profile_t){ 1785000000u, 768000000u, 1600000000u, 0u };
    c->handheld = (oc_profile_t){ 1020000000u, 460800000u, 1331200000u, 0u };
}

static void trim(char *s)
{
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int cfg_load(struct cfg *c)
{
    cfg_defaults(c);
    FILE *f = fopen("sdmc:/config/stream-oc/config.ini", "rb");
    if (!f) return -1;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *h = strpbrk(line, "#;"); if (h) *h = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char key[128], val[128];
        snprintf(key, sizeof(key), "%s", line);
        snprintf(val, sizeof(val), "%s", eq + 1);
        trim(key); trim(val);
        u32 v = (u32)strtoul(val, NULL, 0);

        if      (!strcmp(key, "enable"))          c->enable = atoi(val);
        else if (!strcmp(key, "require_game"))    c->require_game = atoi(val);
        else if (!strcmp(key, "require_charger")) c->charger_any = !strcmp(val, "any");
        else if (!strcmp(key, "poll_ms"))        { if (v >= 250) c->poll_ms = v; }
        else if (!strcmp(key, "cpu_hz"))          c->boost.cpu_hz = v;
        else if (!strcmp(key, "gpu_hz"))          c->boost.gpu_hz = v;
        else if (!strcmp(key, "emc_hz"))          c->boost.emc_hz = v;
        else if (!strcmp(key, "nvenc_hz"))        c->boost.nvenc_hz = v;
        else if (!strcmp(key, "handheld_cpu_hz")) c->handheld.cpu_hz = v;
        else if (!strcmp(key, "handheld_gpu_hz")) c->handheld.gpu_hz = v;
        else if (!strcmp(key, "handheld_emc_hz")) c->handheld.emc_hz = v;
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ main - */

static bool game_running(void)
{
    if (!g_have_pmdmnt) return true;   /* fail-open */
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
}

static const char *chg_name(PsmChargerType t)
{
    switch (t) {
        case PsmChargerType_Unconnected: return "none";
        case PsmChargerType_EnoughPower: return "EnoughPower";
        case PsmChargerType_LowPower:    return "LowPower";
        default:                         return "NotSupported";
    }
}

int main(void)
{
    { FILE *f = fopen(LOG_PATH, "wb"); if (f) fclose(f); }
    lg("stream-oc boot");

    struct cfg c;
    if (cfg_load(&c) != 0 || !c.enable) {
        lg("not enabled (need sdmc:/config/stream-oc/config.ini with enable=1). idling.");
        for (;;) svcSleepThread(60000000000ULL);
    }

    lg("settling 20s...");
    svcSleepThread(20000000000ULL);

    if (oc_init() != 0) { lg("oc_init failed (clkrst). idling."); for (;;) svcSleepThread(60000000000ULL); }

    oc_profile_t base; oc_read(&base);
    lg("baseline: cpu=%u gpu=%u emc=%u nvenc=%u",
       base.cpu_hz, base.gpu_hz, base.emc_hz, base.nvenc_hz);
    lg("config: boost cpu=%u gpu=%u emc=%u nvenc=%u | charger=%s | require_game=%d | poll=%ums",
       c.boost.cpu_hz, c.boost.gpu_hz, c.boost.emc_hz, c.boost.nvenc_hz,
       c.charger_any ? "any" : "EnoughPower", c.require_game, c.poll_ms);

    int boosted = 0, tick = 0;

    for (;;) {
        PsmChargerType ct = PsmChargerType_Unconnected;
        psmGetChargerType(&ct);
        bool charger_ok = c.charger_any ? (ct != PsmChargerType_Unconnected)
                                        : (ct == PsmChargerType_EnoughPower);
        bool want = charger_ok && (!c.require_game || game_running());

        if (want && !boosted) {
            oc_profile_t got;
            int r = oc_apply(&c.boost, &got);
            lg("BOOST  (charger=%s) rc=%d -> cpu=%u gpu=%u emc=%u nvenc=%u",
               chg_name(ct), r, got.cpu_hz, got.gpu_hz, got.emc_hz, got.nvenc_hz);
            boosted = 1;
        } else if (!want && boosted) {
            oc_profile_t got;
            int r = oc_apply(&c.handheld, &got);
            lg("RESTORE (charger=%s) rc=%d -> cpu=%u gpu=%u emc=%u nvenc=%u",
               chg_name(ct), r, got.cpu_hz, got.gpu_hz, got.emc_hz, got.nvenc_hz);
            boosted = 0;
        } else if (want && boosted && (++tick % 15) == 0) {
            oc_apply(&c.boost, NULL);   /* reassert after possible dock/undock DVFS change */
        }

        svcSleepThread((u64)c.poll_ms * 1000000ULL);
    }
    return 0;
}
