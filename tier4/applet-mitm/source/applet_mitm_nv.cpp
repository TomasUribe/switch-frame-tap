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

        /* Destination for a VIC blit must be memory we own; page-aligned. */
        alignas(0x1000) constinit u8 g_own_buf[0x10000] = {};

        constinit ::Service g_nv_srv = {};
        constinit ::TransferMemory g_nv_tmem = {};

        /* Linux-style ioctl encoding: (dir<<30)|(size<<16)|(type<<8)|nr, IOWR dir=3 */
        constexpr u32 MakeIowr(u32 type, u32 nr, u32 size) {
            return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 NvmapIocCreate = MakeIowr(0x01, 0x01, 8);   /* {u32 size; u32 handle;}        */
        constexpr u32 NvmapIocFromId = MakeIowr(0x01, 0x03, 8);   /* {u32 id; u32 handle;}          */
        constexpr u32 NvmapIocAlloc  = MakeIowr(0x01, 0x04, 32);  /* handle/heapmask/flags/align/kind/addr */
        constexpr u32 NvmapIocGetId  = MakeIowr(0x01, 0x0E, 8);   /* {u32 id; u32 handle;}          */
        constexpr u32 NvmapIocParam  = MakeIowr(0x01, 0x09, 12);  /* {u32 handle; u32 param; u32 v;} */
        constexpr u32 NvMapParamSize = 1;
        constexpr u32 NvMapParamKind = 5;

        /* nvhost host1x channel ioctls (type 'H' = 0x00). Sizes per switchbrew. */
        constexpr u32 NvHostIocChannelGetSyncpoint  = MakeIowr(0x00, 0x02, 8);   /* {u32 module_id; u32 syncpt}  */
        constexpr u32 NvHostIocChannelSetSubmitTo   = (UINT32_C(1) << 30) | (4 << 16) | (0x00 << 8) | 0x07; /* _IOW, {u32 timeout} */
        constexpr u32 NvHostIocCtrlSyncptRead       = MakeIowr(0x00, 0x14, 8);   /* {u32 id; u32 value}          */
        /* MAP_CMD_BUFFER: header is 12 bytes (num_handles + reserved + is_compr
         * + padding[3]); each handle entry is 8. So 1 handle = 20 bytes, and
         * the ioctl request code must encode 20 or the kernel's _NV_IOC_SIZE
         * disagrees with the buffer -> nverr=11 (BadParameter). */
        constexpr u32 NvHostIocChannelMapCmdBuf1    = MakeIowr(0x00, 0x09, 12 + 1 * 8);

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
                /* Channel nodes are exclusive per session - a leaked fd here
                 * makes the real open in nv:10 fail with nverr=4096. Close
                 * every survey fd immediately (nvdrv Close = cmd 2). */
                if (R_SUCCEEDED(r) && out.error == 0) {
                    const struct { u32 fd; } ci = { out.fd };
                    u32 ce = 0;
                    serviceDispatchInOut(std::addressof(g_nv_srv), 2, ci, ce);
                }
            }
        }

        u32 own_handle_out = 0;
        LogMark("nv:8_own_buffer");
        {
            /* The VIC's destination must be memory WE own: an nvmap object we
             * create is backed by our own pages, so we can read it directly
             * (unlike an imported handle, which has no CPU mapping). Prove the
             * CREATE + ALLOC + GET_ID round trip on a small buffer. */
            u32 own_handle = 0, own_id = 0, nverr = 0;

            struct { u32 size; u32 handle; } cr = { static_cast<u32>(sizeof(g_own_buf)), 0 };
            rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
            LogLine("   CREATE(0x%zx) rc=0x%x nverr=%u -> handle=%u", sizeof(g_own_buf), rc, nverr, cr.handle);
            own_handle = cr.handle;

            if (R_SUCCEEDED(rc) && nverr == 0) {
                struct {
                    u32 handle; u32 heapmask; u32 flags; u32 align;
                    u8 kind; u8 pad[7]; u64 addr;
                } al = {};
                al.handle   = own_handle;
                al.heapmask = 0;
                al.flags    = 0;                 /* 0 = read/write */
                al.align    = 0x1000;
                al.kind     = 0;                 /* Pitch (linear) */
                al.addr     = reinterpret_cast<u64>(g_own_buf);
                nverr = 0;
                rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
                LogLine("   ALLOC(handle=%u, cpu=%p, linear) rc=0x%x nverr=%u",
                        own_handle, static_cast<void *>(g_own_buf), rc, nverr);

                struct { u32 id; u32 handle; } gi = { 0, own_handle };
                nverr = 0;
                rc = NvIoctl(fd, NvmapIocGetId, std::addressof(gi), sizeof(gi), std::addressof(nverr));
                LogLine("   GET_ID(handle=%u) rc=0x%x nverr=%u -> id=%u", own_handle, rc, nverr, gi.id);
                own_id = gi.id;

                /* CPU write/read-back proves we really own these pages. */
                g_own_buf[0] = 0xA5; g_own_buf[1] = 0x5A;
                g_own_buf[sizeof(g_own_buf) - 1] = 0xC3;
                LogLine("   cpu readback: %02x %02x ... %02x  (own_id=%u)",
                        g_own_buf[0], g_own_buf[1], g_own_buf[sizeof(g_own_buf) - 1], own_id);
            }
            own_handle_out = own_handle;
        }

        /* --- Stage 1: is the VIC channel actually usable? -----------------
         * /dev/nvhost-vic opened in the survey, but so did nothing-useful nodes
         * before. Prove we can GET_SYNCPOINT, read it via /dev/nvhost-ctrl, set
         * a submit timeout, and MAP_CMD_BUFFER an nvmap handle into the channel.
         * If all four work, the config-struct + real blit in Stage 2 is pure
         * implementation. */
        LogMark("nv:10_vic_channel");
        {
            struct { u32 fd; u32 error; } vo = {};
            const char *vpath = "/dev/nvhost-vic";
            rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, vo,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers      = { { vpath, std::strlen(vpath) } });
            LogLine("   open /dev/nvhost-vic rc=0x%x fd=%u nverr=%u", rc, vo.fd, vo.error);

            if (R_SUCCEEDED(rc) && vo.error == 0) {
                const u32 vfd = vo.fd;
                u32 nverr = 0;

                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                rc = NvIoctl(vfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                LogLine("   GET_SYNCPOINT(0) rc=0x%x nverr=%u -> syncpt=%u", rc, nverr, gs.syncpt);
                const u32 syncpt_id = gs.syncpt;

                struct { u32 fd; u32 error; } co = {};
                const char *cpath = "/dev/nvhost-ctrl";
                rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, co,
                    .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                    .buffers      = { { cpath, std::strlen(cpath) } });
                if (R_SUCCEEDED(rc) && co.error == 0) {
                    struct { u32 id; u32 value; } sr = { syncpt_id, 0 };
                    nverr = 0;
                    rc = NvIoctl(co.fd, NvHostIocCtrlSyncptRead, std::addressof(sr), sizeof(sr), std::addressof(nverr));
                    LogLine("   ctrl SYNCPT_READ(%u) rc=0x%x nverr=%u -> value=%u", syncpt_id, rc, nverr, sr.value);
                }

                struct { u32 timeout; } st = { 1000 };
                nverr = 0;
                rc = NvIoctl(vfd, NvHostIocChannelSetSubmitTo, std::addressof(st), sizeof(st), std::addressof(nverr));
                LogLine("   SET_SUBMIT_TIMEOUT(1000) rc=0x%x nverr=%u", rc, nverr);

                /* MAP_CMD_BUFFER our own buffer's handle into the channel. */
                {
                    struct {
                        u32 num_handles; u32 reserved; u8 is_compr; u8 pad[3];
                        struct { u32 handle_id; u32 phys_out; } h[1];
                    } mc = {};
                    mc.num_handles = 1;
                    mc.h[0].handle_id = own_handle_out;
                    nverr = 0;
                    rc = NvIoctl(vfd, NvHostIocChannelMapCmdBuf1, std::addressof(mc), sizeof(mc), std::addressof(nverr));
                    LogLine("   MAP_CMD_BUFFER(handle=%u) rc=0x%x nverr=%u -> phys=0x%08x",
                            own_handle_out, rc, nverr, mc.h[0].phys_out);
                    if (R_SUCCEEDED(rc) && nverr == 0 && mc.h[0].phys_out != 0) {
                        LogMark("nv:VIC_CHANNEL_USABLE");
                    }
                }

                /* close vfd + ctrl fd via nvClose cmd (cmd 2) */
                { const struct { u32 fd; } in = { vfd }; u32 e = 0;
                  serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e); }
                { const struct { u32 fd; } in = { co.fd }; u32 e = 0;
                  serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e); }
            }
        }

        /* Release everything. A probe must not hold an nvdrv session or a
         * handle on the game's buffer - doing so is what wedged homebrew. */
        LogMark("nv:9_cleanup");
        serviceClose(std::addressof(g_nv_srv));
        tmemClose(std::addressof(g_nv_tmem));

        LogMark("nv:DONE_OPENED");
        LogLine("*** nvmap object %u reachable (handle=%u, 25 MB = the full swapchain); session released",
                nvmap_id, handle);
    }

}
