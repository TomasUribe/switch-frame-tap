/* tier4 recon sysmodule - Phase 0 diagnostics.
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000C00/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000C00/flags/boot2.flag   (empty)
 * Output:
 *   sdmc:/tier4-recon.log   (also mirrored to the Atmosphere log)
 *
 * It probes once at boot, then re-runs the caps:sc probes every 15 s for ~7 min.
 * While it loops, move between a game, the HOME menu, and System Settings so we
 * can compare what each capture path returns in each foreground state.
 *
 * This module only READS. It maps no RW MMIO and forces no state.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "probes.h"

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x100000
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
        /* keep setsys open - probe_sys uses it */
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

int main(void)
{
    log_init();
    rlog("tier4 recon: boot. settling 3s...");
    svcSleepThread(3000000000ULL);

    probe_sys();
    probe_apm();
    probe_nv();
    probe_mmio();

    for (int pass = 0; pass < 28; pass++) {
        rlog(" ");
        rlog("==== PASS %d  (switch foreground: game / HOME / Settings) ====", pass);
        probe_psm();
        probe_caps(pass);
        svcSleepThread(15000000000ULL);
    }

    rlog("recon finished. Pull sdmc:/tier4-recon.log and send it over.");
    log_exit();
    for (;;) svcSleepThread(60000000000ULL);
    return 0;
}
