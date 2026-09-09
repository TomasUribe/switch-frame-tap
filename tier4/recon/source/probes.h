#ifndef TIER4_PROBES_H
#define TIER4_PROBES_H

#include <switch.h>

/* which probes to run - selected by keywords in sdmc:/config/tier4-recon/RUN */
enum {
    P_SYS         = 1u << 0,   /* firmware / model / ticks              (safe)   */
    P_PSM         = 1u << 1,   /* charger type / battery                (safe)   */
    P_APM         = 1u << 2,   /* performance mode                      (safe)   */
    P_NV          = 1u << 3,   /* open common /dev/nvhost* + /dev/nvmap (safe-ish) */
    P_NV_DISP     = 1u << 4,   /* also try /dev/nvdisp-*                (RISKY)  */
    P_CAPS_JPEG   = 1u << 5,   /* libnx capsscCaptureJpegScreenShot     (medium) */
    P_CAPS_RAW    = 1u << 6,   /* caps:sc cmd 2 (libnx)                          */
    P_CAPS_STREAM = 1u << 7,   /* caps:sc cmd 1201/1203 raw stream (libnx)       */
    P_CAPS_ATTACH = 1u << 11,  /* caps:sc cmd 3+5 shared-buffer  (EXPERIMENTAL)  */
    P_MMIO_MAP    = 1u << 8,   /* svcQueryMemoryMapping only (needs -mmio build)  */
    P_MMIO_READ   = 1u << 9,   /* dereference mapped DC regs (needs -mmio build) (RISKY) */
    P_LOOP        = 1u << 10,  /* repeat the caps probes ~20 passes, 15s apart    */
};

void run_probes(u32 flags, int pass);

#endif
