/* probes.c - Phase 0 recon. Every probe is best-effort and logs its result;
 * nothing here aborts. Anything marked EXPERIMENTAL is a guess at an
 * undocumented ABI and may return garbage - that's fine, we're mapping it out.
 */
#include "probes.h"
#include "log.h"

#include <switch.h>
#include <string.h>

/* big shared scratch - 1080p RGBA + a JPEG buffer, in .bss */
static u8 g_frame[1920 * 1080 * 4] __attribute__((aligned(0x1000)));
static u8 g_jpeg[1024 * 1024]      __attribute__((aligned(0x1000)));

static unsigned nonzero(const void *p, unsigned n)
{
    const u8 *b = p; unsigned c = 0;
    for (unsigned i = 0; i < n; i++) if (b[i]) c++;
    return c;
}

/* ---------------------------------------------------------------- sys ---- */

void probe_sys(void)
{
    rlog("## probe_sys");

    SetSysFirmwareVersion fw;
    Result rc = setsysGetFirmwareVersion(&fw);
    if (R_SUCCEEDED(rc))
        rlog("  firmware: %u.%u.%u  \"%s\" (%s)",
             fw.major, fw.minor, fw.micro, fw.display_version, fw.display_title);
    else
        rlog("  setsysGetFirmwareVersion: rc=0x%x", rc);

    /* product model: 1=Erista 3=Mariko 4=Hoag(Lite) 6=Aula(OLED) */
    SetSysProductModel model = SetSysProductModel_Invalid;
    rc = setsysGetProductModel(&model);
    rlog("  productModel: %d (rc=0x%x)  [1=Erista 3=Mariko 4=Lite 6=OLED]", (int)model, rc);

    rlog("  tick freq: %llu Hz", (unsigned long long)armGetSystemTickFreq());
    rlog("  running on core %u", svcGetCurrentProcessorNumber());
}

/* ---------------------------------------------------------------- psm ---- */

void probe_psm(void)
{
    PsmChargerType ct = 0;
    u32 pct = 0;
    Result r1 = psmGetChargerType(&ct);
    Result r2 = psmGetBatteryChargePercentage(&pct);
    rlog("  psm: chargerType=%d (0=none 1=EnoughPower 2=LowPower 3=NotSupported) rc=0x%x | battery=%u%% rc=0x%x",
         ct, r1, pct, r2);
}

/* ---------------------------------------------------------------- caps --- */

/* cmd 2: CaptureRawImageRgba32IntoArrayWithTimeout - expected STUB (0x7FECE) on >=5.0.0 */
static Result caps_cmd2(Service *s, u32 stack, u64 w, u64 h, void *out, size_t outsz)
{
    const struct {
        u32 layer_stack; u32 pad;
        u64 width; u64 height;
        s64 buffer_count; s64 buffer_index;
        u64 timeout;
    } in = { stack, 0, w, h, 1, 0, 1000000000ULL };

    return serviceDispatchIn(s, 2, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, outsz } });
}

/* cmd 1201/1203/1202: raw screenshot read stream (needs debug-mode flag) */
static void caps_try_stream(Service *s, u32 stack)
{
    struct { u32 layer_stack; u32 pad; s64 timeout; } in = { stack, 0, 1000000000LL };
    struct { u64 size; u64 width; u64 height; } out = {0};

    Result rc = serviceDispatchInOut(s, 1201, in, out);
    rlog("    OpenRawScreenShotReadStream(stack=%u): rc=0x%x  size=%llu %llux%llu",
         stack, rc, (unsigned long long)out.size,
         (unsigned long long)out.width, (unsigned long long)out.height);
    if (R_FAILED(rc)) return;

    u64 total = out.size < sizeof(g_frame) ? out.size : sizeof(g_frame);
    u64 got = 0, t0 = armGetSystemTick();
    while (got < total) {
        u64 chunk = total - got; if (chunk > 0x80000) chunk = 0x80000;
        struct { u64 offset; } rin = { got };
        u64 rout = 0;
        Result r = serviceDispatchInOut(s, 1203, rin, rout,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { g_frame + got, chunk } });
        if (R_FAILED(r)) { rlog("    ReadRawStream @%llu: rc=0x%x", (unsigned long long)got, r); break; }
        if (rout == 0) break;
        got += rout;
    }
    u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
    rlog("    ReadRawStream: got %llu bytes in %llu us, nonzero=%u",
         (unsigned long long)got, (unsigned long long)us, nonzero(g_frame, got < 65536 ? got : 65536));
    serviceDispatch(s, 1202);
}

