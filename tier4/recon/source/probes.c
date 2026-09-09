/* probes.c - Phase 0 recon, v2 (memory-safe, opt-in).
 *
 * v1 crashed `am` with Kernel LimitReached: it held a 9.3 MB static buffer and
 * ran every probe unconditionally at boot. v2 uses one 256 KB buffer, requests
 * tiny captures (we only want the return code + first bytes), and each probe is
 * gated by a keyword in sdmc:/config/tier4-recon/RUN.
 *
 * Every probe logs "-> name" (flushed) before doing anything and "<- name ..."
 * after, so if a probe faults a system service, the log's last line names it.
 */
#include "probes.h"
#include "log.h"

#include <switch.h>
#include <string.h>
#include <stdio.h>

#define BUF_SZ (256 * 1024)
static u8 g_buf[BUF_SZ] __attribute__((aligned(0x1000)));

static unsigned nonzero(const void *p, unsigned n)
{
    const u8 *b = p; unsigned c = 0;
    for (unsigned i = 0; i < n; i++) if (b[i]) c++;
    return c;
}

/* ------------------------------------------------------------------ sys -- */

static void probe_sys(void)
{
    log_mark("sys");
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
        rlog("   fw %u.%u.%u \"%s\" (%s)", fw.major, fw.minor, fw.micro,
             fw.display_version, fw.display_title);

    SetSysProductModel model = SetSysProductModel_Invalid;
    Result rc = setsysGetProductModel(&model);
    rlog("   productModel=%d rc=0x%x  [1=Erista 3=Mariko 4=Lite 6=OLED]", (int)model, rc);
    rlog("   tickfreq=%llu core=%u",
         (unsigned long long)armGetSystemTickFreq(), svcGetCurrentProcessorNumber());
    rlog("<- sys");
}

/* ------------------------------------------------------------------ psm -- */

static void probe_psm(void)
{
    log_mark("psm");
    PsmChargerType ct = 0; u32 pct = 0;
    Result r1 = psmGetChargerType(&ct);
    Result r2 = psmGetBatteryChargePercentage(&pct);
    rlog("   charger=%d (0=none 1=EnoughPower 2=LowPower 3=NotSupported) rc=0x%x  batt=%u%% rc=0x%x",
         ct, r1, pct, r2);
    rlog("<- psm");
}

/* ------------------------------------------------------------------ apm -- */

static void probe_apm(void)
{
    log_mark("apm");
    if (R_SUCCEEDED(apmInitialize())) {
        ApmPerformanceMode pm = ApmPerformanceMode_Invalid;
        Result rc = apmGetPerformanceMode(&pm);
        rlog("   perfMode=%d (0=Normal 1=Boost) rc=0x%x", pm, rc);
        apmExit();
    } else {
        rlog("   apmInitialize failed");
    }
    rlog("<- apm");
}

/* ------------------------------------------------------------------- nv -- */

static void probe_nv(u32 flags)
{
    log_mark("nv");
    if (R_FAILED(nvInitialize())) { rlog("   nvInitialize failed"); rlog("<- nv"); return; }

    static const char *safe[] = {
        "/dev/nvhost-ctrl", "/dev/nvmap", "/dev/nvhost-gpu", "/dev/nvhost-as-gpu",
        "/dev/nvhost-ctrl-gpu", "/dev/nvhost-vic", "/dev/nvhost-msenc",
        "/dev/nvhost-nvdec", "/dev/nvhost-nvjpg",
    };
    static const char *disp[] = {
        "/dev/nvhost-display", "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0", "/dev/nvdisp-disp1",
    };
    for (unsigned i = 0; i < sizeof(safe)/sizeof(safe[0]); i++) {
        u32 fd = 0; Result rc = nvOpen(&fd, safe[i]);
        rlog("   open %-22s rc=0x%-8x fd=%d", safe[i], rc, (int)fd);
        if (R_SUCCEEDED(rc)) nvClose(fd);
    }
    if (flags & P_NV_DISP) {
        for (unsigned i = 0; i < sizeof(disp)/sizeof(disp[0]); i++) {
            rlog("   (RISKY) opening %s ...", disp[i]);
            u32 fd = 0; Result rc = nvOpen(&fd, disp[i]);
            rlog("   open %-22s rc=0x%-8x fd=%d", disp[i], rc, (int)fd);
            if (R_SUCCEEDED(rc)) nvClose(fd);
        }
    }
    nvExit();
    rlog("<- nv");
}

/* ------------------------------------------------------------ caps:sc --- */

static const u32 g_stacks[] = { ViLayerStack_Recording, ViLayerStack_Default, ViLayerStack_Screenshot };
static const char *g_stackn[] = { "Recording", "Default(all)", "Screenshot" };

static void probe_caps_jpeg(void)
{
    log_mark("caps_jpeg:init");
    if (R_FAILED(capsscInitialize())) { rlog("   capsscInitialize failed"); rlog("<- caps_jpeg"); return; }

    for (int si = 0; si < 3; si++) {
        char mk[48];
        snprintf(mk, sizeof(mk), "caps_jpeg:%s", g_stackn[si]);
        log_mark(mk);                       /* .last names the exact stack in flight */

        u64 sz = 0, t0 = armGetSystemTick();
        Result r = capsscCaptureJpegScreenShot(&sz, g_buf, BUF_SZ, g_stacks[si], 1000000000LL);
        u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        int soi = (sz >= 2 && g_buf[0] == 0xFF && g_buf[1] == 0xD8);
        rlog("   jpeg stack=%-12s rc=0x%-8x size=%llu %lluus%s",
             g_stackn[si], r, (unsigned long long)sz, (unsigned long long)us,
             soi ? "  [valid JPEG SOI]" : (sz >= BUF_SZ ? "  [>=buf, truncated]" : ""));
    }
    capsscExit();
    rlog("<- caps_jpeg");
}

