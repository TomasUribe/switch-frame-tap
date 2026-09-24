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
    };
    extern GameSurface g_game_surface;

    /* Record one setPreallocatedBuffer's NvGraphicBuffer into g_game_surface. */
    void CaptureGameSurface(const NvGraphicBufferRaw *gb, u32 which);

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
    extern u32  g_matrix_mode;
    extern bool g_stream_armed;
    extern u32  g_stream_w, g_stream_h, g_stream_frames;

    extern std::atomic<u32> g_queue_count;
    extern std::atomic<s32> g_queue_slot;

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
