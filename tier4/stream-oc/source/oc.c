#include "oc.h"
#include <string.h>

/* clkrst is [8.0.0+]. Module IDs from libnx pcv.h. */
#define M_CPU   PcvModuleId_CpuBus   /* 0x40000001 */
#define M_GPU   PcvModuleId_GPU      /* 0x40000002 */
#define M_EMC   PcvModuleId_EMC      /* 0x40000039 */
#define M_NVENC PcvModuleId_NVENC    /* 0x40000030 */

static bool g_ok;

int oc_init(void)
{
    if (hosversionBefore(8, 0, 0)) return -1;
    Result rc = clkrstInitialize();
    g_ok = R_SUCCEEDED(rc);
    return g_ok ? 0 : -1;
}

void oc_exit(void)
{
    if (g_ok) clkrstExit();
    g_ok = false;
}

/* Snap `want` to the highest allowed rate <= want (or the lowest if want is
 * below the whole table). Returns 0 if the rate list can't be read. */
static u32 snap_rate(PcvModuleId mid, u32 want)
{
    ClkrstSession s;
    if (R_FAILED(clkrstOpenSession(&s, mid, 3))) return 0;

    u32 rates[40];
    s32 count = 0;
    PcvClockRatesListType type = 0;
    Result rc = clkrstGetPossibleClockRates(&s, rates, 40, &type, &count);
    clkrstCloseSession(&s);
    if (R_FAILED(rc) || count <= 0) return 0;

    u32 best = rates[0];
    for (s32 i = 0; i < count; i++) {
        if (rates[i] <= want && rates[i] > best) best = rates[i];
        if (best > want && rates[i] < best) best = rates[i];   /* keep lowest if all > want */
    }
    /* if every entry is <= want, take the max */
    if (best < want) {
        for (s32 i = 0; i < count; i++) if (rates[i] > best) best = rates[i];
    }
    return best;
}

static int set_one(PcvModuleId mid, u32 want, u32 *out_applied)
{
    if (out_applied) *out_applied = 0;
    if (!want) return 0;

    u32 hz = snap_rate(mid, want);
    if (!hz) hz = want;   /* couldn't read the table; try the raw value */

    ClkrstSession s;
    if (R_FAILED(clkrstOpenSession(&s, mid, 3))) return -1;
    Result rc = clkrstSetClockRate(&s, hz);
    if (R_SUCCEEDED(rc) && out_applied) clkrstGetClockRate(&s, out_applied);
    clkrstCloseSession(&s);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

static u32 get_one(PcvModuleId mid)
{
    ClkrstSession s;
    if (R_FAILED(clkrstOpenSession(&s, mid, 3))) return 0;
    u32 hz = 0;
    clkrstGetClockRate(&s, &hz);
    clkrstCloseSession(&s);
    return hz;
}

int oc_apply(const oc_profile_t *p, oc_profile_t *applied)
{
    if (!g_ok) return -1;
    oc_profile_t a = {0};
    int r = 0;
    r |= set_one(M_CPU,   p->cpu_hz,   &a.cpu_hz);
    r |= set_one(M_GPU,   p->gpu_hz,   &a.gpu_hz);
    r |= set_one(M_EMC,   p->emc_hz,   &a.emc_hz);
    r |= set_one(M_NVENC, p->nvenc_hz, &a.nvenc_hz);
    if (applied) *applied = a;
    return r;
}

void oc_read(oc_profile_t *out)
{
    out->cpu_hz   = get_one(M_CPU);
    out->gpu_hz   = get_one(M_GPU);
    out->emc_hz   = get_one(M_EMC);
    out->nvenc_hz = get_one(M_NVENC);
}
