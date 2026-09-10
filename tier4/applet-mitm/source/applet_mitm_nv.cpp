/*
 * applet-mitm - hand-rolled nvdrv access + VIC hardware blit.
 *
 * Stage 1 (M4b..M7d, verified on hardware): the sequence below hand-rolls the
 * nvdrv IPC (libnx's nvInitialize() fatals a sysmodule), imports the game's
 * swapchain nvmap, surveys engines, allocates our own linear buffer, and shows
 * the VIC channel is usable. It then RELEASES everything - holding an nvdrv
 * session wedged homebrew.
 *
 * Stage 2 (M8, this file): TryVicBlit() does the real thing. One
 * NVHOST_IOCTL_CHANNEL_SUBMIT that kicks the VIC to de-swizzle + format-convert
 * a crop of the live frame (block-linear, kind 0xFE) into a linear buffer we
 * own, then waits the syncpoint and checksums the result. No MAP_CMD_BUFFER
 * (it crashed nvservices); the reloc list lets the kernel pin per-submit.
 *
 * nvdrv IPC (libnx nx/source/services/nv.c):
 *   cmd 0 Open      : InBuffer(MapAlias) = path -> {u32 fd, u32 error}
 *   cmd 1 Ioctl     : in {u32 fd, u32 request}, out u32 error, AutoSelect in+out = argp
 *   cmd 2 Close     : in {u32 fd}, out u32 error
 *   cmd 3 Initialize: in u32 transfermem_size, handles {process, tmem}
 *
 * CHANNEL_SUBMIT arg layout (switchbrew NV_services, libnx nvchannel.c):
 *   u32 num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences;
 *   cmdbuf      cmdbufs[n]      {u32 mem, offset, words}                 (12)
 *   reloc       relocs[n]       {u32 cmdbuf_mem, cmdbuf_offset, target, target_offset} (16)
 *   reloc_shift reloc_shifts[n] {u32 shift}                             (4)
 *   syncpt_incr syncpt_incrs[n] {u32 syncpt_id, syncpt_incrs, rsvd[3]}  (20)
 *   u32         fence_thresholds[n]  (out)                              (4)
 * request = _NV_IOWR(0, 0x01, sizeof) -> 0xC0??0001 with ?? = total size.
 */
#include "applet_mitm_nv.hpp"
#include "applet_mitm_log.hpp"
#include "vic40_config.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>

namespace ams::mitm::applet {

    constinit GameSurface g_game_surface = {};
    constinit bool g_vic_armed   = false;
    constinit bool g_vic_execute = false;
    constinit std::atomic<const char *> g_vic_stage{"idle"};
    constinit u64 g_game_aruid = 0;

    namespace {

        constinit std::atomic<bool> g_blit_done{false};

        /* Static so the allocator is never in the path. */
        alignas(0x1000) constinit u8 g_nv_tmem_buf[0x40000] = {};

        /* VIC buffers live on the REAL process heap, not in .bss.
         *
         * nvmap ALLOC binds the caller's pages, but a device-shared buffer also
         * has to have its CPU mapping marked uncached - libnx's nvMapCreate does
         * armDCacheFlush + svcSetMemoryAttribute(.., 8, 8) for exactly this. That
         * SVC is only permitted on MemoryState_Normal, i.e. heap; a sysmodule's
         * .bss is code-mutable and it would be rejected. Every previous run had
         * cached, non-shared .bss standing in for device memory, which is why the
         * engine reported OP_DONE and we read back nothing. */
        constexpr size_t VicCfgSize = 0x4000;    /* VicConfigStruct (1552 B) */
        constexpr size_t VicCmdSize = 0x1000;    /* host1x pushbuf           */
        constexpr size_t VicDstSize = 0x10000;   /* linear output            */
        constexpr size_t VicSrcSize = 0x10000;   /* a SOURCE WE OWN, for the self-blit */
        constexpr size_t VicHeapSize = 8_MB;     /* 2 MB granularity; 720p indirect capture needs 3.8 MB */
        constexpr size_t IndirectOff  = 0x100000; /* capture buffer starts 1 MB in */
        constexpr size_t IndirectSize = 6_MB;

        constinit uintptr_t g_vic_heap    = 0;
        constinit u8       *g_vic_cfg_buf = nullptr;
        constinit u8       *g_vic_cmd_buf = nullptr;
        constinit u8       *g_vic_dst_buf = nullptr;
        constinit u8       *g_vic_src_buf = nullptr;
        constinit u8       *g_ind_buf     = nullptr;

