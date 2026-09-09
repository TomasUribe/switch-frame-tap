/* tier4 recon sysmodule - Phase 0 diagnostics, v2 (armed / self-disarming).
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000C00/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000C00/flags/boot2.flag   (empty)
 *
 * SAFETY: on boot the module does NOTHING unless the file
 *   sdmc:/config/tier4-recon/RUN
 * exists. If it does, the module DELETES it first (so a crash cannot loop),
 * then runs only the probes named in it. Put keywords in that file, one or
 * more, separated by spaces / newlines:
 *
 *   sys psm apm nv            - always safe
 *   caps_jpeg                 - libnx JPEG capture (medium risk)
 *   nv_disp caps_raw caps_stream mmio_map mmio_read   - RISKY, opt-in one at a time
 *   loop                      - repeat the caps probes ~20x, 15s apart
 *   all_safe                  - sys psm apm nv caps_jpeg
 *
 * An empty RUN file = "sys psm".
 * Output: sdmc:/tier4-recon.log  (flushed every line; also to the AMS log)
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "probes.h"

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x80000
char   __nx_inner_heap[INNER_HEAP_SIZE];
size_t __nx_inner_heap_size = INNER_HEAP_SIZE;

void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = __nx_inner_heap;
    fake_heap_end   = __nx_inner_heap + INNER_HEAP_SIZE;
}

void __appInit(void)
{
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(1, 1));

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    }

    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(1, 2));
    fsdevMountSdmc();
    psmInitialize();
}

void __appExit(void)
{
    psmExit();
    setsysExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

#define RUN_DIR  "sdmc:/config/tier4-recon"
#define RUN_FILE RUN_DIR "/RUN"

static u32 keyword(const char *w)
{
    if (!strcmp(w, "sys"))         return P_SYS;
    if (!strcmp(w, "psm"))         return P_PSM;
    if (!strcmp(w, "apm"))         return P_APM;
    if (!strcmp(w, "nv"))          return P_NV;
    if (!strcmp(w, "nv_disp"))     return P_NV | P_NV_DISP;
    if (!strcmp(w, "caps_jpeg"))   return P_CAPS_JPEG;
    if (!strcmp(w, "caps_raw"))    return P_CAPS_RAW;
    if (!strcmp(w, "caps_stream")) return P_CAPS_STREAM;
    if (!strcmp(w, "mmio_map"))    return P_MMIO_MAP;
    if (!strcmp(w, "mmio_read"))   return P_MMIO_MAP | P_MMIO_READ;
    if (!strcmp(w, "loop"))        return P_LOOP;
    /* NB: apm is NOT in all_safe - v1 fatalled am inside apmInitialize/apmGetPerformanceMode */
    if (!strcmp(w, "all_safe"))    return P_SYS | P_PSM | P_NV | P_CAPS_JPEG;
    return 0;
}

/* returns selected flags, or 0xFFFFFFFF if not armed */
static u32 read_run_file(void)
{
    FILE *f = fopen(RUN_FILE, "rb");
    if (!f) return 0xFFFFFFFFu;

    char b[512] = {0};
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    remove(RUN_FILE);            /* self-disarm before doing anything */

    u32 fl = 0; int any = 0;
    for (char *w = strtok(b, " \t\r\n"); w; w = strtok(NULL, " \t\r\n")) {
        any = 1;
        u32 k = keyword(w);
        if (k) fl |= k;
        else   rlog("   (ignored unknown keyword \"%s\")", w);
    }
    if (!any) fl = P_SYS | P_PSM;
    return fl;
}

int main(void)
{
    log_init();
    mkdir("sdmc:/config", 0777);
    mkdir(RUN_DIR, 0777);

    u32 fl = read_run_file();
    if (fl == 0xFFFFFFFFu) {
        rlog("NOT ARMED.");
        rlog("  Create %s with keywords to run one batch of probes.", RUN_FILE);
        rlog("  e.g.  echo 'sys psm caps_jpeg' > <sd>/config/tier4-recon/RUN");
        rlog("  It self-deletes on boot, so a crash can never loop. Idling.");
        log_exit();
        for (;;) svcSleepThread(60000000000ULL);
    }

    rlog("ARMED flags=0x%03x. Settling 20s (let am / HOME finish booting)...", fl);
    svcSleepThread(20000000000ULL);

    int passes = (fl & P_LOOP) ? 20 : 1;
    for (int p = 0; p < passes; p++) {
        rlog(" ");
        rlog("======== PASS %d / %d ========", p, passes);
        run_probes(fl, p);
        if (passes > 1 && p + 1 < passes) svcSleepThread(15000000000ULL);
    }

    rlog("DONE. Re-create the RUN file and reboot to run another batch.");
    rlog("Pull sdmc:/tier4-recon.log");
    log_exit();
    for (;;) svcSleepThread(60000000000ULL);
    return 0;
}
