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
        constexpr size_t VicHeapSize = 2_MB;     /* os::AllocateMemoryBlock granularity */

        constinit uintptr_t g_vic_heap    = 0;
        constinit u8       *g_vic_cfg_buf = nullptr;
        constinit u8       *g_vic_cmd_buf = nullptr;
        constinit u8       *g_vic_dst_buf = nullptr;

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
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);   /* room to grow */
            std::memset(reinterpret_cast<void *>(addr), 0, VicHeapSize);
            LogLine("   VIC heap at 0x%lx: cfg=%p cmd=%p dst=%p",
                    static_cast<unsigned long>(addr),
                    static_cast<void *>(g_vic_cfg_buf),
                    static_cast<void *>(g_vic_cmd_buf),
                    static_cast<void *>(g_vic_dst_buf));
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

        void NvClose(u32 fd) {
            const struct { u32 fd; } in = { fd };
            u32 e = 0;
            serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e);
        }

        /* Pin one nvmap handle into the channel and get its device physical
         * address. Each call is breadcrumbed by the caller so that if this ever
         * takes nvservices down again we know exactly which handle did it. */
        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.is_compr     = 0;
            a.handle_id_in = handle;
            u32 nverr = 0;
            const ::Result rc = NvIoctl(chan_fd, NvHostIocChannelMapCmdBuf,
                                        std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   MAP_CMD_BUFFER(%s handle=%u) rc=0x%x nverr=%u -> phys=0x%x",
                    what, handle, rc, nverr, a.phys_addr_out);
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
        enum class VicJob { Fill, BlitDirect, BlitReloc };

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
            c->outputConfig.BackgroundAlpha = 1023;
            c->outputConfig.BackgroundR     = 1023;   /* opaque red */
            c->outputConfig.BackgroundG     = 0;
            c->outputConfig.BackgroundB     = 0;
        }

        void FillBlitConfig(vic::VicConfigStruct *c) {
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
            s->SlotPixelFormat   = g_game_surface.pix_format;                /* A8B8G8R8 */
            s->SlotBlkKind       = vic::BLK_KIND_GENERIC_16Bx2;             /* game kind 0xFE */
            s->SlotBlkHeight     = g_game_surface.block_h_log2;             /* MK8: 4 */
            s->SlotCacheWidth    = vic::CACHE_WIDTH_64Bx4;
            s->SlotSurfaceWidth  = g_game_surface.width  - 1;              /* 1919 */
            s->SlotSurfaceHeight = g_game_surface.height - 1;              /* 1079 */
            s->SlotLumaWidth     = g_game_surface.stride_px - 1;          /* 1919 */
            s->SlotLumaHeight    = g_game_surface.height - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;
        }

        /* host1x pushbuf: VIC_PUSH_METHOD / VIC_PUSH_BUFFER from libdrm vic.h.
         * Each entry: [INCR(0x10,2)] [method>>2] [value|reloc-placeholder]. */
        constexpr u32 Host1xIncr0x10x2 = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;

        /* Word indices of the three address slots, for the reloc variant. */
        constexpr u32 CfgAddrWord = 8, DstAddrWord = 11, SrcAddrWord = 14;

        u32 BuildCmdbuf(u32 *w, VicJob job, u32 cfg_addr, u32 dst_addr, u32 src_addr) {
            const bool reloc = (job == VicJob::BlitReloc);
            u32 n = 0;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_APPLICATION_ID >> 2; w[n++] = 1;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONTROL_PARAMS >> 2;
            w[n++] = (static_cast<u32>(sizeof(vic::VicConfigStruct) / 16)) << 16;
            /* MAP_CMD_BUFFER already handed back a device address for buffers we
             * own, so those go in directly and the submit needs no reloc list.
             * The BlitReloc variant instead leaves all three as placeholders and
             * lets nvservices patch them. */
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONFIG_STRUCT_OFFSET >> 2;
            w[n++] = reloc ? 0u : (cfg_addr >> 8);
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2;
            w[n++] = reloc ? 0u : (dst_addr >> 8);
            if (job != VicJob::Fill) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2;
                w[n++] = reloc ? 0u : (src_addr >> 8);
            }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::EXECUTE >> 2; w[n++] = 1u << 8;
            return n;
        }

        struct JobCtx {
            u32 vfd, cmd_handle, syncpt;
            u32 cfg_addr, dst_addr, src_addr, src_off;
            u32 cfg_handle, dst_handle, src_handle;
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
        void RunOneJob(const char *stage, VicJob job, const JobCtx &c) {
            VicStage(stage);

            auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
            if (job == VicJob::Fill) { FillClearConfig(cfg); } else { FillBlitConfig(cfg); }

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = BuildCmdbuf(w, job, c.cfg_addr, c.dst_addr, c.src_addr + c.src_off);
            words = AppendIncrSyncpt(w, words, c.syncpt, true);   /* OP_DONE: EXECUTE is present */

            /* Prefill with a poison pattern rather than zero. "All zero" cannot
             * distinguish "engine wrote zeros" from "engine never touched our
             * memory"; surviving 0xAB proves the latter outright. */
            std::memset(g_vic_dst_buf, 0xAB, DstSize);
            armDCacheFlush(g_vic_dst_buf, DstSize);
            armDCacheFlush(g_vic_cfg_buf, VicCfgSize);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            const bool reloc = (job == VicJob::BlitReloc);
            const u32  nr    = reloc ? ((job == VicJob::Fill) ? 2u : 3u) : 0u;

            alignas(8) u8 sb[16 + 12 + 3 * (16 + 4) + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };

            put(1); put(nr); put(1); put(1);
            put(c.cmd_handle); put(0); put(words);
            if (reloc) {
                put(c.cmd_handle); put(CfgAddrWord * 4); put(c.cfg_handle); put(0);
                put(c.cmd_handle); put(DstAddrWord * 4); put(c.dst_handle); put(0);
                put(c.cmd_handle); put(SrcAddrWord * 4); put(c.src_handle); put(c.src_off);
                put(8); put(8); put(8);
            }
            put(c.syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 nverr = 0, fence_val = 0;
            ::Result rc = NvIoctl(c.vfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogLine("   [%s] words=%u req=0x%08x sz=%u nr=%u rc=0x%x nverr=%u -> fence=%u",
                    stage, words, req, sz, nr, rc, nverr, fence_val);
            if (R_FAILED(rc) || nverr != 0) { LogLine("   [%s] SUBMIT REJECTED", stage); return; }

            u32 cfd = 0, ce = 0;
            if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(cfd), std::addressof(ce))) && ce == 0) {
                struct { u32 id; u32 thresh; u32 timeout; } a = { c.syncpt, fence_val, 100 };
                nverr = 0;
                rc = NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(nverr));
                struct { u32 id; u32 value; } r = { c.syncpt, 0 };
                u32 e2 = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                LogLine("   [%s] WAIT nverr=%u  syncpt=%u (want >= %u)%s",
                        stage, nverr, r.value, fence_val,
                        (r.value >= fence_val) ? "  OP_DONE fired" : "  ENGINE DID NOT COMPLETE");

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
        u32 cfg_addr = 0, dst_addr = 0, src_addr = 0;

        VicStage("vb:0_heap");
        if (!AllocVicHeap()) { VicStage("vb:0_FAILED"); return; }

        VicStage("vb:1_smGetService");
        ::Result rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
        LogLine("   nvdrv:s rc=0x%x", rc);
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
        VicStage("vb:4_open_nvmap");
        rc = NvOpen("/dev/nvmap", std::addressof(nvmap_fd), std::addressof(nverr));
        LogLine("   /dev/nvmap rc=0x%x fd=%u nverr=%u", rc, nvmap_fd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:4_FAILED"); goto close_sess; }

        u32 src_handle;
        VicStage("vb:5_FROM_ID");
        {
            struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
            rc = NvIoctl(nvmap_fd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   FROM_ID(%u) rc=0x%x nverr=%u -> handle=%u", g_game_surface.nvmap_id, rc, nverr, a.handle);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:5_FAILED"); goto close_sess; }
            src_handle = a.handle;
        }

        u32 cfg_handle, cfg_id, cmd_handle, cmd_id, dst_handle, dst_id;
        VicStage("vb:6_alloc_bufs");
        rc = NvmapOwn(nvmap_fd, g_vic_cfg_buf, VicCfgSize, 0, std::addressof(cfg_handle), std::addressof(cfg_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cfg_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_cmd_buf, VicCmdSize, 0, std::addressof(cmd_handle), std::addressof(cmd_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cmd_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_dst_buf, VicDstSize, 0, std::addressof(dst_handle), std::addressof(dst_id));
        if (R_FAILED(rc)) { VicStage("vb:6_dst_FAILED"); goto close_sess; }
        AMS_UNUSED(cfg_id, cmd_id, dst_id);
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
            VicStage("vb:8c_map_src");
            if (!MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)")) { VicStage("vb:8c_FAILED"); goto close_vic; }
            LogLine("   pinned: cfg=0x%x dst=0x%x src=0x%x (+slot off 0x%x)", cfg_addr, dst_addr, src_addr, src_off);
        }

        RunOneJob("vb:job_fill", VicJob::Fill,
                  JobCtx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, src_addr, src_off,
                          cfg_handle, dst_handle, src_handle });
        RunOneJob("vb:job_blit_direct", VicJob::BlitDirect,
                  JobCtx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, src_addr, src_off,
                          cfg_handle, dst_handle, src_handle });
        RunOneJob("vb:job_blit_reloc", VicJob::BlitReloc,
                  JobCtx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, src_addr, src_off,
                          cfg_handle, dst_handle, src_handle });
        VicStage("vb:ALL_JOBS_DONE");

    close_vic:
        if (cfg_addr != 0) { UnmapCmdBuffer(vfd, cfg_handle); }
        if (dst_addr != 0) { UnmapCmdBuffer(vfd, dst_handle); }
        if (src_addr != 0) { UnmapCmdBuffer(vfd, src_handle); }
        NvClose(vfd);
    close_sess:
        serviceClose(std::addressof(g_nv_srv));
        tmemClose(std::addressof(g_nv_tmem));
        VicStage("vb:released");
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
