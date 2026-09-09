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

    /* did the system_settings.ini debug-mode override take? */
    u8 dbg = 0xEE; u64 osz = 0;
    Result dr = setsysGetSettingsItemValue("settings_debug", "is_debug_mode_enabled",
                                           &dbg, sizeof(dbg), &osz);
    rlog("   settings_debug!is_debug_mode_enabled = 0x%02x (rc=0x%x outsz=%llu)",
         dbg, dr, (unsigned long long)osz);
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

/* caps:sc cmd 2 - CaptureRawImageRgba32IntoArrayWithTimeout (libnx wrapper).
 * switchbrew says stubbed since [5.0.0] -> 0x7FECE (2206-1023). Confirm. */
static void probe_caps_raw(void)
{
    log_mark("caps_raw:init");
    if (R_FAILED(capsscInitialize())) { rlog("   capsscInitialize failed"); rlog("<- caps_raw"); return; }

    for (int si = 0; si < 3; si++) {
        char mk[48]; snprintf(mk, sizeof(mk), "caps_raw:%s", g_stackn[si]); log_mark(mk);
        memset(g_buf, 0, 128 * 128 * 4);
        u64 t0 = armGetSystemTick();
        Result r = capsscCaptureRawImageWithTimeout(g_buf, 128 * 128 * 4, g_stacks[si],
                                                    128, 128, 1, 0, 1000000000LL);
        u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        rlog("   cmd2 stack=%-12s rc=0x%-8x %lluus nonzero=%u   (0x7FECE=stub)",
             g_stackn[si], r, (unsigned long long)us, nonzero(g_buf, 4096));
    }
    capsscExit();
    rlog("<- caps_raw");
}

/* Read the whole open stream in BUF_SZ chunks (data discarded). Returns total
 * bytes; *first8 gets the first 8 bytes of the frame. */
static u64 stream_drain(u8 first8[8])
{
    u64 off = 0;
    for (;;) {
        u64 got = 0;
        Result r = capsscReadRawScreenShotReadStream(&got, g_buf, BUF_SZ, off);
        if (R_FAILED(r)) { rlog("     read @%llu rc=0x%x", (unsigned long long)off, r); break; }
        if (off == 0 && got >= 8) memcpy(first8, g_buf, 8);
        if (got == 0) break;
        off += got;
        if (off > 64ull * 1024 * 1024) break;   /* safety */
    }
    return off;
}

/* caps:sc cmd 1201/1203/1202 - raw screenshot read stream (libnx wrapper).
 * Gated by set:sys GetDebugModeFlag  (settings_debug!is_debug_mode_enabled).
 * If it opens: report resolution, then time 5 full-frame reads for an fps
 * estimate. */
static void probe_caps_stream(void)
{
    log_mark("caps_stream:init");
    if (R_FAILED(capsscInitialize())) { rlog("   capsscInitialize failed"); rlog("<- caps_stream"); return; }

    for (int si = 0; si < 3; si++) {
        char mk[48]; snprintf(mk, sizeof(mk), "caps_stream:%s", g_stackn[si]); log_mark(mk);

        u64 sz = 0, w = 0, h = 0;
        Result rc = capsscOpenRawScreenShotReadStream(&sz, &w, &h, g_stacks[si], 1000000000LL);
        rlog("   Open  stack=%-12s rc=0x%-8x size=%llu (%llux%llu, %llu B/px)",
             g_stackn[si], rc, (unsigned long long)sz,
             (unsigned long long)w, (unsigned long long)h,
             (unsigned long long)((w && h) ? sz / (w * h) : 0));
        if (R_FAILED(rc)) continue;

        u8 f8[8] = {0};
        u64 b0 = stream_drain(f8);
        capsscCloseRawScreenShotReadStream();
        rlog("   frame0: %llu bytes  first8=%02x %02x %02x %02x %02x %02x %02x %02x",
             (unsigned long long)b0, f8[0], f8[1], f8[2], f8[3], f8[4], f8[5], f8[6], f8[7]);

        /* fps: 5x open/drain/close */
        u64 best = ~0ULL, sum = 0; int ok = 0;
        for (int k = 0; k < 5; k++) {
            u64 t0 = armGetSystemTick();
            if (R_FAILED(capsscOpenRawScreenShotReadStream(&sz, &w, &h, g_stacks[si], 1000000000LL))) break;
            stream_drain(f8);
            capsscCloseRawScreenShotReadStream();
            u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000;
            if (ms < best) best = ms;
            sum += ms; ok++;
        }
        if (ok)
            rlog("   timing: %d frames, best %llums (~%llu fps), avg %llums (~%llu fps)",
                 ok, (unsigned long long)best, (unsigned long long)(best ? 1000 / best : 0),
                 (unsigned long long)(sum / ok), (unsigned long long)(sum ? 1000 * ok / sum : 0));
    }
    capsscExit();
    rlog("<- caps_stream");
}

