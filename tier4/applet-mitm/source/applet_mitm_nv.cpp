/*
 * applet-mitm - M4b: hand-rolled nvdrv access
 *
 * M4 fatalled inside libnx's nvInitialize() with no rc logged - it aborted
 * rather than returning an error. nvInitialize() does five things (sm lookup,
 * tmem alloc via the allocator, cmd 3, serviceCloneEx, appletGetAppletResourceUserId)
 * so a single breadcrumb around it told us nothing.
 *
 * This hand-rolls the sequence with a breadcrumb before EVERY step, and uses a
 * static page-aligned buffer + tmemCreateFromMemory so libstratosphere's
 * allocator is not involved. It also skips serviceCloneEx and the aruid call,
 * which we do not need.
 *
 * nvdrv IPC (from libnx nx/source/services/nv.c):
 *   cmd 0 Open      : InBuffer(MapAlias) = path (strlen, no NUL) -> {u32 fd, u32 error}
 *   cmd 1 Ioctl     : in {u32 fd, u32 request}, out u32 error,
 *                     buffers = AutoSelect In + AutoSelect Out, both = argp
 *   cmd 3 Initialize: in u32 transfermem_size, handles {process, tmem}
 */
#include "applet_mitm_nv.hpp"
#include "applet_mitm_log.hpp"
#include <atomic>
#include <cstring>

namespace ams::mitm::applet {

    namespace {

        constinit std::atomic<bool> g_probe_done{false};

        /* Static so the allocator is never in the path. nvdrv uses this as
         * scratch for ioctl buffers; ours are a few bytes. */
        alignas(0x1000) constinit u8 g_nv_tmem_buf[0x40000] = {};

        constinit ::Service g_nv_srv = {};
        constinit ::TransferMemory g_nv_tmem = {};

        /* Linux-style ioctl encoding: (dir<<30)|(size<<16)|(type<<8)|nr, IOWR dir=3 */
        constexpr u32 MakeIowr(u32 type, u32 nr, u32 size) {
            return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 NvmapIocFromId = MakeIowr(0x01, 0x03, 8);   /* {u32 id; u32 handle;}          */
        constexpr u32 NvmapIocParam  = MakeIowr(0x01, 0x09, 12);  /* {u32 handle; u32 param; u32 v;} */
        constexpr u32 NvMapParamSize = 1;
        constexpr u32 NvMapParamKind = 5;

        ::Result NvIoctl(u32 fd, u32 request, void *argp, size_t argsz, u32 *out_err) {
            const struct { u32 fd; u32 request; } in = { fd, request };
            u32 error = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 1, in, error,
                .buffer_attrs = {
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
                },
                .buffers = { { argp, argsz }, { argp, argsz } },
            );
            *out_err = error;
            return rc;
        }

    }

    void TryNvmapProbe(u32 nvmap_id) {
        bool expected = false;
        if (!g_probe_done.compare_exchange_strong(expected, true)) {
            return;
        }

        LogMark("nv:1_smGetService");
        ::Result rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
        LogLine("   smGetService(\"nvdrv:s\") rc=0x%x", rc);
        if (R_FAILED(rc)) { LogMark("nv:1_FAILED"); return; }

        LogMark("nv:2_tmemCreateFromMemory");
        rc = tmemCreateFromMemory(std::addressof(g_nv_tmem), g_nv_tmem_buf, sizeof(g_nv_tmem_buf), Perm_None);
        LogLine("   tmemCreateFromMemory(0x%zx) rc=0x%x handle=0x%x",
                sizeof(g_nv_tmem_buf), rc, g_nv_tmem.handle);
        if (R_FAILED(rc)) { LogMark("nv:2_FAILED"); return; }

        LogMark("nv:3_Initialize");
        {
            const u32 tmem_size = static_cast<u32>(sizeof(g_nv_tmem_buf));
            rc = serviceDispatchIn(std::addressof(g_nv_srv), 3, tmem_size,
                .in_num_handles = 2,
                .in_handles     = { CUR_PROCESS_HANDLE, g_nv_tmem.handle },
            );
            LogLine("   nvdrv Initialize rc=0x%x", rc);
            if (R_FAILED(rc)) { LogMark("nv:3_FAILED"); return; }
        }

        u32 fd = 0;
        LogMark("nv:4_open_nvmap");
        {
            struct { u32 fd; u32 error; } out = {};
            const char *path = "/dev/nvmap";
            rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers      = { { path, std::strlen(path) } },
            );
            LogLine("   Open(\"/dev/nvmap\") rc=0x%x fd=%u nverr=%u", rc, out.fd, out.error);
            if (R_FAILED(rc) || out.error != 0) { LogMark("nv:4_FAILED"); return; }
            fd = out.fd;
        }

        u32 handle = 0;
        LogMark("nv:5_FROM_ID");
        {
            struct { u32 id; u32 handle; } args = { nvmap_id, 0 };
            u32 nverr = 0;
            rc = NvIoctl(fd, NvmapIocFromId, std::addressof(args), sizeof(args), std::addressof(nverr));
            LogLine("   FROM_ID(id=%u) rc=0x%x nverr=%u -> handle=%u", nvmap_id, rc, nverr, args.handle);
            if (R_FAILED(rc) || nverr != 0) { LogMark("nv:5_FAILED"); return; }
            handle = args.handle;
        }

        LogMark("nv:6_PARAM");
        {
            struct { u32 handle; u32 param; u32 value; } args = { handle, NvMapParamSize, 0 };
            u32 nverr = 0;
            rc = NvIoctl(fd, NvmapIocParam, std::addressof(args), sizeof(args), std::addressof(nverr));
            LogLine("   PARAM(Size) rc=0x%x nverr=%u -> %u B (%u MB)",
                    rc, nverr, args.value, args.value / (1024 * 1024));

            args = { handle, NvMapParamKind, 0 };
            nverr = 0;
            rc = NvIoctl(fd, NvmapIocParam, std::addressof(args), sizeof(args), std::addressof(nverr));
            LogLine("   PARAM(Kind) rc=0x%x nverr=%u -> 0x%x", rc, nverr, args.value);
        }

        LogMark("nv:7_engine_survey");
        {
            /* Which engines can we reach? The VIC is the one that matters: it
             * reads a block-linear surface and writes a linear one, doing the
             * de-swizzle AND format conversion in hardware. That is how
             * nvnflinger consumes these very buffers. msenc is NVENC, for the
             * encode stage later. */
            static const char *const nodes[] = {
                "/dev/nvhost-vic",
                "/dev/nvhost-msenc",
                "/dev/nvhost-gpu",
                "/dev/nvhost-as-gpu",
                "/dev/nvhost-ctrl",
                "/dev/nvhost-ctrl-gpu",
                "/dev/nvhost-nvdec",
                "/dev/nvhost-nvjpg",
            };
            for (const char *path : nodes) {
                struct { u32 fd; u32 error; } out = {};
                const ::Result r = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                    .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                    .buffers      = { { path, std::strlen(path) } },
                );
                LogLine("   open %-22s rc=0x%-8x fd=%-10u nverr=%u", path, r, out.fd, out.error);
            }
        }

        /* Release everything. A probe must not hold an nvdrv session or a
         * handle on the game's buffer - doing so is what wedged homebrew. */
        LogMark("nv:8_cleanup");
        serviceClose(std::addressof(g_nv_srv));
        tmemClose(std::addressof(g_nv_tmem));

        LogMark("nv:DONE_OPENED");
        LogLine("*** nvmap object %u reachable (handle=%u, 25 MB = the full swapchain); session released",
                nvmap_id, handle);
    }

}
