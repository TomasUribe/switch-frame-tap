/*
 * applet-mitm - hand-rolled nvdrv access + VIC hardware blit.
 *
 * Stage 1 (verified): open nvdrv:s, import the game's swapchain nvmap, survey
 * engines, allocate our own linear buffer, prove the VIC channel is usable.
 *
 * Stage 2 (this file): actually drive the VIC. Capture the swapchain geometry
 * from the setPreallocatedBuffer parcels, then on a later queueBuffer run one
 * NVHOST_IOCTL_CHANNEL_SUBMIT that de-swizzles + format-converts a crop of the
 * live frame (block-linear) into our linear buffer, wait the syncpoint, and
 * checksum the result.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "applet_mitm_gbuf.hpp"

namespace ams::mitm::applet {

    /* Swapchain description, filled from the first few setPreallocatedBuffer
     * parcels. MK8 registers 3 slots, all inside ONE nvmap object (id 1268),
     * at offsets 0 / 0x870000 / 0x10E0000. */
    struct GameSurface {
        u32 nvmap_id;          /* the nvmap object id to import                */
        u32 width;             /* plane[0] width  in pixels                    */
        u32 height;            /* plane[0] height in pixels                    */
        u32 stride_px;         /* plane[0] pitch bytes / 4                     */
        u32 block_h_log2;      /* plane[0] block_height_log2 (MK8: 4)          */
        u32 pix_format;        /* vic::PIXFMT_* for plane[0]                   */
        u32 slot_offset[8];    /* plane[0].offset per registered slot          */
        u32 num_slots;
        bool armed;            /* >=1 slot captured, blit not yet done         */
        /* M87: per-game geometry for live mode */
        u32 buf_size;          /* one buffer's size (NvGraphicBuffer total_size) */
        u64 color_format;      /* plane[0] NvColorFormat, raw                  */
        u32 layout;            /* plane[0] NvLayout: 3 = BlockLinear           */
        u32 generation;        /* bumped when a new buffer set (nvmap id) starts */
        u32 slot_nvmap[8];     /* v0.7.1: each slot's nvmap id - Minecraft gives
                                * every slot its own buffer object */
    };
    extern GameSurface g_game_surface;

    /* Record one setPreallocatedBuffer's NvGraphicBuffer into g_game_surface. */
    /* M87: `slot` is the swapchain slot the parcel names (not a running
     * count), or -1 if it could not be parsed. */
    void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot);

    /* Opt-in gates, parsed once at startup from sdmc:/applet-mitm.armed:
     *   "vic"        -> g_vic_armed:   run the probe at all
     *   "exec"       -> g_vic_execute: push the real VIC blit (Phase B) rather
     *                   than a no-op cmdbuf that only exercises the submit ABI
     * Default (no file) = pure observer, byte-for-byte M7d behaviour. */
    extern bool g_vic_armed;
    extern bool g_vic_execute;
    /* "dbg" in the arm file: attach as a debugger and read the game's memory. */
    extern bool g_dbg_armed;
    /* result of pmdmntInitialize() at startup; 0 means the pid lookup is usable */
    extern u32  g_pmdmnt_rc;

    /* Current probe step, for the heartbeat to snapshot into .last. A run that
     * wedges therefore names the exact ioctl it wedged on. */
    extern std::atomic<const char *> g_vic_stage;

    /* Written by the binder thread on every queueBuffer (code 7): how many
     * frames the game has presented, and which swapchain slot the last one went
     * into. Both are relaxed atomic stores - the capture loop polls them from
     * the worker thread, and nothing that can block ever touches the binder
     * thread. That rule has held since M8 froze the console. */
    /* "dump" in the arm file: write a whole frame to the SD card. It freezes
     * the game for the duration of the write - 0.3 s on an idle card, 2.5 s when
     * the game is loading and competing for it - so it is off by default.
     * "wait=N": seconds of uptime before the probe fires (default 120). */
    extern bool g_dump_armed;
    extern u32  g_probe_delay_s;

    /* M59 streaming prototype: "stream" arms it, sw=/sh= set the output size,
     * sframes= how many to send. Defaults are 480x270 because that is the only
     * size that fits 60 fps on USB 2.0 (518,400 B/frame = 12.4 ms transfer);
     * 640x360 is selectable but caps near 45 fps until NV12 or SuperSpeed. */
    /* M63: "bench" times the VIC with instrumentation OFF and proves whether
     * NV12 output works. M59's 119 ms/blit was measured with ~7 SD-flushing log
     * calls and a 65,536-iteration checksum inside the timed region. */
    extern bool g_bench_armed;
    extern bool g_nvenc_armed;
    extern bool g_sweep_armed;
    extern bool g_stream_auto;
    extern bool g_matrix_armed;
    extern bool g_grcscan_armed;
    extern bool g_jpgdec_armed;   /* M76: NVJPG decode positive control */
    extern bool g_nvgrc_armed;    /* M80: grc's IDR job replayed from our channel */
    extern bool g_csc_armed;      /* M82: VIC colour-matrix probe sweep */
    extern bool g_nvframe_armed;  /* M82: real game frames -> VIC NV12 -> NVENC */
    extern u32  g_nvframe_n;
    extern bool g_nvstream_armed; /* M83: game -> VIC -> NVENC H.264 -> USB */
    extern u32  g_nvstream_n, g_nvstream_qp;
    extern u32  g_nvstream_gop;   /* M85: 0 = IDR-only; N = an IDR every N frames, P frames between */
    /* v0.6 bitrate tuning: "nvab" cycles the P-frame encoder settings through
     * four variants, 10 s each, and logs each window's sizes and intra share */
    extern bool g_nvab_armed;
    extern bool g_live_armed;     /* M86: stream whenever a viewer and a game are there */
    extern bool g_nv1080_armed;   /* M90: 1080p encoder probe (frame 0, GOP, timed) */
    extern u32  g_nv1080_n;
    extern bool g_anyfw_armed;    /* M98: capture on firmware this build was not tested on */
    extern bool g_cap720_armed;   /* M93: stream 1080p content scaled to 720p (the M86-M89 behaviour) */
    extern bool g_nvp_armed;      /* M83: IDR + P frames probe */
    extern u32  g_nvp_n;
    extern u32  g_matrix_mode;
    extern bool g_stream_armed;
    extern u32  g_stream_w, g_stream_h, g_stream_frames;

    extern std::atomic<u32> g_queue_count;
    extern std::atomic<s32> g_queue_slot;
    extern std::atomic<u64> g_queue_tick;   /* M85: system tick of the latest queueBuffer */
    /* M88: the latest queueBuffer's acquire fence - the GPU is done drawing
     * the slot once every syncpoint (id << 32 | value) has reached its value */
    extern std::atomic<u32> g_queue_fence_n;
    /* v0.4.1: the latest queueBuffer's NATIVE_WINDOW_TRANSFORM_* - some games
     * (Kirby's Return to Dream Land Deluxe, Kirby and the Forgotten Land)
     * render upside down and have the compositor flip the picture */
    extern std::atomic<u32> g_queue_transform;
    extern std::atomic<u64> g_app_pid;         /* applet_mitm_service.cpp: the running application */
    extern std::atomic<u64> g_crop_full_pid;   /* v0.7.9: the application whose crop was ever 1920x1080 */
    extern std::atomic<u32> g_queue_crop_wh;   /* v0.7.3: the last present's crop, w << 16 | h (0: none) */
    extern std::atomic<u64> g_queue_fence[4];
    /* M89: a seqlock over slot + fence: odd while the binder thread writes */
    extern std::atomic<u32> g_queue_seq;
    /* M96: the last few presents, each under its own seqlock and written
     * before g_queue_count moves, so the capture can take the newest one the
     * GPU has FINISHED. Run V: MK8 at 1080p queues a frame before the
     * previous one is drawn, and waiting on the latest present's fence
     * skipped the finished one before it - 20% of presents. Present c
     * (g_queue_count after its increment) lives at [c % PresentRingSize]. */
    struct PresentRec {
        std::atomic<u32> seq;
        std::atomic<u32> count;
        std::atomic<s32> slot;
        std::atomic<u32> fence_n;
        std::atomic<u64> fence[4];
        std::atomic<u64> tick;
        std::atomic<u32> transform;   /* v0.4.1: queueBuffer's transform (bit 0 flip H, bit 1 flip V) */
    };
    constexpr u32 PresentRingSize = 8;
    extern PresentRec g_present_ring[PresentRingSize];

    /* M97: the overlay's view of live mode (sftap service). g_stream_enabled
     * is the overlay's switch: off, live mode ends any session, detaches from
     * the game and stops probing USB until it is turned on again. */
    enum LiveState : u32 {
        LiveState_Starting   = 0,   /* module up; live mode not entered yet (wait=, first game) */
        LiveState_Off        = 1,   /* turned off from the overlay */
        LiveState_WaitViewer = 2,
        LiveState_WaitGame   = 3,
        LiveState_Streaming  = 4,
        LiveState_Wedged     = 5,   /* an engine stalled: no streaming until reboot */
        LiveState_NotArmed   = 6,   /* "live" is not in the arm file */
        LiveState_Unsupported = 7,  /* M98: untested firmware; allow_untested_firmware=1 overrides */
        LiveState_Excluded   = 8,   /* v0.1.1b: the running app is on the never-attach list */
    };
    extern std::atomic<bool> g_stream_enabled;
    /* v0.1.1b: the running application's program id and whether it is on the
     * never-attach list (sdmc:/config/switch-frame-tap/excluded.txt) - kept
     * current by the app watcher (applet_mitm_service.cpp) */
    extern std::atomic<u64>  g_app_tid;
    extern std::atomic<bool> g_app_excluded;
    /* M98: new settings arrived (ReloadConfig): end the session, restart with them */
    extern std::atomic<bool> g_reconfig_request;
    extern std::atomic<u32>  g_live_state;
    extern std::atomic<u32>  g_live_w, g_live_h, g_live_fps_x10, g_live_game_fps_x10, g_live_sessions;

    /* Spawn the worker. The VIC probe MUST NOT run on the binder dispatch
     * thread: doing so blocks the game's queueBuffer, which wedges vi, which
     * forces a power-off, which loses the very log we need. M8b died exactly
     * that way. The worker owns all nvdrv work; a hang there costs us the
     * probe, not the console. */
    void StartVicWorker();

    /* Called from the binder thread. Records the slot and returns immediately -
     * never blocks, never touches nvdrv. */
    void RequestVicBlit(s32 slot);

}
