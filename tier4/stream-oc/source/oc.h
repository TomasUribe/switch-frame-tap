/* oc.h - CPU/GPU/EMC(/NVENC) clock control via clkrst.
 * Requested rates are snapped to the nearest allowed DVFS table entry. */
#ifndef STREAM_OC_OC_H
#define STREAM_OC_OC_H

#include <switch.h>

typedef struct {
    u32 cpu_hz;    /* 0 = leave module alone */
    u32 gpu_hz;
    u32 emc_hz;
    u32 nvenc_hz;
} oc_profile_t;

int  oc_init(void);
void oc_exit(void);

/* Apply a profile (each non-zero field). Returns 0 on success.
 * On return, `applied` (if non-NULL) holds the rates actually set. */
int  oc_apply(const oc_profile_t *p, oc_profile_t *applied);

/* Read current rates into `out`. */
void oc_read(oc_profile_t *out);

#endif