/* caps:sc cmd 3 + cmd 5 - AttachSharedBufferToCaptureModule /
 * CaptureRawImageToAttachedSharedBuffer. Not wrapped by libnx. switchbrew:
 * cmd3 = 8 bytes in / no out; cmd5 = 0x10 bytes in / no out.
 * We try a few plausible ABIs, all EXPERIMENTAL - garbage return codes are
 * fine, we're mapping it. Buffer is our 256 KB g_buf via TransferMemory. */
static void probe_caps_attach(void)
{
    log_mark("caps_attach:init");
    Service s;
    if (R_FAILED(smGetService(&s, "caps:sc"))) { rlog("   smGetService(caps:sc) failed"); rlog("<- caps_attach"); return; }

    TransferMemory tmem;
    Result rc = tmemCreateFromMemory(&tmem, g_buf, BUF_SZ, Perm_Rw);
    rlog("   tmemCreateFromMemory(256KB) rc=0x%x handle=0x%x", rc, tmem.handle);
    if (R_FAILED(rc)) { serviceClose(&s); rlog("<- caps_attach"); return; }

    /* cmd 3 variant A: u64 size in, transfer-memory as copy handle */
    {
        u64 in = BUF_SZ;
        Result r = serviceDispatchIn(&s, 3, in,
            .in_num_handles = 1, .in_handles = { tmem.handle });
        rlog("   cmd3(A: size+handle) rc=0x%x", r);
    }
    /* cmd 3 variant B: (s32 index, u32 pad), no handle */
    {
        struct { s32 index; u32 pad; } in = { 0, 0 };
        Result r = serviceDispatchIn(&s, 3, in);
        rlog("   cmd3(B: index,no handle) rc=0x%x", r);
    }

    /* cmd 5 variant A: (s32 layer_stack, u32 pad, s64 timeout) */
    for (int si = 0; si < 2; si++) {
        char mk[48]; snprintf(mk, sizeof(mk), "caps_attach:cmd5:%s", g_stackn[si]); log_mark(mk);
        memset(g_buf, 0, 4096);
        struct { s32 layer_stack; u32 pad; s64 timeout; } in = { (s32)g_stacks[si], 0, 1000000000LL };
        u64 t0 = armGetSystemTick();
        Result r = serviceDispatchIn(&s, 5, in);
        u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        rlog("   cmd5(A) stack=%-12s rc=0x%-8x %lluus  buf: %02x %02x %02x %02x  nonzero=%u",
             g_stackn[si], r, (unsigned long long)us,
             g_buf[0], g_buf[1], g_buf[2], g_buf[3], nonzero(g_buf, 4096));
    }
    /* cmd 5 variant B: (u64 width, u64 height) */
    {
        memset(g_buf, 0, 4096);
        struct { u64 w, h; } in = { 256, 256 };
        Result r = serviceDispatchIn(&s, 5, in);
        rlog("   cmd5(B: 256x256) rc=0x%x  nonzero=%u", r, nonzero(g_buf, 4096));
    }

    tmemClose(&tmem);
    serviceClose(&s);
    rlog("<- caps_attach");
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
    if (f & P_CAPS_ATTACH) probe_caps_attach();

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