/* hand-rolled caps:sc cmd 2 (CaptureRawImageRgba32IntoArrayWithTimeout) - guess */
static void probe_caps_raw(void)
{
    log_mark("caps_raw (hand-rolled cmd 2, EXPERIMENTAL)");
    Service s;
    Result rc = smGetService(&s, "caps:sc");
    if (R_FAILED(rc)) { rlog("   smGetService(caps:sc) rc=0x%x", rc); rlog("<- caps_raw"); return; }

    for (int si = 0; si < 2; si++) {
        const struct {
            u32 layer_stack; u32 pad;
            u64 width; u64 height;
            s64 buffer_count; s64 buffer_index;
            u64 timeout;
        } in = { g_stacks[si], 0, 128, 128, 1, 0, 1000000000ULL };   /* 128x128 = 64 KB */

        memset(g_buf, 0, 128 * 128 * 4);
        u64 t0 = armGetSystemTick();
        Result r = serviceDispatchIn(&s, 2, in,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { g_buf, 128 * 128 * 4 } });
        u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        rlog("   cmd2 stack=%-12s rc=0x%-8x %lluus nonzero=%u  (0x7FECE=stub)",
             g_stackn[si], r, (unsigned long long)us, nonzero(g_buf, 4096));
    }
    serviceClose(&s);
    rlog("<- caps_raw");
}

/* hand-rolled caps:sc cmd 1201/1203/1202 (raw screenshot read stream) - guess */
static void probe_caps_stream(void)
{
    log_mark("caps_stream (hand-rolled cmd 1201/1203, EXPERIMENTAL)");
    Service s;
    if (R_FAILED(smGetService(&s, "caps:sc"))) { rlog("   smGetService failed"); rlog("<- caps_stream"); return; }

    for (int si = 0; si < 2; si++) {
        struct { u32 layer_stack; u32 pad; s64 timeout; } in = { g_stacks[si], 0, 1000000000LL };
        struct { u64 size, width, height; } out = {0};
        Result rc = serviceDispatchInOut(&s, 1201, in, out);
        rlog("   Open  stack=%-12s rc=0x%-8x size=%llu %llux%llu",
             g_stackn[si], rc, (unsigned long long)out.size,
             (unsigned long long)out.width, (unsigned long long)out.height);
        if (R_FAILED(rc)) continue;

        u64 chunk = BUF_SZ;
        struct { u64 offset; } rin = { 0 };
        u64 rout = 0, t0 = armGetSystemTick();
        Result r = serviceDispatchInOut(&s, 1203, rin, rout,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { g_buf, chunk } });
        u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        rlog("   Read  rc=0x%-8x bytes=%llu %lluus nonzero=%u",
             r, (unsigned long long)rout, (unsigned long long)us, nonzero(g_buf, 4096));
        serviceDispatch(&s, 1202);
    }
    serviceClose(&s);
    rlog("<- caps_stream");
}

/* ------------------------------------------------------------- mmio ----- */
/* Only meaningful in the -mmio build (recon-mmio.json declares the map caps). */

static void dc_probe(const char *name, u64 phys, u32 flags)
{
    u64 va = 0, mapped = 0;
    Result rc = svcQueryMemoryMapping(&va, &mapped, phys, 0x40000);
    rlog("   %s: svcQueryMemoryMapping(0x%llx) rc=0x%x va=0x%llx size=0x%llx",
         name, (unsigned long long)phys, rc, (unsigned long long)va, (unsigned long long)mapped);
    if (R_FAILED(rc) || !va || !(flags & P_MMIO_READ)) return;

    rlog("   (RISKY) reading %s registers...", name);
    volatile u32 *r = (volatile u32 *)va;
    for (u32 i = 0; i < 0x20; i += 4)
        rlog("     [0x%03x] %08x %08x %08x %08x", i, r[i], r[i+1], r[i+2], r[i+3]);
    for (u32 i = 0x700; i < 0x71c; i += 4)
        rlog("     [0x%03x] %08x %08x %08x %08x", i, r[i], r[i+1], r[i+2], r[i+3]);
}

static void probe_mmio(u32 flags)
{
    log_mark("mmio");
    dc_probe("DC0", 0x54200000ULL, flags);
    dc_probe("DC1", 0x54240000ULL, flags);
    rlog("<- mmio");
}

/* ---------------------------------------------------------- dispatch ---- */

void run_probes(u32 f, int pass)
{
    /* Known-safe first, then caps:sc (the actual Phase 0 question), then the
     * probes that fatal a system process on 22.5.0 (nv, apm) dead last so
     * they can't rob us of earlier results. */
    if (pass == 0) {
        if (f & P_SYS) probe_sys();
        if (f & P_PSM) probe_psm();
    }

    if (f & P_CAPS_JPEG)   probe_caps_jpeg();
    if (f & P_CAPS_RAW)    probe_caps_raw();
    if (f & P_CAPS_STREAM) probe_caps_stream();

    if (pass == 0) {
        if (f & (P_MMIO_MAP | P_MMIO_READ)) probe_mmio(f);
        if (f & P_NV) {
            rlog("NOTE: nv fatalled a system process in the prior run. Expect a crash.");
            probe_nv(f);
        }
        if (f & P_APM) {
            rlog("NOTE: apm fatalled am in the prior run. Expect a crash.");
            probe_apm();
        }
    }
    if ((f & P_PSM) && pass > 0) probe_psm();   /* loop mode: re-sample charger */
}
