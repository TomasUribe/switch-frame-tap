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

    /* Opt-in gate. Nothing in this file touches nvdrv unless the SD card holds
     * sdmc:/applet-mitm.armed containing the keyword "vic". Set once at
     * startup from Main(). Default false = the module is a pure observer. */
    extern bool g_vic_armed;

    /* One-shot: bring up nvdrv + VIC, blit slot `slot` of the captured
     * swapchain into our linear buffer, wait, checksum, release. Safe to call
     * every queueBuffer - it self-disables after the first run, and is a no-op
     * unless g_vic_armed. */
    void TryVicBlit(s32 slot);

}