        bool AllocVicHeap() {
            if (g_vic_heap != 0) { return true; }
            if (const auto rc = os::SetMemoryHeapSize(VicHeapSize); R_FAILED(rc)) {
                LogLine("   SetMemoryHeapSize(2MB) FAILED rc=0x%x", rc.GetValue());
                return false;
            }
            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), VicHeapSize); R_FAILED(rc)) {
                LogLine("   AllocateMemoryBlock(2MB) FAILED rc=0x%x", rc.GetValue());
                return false;
            }
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x20000);
            g_ind_buf     = reinterpret_cast<u8 *>(addr + IndirectOff);
            std::memset(reinterpret_cast<void *>(addr), 0, VicHeapSize);
            LogLine("   VIC heap at 0x%lx: cfg=%p cmd=%p dst=%p src=%p",
                    static_cast<unsigned long>(addr),
                    static_cast<void *>(g_vic_cfg_buf),
                    static_cast<void *>(g_vic_cmd_buf),
                    static_cast<void *>(g_vic_dst_buf),
                    static_cast<void *>(g_vic_src_buf));
            return true;
        }

        /* Phase B geometry: a scale-free 64x64 crop of the frame's top-left.
         * The output STRIDE is aligned to 256 pixels, not to the width: that is
         * what libdrm's vic_image_new does for every VIC surface, pitch-linear
         * included (align = 256, stride = ALIGN(width, align)). 64 px of stride
         * would be a 256-byte pitch, well under what the engine expects. */
        constexpr u32 DstW        = 64;
        constexpr u32 DstH        = 64;
        constexpr u32 DstStridePx = 256;                 /* 1024 B pitch */
        constexpr u32 DstPitch    = DstStridePx * 4;
        constexpr u32 DstSize     = DstPitch * DstH;     /* 65536 */
        static_assert(DstSize <= VicDstSize);

        constinit ::Service        g_nv_srv  = {};
        constinit ::TransferMemory g_nv_tmem = {};

        /* Linux-style ioctl encoding: (dir<<30)|(size<<16)|(type<<8)|nr, IOWR dir=3 */
        constexpr u32 MakeIowr(u32 type, u32 nr, u32 size) {
            return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 MakeIow(u32 type, u32 nr, u32 size) {
            return (UINT32_C(1) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 NvmapIocCreate = MakeIowr(0x01, 0x01, 8);
        constexpr u32 NvmapIocFromId = MakeIowr(0x01, 0x03, 8);
        constexpr u32 NvmapIocAlloc  = MakeIowr(0x01, 0x04, 32);
        constexpr u32 NvmapIocGetId  = MakeIowr(0x01, 0x0E, 8);
        /* {u64 aruid; u32 handle; u8 pad[4]} - asks whether an nvmap object is
         * bound to a given AppletResourceUserId. A pure query, so it doubles as
         * a safe way to DISCOVER the game's aruid. */
        constexpr u32 NvmapIocIsOwnedByAruid = MakeIow(0x01, 0x13, 16);   /* 0x40100113 */

        constexpr u32 NvHostIocChannelGetSyncpoint  = MakeIowr(0x00, 0x02, 8);   /* {u32 module_id; u32 syncpt}   */
        constexpr u32 NvHostIocChannelSetSubmitTo   = MakeIow (0x00, 0x07, 4);   /* {u32 timeout}                 */
        constexpr u32 NvHostIocChannelSetNvmapFd    = MakeIow (0x48, 0x01, 4);   /* {u32 fd}  -> 0x40044801       */
        /* MAP_CMD_BUFFER pins nvmap handles and RETURNS their device physical
         * address. Horizon replaced Linux's per-submit pinning with this, which
         * is why a submit carrying relocs for unpinned handles answers
         * InvalidState(8). Header is 12 B (num_handles, reserved, is_compr,
         * pad[3]) + 8 B per handle -> 20 for one. */
        constexpr u32 NvHostIocChannelMapCmdBuf     = MakeIowr(0x00, 0x09, 20);  /* -> 0xC0140009 */
        constexpr u32 NvHostIocChannelUnmapCmdBuf   = MakeIowr(0x00, 0x0A, 20);
        constexpr u32 NvHostIocChannelMapCmdBufEx   = MakeIowr(0x00, 0x25, 20);   /* unpins on error */
        constexpr u32 NvHostIocCtrlSyncptRead       = MakeIowr(0x00, 0x14, 8);   /* {u32 id; u32 value}           */
        constexpr u32 NvHostIocCtrlSyncptWait       = MakeIowr(0x00, 0x16, 12);  /* {u32 id; u32 thresh; u32 to}  */
        constexpr u32 NvHostIocCtrlSyncptIncrW      = MakeIow (0x00, 0x15, 4);   /* {u32 id} - switchbrew 0x40040015 */
        constexpr u32 NvHostIocCtrlSyncptIncrWR     = MakeIowr(0x00, 0x15, 4);   /* libnx encodes this one instead   */

        struct MapCmdBufArgs {
            u32 num_handles;
            u32 reserved;
            u8  is_compr;
            u8  padding[3];
            u32 handle_id_in;
            u32 phys_addr_out;
        };
        static_assert(sizeof(MapCmdBufArgs) == 20);
        static_assert(__builtin_offsetof(MapCmdBufArgs, handle_id_in) == 12);

        /* breadcrumb + stage, so the heartbeat can name where a hang happened */
        void VicStage(const char *s) {
            g_vic_stage.store(s, std::memory_order_relaxed);
            LogMark(s);
        }

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

        ::Result NvOpen(const char *path, u32 *out_fd, u32 *out_err) {
            struct { u32 fd; u32 error; } out = {};
            const ::Result rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers      = { { path, std::strlen(path) } });
            *out_fd  = out.fd;
            *out_err = out.error;
            return rc;
        }

        /* nvdrv cmd 7. Plain u64 in, u32 err out - no PID descriptor, so unlike
         * vi's OpenLayer this one is actually expressible for us. Needs
         * NvDrvPermission bit 10, which only nvdrv:t grants.
         *
         * libnx's nvInitialize does the equivalent for every normal client:
         *     u64 aruid = appletGetAppletResourceUserId();
         *     if (aruid) _nvSetClientPID(aruid);
         * Our session has never set one, so it is aruid 0 while the game's
         * buffers belong to the game's aruid. That asymmetry is the last
         * candidate explanation for the silent phys=0. */
        ::Result NvSetAruidWithoutCheck(u64 aruid, u32 *out_err) {
            u32 err = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 7, aruid, err);
            *out_err = err;
            return rc;
        }

        bool NvIsOwnedByAruid(u32 fd, u64 aruid, u32 handle) {
            struct { u64 aruid; u32 handle; u32 pad; } a = { aruid, handle, 0 };
            u32 nverr = 0;
            const ::Result rc = NvIoctl(fd, NvmapIocIsOwnedByAruid, std::addressof(a), sizeof(a), std::addressof(nverr));
            return R_SUCCEEDED(rc) && nverr == 0;
        }

        void NvClose(u32 fd) {
            const struct { u32 fd; } in = { fd };
            u32 e = 0;
            serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e);
        }

        /* Pin one nvmap handle into the channel and get its device physical
         * address. Each call is breadcrumbed by the caller so that if this ever
         * takes nvservices down again we know exactly which handle did it. */
        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr = 0, u32 req = 0) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.is_compr     = is_compr;
            a.handle_id_in = handle;
            u32 nverr = 0;
            if (req == 0) { req = NvHostIocChannelMapCmdBuf; }
            const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   MAP_CMD_BUFFER(%s handle=%u compr=%u req=0x%08x) rc=0x%x nverr=%u -> phys=0x%x",
                    what, handle, is_compr, req, rc, nverr, a.phys_addr_out);
            if (R_FAILED(rc) || nverr != 0) { return false; }
            *out_addr = a.phys_addr_out;
            return true;
        }

        void UnmapCmdBuffer(u32 chan_fd, u32 handle) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = handle;
            u32 nverr = 0;
            NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
        }

        /* CREATE + ALLOC(kind, our cpu pages) + GET_ID for a buffer we own. */
        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id) {
            u32 nverr = 0;
            struct { u32 size; u32 handle; } cr = { size, 0 };
            ::Result rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn CREATE fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            struct {
                u32 handle; u32 heapmask; u32 flags; u32 align;
                u8 kind; u8 pad[7]; u64 addr;
            } al = {};
            al.handle = cr.handle;
            al.flags  = 0;
            al.align  = 0x1000;
            al.kind   = kind;
            al.addr   = reinterpret_cast<u64>(cpu);
            rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn ALLOC fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            /* flags bit0 is CACHEABLE (libnx passes is_cpu_cacheable?1:0), not
             * read-write as the wiki's comment suggests. We asked for
             * non-cacheable, so we owe the matching CPU-side step that libnx
             * does and we never did: flush, then mark the mapping uncached.
             * Without it the CPU keeps a cached, non-coherent view. */
            armDCacheFlush(cpu, size);
            const auto sma = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(cpu), size,
                                                     svc::MemoryAttribute_Uncached,
                                                     svc::MemoryAttribute_Uncached);
            LogLine("   SetMemoryAttribute(%p, 0x%x, uncached) rc=0x%x", cpu, size, sma.GetValue());

            struct { u32 id; u32 handle; } gi = { 0, cr.handle };
            rc = NvIoctl(fd, NvmapIocGetId, std::addressof(gi), sizeof(gi), std::addressof(nverr));
            *out_handle = cr.handle;
            *out_id     = gi.id;
            LogLine("   nvmapOwn ok handle=%u id=%u kind=%u size=0x%x", cr.handle, gi.id, kind, size);
            return rc;
        }

        /* Three jobs, run back-to-back in one probe so a single reboot answers
         * everything:
         *   Fill       - no source at all. Isolates the OUTPUT path: config
         *                struct, dst address, EXECUTE, cache handling.
         *   BlitDirect - source address inlined from MAP_CMD_BUFFER.
         *   BlitReloc  - source address left to nvservices via a reloc, which
         *                knows the game buffer's address even though pinning
         *                would not tell us. */
        enum class VicJob { Fill, BlitSelf, BlitGame };

        struct SrcDesc { u32 w, h, stride_px, blk_kind, blk_h_log2, pixfmt; };

        /* our own pitch-linear 64x64 buffer, stride 256 px like the destination */
        constexpr SrcDesc SelfSrc { DstW, DstH, DstStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8 };

        void FillOutputConfig(vic::VicConfigStruct *c) {
            c->outputConfig.TargetRectLeft   = 0;
            c->outputConfig.TargetRectTop    = 0;
            c->outputConfig.TargetRectRight  = DstW - 1;
            c->outputConfig.TargetRectBottom = DstH - 1;

            c->outputSurfaceConfig.OutPixelFormat   = vic::PIXFMT_A8B8G8R8;
            c->outputSurfaceConfig.OutBlkKind       = vic::BLK_KIND_PITCH;
            c->outputSurfaceConfig.OutBlkHeight     = 0;
            c->outputSurfaceConfig.OutSurfaceWidth  = DstW - 1;
            c->outputSurfaceConfig.OutSurfaceHeight = DstH - 1;
            c->outputSurfaceConfig.OutLumaWidth     = DstStridePx - 1;
            c->outputSurfaceConfig.OutLumaHeight    = DstH - 1;
            c->outputSurfaceConfig.OutChromaWidth   = 16383;
            c->outputSurfaceConfig.OutChromaHeight  = 16383;
        }

        /* libdrm vic40_fill / vic_clear: paint the whole target one colour with
         * no slot enabled. Anything non-zero in dst afterwards proves the whole
         * output half of the pipeline. */
        void FillClearConfig(vic::VicConfigStruct *c) {
            std::memset(c, 0, sizeof(*c));
            FillOutputConfig(c);
            /* Four DISTINCT levels so the output byte order can be read straight
             * off the dump: 1023->0xFF, 768->0xC0, 512->0x80, 256->0x40.
             * The previous red gave 'ff ff 00 00', which could not say which
             * byte was alpha and which was red. */
            c->outputConfig.BackgroundAlpha = 1023;   /* 0xFF */
            c->outputConfig.BackgroundR     = 768;    /* 0xC0 */
            c->outputConfig.BackgroundG     = 512;    /* 0x80 */
            c->outputConfig.BackgroundB     = 256;    /* 0x40 */
        }

        void FillBlitConfig(vic::VicConfigStruct *c, const SrcDesc &src) {
            std::memset(c, 0, sizeof(*c));

            FillOutputConfig(c);
            c->outputConfig.BackgroundAlpha  = 1023;
            c->outputConfig.BackgroundR      = 1023;
            c->outputConfig.BackgroundG      = 1023;
            c->outputConfig.BackgroundB      = 1023;

            /* slot 0: 1:1 crop of the frame's top-left DstW x DstH */
            vic::SlotConfig *slot = std::addressof(c->slotStruct[0].slotConfig);
            slot->SlotEnable          = 1;
            slot->CurrentFieldEnable  = 1;
            slot->PlanarAlpha         = 1023;
            slot->ConstantAlpha       = 1;
            slot->SourceRectLeft      = 0;
            slot->SourceRectRight     = static_cast<u64>(DstW - 1) << 16;   /* 16.16 fixed point */
            slot->SourceRectTop       = 0;
            slot->SourceRectBottom    = static_cast<u64>(DstH - 1) << 16;
            slot->DestRectLeft        = 0;
            slot->DestRectRight       = DstW - 1;
            slot->DestRectTop         = 0;
            slot->DestRectBottom      = DstH - 1;
            slot->SoftClampHigh       = 1023;

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[0].slotSurfaceConfig);
            s->SlotPixelFormat   = src.pixfmt;
            s->SlotBlkKind       = src.blk_kind;
            s->SlotBlkHeight     = src.blk_h_log2;
            s->SlotCacheWidth    = vic::CACHE_WIDTH_64Bx4;
            s->SlotSurfaceWidth  = src.w - 1;
            s->SlotSurfaceHeight = src.h - 1;
            s->SlotLumaWidth     = src.stride_px - 1;
            s->SlotLumaHeight    = src.h - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;
        }

        /* host1x pushbuf: VIC_PUSH_METHOD / VIC_PUSH_BUFFER from libdrm vic.h.
         * Each entry: [INCR(0x10,2)] [method>>2] [value|reloc-placeholder]. */
        constexpr u32 Host1xIncr0x10x2 = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;

        /* Word indices of the three address slots, for the reloc variant. */
        constexpr u32 CfgAddrWord = 8, DstAddrWord = 11, SrcAddrWord = 14;   /* + 1 if SETCL is emitted */

        u32 BuildCmdbuf(u32 *w, VicJob job, u32 cfg_addr, u32 dst_addr, u32 src_addr, bool set_class) {
            const bool has_src  = (job == VicJob::BlitSelf || job == VicJob::BlitGame);
            u32 n = 0;
            /* Point the channel at the VIC's register space before touching
             * METHOD_OFFSET/METHOD_DATA, which are per-class registers. */
            if (set_class) { w[n++] = vic::Host1xOpcodeSetClass(0, vic::HOST1X_CLASS_VIC, 0); }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_APPLICATION_ID >> 2; w[n++] = 1;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONTROL_PARAMS >> 2;
            w[n++] = (static_cast<u32>(sizeof(vic::VicConfigStruct) / 16)) << 16;
            /* MAP_CMD_BUFFER already handed back a device address for buffers we
             * own, so those go in directly and the submit needs no reloc list.
             * The BlitReloc variant instead leaves all three as placeholders and
             * lets nvservices patch them. */
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONFIG_STRUCT_OFFSET >> 2;
            w[n++] = cfg_addr >> 8;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2;
            w[n++] = dst_addr >> 8;
            if (has_src) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2;
                w[n++] = src_addr >> 8;
            }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::EXECUTE >> 2; w[n++] = 1u << 8;
            return n;
        }

        void TryIndirectCapture();

        struct JobCtx {
            u32 vfd, cmd_handle, syncpt;
            u32 cfg_addr, dst_addr, src_addr, src_off;
            u32 cfg_handle, dst_handle, src_handle;
            SrcDesc game_src;
        };


        /* Program the syncpoint increment the submit already promised. Omitting
         * this is what froze the console: nvhost raised syncpoint 12's max to
         * the fence, nothing ever incremented it, and nvnflinger - which
         * composites every frame on that same VIC syncpoint - waited forever. */
        u32 AppendIncrSyncpt(u32 *w, u32 n, u32 syncpt, bool after_engine_op) {
            const u32 cond = after_engine_op ? vic::INCR_SYNCPT_COND_OP_DONE
                                             : vic::INCR_SYNCPT_COND_IMMEDIATE;
            w[n++] = vic::Host1xOpcodeNonIncr(vic::UCLASS_INCR_SYNCPT, 1);
            w[n++] = (cond << vic::INCR_SYNCPT_COND_SHIFT) | syncpt;
            return n;
        }

        /* One VIC job end to end: config, cmdbuf, submit, wait, rescue, read
         * back. Called once per VicJob so a single reboot answers which half of
         * the pipeline works. */
        /* Returns false if the engine did not complete, so the caller can stop.
         *
         * The guard below is the lesson from two console freezes: a VIC handed a
         * zero address does not fail politely, it HANGS, and a hung VIC takes the
         * compositor and the whole console with it. Every address the engine will
         * dereference is checked here, once, rather than trusted per call site. */
        bool RunOneJob(const char *stage, VicJob job, bool set_class, const JobCtx &c) {
            VicStage(stage);

            const bool needs_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame);
            if (c.cfg_addr == 0 || c.dst_addr == 0 || (needs_src && c.src_addr == 0)) {
                LogLine("   [%s] REFUSING to submit: cfg=0x%x dst=0x%x src=0x%x - a zero "
                        "address hangs the VIC and freezes the console",
                        stage, c.cfg_addr, c.dst_addr, c.src_addr);
                return false;
            }

            auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
            switch (job) {
                case VicJob::Fill:     FillClearConfig(cfg);            break;
                case VicJob::BlitSelf: FillBlitConfig(cfg, SelfSrc);    break;
                case VicJob::BlitGame: FillBlitConfig(cfg, c.game_src); break;
            }

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = BuildCmdbuf(w, job, c.cfg_addr, c.dst_addr, c.src_addr + c.src_off, set_class);
            const u32 rw = set_class ? 1u : 0u;   /* reloc word indices shift when SETCL leads */
            words = AppendIncrSyncpt(w, words, c.syncpt, true);

            /* Prefill with a poison pattern rather than zero. "All zero" cannot
             * distinguish "engine wrote zeros" from "engine never touched our
             * memory"; surviving 0xAB proves the latter outright. */
            std::memset(g_vic_dst_buf, 0xAB, DstSize);
            armDCacheFlush(g_vic_dst_buf, DstSize);
            armDCacheFlush(g_vic_cfg_buf, VicCfgSize);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            const u32 nr = 0u;   /* relocs are inert on Horizon - always inline the addresses */

            alignas(8) u8 sb[16 + 12 + 3 * (16 + 4) + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };

            put(1); put(nr); put(1); put(1);
            put(c.cmd_handle); put(0); put(words);
            put(c.syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 nverr = 0, fence_val = 0;
            ::Result rc = NvIoctl(c.vfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogLine("   [%s] setcl=%d words=%u cmd[0..3]=%08x %08x %08x %08x",
                    stage, static_cast<int>(set_class), words, w[0], w[1], w[2], w[3]);
            LogLine("   [%s] req=0x%08x sz=%u nr=%u rc=0x%x nverr=%u -> fence=%u",
                    stage, req, sz, nr, rc, nverr, fence_val);
            if (R_FAILED(rc) || nverr != 0) { LogLine("   [%s] SUBMIT REJECTED", stage); return false; }

            /* Read the cmdbuf back: nvservices patches reloc targets in place,
             * so these words now hold the addresses the engine would be given. */
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            LogLine("   [%s] resolved addrs: cfg=0x%08x dst=0x%08x src=0x%08x (<<8: 0x%x 0x%x 0x%x)",
                    stage, w[CfgAddrWord + rw], w[DstAddrWord + rw],
                    (job == VicJob::Fill) ? 0u : w[SrcAddrWord + rw],
                    w[CfgAddrWord + rw] << 8, w[DstAddrWord + rw] << 8,
                    (job == VicJob::Fill) ? 0u : (w[SrcAddrWord + rw] << 8));

            bool completed = false;
            u32 cfd = 0, ce = 0;
            if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(cfd), std::addressof(ce))) && ce == 0) {
                struct { u32 id; u32 thresh; u32 timeout; } a = { c.syncpt, fence_val, 100 };
                nverr = 0;
                rc = NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(nverr));
                struct { u32 id; u32 value; } r = { c.syncpt, 0 };
                u32 e2 = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                completed = (r.value >= fence_val);
                LogLine("   [%s] WAIT nverr=%u  syncpt=%u (want >= %u)%s",
                        stage, nverr, r.value, fence_val,
                        completed ? "  OP_DONE fired" : "  ENGINE DID NOT COMPLETE");

                /* Never leave a shared syncpoint short of its declared max. */
                if (r.value < fence_val) {
                    VicStage("vb:RESCUE_INCR");
                    u32 ireq = NvHostIocCtrlSyncptIncrW;
                    for (u32 k = 0, missing = fence_val - r.value; k < missing && k < 64; k++) {
                        struct { u32 id; } ai = { c.syncpt };
                        u32 ae = 0;
                        NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae));
                        if (k == 0 && ae != 0) { ireq = NvHostIocCtrlSyncptIncrWR;
                            NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae)); }
                    }
                    LogLine("   [%s] rescued syncpt %u up to fence %u", stage, c.syncpt, fence_val);
                }
                NvClose(cfd);
            }

            /* The VIC wrote through the device side; our cache still holds the
             * zeros we just stored. Without this invalidate the read-back is
             * guaranteed to look empty no matter what the engine did. */
            armDCacheFlush(g_vic_dst_buf, DstSize);

            u32 sum = 0, changed = 0;
            for (u32 i = 0; i < DstSize; i++) {
                sum += g_vic_dst_buf[i];
                if (g_vic_dst_buf[i] != 0xAB) { changed++; }
            }
            char hex[3 * 32 + 1];
            int k = 0;
            for (u32 i = 0; i < 32; i++) { k += std::snprintf(hex + k, sizeof(hex) - k, "%02x ", g_vic_dst_buf[i]); }
            LogLine("   [%s] dst sum32=%u changed=%u/%u  %s", stage, sum, changed, DstSize,
                    (changed > 0) ? "*** ENGINE WROTE OUR MEMORY ***"
                                  : "(untouched - poison 0xAB intact)");
            LogLine("   [%s] dst[0..32]: %s", stage, hex);
            const u8 *mid = g_vic_dst_buf + (DstH / 2) * DstPitch;
            LogLine("   [%s] row %u: %02x %02x %02x %02x %02x %02x %02x %02x",
                    stage, DstH / 2, mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7]);
            return completed;
        }


    }

    void CaptureGameSurface(const NvGraphicBufferRaw *gb, u32 which) {
        if (gb == nullptr || gb->num_planes == 0) { return; }
        const NvSurfaceRaw *p0 = std::addressof(gb->planes[0]);

        const u32 slot = (which >= 1) ? (which - 1) : 0;      /* setPreallocatedBuffer#1 -> slot 0 */
        if (slot < 8) {
            g_game_surface.slot_offset[slot] = p0->offset;
            if (slot + 1 > g_game_surface.num_slots) { g_game_surface.num_slots = slot + 1; }
        }
        g_game_surface.nvmap_id     = static_cast<u32>(gb->nvmap_id);
        g_game_surface.width        = p0->width;
        g_game_surface.height       = p0->height;
        g_game_surface.stride_px    = p0->pitch / 4;
        g_game_surface.block_h_log2 = p0->block_height_log2;
        /* NvColorFormat -> VIC pixel format. A8B8G8R8 is the swapchain default. */
        g_game_surface.pix_format   = (p0->color_format == 0x0100D12120ULL)
                                    ? vic::PIXFMT_A8R8G8B8 : vic::PIXFMT_A8B8G8R8;
        g_game_surface.armed        = true;

        LogLine("   captured slot %u: nvmap=%u %ux%u stride_px=%u blk_h_log2=%u off=0x%x pixfmt=%u (slots=%u)",
                slot, g_game_surface.nvmap_id, g_game_surface.width, g_game_surface.height,
                g_game_surface.stride_px, g_game_surface.block_h_log2, p0->offset,
                g_game_surface.pix_format, g_game_surface.num_slots);
    }

    namespace {

        constinit std::atomic<s32> g_vic_pending{-1};
        alignas(os::ThreadStackAlignment) constinit u8 g_vic_stack[32_KB];
        constinit os::ThreadType g_vic_thread;

    void RunVicBlit(s32 slot) {
        if (!g_vic_armed || !g_game_surface.armed) { return; }
        bool expected = false;
        if (!g_blit_done.compare_exchange_strong(expected, true)) { return; }

        const u32 sidx = (slot >= 0 && static_cast<u32>(slot) < g_game_surface.num_slots)
                       ? static_cast<u32>(slot) : 0;
        const u32 src_off = g_game_surface.slot_offset[sidx];

        /* declared before the first goto: a jump may not bypass an initialised
         * declaration, and close_vic reads these to decide what to unpin. */
        u32 cfg_addr = 0, dst_addr = 0, src_addr = 0, self_addr = 0;

        VicStage("vb:0_heap");
        if (!AllocVicHeap()) { VicStage("vb:0_FAILED"); return; }

        VicStage("vb:1_smGetService");
        /* Service name decides the NvDrvPermission mask, and phys=0 on the
         * game's handle is a permissions story, not a bug:
         *   nvdrv:s (sysmodules) 0x439E  - no bit 10 (SetAruidWithoutCheck),
         *                                  no bit 12 (import EXPORTED handles),
         *                                  no bit 15 (full VA range - and indeed
         *                                  our IOVAs sit in the restricted
         *                                  0xE0000000+ window)
         *   nvdrv:t (factory)    0xFFFFFFFF - everything
         * nvmap objects are bound to an AppletResourceUserId, so importing
         * another process's buffer plausibly needs the aruid bits we lack. */
        ::Result rc = smGetService(std::addressof(g_nv_srv), "nvdrv:t");
        const char *nv_svc = "nvdrv:t";
        if (R_FAILED(rc)) {
            LogLine("   nvdrv:t unavailable rc=0x%x - falling back", rc);
            rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
            nv_svc = "nvdrv:s";
        }
        LogLine("   using %s rc=0x%x", nv_svc, rc);
        if (R_FAILED(rc)) { VicStage("vb:1_FAILED"); return; }

        VicStage("vb:2_tmem");
        rc = tmemCreateFromMemory(std::addressof(g_nv_tmem), g_nv_tmem_buf, sizeof(g_nv_tmem_buf), Perm_None);
        if (R_FAILED(rc)) { VicStage("vb:2_FAILED"); serviceClose(std::addressof(g_nv_srv)); return; }

        VicStage("vb:3_Initialize");
        {
            const u32 tmem_size = static_cast<u32>(sizeof(g_nv_tmem_buf));
            rc = serviceDispatchIn(std::addressof(g_nv_srv), 3, tmem_size,
                .in_num_handles = 2,
                .in_handles     = { CUR_PROCESS_HANDLE, g_nv_tmem.handle });
            LogLine("   Initialize rc=0x%x", rc);
            if (R_FAILED(rc)) { VicStage("vb:3_FAILED"); goto close_sess; }
        }

        u32 nvmap_fd, nverr;

        /* --- aruid discovery on a THROWAWAY fd ----------------------------
         * M21 found the owner (aruid 142) and adopted it successfully, yet the
         * pin still returned 0 - because we adopted it *after* opening
         * /dev/nvmap. libnx sets the aruid during Initialize, before opening
         * any device node, so an fd evidently captures the client identity at
         * open time. Discover on a scratch fd, close it, adopt, and only then
         * open the fd we actually use. */
        {
            VicStage("vb:4a_discover_aruid");
            u32 tfd = 0, terr = 0;
            if (R_SUCCEEDED(NvOpen("/dev/nvmap", std::addressof(tfd), std::addressof(terr))) && terr == 0) {
                struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
                u32 e = 0;
                if (R_SUCCEEDED(NvIoctl(tfd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(e))) && e == 0) {
                    for (u64 v = 1; v <= 256 && g_game_aruid == 0; v++) {
                        if (NvIsOwnedByAruid(tfd, v, a.handle)) { g_game_aruid = v; }
                    }
                }
                NvClose(tfd);
            }
            LogLine("   discovered game aruid = %llu (on a throwaway fd, now closed)",
                    static_cast<unsigned long long>(g_game_aruid));

            if (g_game_aruid != 0) {
                u32 aerr = 0;
                const ::Result ar = NvSetAruidWithoutCheck(g_game_aruid, std::addressof(aerr));
                LogLine("   SetAruidWithoutCheck(%llu) BEFORE any real Open: rc=0x%x err=%u",
                        static_cast<unsigned long long>(g_game_aruid), ar, aerr);
            }
        }

        VicStage("vb:4_open_nvmap");
        rc = NvOpen("/dev/nvmap", std::addressof(nvmap_fd), std::addressof(nverr));
        LogLine("   /dev/nvmap rc=0x%x fd=%u nverr=%u", rc, nvmap_fd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:4_FAILED"); goto close_sess; }

        {
            u32 gfd = 0, gerr = 0;
            const ::Result gr = NvOpen("/dev/nvhost-gpu", std::addressof(gfd), std::addressof(gerr));
            LogLine("   perm probe: /dev/nvhost-gpu rc=0x%x nverr=%u -> %s",
                    gr, gerr, (R_SUCCEEDED(gr) && gerr == 0) ? "OPEN (bit0 set: full mask)"
                                                             : "denied (restricted mask)");
            if (R_SUCCEEDED(gr) && gerr == 0) { NvClose(gfd); }
        }

        u32 src_handle;
        /* --- what did the full mask actually open? -------------------------
         * Reading another process's swapchain is structurally out (nvservices
         * maps client memory through the process handle we gave it at
         * Initialize, and the game's pages are not in our process). So survey
         * the nodes the full mask now reaches - particularly the DISPLAY group,
         * bit 8 - because a post-composition source needs no foreign handle at
         * all, and would capture the home menu and overlays too. */
        {
            VicStage("vb:4b_node_survey");
            static const char *const nodes[] = {
                "/dev/nvhost-display", "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0",
                "/dev/nvdisp-disp1",   "/dev/nvdcutil-disp0", "/dev/nvcec-ctrl",
                "/dev/nvhost-as-gpu",  "/dev/nvhost-ctrl-gpu", "/dev/nvhost-msenc",
                "/dev/nvhost-nvdec",   "/dev/nvhost-tsec",     "/dev/nvhost-nvjpg",
            };
            for (const char *n : nodes) {
                u32 fd = 0, e = 0;
                const ::Result r = NvOpen(n, std::addressof(fd), std::addressof(e));
                const bool ok = R_SUCCEEDED(r) && e == 0;
                LogLine("   node %-22s %s (rc=0x%x nverr=%u)", n, ok ? "OPEN" : "denied", r, e);
                if (ok) { NvClose(fd); }   /* channels are exclusive - never leak one */
            }
        }

        VicStage("vb:5_FROM_ID");
        {
            struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
            rc = NvIoctl(nvmap_fd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   FROM_ID(%u) under aruid %llu rc=0x%x nverr=%u -> handle=%u",
                    g_game_surface.nvmap_id, static_cast<unsigned long long>(g_game_aruid),
                    rc, nverr, a.handle);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:5_FAILED"); goto close_sess; }
            src_handle = a.handle;
        }

        u32 cfg_handle, cfg_id, cmd_handle, cmd_id, dst_handle, dst_id, self_handle, self_id;
        VicStage("vb:6_alloc_bufs");
        rc = NvmapOwn(nvmap_fd, g_vic_cfg_buf, VicCfgSize, 0, std::addressof(cfg_handle), std::addressof(cfg_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cfg_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_cmd_buf, VicCmdSize, 0, std::addressof(cmd_handle), std::addressof(cmd_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cmd_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_dst_buf, VicDstSize, 0, std::addressof(dst_handle), std::addressof(dst_id));
        if (R_FAILED(rc)) { VicStage("vb:6_dst_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_src_buf, VicSrcSize, 0, std::addressof(self_handle), std::addressof(self_id));
        if (R_FAILED(rc)) { VicStage("vb:6_self_FAILED"); goto close_sess; }
        AMS_UNUSED(cfg_id, cmd_id, dst_id, self_id);
        AMS_UNUSED(cfg_handle, dst_handle, src_handle, src_off);   /* Phase A: relocs disabled */

        u32 vfd, syncpt;
        VicStage("vb:7_open_vic");
        rc = NvOpen("/dev/nvhost-vic", std::addressof(vfd), std::addressof(nverr));
        LogLine("   /dev/nvhost-vic rc=0x%x fd=%u nverr=%u", rc, vfd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_FAILED"); goto close_sess; }
        {
            struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
            rc = NvIoctl(vfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
            LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u", rc, nverr, gs.syncpt);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_syncpt_FAILED"); goto close_vic; }
            syncpt = gs.syncpt;
        }

        VicStage("vb:8_set_nvmap_fd");
        {
            struct { u32 fd; } a = { nvmap_fd };
            rc = NvIoctl(vfd, NvHostIocChannelSetNvmapFd, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
        }
        {
            struct { u32 timeout; } a = { 1000 };
            rc = NvIoctl(vfd, NvHostIocChannelSetSubmitTo, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_SUBMIT_TIMEOUT rc=0x%x nverr=%u", rc, nverr);
        }

        if (g_vic_execute) {
            VicStage("vb:8a_map_cfg");
            if (!MapCmdBuffer(vfd, cfg_handle, std::addressof(cfg_addr), "cfg")) { VicStage("vb:8a_FAILED"); goto close_vic; }
            VicStage("vb:8b_map_dst");
            if (!MapCmdBuffer(vfd, dst_handle, std::addressof(dst_addr), "dst")) { VicStage("vb:8b_FAILED"); goto close_vic; }
            /* The game's imported handle pins with nverr=0 but phys=0. Try every
             * variant before giving up; none of these submit anything. */
            VicStage("vb:8c_map_src");
            MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0);
            if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 1); }
            if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0, NvHostIocChannelMapCmdBufEx); }
            VicStage("vb:8d_map_self");
            MapCmdBuffer(vfd, self_handle, std::addressof(self_addr), "src(ours)", 0);
            LogLine("   pinned: cfg=0x%x dst=0x%x self=0x%x game_src=0x%x (+slot off 0x%x)",
                    cfg_addr, dst_addr, self_addr, src_addr, src_off);

            /* Paint a recognisable pattern into the source we own, so a correct
             * blit is provable byte-for-byte rather than just "non-zero". */
            for (u32 y = 0; y < DstH; y++) {
                u8 *row = g_vic_src_buf + y * DstPitch;
                for (u32 x = 0; x < DstW; x++) {
                    row[x * 4 + 0] = 0xFF;                      /* A */
                    row[x * 4 + 1] = static_cast<u8>(x * 4);    /* R ramps across */
                    row[x * 4 + 2] = static_cast<u8>(y * 4);    /* G ramps down   */
                    row[x * 4 + 3] = 0x11;                      /* B constant     */
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);
            LogLine("   self-src pattern: row0[0..8]=%02x %02x %02x %02x %02x %02x %02x %02x",
                    g_vic_src_buf[0], g_vic_src_buf[1], g_vic_src_buf[2], g_vic_src_buf[3],
                    g_vic_src_buf[4], g_vic_src_buf[5], g_vic_src_buf[6], g_vic_src_buf[7]);
        }

        {
            const SrcDesc game_src{ g_game_surface.width, g_game_surface.height,
                                    g_game_surface.stride_px, vic::BLK_KIND_GENERIC_16Bx2,
                                    g_game_surface.block_h_log2, g_game_surface.pix_format };
            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                              (self_addr != 0) ? self_addr : src_addr, 0,
                              cfg_handle, dst_handle, src_handle, game_src };
            /* Once the VIC stops completing it is wedged; further submits achieve
             * nothing and keep the compositor starved. Stop at the first failure. */

            /* 1. Control: known good since M16. */
            if (!RunOneJob("vb:job_fill", VicJob::Fill, true, ctx)) {
                LogLine("   fill did not complete - engine wedged, skipping the rest");
                VicStage("vb:ABORTED_after_fill");
            } else if (self_addr == 0) {
                LogLine("   no address for our own source buffer; nothing further to try");
                VicStage("vb:no_self_addr");
            } else {
                /* 2. A genuine blit from a buffer WE own, so every address the
                 *    engine touches is one MAP_CMD_BUFFER actually gave us. */
                RunOneJob("vb:job_blit_self", VicJob::BlitSelf, true, ctx);
            }

            /* The game's buffer stays out of reach: its handle pins to phys=0
             * three different ways, and RunOneJob now refuses zero addresses. */
            if (src_addr == 0) {
                LogLine("   game blit still not possible: handle pins to phys=0");
                VicStage("vb:game_blit_SKIPPED");
            }
        }
        VicStage("vb:ALL_JOBS_DONE");

        TryIndirectCapture();

    close_vic:
        if (cfg_addr != 0) { UnmapCmdBuffer(vfd, cfg_handle); }
        if (dst_addr != 0) { UnmapCmdBuffer(vfd, dst_handle); }
        if (src_addr != 0)  { UnmapCmdBuffer(vfd, src_handle); }
        if (self_addr != 0) { UnmapCmdBuffer(vfd, self_handle); }
        NvClose(vfd);
    close_sess:
        serviceClose(std::addressof(g_nv_srv));
        tmemClose(std::addressof(g_nv_tmem));
        VicStage("vb:released");
    }

        /* ---- vi indirect-layer capture ------------------------------------
         * The only mechanism that can satisfy our constraint: 2450 writes the
         * layer image into a type-0x46 buffer, i.e. memory WE supply. Exact
         * marshalling taken from libnx viGetIndirectLayerImageMap, and our 2460
         * call already matched libnx byte-for-byte and returned rc=0.
         *
         * 102 was denied earlier only because the game's session is vi:u with
         * mode 0. Per switchbrew, "passing 1 to vi:s/vi:m results in the
         * IApplicationDisplayService having greater privileges" - so we open
         * vi:m ourselves and ask for mode 1. */
        void TryIndirectCapture() {
            VicStage("ind:1_open_vi_m");
            ::Service vi_root = {};
            ::Result rc = smGetService(std::addressof(vi_root), "vi:m");
            LogLine("   smGetService(vi:m) rc=0x%x", rc);
            if (R_FAILED(rc)) { VicStage("ind:1_FAILED"); return; }

            VicStage("ind:2_GetDisplayService");
            ::Service disp = {};
            {
                const u32 mode = 1;   /* privileged */
                rc = serviceDispatchIn(std::addressof(vi_root), 0, mode,
                    .out_num_objects = 1, .out_objects = std::addressof(disp));
                LogLine("   vi:m GetDisplayService(mode=1) rc=0x%x", rc);
                if (R_FAILED(rc)) { serviceClose(std::addressof(vi_root)); VicStage("ind:2_FAILED"); return; }
            }

            /* what does the privileged session unlock? */
            for (const auto &p : { std::pair<u32, const char *>{ 101, "GetSystemDisplayService" },
                                   std::pair<u32, const char *>{ 102, "GetManagerDisplayService" } }) {
                ::Service sub = {};
                const ::Result r = serviceDispatch(std::addressof(disp), p.first,
                    .out_num_objects = 1, .out_objects = std::addressof(sub));
                LogLine("   %u %-26s rc=0x%x %s", p.first, p.second, r,
                        R_SUCCEEDED(r) ? "GOT IT" : "denied");
                if (R_SUCCEEDED(r)) { serviceClose(std::addressof(sub)); }
            }

            VicStage("ind:3_required_memory");
            for (const auto &d : { std::pair<s32, s32>{ 1280, 720 }, std::pair<s32, s32>{ 1920, 1080 } }) {
                const struct { s64 w; s64 h; } in = { d.first, d.second };
                struct { s64 size; s64 align; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2460, in, out);
                LogLine("   2460(%dx%d) rc=0x%x -> size=%lld align=%lld",
                        d.first, d.second, r,
                        static_cast<long long>(out.size), static_cast<long long>(out.align));
            }

            /* 2450 itself. The consumer handle normally comes from AM, which a
             * sysmodule cannot reach - so try the plausible values and let the
             * error codes tell us which part it objects to. Each call only
             * writes into our own buffer, so a rejection costs nothing. */
            VicStage("ind:4_image_map");
            std::memset(g_ind_buf, 0xAB, 0x1000);
            armDCacheFlush(g_ind_buf, IndirectSize);
            const u64 handles[] = { 0, 1, 2, g_game_aruid };
            for (u64 h : handles) {
                const struct { s64 w; s64 h; u64 handle; u64 aruid; } in =
                    { 1280, 720, h, g_game_aruid };
                struct { s64 size; s64 stride; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2450, in, out,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias |
                                      SfBufferAttr_HipcMapTransferAllowsNonSecure },
                    .buffers      = { { g_ind_buf, 3801088 } },
                    .in_send_pid  = true);
                LogLine("   2450(1280x720, handle=%llu, aruid=%llu) rc=0x%x -> size=%lld stride=%lld",
                        static_cast<unsigned long long>(h),
                        static_cast<unsigned long long>(g_game_aruid), r,
                        static_cast<long long>(out.size), static_cast<long long>(out.stride));
                if (R_SUCCEEDED(r)) {
                    armDCacheFlush(g_ind_buf, IndirectSize);
                    u32 changed = 0;
                    for (u32 i = 0; i < 0x1000; i++) { if (g_ind_buf[i] != 0xAB) { changed++; } }
                    LogLine("   *** 2450 SUCCEEDED *** changed=%u/4096  first16: "
                            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                            changed, g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                            g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                            g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                            g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
                    VicStage("ind:CAPTURED");
                    break;
                }
            }

            serviceClose(std::addressof(disp));
            serviceClose(std::addressof(vi_root));
            VicStage("ind:done");
        }

        void VicWorkerThread(void *) {
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(200));
                const s32 slot = g_vic_pending.exchange(-1, std::memory_order_acq_rel);
                if (slot >= 0) {
                    RunVicBlit(slot);
                }
            }
        }

    }

    void StartVicWorker() {
        if (!g_vic_armed) { return; }
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_vic_thread), VicWorkerThread, nullptr,
                                        g_vic_stack, sizeof(g_vic_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_vic_thread), "applet-mitm.VIC");
        os::StartThread(std::addressof(g_vic_thread));
        g_vic_stage.store("waiting", std::memory_order_relaxed);
        LogLine("VIC worker started (execute=%s)", g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
    }

    void RequestVicBlit(s32 slot) {
        /* binder thread: hand off and get out. */
        g_vic_pending.store(slot, std::memory_order_release);
    }

}