void probe_caps(int pass)
{
    rlog("  -- caps:sc (pass %d) --", pass);

    Service caps;
    Result rc = smGetService(&caps, "caps:sc");
    if (R_FAILED(rc)) { rlog("    smGetService(caps:sc): rc=0x%x  (need service_access)", rc); return; }

    const u32 stacks[] = { ViLayerStack_Default, ViLayerStack_Recording, ViLayerStack_Screenshot };
    const char *sn[]   = { "Default(all)", "Recording",  "Screenshot" };
    const u64 res[][2] = { { 1280, 720 }, { 1920, 1080 } };

    for (int si = 0; si < 3; si++) {
        for (int ri = 0; ri < 2; ri++) {
            memset(g_frame, 0, res[ri][0] * res[ri][1] * 4);
            u64 t0 = armGetSystemTick();
            Result r = caps_cmd2(&caps, stacks[si], res[ri][0], res[ri][1], g_frame,
                                 res[ri][0] * res[ri][1] * 4);
            u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
            rlog("    cmd2 raw  stack=%-13s %llux%llu: rc=0x%x  %lluus  nonzero(64k)=%u",
                 sn[si], (unsigned long long)res[ri][0], (unsigned long long)res[ri][1],
                 r, (unsigned long long)us, nonzero(g_frame, 65536));
        }
    }

    /* JPEG path - album / share-applet uses this; verify alive on this fw */
    if (R_SUCCEEDED(capsscInitialize())) {
        for (int si = 0; si < 2; si++) {
            u64 best = ~0ULL, jsz = 0; Result rlast = 0; int ok = 0;
            for (int k = 0; k < 8; k++) {
                u64 t0 = armGetSystemTick(), sz = 0;
                Result r = capsscCaptureJpegScreenShot(&sz, g_jpeg, sizeof(g_jpeg),
                                                       stacks[si], 1000000000LL);
                u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
                rlast = r;
                if (R_SUCCEEDED(r)) { ok++; jsz = sz; if (us < best) best = us; }
            }
            if (ok)
                rlog("    jpeg      stack=%-13s: OK x%d  size~%llukB  best %llums (~%llufps)",
                     sn[si], ok, (unsigned long long)(jsz / 1024),
                     (unsigned long long)(best / 1000), (unsigned long long)(best ? 1000000 / best : 0));
            else
                rlog("    jpeg      stack=%-13s: rc=0x%x (0x7FECE=stub)", sn[si], rlast);
        }
        capsscExit();
    } else {
        rlog("    capsscInitialize failed");
    }

    if (pass == 0) {
        caps_try_stream(&caps, ViLayerStack_Default);
        caps_try_stream(&caps, ViLayerStack_Recording);
    }

    serviceClose(&caps);
}

/* ---------------------------------------------------------------- mmio --- */

static void dc_dump(const char *name, u64 phys)
{
    u64 va = 0, mapped = 0;
    /* [10.0.0+] name; older firmware uses svcLegacyQueryIoMapping(&va, phys, size) */
    Result rc = svcQueryMemoryMapping(&va, &mapped, phys, 0x40000);
    if (R_FAILED(rc) || !va) {
        rlog("    svcQueryMemoryMapping(%s=0x%llx): rc=0x%x va=0x%llx  -> NOT mapped",
             name, (unsigned long long)phys, rc, (unsigned long long)va);
        return;
    }
    rlog("    %s mapped: phys=0x%llx va=0x%llx size=0x%llx  DUMPING word regs",
         name, (unsigned long long)phys, (unsigned long long)va, (unsigned long long)mapped);

    volatile u32 *r = (volatile u32 *)va;
    /* Tegra DC registers are word-indexed. Dump the interesting index windows;
     * decode offline against the TX1 TRM (DC chapter). */
    const u32 ranges[][2] = { {0x000,0x060}, {0x030,0x048}, {0x400,0x440},
                              {0x700,0x730}, {0x800,0x830}, {0xa00,0xa30} };
    for (unsigned g = 0; g < sizeof(ranges)/sizeof(ranges[0]); g++) {
        for (u32 i = ranges[g][0]; i < ranges[g][1]; i += 4)
            rlog("      [0x%03x] %08x %08x %08x %08x", i,
                 r[i + 0], r[i + 1], r[i + 2], r[i + 3]);
        rlog("      ----");
    }
}

void probe_mmio(void)
{
    rlog("## probe_mmio");
    dc_dump("DC0", 0x54200000ULL);
    dc_dump("DC1", 0x54240000ULL);

    /* also: what does the kernel let us see of our own address space? */
    rlog("  address-space scan (non-free regions, MemType: 3=Io 4=Static 5=Code ...):");
    u64 addr = 0; int lines = 0;
    while (addr < 0x100000000ULL && lines < 120) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
        if (mi.type != 0) {
            rlog("    0x%010llx +0x%09llx  type=%2u perm=%u attr=%u",
                 (unsigned long long)mi.addr, (unsigned long long)mi.size,
                 mi.type, mi.perm, mi.attr);
            lines++;
        }
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
}

/* ---------------------------------------------------------------- nv ----- */

void probe_nv(void)
{
    rlog("## probe_nv");
    if (R_FAILED(nvInitialize())) { rlog("  nvInitialize failed"); return; }

    static const char *paths[] = {
        "/dev/nvhost-ctrl", "/dev/nvmap", "/dev/nvhost-gpu", "/dev/nvhost-as-gpu",
        "/dev/nvhost-ctrl-gpu", "/dev/nvhost-vic", "/dev/nvhost-msenc",
        "/dev/nvhost-nvdec", "/dev/nvhost-nvjpg", "/dev/nvhost-display",
        "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0", "/dev/nvdisp-disp1",
    };
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        u32 fd = 0;
        Result rc = nvOpen(&fd, paths[i]);
        rlog("  open %-22s rc=0x%-8x fd=%d", paths[i], rc, (int)fd);
        if (R_SUCCEEDED(rc)) nvClose(fd);
    }
    nvExit();
}

/* ---------------------------------------------------------------- apm ---- */

void probe_apm(void)
{
    rlog("## probe_apm");
    if (R_SUCCEEDED(apmInitialize())) {
        ApmPerformanceMode pm = ApmPerformanceMode_Invalid;
        Result rc = apmGetPerformanceMode(&pm);
        rlog("  apmGetPerformanceMode: %d (0=Normal 1=Boost) rc=0x%x", pm, rc);
        apmExit();
    } else {
        rlog("  apmInitialize failed");
    }
}
