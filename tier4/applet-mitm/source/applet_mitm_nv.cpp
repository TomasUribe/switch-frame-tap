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
    constinit bool g_dbg_armed = false;
    constinit u32  g_pmdmnt_rc = 0xFFFFFFFF;

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
        /* A fixed 8 MB request was refused with os::ResultOutOfMemory (0x1003):
         * a sysmodule's heap is capped well below that. Ask for the largest the
         * process will actually grant rather than guessing, and size the capture
         * to whatever we get. 2 MB granularity is the AllocateMemoryBlock unit. */
        constexpr size_t VicBufsEnd  = 0x30000;   /* cfg+cmd+dst+src all live below this */
        constexpr size_t FbBlockRowStage = 983040;   /* staging starts past one block-row */
        constinit size_t g_vic_heap_size = 0;

        /* The capture buffer stays on the HEAP, deliberately.
         *
         * Moving it to a 4 MB .bss array to dodge the 2 MB heap cap fataled a
         * different sysmodule (0100000000000023) with 0x10801 LimitReached at
         * boot: a sysmodule's static memory is drawn from the shared system
         * pool (pool_partition 2), so taking 4 MB of it starves everyone else.
         * This project already learned that once, when the early recon module
         * carried 9.3 MB of .bss.
         *
         * Heap is the safe place, so we live within the 2 MB ceiling and let
         * the resolution picker choose whatever actually fits. */
        constinit size_t g_ind_size = 0;

        constinit uintptr_t g_vic_heap    = 0;
        constinit u8       *g_vic_cfg_buf = nullptr;
        constinit u8       *g_vic_cmd_buf = nullptr;
        constinit u8       *g_vic_dst_buf = nullptr;
        constinit u8       *g_vic_src_buf = nullptr;
        constinit u8       *g_ind_buf     = nullptr;
        /* cached heap past the nvmap'd block-row, used to stage uncached engine
         * output before it goes to the filesystem */
        constinit u8       *g_stage_buf   = nullptr;

        bool AllocVicHeap() {
            if (g_vic_heap != 0) { return true; }

            size_t want = 0;
            /* measurement point 2 of 3: what the budget looks like at PROBE
             * time, with a game resident - to be compared against the boot
             * figure. 5 MB and 3 MB are dropped from the ladder: they returned
             * 0xca01 (kernel InvalidSize) because svcSetHeapSize requires 2 MB
             * granularity, so they were never valid requests and only added
             * noise. */
            {
                u64 t = 0, u = 0;
                ::ams::svc::GetInfo(std::addressof(t), ::ams::svc::InfoType_TotalMemorySize, ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(u), ::ams::svc::InfoType_UsedMemorySize,  ::ams::svc::PseudoHandle::CurrentProcess, 0);
                LogLine("   [probe, BEFORE heap grab] process total=%llu KB used=%llu KB free=%lld KB",
                        static_cast<unsigned long long>(t / 1024),
                        static_cast<unsigned long long>(u / 1024),
                        static_cast<long long>((static_cast<s64>(t) - static_cast<s64>(u)) / 1024));
            }
            /* CAPPED AT 2 MB ON PURPOSE. The ladder used to try 8 MB first,
             * and M50 proved what happens if a larger request is ever granted:
             * we drain a System pool with ~14 MB free and am dies with
             * LimitReached, taking the console with it. 2 MB has been safe across
             * ~20 runs. Widening this is a deliberate decision that needs
             * pool_partition to change first, not an optimisation. */
            for (const size_t sz : { 2_MB }) {
                const auto rc = os::SetMemoryHeapSize(sz);
                LogLine("   SetMemoryHeapSize(%zu MB) rc=0x%x", sz / (1024 * 1024), rc.GetValue());
                if (R_SUCCEEDED(rc)) { want = sz; break; }
            }
            if (want == 0) { LogLine("   no heap size accepted"); return false; }

            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), want); R_FAILED(rc)) {
                LogLine("   AllocateMemoryBlock(%zu MB) FAILED rc=0x%x", want / (1024 * 1024), rc.GetValue());
                return false;
            }
            /* masagrator's point on GBAtemp, and he is right: a sysmodule cannot
             * hold a 1080p frame. One is 7,913 KB against a 2 MB heap - 4x the
             * whole allocation - which is why everything here works in 983,040 B
             * block-rows. What we never did is ask the kernel what the budget
             * actually IS, instead of inferring it from a failed request. That
             * matters now, because NVENC needs its own input surface, bitstream
             * buffer and (unless we go all-intra) reference frames on top. */
            {
                u64 tot = 0, used = 0, srtot = 0, srused = 0;
                ::ams::svc::GetInfo(std::addressof(tot),    ::ams::svc::InfoType_TotalMemorySize,         ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(used),   ::ams::svc::InfoType_UsedMemorySize,          ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(srtot),  ::ams::svc::InfoType_SystemResourceSizeTotal, ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(srused), ::ams::svc::InfoType_SystemResourceSizeUsed,  ::ams::svc::PseudoHandle::CurrentProcess, 0);
                LogLine("   [probe, AFTER heap grab] total=%llu KB used=%llu KB free=%lld KB | sysresource %llu/%llu KB",
                        static_cast<unsigned long long>(tot / 1024),
                        static_cast<unsigned long long>(used / 1024),
                        static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024),
                        static_cast<unsigned long long>(srused / 1024),
                        static_cast<unsigned long long>(srtot / 1024));
                LogLine("   one 1080p frame is 7913 KB; heap granted %zu KB; NVENC must fit in what is left",
                        want / 1024);
            }
            g_vic_heap_size = want;
            g_ind_size      = want - VicBufsEnd;
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x20000);
            g_ind_buf     = reinterpret_cast<u8 *>(addr + VicBufsEnd);
            g_stage_buf   = g_ind_buf + FbBlockRowStage;

            std::memset(reinterpret_cast<void *>(addr), 0, want);
            LogLine("   VIC heap %zu MB at 0x%lx (capture buffer %zu KB on heap): cfg=%p cmd=%p dst=%p src=%p",
                    want / (1024 * 1024), static_cast<unsigned long>(addr), g_ind_size / 1024,
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
        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable = false) {
            u32 nverr = 0;
            struct { u32 size; u32 handle; } cr = { size, 0 };
            ::Result rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn CREATE fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            struct {
                u32 handle; u32 heapmask; u32 flags; u32 align;
                u8 kind; u8 pad[7]; u64 addr;
            } al = {};
            al.handle = cr.handle;
            al.flags  = cacheable ? 1u : 0u;      /* bit0 = CACHEABLE */
            al.align  = 0x1000;
            al.kind   = kind;
            al.addr   = reinterpret_cast<u64>(cpu);
            rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn ALLOC fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            /* flags bit0 is CACHEABLE (libnx passes is_cpu_cacheable?1:0), not
             * read-write as the wiki's comment suggests. For a non-cacheable
             * buffer we owe the matching CPU-side step that libnx does and we
             * never did: flush, then mark the mapping uncached. Without it the
             * CPU keeps a cached, non-coherent view.
             *
             * The capture buffer is the exception. It is written by
             * ReadDebugProcessMemory - a kernel memcpy of 983,040 B, every
             * frame - and an uncached destination would cost far more than
             * cache maintenance does. It stays CACHED, and the caller flushes
             * it before the engine reads. */
            armDCacheFlush(cpu, size);
            if (!cacheable) {
                const auto sma = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(cpu), size,
                                                         svc::MemoryAttribute_Uncached,
                                                         svc::MemoryAttribute_Uncached);
                LogLine("   SetMemoryAttribute(%p, 0x%x, uncached) rc=0x%x", cpu, size, sma.GetValue());
            } else {
                LogLine("   nvmapOwn(%p, 0x%x) kept CACHEABLE - caller must flush before submit", cpu, size);
            }

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
        enum class VicJob { Fill, BlitSelf, BlitGame, BlitStrip };

        /* stride_px is what goes into SlotLumaWidth (+1). For pitch-linear that
         * is plainly the pixel stride. For BLOCK-linear nobody has told us
         * whether the field wants pixels or BYTES: libdrm only ever blits
         * pitch surfaces and marks both SlotBlkHeight and SlotCacheWidth
         * "XXX". So it is swept below rather than guessed. */
        /* w/h describe the SURFACE; rect_w/rect_h the region actually sampled.
         * They differ only for the unscaled 1:1 control. */
        struct SrcDesc { u32 w, h, stride_px, blk_kind, blk_h_log2, pixfmt, cache_w, rect_w, rect_h; };

        /* our own pitch-linear 64x64 buffer, stride 256 px like the destination */
        constexpr SrcDesc SelfSrc { DstW, DstH, DstStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, DstW, DstH };

        /* The OUTPUT format has been hardcoded A8B8G8R8 since M19 and never
         * swept. The 64x64 self-blit could not reveal an R<->B swap either: its
         * painted ramp differed in R and B only by a constant. */
        struct OutDesc { u32 w, h, stride_px, fmt; };

        /* the 64x64 self-blit target, unchanged: it is the byte-exact regression
         * reference and must keep producing identical output */
        constexpr OutDesc SelfOut { DstW, DstH, DstStridePx, vic::PIXFMT_A8B8G8R8 };

        /* One block-row of the GAME's surface: full 1920 px width by 128 rows,
         * block-linear, exactly what the binder parcels describe.
         *
         * NOTE the kind. SlotBlkKind is a 4-BIT field carrying the VIC's own
         * enum - 0 pitch, 1 generic 16Bx2 - not the nvmap kind 0xFE that the
         * parcels report. Writing 0xFE there truncates to 0xE and hands the
         * engine a surface layout that does not exist, and a bad layout hangs
         * the VIC, which takes the compositor and the console with it. */
        constexpr u32 StripW = 1920, StripH = 128;
        constexpr SrcDesc StripSrc { StripW, StripH, StripW,
                                     vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8,
                                     vic::CACHE_WIDTH_64Bx4, StripW, StripH };
        static_assert(StripSrc.rect_w > 0 && StripSrc.rect_h > 0,
                      "a zero rect underflows to 0xFFFFFFFF in FillBlitConfig");
        /* set per variant before each BlitStrip submit */
        constinit SrcDesc g_strip_src = StripSrc;

        /* 4x downscale into the existing 64 KB destination. The stride is
         * aligned to 256 px the way libdrm aligns every VIC surface (M13):
         * 512 px = 2048 B, x 32 rows = 65536 B, exactly DstSize. */
        /* M41 produced a completed blit whose pixels were scrambled: the right
         * brightness structure, no coherent image. That is a source-LAYOUT
         * error, not scale or crop. Three fields could cause it and none has a
         * reference, so sweep them in one boot the way M16 settled SETCL. */
        /* M43 settled it, and it was never the layout. The engine's output came
         * back as [0xFF, G_src, B_src, A_src] - the source shifted down one lane
         * with alpha forced to max by ConstantAlpha/PlanarAlpha. Per-lane error
         * against a software de-swizzle of the same bytes:
         *
         *     aligned:  out[1]vsR 7.15   out[2]vsG 21.02   out[3]vsB 195.71
         *     shifted:  out[1]vsG 1.87   out[2]vsB  1.69   out[3]vsA   0.70
         *
         * Under 2 LSB on every lane once shifted - filter-difference magnitude.
         * So geometry, scale and block-linear addressing were all correct, and
         * the engine simply ate the source's R byte as alpha.
         *
         * The game's surface is R,G,B,A in memory, which the VIC calls
         * R8G8B8A8, not the A8B8G8R8 I declared. Sweep the three candidates
         * rather than assume, keeping the layout that already works. */
        /* M44's mapping table showed every format is a LOSSLESS permutation -
         * each output lane matched some source lane under 5 LSB, alpha included.
         * Nothing is destroyed; the channels are merely shuffled. P32_argb came
         * closest, with only R and B transposed:
         *
         *     out0(A)=srcA 1.11   out1(R)=srcB 4.12
         *     out2(G)=srcG 4.52   out3(B)=srcR 4.98
         *
         * So this sweeps SOURCE x OUTPUT format - the output side has been fixed
         * at A8B8G8R8 since M19 and never tested - and one of the four must be
         * the identity mapping.
         *
         * ONE2ONE is the separate question: the residual 4-5 LSB is either the
         * VIC's polyphase scaler differing from a box filter, or a small
         * sampling offset. An unscaled 480x32 crop cannot have a filter error,
         * so if it lands near zero the residual is filtering and harmless. */
        /* M45 settled the VIC completely. ONE2ONE - the unscaled 480x32 crop -
         * matched a software de-swizzle with err 0.00 on ALL FOUR LANES. Not
         * approximately: bit for bit. So block-linear addressing, GOB tiling,
         * stride and block height are exact, and the 2-3 LSB seen on scaled
         * variants is just the VIC's polyphase scaler differing from a box
         * filter - not an error.
         *
         * Channel order: S32O33 maps out=ABGR, a pure R<->B swap from identity.
         * Every variant is a LOSSLESS permutation, so this costs nothing to undo
         * on the PC, or by choosing NVENC's input format - which has to be
         * configured anyway. Not worth another docked run.
         *
         * Two variants kept as regression only. */
        struct StripVariant { const char *name; u32 src_fmt; u32 out_fmt; u32 rect_w, rect_h; };
        constexpr StripVariant StripVariants[] = {
            { "BEST",    vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, StripW, StripH },
            { "ONE2ONE", vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, 480,    32     },
        };
        constexpr u32 StripVariantCount = sizeof(StripVariants) / sizeof(StripVariants[0]);

        constexpr OutDesc StripOut { 480, 32, 512, vic::PIXFMT_A8B8G8R8 };
        constinit OutDesc g_strip_out = StripOut;   /* set per variant */
        constexpr u32 StripOutSize = StripOut.stride_px * 4 * StripOut.h;
        static_assert(StripOutSize <= DstSize);

        void FillOutputConfig(vic::VicConfigStruct *c, const OutDesc &out) {
            c->outputConfig.TargetRectLeft   = 0;
            c->outputConfig.TargetRectTop    = 0;
            c->outputConfig.TargetRectRight  = out.w - 1;
            c->outputConfig.TargetRectBottom = out.h - 1;

            c->outputSurfaceConfig.OutPixelFormat   = out.fmt;
            c->outputSurfaceConfig.OutBlkKind       = vic::BLK_KIND_PITCH;
            c->outputSurfaceConfig.OutBlkHeight     = 0;
            c->outputSurfaceConfig.OutSurfaceWidth  = out.w - 1;
            c->outputSurfaceConfig.OutSurfaceHeight = out.h - 1;
            c->outputSurfaceConfig.OutLumaWidth     = out.stride_px - 1;
            c->outputSurfaceConfig.OutLumaHeight    = out.h - 1;
            c->outputSurfaceConfig.OutChromaWidth   = 16383;
            c->outputSurfaceConfig.OutChromaHeight  = 16383;
        }

        /* libdrm vic40_fill / vic_clear: paint the whole target one colour with
         * no slot enabled. Anything non-zero in dst afterwards proves the whole
         * output half of the pipeline. */
        void FillClearConfig(vic::VicConfigStruct *c) {
            std::memset(c, 0, sizeof(*c));
            FillOutputConfig(c, SelfOut);
            /* Four DISTINCT levels so the output byte order can be read straight
             * off the dump: 1023->0xFF, 768->0xC0, 512->0x80, 256->0x40.
             * The previous red gave 'ff ff 00 00', which could not say which
             * byte was alpha and which was red. */
            c->outputConfig.BackgroundAlpha = 1023;   /* 0xFF */
            c->outputConfig.BackgroundR     = 768;    /* 0xC0 */
            c->outputConfig.BackgroundG     = 512;    /* 0x80 */
            c->outputConfig.BackgroundB     = 256;    /* 0x40 */
        }

        void FillBlitConfig(vic::VicConfigStruct *c, const SrcDesc &src, const OutDesc &out) {
            std::memset(c, 0, sizeof(*c));

            FillOutputConfig(c, out);
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
            slot->SourceRectRight     = static_cast<u64>(src.rect_w - 1) << 16;   /* 16.16 fixed point */
            slot->SourceRectTop       = 0;
            slot->SourceRectBottom    = static_cast<u64>(src.rect_h - 1) << 16;
            slot->DestRectLeft        = 0;
            slot->DestRectRight       = out.w - 1;
            slot->DestRectTop         = 0;
            slot->DestRectBottom      = out.h - 1;
            slot->SoftClampHigh       = 1023;

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[0].slotSurfaceConfig);
            s->SlotPixelFormat   = src.pixfmt;
            s->SlotBlkKind       = src.blk_kind;
            s->SlotBlkHeight     = src.blk_h_log2;
            s->SlotCacheWidth    = src.cache_w;
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
            const bool has_src  = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
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
        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr);

        /* NVENC, phase A: exactly what M11 did for the VIC before trusting it
         * with real work - open the channel, take its syncpoint, bind the nvmap
         * fd, and submit a command buffer that does nothing but increment that
         * syncpoint. No SETCL, no method writes, so the engine cannot be pointed
         * at a bad address and cannot hang. If the fence advances, the msenc
         * channel and submit ABI are usable and the encode config is the only
         * thing left unknown. */
        void TryNvencPhaseA(u32 nvmap_fd, u32 cmd_handle);

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

            const bool needs_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
            if (c.cfg_addr == 0 || c.dst_addr == 0 || (needs_src && c.src_addr == 0)) {
                LogLine("   [%s] REFUSING to submit: cfg=0x%x dst=0x%x src=0x%x - a zero "
                        "address hangs the VIC and freezes the console",
                        stage, c.cfg_addr, c.dst_addr, c.src_addr);
                return false;
            }

            auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
            switch (job) {
                case VicJob::Fill:     FillClearConfig(cfg);            break;
                case VicJob::BlitSelf:  FillBlitConfig(cfg, SelfSrc,   SelfOut);  break;
                case VicJob::BlitGame:  FillBlitConfig(cfg, c.game_src, SelfOut); break;
                case VicJob::BlitStrip: FillBlitConfig(cfg, g_strip_src, g_strip_out); break;
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

    constinit bool g_dump_armed    = false;
    constinit u32  g_probe_delay_s = 120;

    constinit std::atomic<u32> g_queue_count{0};
    constinit std::atomic<s32> g_queue_slot{-1};

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
                                    g_game_surface.block_h_log2, g_game_surface.pix_format,
                                    vic::CACHE_WIDTH_64Bx4,
                                    g_game_surface.width, g_game_surface.height };
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
        TryDebugCapture(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, dst_addr);
        TryNvencPhaseA(nvmap_fd, cmd_handle);

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
            /* GetDisplayService's COMMAND ID is the service type, not 0.
             * libnx: _viCmdGetSession(root, out, inval, g_viServiceType) - the
             * service type is passed as the cmd_id, so vi:u=0, vi:s=1, vi:m=2,
             * with inval 1 for system/manager. Sending cmd 0 to
             * IManagerRootService is an unknown command, which is why vi closed
             * the session on us (0xF601 SessionClosed). A closed session cannot
             * be reused, so each attempt re-opens the root service. */
            VicStage("ind:1_open_vi_root");
            ::Service vi_root = {};
            ::Service disp    = {};
            bool got_disp = false;
            for (const auto &v : { std::tuple<const char *, u32, u32>{ "vi:m", 2, 1 },
                                   std::tuple<const char *, u32, u32>{ "vi:s", 1, 1 } }) {
                const char *name = std::get<0>(v);
                const u32 cmd = std::get<1>(v), mode = std::get<2>(v);

                ::Result r = smGetService(std::addressof(vi_root), name);
                if (R_FAILED(r)) { LogLine("   smGetService(%s) rc=0x%x", name, r); continue; }

                r = serviceDispatchIn(std::addressof(vi_root), cmd, mode,
                    .out_num_objects = 1, .out_objects = std::addressof(disp));
                LogLine("   %s GetDisplayService cmd=%u mode=%u rc=0x%x", name, cmd, mode, r);
                if (R_SUCCEEDED(r)) { got_disp = true; break; }
                serviceClose(std::addressof(vi_root));
            }
            if (!got_disp) { VicStage("ind:2_FAILED"); return; }
            ::Result rc = 0;
            AMS_UNUSED(rc);

            /* what does the privileged session unlock? */
            ::Service mgr_keep = {};
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

            /* Open a display on OUR privileged session, then make a real
             * indirect layer. 0x60A is ams::sf::PreconditionViolation - the
             * marshalling was accepted and only the consumer handle was bogus,
             * so a genuine handle is the whole remaining gap.
             *
             * 2050 CreateIndirectLayer is undocumented, but its sibling
             * viCreateManagedLayer(2010) is { u32 flags; u32 pad; u64 display_id;
             * u64 aruid; } -> u64. Probing shapes is safe: a wrong raw size
             * answers InvalidCmifHeaderSize(0x1940A) and a missing command
             * answers UnknownMethodId(0x1BA0A), so the errors discriminate. */
            u64 display_id = 0;
            {
                struct { char data[0x40]; } name = {};
                std::strncpy(name.data, "Default", sizeof(name.data) - 1);
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 1010, name, display_id);
                LogLine("   1010 OpenDisplay(\"Default\") rc=0x%x -> display_id=%llu",
                        r, static_cast<unsigned long long>(display_id));
            }

            u64 consumer = 0;
            if (R_SUCCEEDED(serviceDispatch(std::addressof(disp), 102,
                                            .out_num_objects = 1, .out_objects = std::addressof(mgr_keep)))) {
                VicStage("ind:3b_create_indirect_layer");
                /* shape A: { u64 display_id; u64 aruid } -> u64 */
                {
                    const struct { u64 display_id; u64 aruid; } in = { display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeA{disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                /* shape B: { u32 flags; u32 pad; u64 display_id; u64 aruid } -> u64 */
                if (consumer == 0) {
                    const struct { u32 flags; u32 pad; u64 display_id; u64 aruid; } in =
                        { 0, 0, display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeB{flags,disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                /* shape C: { u64 aruid } -> u64 */
                if (consumer == 0) {
                    const u64 in = g_game_aruid;
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeC{aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                LogLine("   consumer handle = %llu", static_cast<unsigned long long>(consumer));

                /* A bare indirect layer has no producer, so there is no image to
                 * return - hence the same PreconditionViolation. The command
                 * names spell out the intended sequence: layer, then a producer
                 * endpoint, then a consumer endpoint. 2050's ABI was guessable
                 * from its documented sibling, so try the same shape here. */
                if (consumer != 0) {
                    VicStage("ind:3c_endpoints");
                    for (const auto &e : { std::pair<u32, const char *>{ 2052, "CreateIndirectProducerEndPoint" },
                                           std::pair<u32, const char *>{ 2054, "CreateIndirectConsumerEndPoint" } }) {
                        {   /* shape {u64 handle; u64 aruid} */
                            const struct { u64 h; u64 aruid; } in = { consumer, g_game_aruid };
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h,aruid} rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                            if (R_SUCCEEDED(r)) { continue; }
                        }
                        {   /* shape {u64 handle} */
                            const u64 in = consumer;
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h}       rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                        }
                    }
                }
            }

            /* 2450 itself. 0x60A on every made-up handle told us the request
             * shape is right and only the handle was wrong, so try the real one
             * first and keep the guesses as a fallback. Each call only writes
             * into our own buffer, so a rejection costs nothing. */
            VicStage("ind:4_image_map");
            /* Pick the largest resolution whose required size fits the heap we
             * were actually granted, rather than assuming 720p fits. */
            s32 cw = 0, ch = 0;
            for (const auto &d : { std::pair<s32, s32>{ 1920, 1080 }, std::pair<s32, s32>{ 1280, 720 },
                                   std::pair<s32, s32>{ 960, 540 },  std::pair<s32, s32>{ 640, 360 } }) {
                const struct { s64 w; s64 h; } q = { d.first, d.second };
                struct { s64 size; s64 align; } a = {};
                if (R_SUCCEEDED(serviceDispatchInOut(std::addressof(disp), 2460, q, a)) &&
                    a.size > 0 && static_cast<size_t>(a.size) <= g_ind_size) {
                    cw = d.first; ch = d.second;
                    LogLine("   capture size %dx%d needs %lld B, buffer is %zu B - OK",
                            cw, ch, static_cast<long long>(a.size), g_ind_size);
                    break;
                }
            }
            if (cw == 0) { LogLine("   no resolution fits the %zu B buffer", g_ind_size); }

            std::memset(g_ind_buf, 0xAB, 0x1000);
            armDCacheFlush(g_ind_buf, g_ind_size);
            const u64 handles[] = { consumer, 0, 1, 2, g_game_aruid };
            for (u64 h : handles) {
                if (cw == 0) { break; }
                const struct { s64 w; s64 h; u64 handle; u64 aruid; } in =
                    { cw, ch, h, g_game_aruid };
                struct { s64 size; s64 stride; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2450, in, out,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias |
                                      SfBufferAttr_HipcMapTransferAllowsNonSecure },
                    .buffers      = { { g_ind_buf, g_ind_size } },
                    .in_send_pid  = true);
                LogLine("   2450(%dx%d, handle=%llu, aruid=%llu) rc=0x%x -> size=%lld stride=%lld", cw, ch,
                        static_cast<unsigned long long>(h),
                        static_cast<unsigned long long>(g_game_aruid), r,
                        static_cast<long long>(out.size), static_cast<long long>(out.stride));
                if (R_SUCCEEDED(r)) {
                    armDCacheFlush(g_ind_buf, g_ind_size);
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

            if (serviceIsActive(std::addressof(mgr_keep))) { serviceClose(std::addressof(mgr_keep)); }
            serviceClose(std::addressof(disp));
            serviceClose(std::addressof(vi_root));
            VicStage("ind:done");
        }

        /* ---- the debugger route -------------------------------------------
         * nvservices will not map another process's pages for us, and vi will
         * not populate an indirect layer without AM. But the kernel has one
         * mechanism designed for exactly this, and Atmosphere's own cheat
         * engine dmnt uses it against running games at 60 Hz:
         *
         *     svcDebugActiveProcess -> svcQueryDebugProcessMemory
         *                           -> svcReadDebugProcessMemory
         *
         * DebugActiveProcess STOPS the target. Our logger does an SD write per
         * line, which would freeze the game for hundreds of ms, so everything
         * is collected silently while attached and logged only after detaching.
         * Closing the debug handle resumes the process. Read-only: the NPDM
         * deliberately omits WriteDebugProcessMemory and TerminateDebugProcess.
         *
         * The three kernel gates, read straight out of mesosphere (M34):
         *
         *   kern_svc_debug.cpp:28   DebugActiveProcess needs
         *                             IsDebugMode() || CanForceDebugProd()
         *   kern_svc_debug.cpp:38   ...and
         *                             target->IsPermittedDebug()
         *                             || CanForceDebug() || CanForceDebugProd()
         *   kern_svc_debug.cpp:232/276  Query/ReadDebugProcessMemory need
         *                             IsDebugMode() || CanForceDebugProd()
         *
         * M33 declared none of those NPDM debug flags, so its attach could only
         * ever have worked if the game itself were marked permitted-debug.
         * M34 adds "force_debug": true - the same flag creport and dmnt.gen2
         * declare - which satisfies the second gate unconditionally.
         *
         * And the read itself is NOT blocked by the framebuffer's attributes:
         * kern_k_page_table_base.cpp:2743 checks state/permission with an
         * attribute mask of None, so MemoryAttribute_DeviceShared - which every
         * nvmap-pinned page carries - does not disqualify the range. That is the
         * whole reason this route can work where nvmap FROM_ID could not.
         *
         * Not done here, deliberately: after attaching, draining GetDebugEvent
         * and calling ContinueDebugEvent(ExceptionHandled | ContinueAll) would
         * let the game keep running while we stay attached - which is how dmnt
         * reads cheat addresses at 60 Hz, and is what a streaming capture would
         * need. Both SVCs are already in our NPDM. This one-shot probe keeps the
         * game stopped instead, because a frozen frame is the cleaner sample. */
        struct RegionHit { u64 base; u64 size; u32 state; u32 perm; u32 attr; u32 devices; };

        /* The swapchain is ONE nvmap object: three slots of 8,847,360 B, laid out
         * contiguously at the offsets the binder parcels already handed us. */
        constexpr u64 FbSlotSize   = 8847360ull;
        constexpr u64 FbSwapSize   = 3ull * FbSlotSize;               /* 26,542,080 */
        constexpr u64 FbSlotOff[3] = { 0ull, 0x870000ull, 0x10E0000ull };

        struct FbCandidate { u64 addr; u64 region_base; u32 lane; u32 varied; u8 head[3][16]; };

        /* One block-row: 120 blocks x 8192 B, covering the full 1920 px width by
         * 128 rows, contiguous. Nine of them are exactly one 8,847,360 B slot. */
        constexpr u64 FbBlockRow = 983040ull;

        /* M35 found it. Region base 0x10851e6000, size EXACTLY 26,542,080 - three
         * slots, and the same size nvmap PARAM reported for handle 1268 - with the
         * opaque-pixel signature at all three slot offsets:
         *
         *   slot0 ff fb ff ff | slot1 ff f7 ff ff | slot2 ff f4 ff ff
         *
         * Same lane constant, the varying lane differing per slot: three
         * successive frames of a near-white top-left corner. That is the
         * swapchain. Exactly one candidate survived ~24,000 probed offsets.
         *
         * Two things M35 got wrong, fixed here:
         *   - it reported the offset against big[0] rather than the region the
         *     candidate was actually found in (cosmetic, but it read as 0x11d79000
         *     when the true answer is offset 0 of an exactly-sized region);
         *   - it scanned 24,000 offsets and burned its whole read budget when an
         *     exactly-26,542,080-byte region is a dead giveaway. Check those
         *     first: the freeze drops from ~340 ms to ~1 ms.
         *
         * ADDRESSES ARE NOT STABLE. The same region was at 0x14f5e6d000 one boot
         * and 0x107346d000 the next. It must be located at runtime, every time. */
        /* M36 matched on an all-0xFF corner, so every lane looked "opaque" and
         * lane 0 won by being first. The real alpha lane is 3: the captured
         * strip reads aa aa af ff / ab ab af ff, i.e. A8B8G8R8 stored
         * little-endian as R,G,B,A. Hence "opaque pixels 0/245760" - the stat
         * counted lane 0, which is red. Two fixes: the fallback scan now really
         * does reject a uniform block (M36 computed `varied` and never acted on
         * it), and the alpha lane is measured from the strip rather than trusted
         * from 16 bytes of white. */
        s32 FbPixelLane(const u8 *p, u32 *out_varied) {
            for (u32 lane = 0; lane < 4; ++lane) {
                bool opaque = true;
                for (u32 i = 0; i < 16; ++i) {
                    if (p[i * 4 + lane] != 0xFF) { opaque = false; break; }
                }
                if (!opaque) { continue; }

                u32 varied = 0;
                for (u32 o = 0; o < 4; ++o) {
                    if (o == lane) { continue; }
                    for (u32 i = 1; i < 16; ++i) {
                        if (p[i * 4 + o] != p[o]) { ++varied; break; }
                    }
                }
                if (varied == 0) { continue; }          /* uniform block - not a frame */
                if (out_varied != nullptr) { *out_varied = varied; }
                return static_cast<s32>(lane);
            }
            return -1;
        }

        /* Stream one whole slot out to the SD card, 64 KB at a time, while the
         * game runs. 8,847,360 B = 120 blocks x 8192 B x 9 block-rows = a
         * 1920x1152 block-linear surface, of which the top 1080 rows are the
         * frame. De-swizzling happens on the PC (tools/deswizzle.py). */
        constexpr const char *FrameBinPath = "sdmc:/applet-mitm-frame.bin";
        constexpr const char *FrameTxtPath = "sdmc:/applet-mitm-frame.txt";

        bool WriteBufToSd(const char *path, const void *buf, size_t len) {
            fs::DeleteFile(path);
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return false; }
            const auto wrc = fs::WriteFile(f, 0, buf, len, fs::WriteOption::Flush);
            fs::CloseFile(f);
            if (R_FAILED(wrc)) { LogLine("   WriteBufToSd(%s) rc=0x%x", path, wrc.GetValue()); }
            return R_SUCCEEDED(wrc);
        }

        constexpr const char *StripBinPath = "sdmc:/applet-mitm-strip.bin";
        constexpr const char *VicBinPath   = "sdmc:/applet-mitm-vic.bin";

        bool DumpSlotToSd(::ams::svc::Handle dbg, u64 src, u64 *out_ns, u32 *out_chunks) {
            fs::DeleteFile(FrameBinPath);
            if (R_FAILED(fs::CreateFile(FrameBinPath, static_cast<s64>(FbSlotSize)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), FrameBinPath, fs::OpenMode_Write))) { return false; }

            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 chunks = 0;
            bool ok = true;
            for (u64 done = 0; done < FbSlotSize; done += 0x10000) {
                const u64 n = (FbSlotSize - done < 0x10000) ? (FbSlotSize - done) : 0x10000;
                if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_ind_buf), dbg, src + done, n))) { ok = false; break; }
                const auto opt = (done + n >= FbSlotSize) ? fs::WriteOption::Flush : fs::WriteOption::None;
                if (R_FAILED(fs::WriteFile(f, static_cast<s64>(done), g_ind_buf, n, opt))) { ok = false; break; }
                ++chunks;
            }
            *out_ns     = armTicksToNs(armGetSystemTick()) - t0;
            *out_chunks = chunks;
            fs::CloseFile(f);
            return ok;
        }

        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr) {
            if (!g_dbg_armed) { return; }

            VicStage("dbg:1_find_pid");
            if (g_pmdmnt_rc != 0) {
                LogLine("   pmdmntInitialize() failed at boot (rc=0x%x) - skipping", g_pmdmnt_rc);
                VicStage("dbg:1_no_pmdmnt");
                return;
            }
            ::ams::os::ProcessId pid{};
            {
                const Result r = ::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid));
                LogLine("   pm:dmnt GetApplicationProcessId rc=0x%x -> pid=%llu",
                        r.GetValue(), static_cast<unsigned long long>(pid.value));
                if (R_FAILED(r)) { VicStage("dbg:1_FAILED"); return; }
            }

            /* ---- attached from here. The game is STOPPED until we Continue. -- */
            RegionHit   big[8]  = {};   u32 nbig  = 0;
            FbCandidate cand    = {};   bool found = false, by_size = false;
            u32  steps = 0, reads = 0, budget = 24000, nexact = 0;
            bool walk_complete = false;

            u32  nev = 0;
            ::ams::Result r_attach{}, r_cont = ::ams::svc::ResultInvalidHandle();
            bool attached = false, resumed = false;

            u64  strip_ns = 0, dump_ns = 0, full_ns = 0;
            u32  strip_chunks_ok = 0, dump_chunks = 0, full_strips = 0;
            u32  cap_done = 0, cap_missed = 0, cap_distinct = 0;
            u64  cap_min = ~UINT64_C(0), cap_max = 0, cap_sum = 0, cap_ns = 0;
            u32  fps_before = 0, fps_during = 0, fps_after = 0;
            u32  cap_over = 0, slot_hits[4] = {};
            u32  strip_own_rc = 0, strip_pin = 0;
            u64  frozen_ns = 0;
            bool strip_read_ok = false, strip_blit_ok = false, strip_dumped = false, vic_dumped = false;
            u32  strip_ok_count = 0, strip_dump_count = 0, variant_sum[8] = {};
            bool dumped = false;
            u32  nonzero = 0, distinct = 0, lane_ff[4] = {}, total_px = 0;
            u8   live_a[16] = {}, live_b[16] = {};
            bool live_ok = false, live_changed = false;

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            r_attach = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid.value);
            if (R_SUCCEEDED(r_attach)) {
                attached = true;

                u64 addr = 0;
                for (; steps < 4000; ++steps) {
                    ::ams::svc::MemoryInfo mi = {};
                    ::ams::svc::PageInfo   pi = {};
                    if (R_FAILED(::ams::svc::QueryDebugProcessMemory(std::addressof(mi), std::addressof(pi), dbg, addr))) { walk_complete = true; break; }
                    if (mi.size == 0) { walk_complete = true; break; }

                    const u32  attr = static_cast<u32>(mi.attribute);
                    const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;

                    if (dev && mi.size >= FbSwapSize && nbig < 8) {
                        const RegionHit r = { mi.base_address, mi.size, static_cast<u32>(mi.state),
                                              static_cast<u32>(mi.permission), attr, mi.device_count };
                        if (mi.size == FbSwapSize) {
                            for (u32 k = nbig; k > nexact; --k) { big[k] = big[k - 1]; }
                            big[nexact++] = r;
                        } else {
                            big[nbig] = r;
                        }
                        ++nbig;
                    }
                    const u64 next = mi.base_address + mi.size;
                    if (next <= addr) { walk_complete = true; break; }
                    addr = next;
                }

                /* A device-shared region of EXACTLY three slots is the swapchain:
                 * that is nvmap 1268's size, and it held at offset 0 on both runs
                 * that found it. Take it without demanding a pixel pattern - the
                 * corner can legitimately be flat white, which is what confused
                 * M36's lane detection. The signature scan stays as the fallback
                 * for a console whose layout does not produce an exact region. */
                u8 s[3][64];
                if (nexact > 0) {
                    cand.addr        = big[0].base;
                    cand.region_base = big[0].base;
                    cand.lane        = 3;                  /* A8B8G8R8 -> R,G,B,A */
                    found            = true;
                    by_size          = true;
                    for (u32 k = 0; k < 3; ++k) {
                        ++reads;
                        if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(s[k]), dbg, cand.addr + FbSlotOff[k], 64))) {
                            std::memcpy(cand.head[k], s[k], 16);
                        }
                    }
                }
                for (u32 r = 0; r < nbig && !found && budget > 0; ++r) {
                    const u64 last = big[r].size - FbSwapSize;
                    for (u64 off = 0; off <= last && !found && budget > 0; off += 0x1000) {
                        --budget; ++reads;
                        if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(s[0]), dbg, big[r].base + off, 64))) { continue; }
                        u32 varied = 0;
                        const s32 lane = FbPixelLane(s[0], std::addressof(varied));
                        if (lane < 0) { continue; }

                        bool all_slots = true;
                        for (u32 k = 1; k < 3; ++k) {
                            --budget; ++reads;
                            if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(s[k]), dbg, big[r].base + off + FbSlotOff[k], 64))
                                || FbPixelLane(s[k], nullptr) != lane) { all_slots = false; break; }
                        }
                        if (!all_slots) { continue; }

                        cand.addr        = big[r].base + off;
                        cand.region_base = big[r].base;
                        cand.lane        = static_cast<u32>(lane);
                        cand.varied      = varied;
                        for (u32 k = 0; k < 3; ++k) { std::memcpy(cand.head[k], s[k], 16); }
                        found = true;
                    }
                }

                /* Resuming is now done as early as possible, so it is a guarded
                 * lambda rather than a fixed point in the sequence. */
                const u64 t_frozen0 = armTicksToNs(armGetSystemTick());
                auto resume_game = [&]() {
                    if (resumed) { return; }
                    ::ams::svc::DebugEventInfo ev;
                    while (nev < 64 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++nev; }
                    r_cont  = ::ams::svc::ContinueDebugEvent(dbg,
                                 ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll,
                                 nullptr, 0);
                    resumed = R_SUCCEEDED(r_cont);
                    frozen_ns = armTicksToNs(armGetSystemTick()) - t_frozen0;
                };

                /* ---- THE VIC, ON REAL GAME PIXELS -------------------------
                 * Everything here happens while the game is stopped, so the raw
                 * strip and the VIC's output are the SAME pixels and can be
                 * compared byte-for-byte on the PC. If they came from different
                 * moments, any mismatch would be ambiguous between "wrong VIC
                 * config" and "the frame moved", which is untestable.
                 *
                 * The capture buffer is nvmap'd CACHEABLE: ReadDebugProcessMemory
                 * writes 983,040 B into it and an uncached destination would cost
                 * far more than the explicit flush below. */
                if (found && g_ind_buf != nullptr && g_ind_size >= FbBlockRow && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("vs:1_own_capture_buf");
                    u32 cap_handle = 0, cap_id = 0, cap_addr = 0;
                    const auto orc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbBlockRow), 0,
                                              std::addressof(cap_handle), std::addressof(cap_id), true);
                    if (R_SUCCEEDED(orc)) {
                        VicStage("vs:2_pin");
                        MapCmdBuffer(vfd, cap_handle, std::addressof(cap_addr), "capture", 0);
                        if (cap_addr != 0) {
                            VicStage("vs:3_read_block_row");
                            strip_read_ok = R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(
                                    reinterpret_cast<uintptr_t>(g_ind_buf), dbg, cand.addr, FbBlockRow));
                            if (strip_read_ok) {
                                /* the engine reads this through the SMMU; our
                                 * cached writes must reach memory first */
                                armDCacheFlush(g_ind_buf, FbBlockRow);

                                /* THE STUTTER FIX. Only the 983 KB read above needs
                                 * the game stopped - everything below works on our
                                 * own buffer. M45 kept the game frozen for 4.78 s
                                 * (181.195 -> 185.971) doing blits and 1.3 MB of SD
                                 * writes with the target halted, which is the
                                 * stutter felt on the console. Resume here. */
                                resume_game();

                                VicStage("vs:4_dump_strip");
                                strip_dumped = WriteBufToSd(StripBinPath, g_ind_buf, FbBlockRow);

                                const JobCtx sctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                                                   cap_addr, 0, 0, 0, 0, SelfSrc };
                                for (u32 v = 0; v < StripVariantCount; ++v) {
                                    const StripVariant &sv = StripVariants[v];
                                    /* layout fixed at what M43 proved correct;
                                     * only the source pixel format varies */
                                    g_strip_src = SrcDesc{ StripW, StripH, StripW,
                                                           vic::BLK_KIND_GENERIC_16Bx2, 4,
                                                           sv.src_fmt, vic::CACHE_WIDTH_64Bx4,
                                                           sv.rect_w, sv.rect_h };
                                    g_strip_out = OutDesc{ StripOut.w, StripOut.h,
                                                           StripOut.stride_px, sv.out_fmt };
                                    char stage[48];
                                    std::snprintf(stage, sizeof(stage), "vb:strip_%s", sv.name);
                                    VicStage("vs:5_sweep");
                                    if (RunOneJob(stage, VicJob::BlitStrip, true, sctx)) { ++strip_ok_count; }

                                    /* g_vic_dst_buf is UNCACHED nvmap memory, and handing that
                                     * straight to fs::WriteFile is what made every vic dump report
                                     * failure in M41 and M42 while strip.bin - which comes from the
                                     * CACHEABLE capture buffer - succeeded every time. The files it
                                     * left behind did not even match the engine's own checksum, so
                                     * they were never the VIC's output. Stage through normal cached
                                     * heap past the nvmap'd region first. */
                                    std::memcpy(g_stage_buf, g_vic_dst_buf, StripOutSize);
                                    variant_sum[v] = 0;
                                    for (u32 k = 0; k < StripOutSize; ++k) { variant_sum[v] += g_stage_buf[k]; }
                                    char path[64];
                                    std::snprintf(path, sizeof(path), "sdmc:/applet-mitm-vic-%s.bin", sv.name);
                                    if (WriteBufToSd(path, g_stage_buf, StripOutSize)) { ++strip_dump_count; }
                                }
                                strip_blit_ok = strip_ok_count > 0;
                                vic_dumped    = strip_dump_count > 0;
                            }
                            UnmapCmdBuffer(vfd, cap_handle);
                        }
                    }
                    strip_own_rc = orc;   /* ::Result is libnx's u32, not ams::Result */
                    strip_pin    = cap_addr;
                }

                /* Dump the frame BEFORE resuming. M37 continued the game first
                 * and then streamed 8.8 MB to the SD at 24 MB/s, so the game
                 * redrew that slot ~21 times across the 358 ms write. The decoded
                 * PNG showed it exactly: a monotonic brightness ramp, one step per
                 * 128-row block-row - mean luma 191, 223, 237, 249, 254, 251 - as
                 * MK8's fade-to-white advanced down the image in the same order we
                 * read it. Tearing, not a decode fault.
                 *
                 * With the game still stopped nothing can write to the slot, so the
                 * frame comes out coherent. It costs ~400 ms frozen, once, which is
                 * a fine trade for a screenshot. A streaming capture would instead
                 * read into RAM at 1129 MB/s and never go near the SD card. */
                if (found && g_dump_armed && g_ind_buf != nullptr && g_ind_size >= 0x10000) {
                    dumped = DumpSlotToSd(dbg, cand.addr, std::addressof(dump_ns), std::addressof(dump_chunks));
                }

                /* fallback: if the strip section was skipped, resume here */
                resume_game();

                if (found) {
                    if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(live_a), dbg, cand.addr, 16))) {
                        os::SleepThread(TimeSpan::FromMilliSeconds(120));
                        if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(live_b), dbg, cand.addr, 16))) {
                            live_ok = true;
                            live_changed = std::memcmp(live_a, live_b, 16) != 0;
                        }
                    }

                    if (g_ind_buf != nullptr && g_ind_size >= FbBlockRow) {
                        const u64 t0 = armTicksToNs(armGetSystemTick());
                        for (u64 done = 0; done < FbBlockRow; done += 0x10000) {
                            const u64 n = (FbBlockRow - done < 0x10000) ? (FbBlockRow - done) : 0x10000;
                            if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_ind_buf + done), dbg, cand.addr + done, n))) { break; }
                            ++strip_chunks_ok;
                        }
                        strip_ns = armTicksToNs(armGetSystemTick()) - t0;

                        u32 hist[256] = {};
                        const u64 got = static_cast<u64>(strip_chunks_ok) * 0x10000;
                        const u64 lim = (got > FbBlockRow) ? FbBlockRow : got;
                        for (u64 i = 0; i < lim; ++i) {
                            const u8 v = g_ind_buf[i];
                            if (v != 0) { ++nonzero; }
                            ++hist[v];
                        }
                        for (u32 i = 0; i < 256; ++i) { if (hist[i] != 0) { ++distinct; } }
                        for (u64 i = 0; i + 3 < lim; i += 4) {
                            ++total_px;
                            for (u32 l = 0; l < 4; ++l) { if (g_ind_buf[i + l] == 0xFF) { ++lane_ff[l]; } }
                        }

                        /* The real per-frame cost: read a whole slot into RAM in
                         * block-row strips with no SD in the loop. This is what a
                         * streaming capture actually does every frame, so it is the
                         * number that decides whether 60 fps holds up. */
                        {
                            const u64 t1 = armTicksToNs(armGetSystemTick());
                            for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_ind_buf), dbg, cand.addr + done, n))) { break; }
                                ++full_strips;
                            }
                            full_ns = armTicksToNs(armGetSystemTick()) - t1;
                        }

                        /* ---- sustained per-frame capture ------------------
                         * One timing on a title screen is not a streaming
                         * capture. This is the loop a real implementation runs:
                         * wait until the game presents, read the slot it just
                         * presented into, repeat. It measures the per-frame cost
                         * and - just as important - what the GAME's own frame
                         * rate does while we are doing it. */
                        {
                            constexpr u32 CapFrames = 120;

                            const u32 c0 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_before = g_queue_count.load(std::memory_order_relaxed) - c0;

                            const u32 during0 = g_queue_count.load(std::memory_order_relaxed);
                            u32 seen = during0, last_sig = 0;
                            const u64 cap_t0 = armTicksToNs(armGetSystemTick());

                            for (u32 i = 0; i < CapFrames; ++i) {
                                u32 spins = 0;
                                while (g_queue_count.load(std::memory_order_relaxed) == seen && spins < 200) {
                                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                                    ++spins;
                                }
                                if (g_queue_count.load(std::memory_order_relaxed) == seen) { ++cap_missed; break; }
                                seen = g_queue_count.load(std::memory_order_relaxed);

                                /* read the slot the game just PRESENTED, not the
                                 * one it is drawing into - that is the whole
                                 * point of having the binder intercept. */
                                const s32 slot = g_queue_slot.load(std::memory_order_relaxed);
                                const u64 off  = (slot >= 0 && static_cast<u32>(slot) < 3) ? FbSlotOff[slot] : 0;

                                if (slot >= 0 && slot < 4) { ++slot_hits[slot]; }

                                const u64 t2 = armTicksToNs(armGetSystemTick());
                                bool ok = true;
                                u32  sig = 0;
                                for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                    const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_ind_buf), dbg, cand.addr + off + done, n))) { ok = false; break; }
                                    if (done == 0) {
                                        /* Hash block-row 0 - the TOP of the picture.
                                         * M39 hashed the buffer AFTER the loop, which
                                         * holds the last strip: rows 1024-1151, almost
                                         * all padding below the 1080-row image. It
                                         * barely changes, which is why only 31 of 120
                                         * frames looked distinct. */
                                        for (u32 k = 0; k < 8192; k += 8) { sig = sig * 31u + g_ind_buf[k]; }
                                    }
                                }
                                const u64 dt = armTicksToNs(armGetSystemTick()) - t2;
                                if (!ok) { ++cap_missed; continue; }

                                ++cap_done;
                                cap_sum += dt;
                                if (dt < cap_min) { cap_min = dt; }
                                if (dt > cap_max) { cap_max = dt; }
                                if (dt > UINT64_C(16667000)) { ++cap_over; }
                                if (sig != last_sig) { ++cap_distinct; last_sig = sig; }
                            }
                            cap_ns = armTicksToNs(armGetSystemTick()) - cap_t0;
                            if (cap_ns > 0) {
                                fps_during = static_cast<u32>((static_cast<u64>(seen - during0) * UINT64_C(1000000000)) / cap_ns);
                            }

                            const u32 c1 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_after = g_queue_count.load(std::memory_order_relaxed) - c1;
                        }
                    }
                }

                ::ams::svc::CloseHandle(dbg);
            }
            /* ---- detached; log everything --------------------------------- */

            VicStage("dbg:2_attached");
            LogLine("   DebugActiveProcess(pid=%llu) rc=0x%x %s",
                    static_cast<unsigned long long>(pid.value), r_attach.GetValue(),
                    attached ? "ATTACHED" : "failed");
            if (!attached) { VicStage("dbg:2_FAILED"); return; }

            LogLine("   walked %u regions (%s); %u big enough (%u exactly %llu B); %u probe reads",
                    steps, walk_complete ? "complete" : "HIT THE STEP CAP", nbig, nexact,
                    static_cast<unsigned long long>(FbSwapSize), reads);
            for (u32 i = 0; i < nbig; ++i) {
                LogLine("     region %u: base=0x%010llx size=%llu%s", i,
                        static_cast<unsigned long long>(big[i].base),
                        static_cast<unsigned long long>(big[i].size),
                        (big[i].size == FbSwapSize) ? "   <== EXACT SWAPCHAIN SIZE" : "");
            }

            if (!found) {
                LogLine("   no exactly-sized region and no signature match");
                VicStage("dbg:no_signature");
                return;
            }

            LogLine("   *** SWAPCHAIN AT 0x%010llx (offset %llu into its region) - matched by %s ***",
                    static_cast<unsigned long long>(cand.addr),
                    static_cast<unsigned long long>(cand.addr - cand.region_base),
                    by_size ? "EXACT SIZE" : "pixel signature");
            for (u32 k = 0; k < 3; ++k) {
                const u8 *h = cand.head[k];
                LogLine("       slot%u +0x%07llx: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                        k, static_cast<unsigned long long>(FbSlotOff[k]),
                        h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
                        h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
            }

            LogLine("   GAME FROZEN FOR %llu ms (M45 was 4780 ms - only the strip read needs a halt)",
                    static_cast<unsigned long long>(frozen_ns / 1000000));
            LogLine("   drained %u debug events; ContinueDebugEvent rc=0x%x -> %s",
                    nev, r_cont.GetValue(),
                    resumed ? "GAME RUNNING WHILE WE STAY ATTACHED" : "still frozen");

            if (live_ok) {
                LogLine("   live sample over 120 ms: %s", live_changed
                        ? "*** PIXELS CHANGED - the game is presenting while attached ***"
                        : "identical (game may be paused, or the corner is static)");
                LogLine("       t0: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_a[0], live_a[1], live_a[2], live_a[3], live_a[4], live_a[5], live_a[6], live_a[7]);
                LogLine("       t1: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_b[0], live_b[1], live_b[2], live_b[3], live_b[4], live_b[5], live_b[6], live_b[7]);
            }

            if (strip_chunks_ok > 0 && strip_ns > 0) {
                const u64 got     = static_cast<u64>(strip_chunks_ok) * 0x10000;
                const u64 kbps    = (got * UINT64_C(1000000)) / strip_ns;
                const u64 slot_us = (strip_ns * (FbSlotSize / 1024)) / (got / 1024) / 1000;
                LogLine("   read %llu B of block-row in %llu us -> %llu MB/s",
                        static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(strip_ns / 1000),
                        static_cast<unsigned long long>(kbps / 1000));
                LogLine("   => one full 8,847,360 B slot ~%llu us; 60 fps needs <= 16667 us  [%s]",
                        static_cast<unsigned long long>(slot_us),
                        (slot_us <= 16667) ? "FEASIBLE" : "too slow for 60 fps at full res");
                LogLine("   block-row: nonzero=%u/%llu  distinct=%u  pixels=%u", nonzero,
                        static_cast<unsigned long long>(got), distinct, total_px);
                LogLine("   0xFF per lane: [0]=%u [1]=%u [2]=%u [3]=%u  (the alpha lane is the big one)",
                        lane_ff[0], lane_ff[1], lane_ff[2], lane_ff[3]);
                LogLine("   first 16 B: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                        g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                        g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                        g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
            }

            if (full_strips > 0 && full_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / full_ns / 1000;
                LogLine("   FULL SLOT into RAM: %u/9 strips, %llu us total, %llu MB/s -> %s",
                        full_strips,
                        static_cast<unsigned long long>(full_ns / 1000),
                        static_cast<unsigned long long>(mbps),
                        (full_ns / 1000 <= 16667) ? "*** FITS IN A 60 fps FRAME BUDGET ***"
                                                  : "over budget for 60 fps");
            }

            LogLine("   ---- VIC on real game pixels ----");
            LogLine("   nvmapOwn(capture, cacheable) rc=0x%x -> pinned at 0x%08x", strip_own_rc, strip_pin);
            LogLine("   block-row read %s; strip blit %s", strip_read_ok ? "OK" : "FAILED",
                    strip_blit_ok ? "*** COMPLETED ***" : "did not complete");
            LogLine("   swept %u variants: %u completed, %u dumped", StripVariantCount, strip_ok_count, strip_dump_count);
            for (u32 v = 0; v < StripVariantCount && v < 8; ++v) {
                /* No "identical - field inert" claim here. A byte sum is blind to
                 * reordering, which is exactly what these variants do, and that
                 * label sent three runs chasing a layout bug that did not exist.
                 * Sameness is decided on the PC by md5, not here by a sum. */
                LogLine("     %-12s bytesum=%u", StripVariants[v].name, variant_sum[v]);
            }
            LogLine("   dumps: %s %s   (compare with tools/compare_vic.py)",
                    strip_dumped ? "strip.bin OK" : "strip.bin FAILED",
                    vic_dumped   ? "vic.bin OK"   : "vic.bin FAILED");

            if (cap_done > 0) {
                const u64 avg     = cap_sum / cap_done;
                const u32 cap_fps = (cap_ns > 0) ? static_cast<u32>((static_cast<u64>(cap_done) * UINT64_C(1000000000)) / cap_ns) : 0;
                LogLine("   ---- sustained per-frame capture ----");
                LogLine("   captured %u full frames in %llu ms -> %u fps  (%u distinct, %u missed)",
                        cap_done, static_cast<unsigned long long>(cap_ns / 1000000),
                        cap_fps, cap_distinct, cap_missed);
                LogLine("   per-frame read: min %llu us  avg %llu us  max %llu us  (%u of %u over the 16667 us budget)",
                        static_cast<unsigned long long>(cap_min / 1000),
                        static_cast<unsigned long long>(avg / 1000),
                        static_cast<unsigned long long>(cap_max / 1000),
                        cap_over, cap_done);
                LogLine("   slots read: [0]=%u [1]=%u [2]=%u  (a rotating swapchain should hit all three)",
                        slot_hits[0], slot_hits[1], slot_hits[2]);
                /* Compare against the average of before and after, with a tight
                 * tolerance. M39 used "before - 6" and called 56 vs 59/61 fps
                 * unaffected; that hid a real ~4 fps dip. */
                const u32 base = (fps_before + fps_after) / 2;
                LogLine("   game presented: %u fps before, %u during, %u after -> %d fps delta  [%s]",
                        fps_before, fps_during, fps_after,
                        static_cast<int>(fps_during) - static_cast<int>(base),
                        (fps_during + 3 >= base) ? "*** GAME UNAFFECTED ***" : "GAME SLOWED while capturing");
            } else if (cap_missed > 0) {
                LogLine("   capture loop never got a frame (%u misses) - was the game presenting?", cap_missed);
            }

            if (dumped && dump_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / dump_ns / 1000;
                LogLine("   *** WROTE A WHOLE FRAME (game stopped - no tearing): %s (%llu B, %u chunks, %llu ms, %llu MB/s incl. SD) ***",
                        FrameBinPath, static_cast<unsigned long long>(FbSlotSize), dump_chunks,
                        static_cast<unsigned long long>(dump_ns / 1000000),
                        static_cast<unsigned long long>(mbps));
                char meta[256];
                const int n = std::snprintf(meta, sizeof(meta),
                        "addr=0x%llx\nslot_bytes=%llu\nwidth=1920\nheight=1080\npadded_height=1152\n"
                        "pitch=7680\nbpp=4\nblock_height_log2=4\nkind=0xfe\nformat=A8B8G8R8 (bytes R,G,B,A)\n",
                        static_cast<unsigned long long>(cand.addr),
                        static_cast<unsigned long long>(FbSlotSize));
                fs::DeleteFile(FrameTxtPath);
                if (n > 0 && R_SUCCEEDED(fs::CreateFile(FrameTxtPath, n))) {
                    fs::FileHandle mf;
                    if (R_SUCCEEDED(fs::OpenFile(std::addressof(mf), FrameTxtPath, fs::OpenMode_Write))) {
                        static_cast<void>(fs::WriteFile(mf, 0, meta, n, fs::WriteOption::Flush));
                        fs::CloseFile(mf);
                    }
                }
                VicStage("dbg:FRAME_ON_SD");
            } else {
                LogLine("   frame dump did not complete (%u chunks)", dump_chunks);
                VicStage("dbg:dump_failed");
            }
        }

        void TryNvencPhaseA(u32 nvmap_fd, u32 cmd_handle) {
            VicStage("nv:1_open_msenc");
            u32 efd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(efd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   NVENC: /dev/nvhost-msenc open FAILED nverr=%u", nverr);
                VicStage("nv:1_FAILED");
                return;
            }
            LogLine("   ---- NVENC phase A ----");
            LogLine("   /dev/nvhost-msenc open fd=%u", efd);

            u32 esyncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                const auto rc = NvIoctl(efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                esyncpt = gs.syncpt;
                LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u  (VIC uses 12)", rc, nverr, esyncpt);
                if (R_FAILED(rc) || nverr != 0) { NvClose(efd); VicStage("nv:2_FAILED"); return; }
            }
            {
                struct { u32 fd; } sn = { nvmap_fd };
                nverr = 0;
                const auto rc = NvIoctl(efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr));
                LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
            }

            VicStage("nv:3_submit");
            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = AppendIncrSyncpt(w, 0, esyncpt, false);   /* IMMEDIATE, no engine op */
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(words);
            put(esyncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 fence_val = 0;
            nverr = 0;
            const auto rc = NvIoctl(efd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogLine("   CHANNEL_SUBMIT req=0x%08x sz=%u words=%u rc=0x%x nverr=%u -> fence=%u",
                    req, sz, words, rc, nverr, fence_val);

            if (R_SUCCEEDED(rc) && nverr == 0) {
                u32 cfd = 0, ce = 0;
                if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(cfd), std::addressof(ce))) && ce == 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { esyncpt, fence_val, 100 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { esyncpt, 0 };
                    u32 re = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(re));
                    const bool ok = r.value >= fence_val;
                    LogLine("   WAIT nverr=%u syncpt=%u (want >= %u)  %s", we, r.value, fence_val,
                            ok ? "*** NVENC CHANNEL USABLE ***" : "fence did not advance");
                    NvClose(cfd);
                    VicStage(ok ? "nv:PHASE_A_OK" : "nv:fence_stuck");
                }
            } else {
                LogLine("   submit rejected - the msenc channel does not take this ABI");
                VicStage("nv:submit_rejected");
            }
            NvClose(efd);
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
