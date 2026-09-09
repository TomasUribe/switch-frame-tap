/*
 * applet-mitm - NvGraphicBuffer parsing
 *
 * Nintendo's nvnflinger adds SET_PREALLOCATED_BUFFER (IGraphicBufferProducer
 * code 14) to register a game-allocated graphic buffer with the buffer queue.
 * Its input parcel carries a flattened NvGraphicBuffer, which is the complete
 * description of one frame's backing memory:
 *
 *   nvmap_id  - the nvmap object holding the pixels (what we must import)
 *   width/height/pitch/stride/format
 *   offset    - plane offset within the nvmap object
 *   kind + block_height_log2 - Tegra block-linear swizzle parameters
 *   size      - plane size in bytes
 *
 * Layout mirrors libnx (nx/include/switch/nvidia/graphic_buffer.h). Defined
 * locally so we are not at the mercy of include ordering, with static_asserts
 * pinning the offsets we rely on.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct NvNativeHandle {
        u32 unk0;
        s32 num_fds;
        s32 num_ints;
    };
    static_assert(sizeof(NvNativeHandle) == 0xC);

    struct NvSurfaceRaw {
        u32 width;
        u32 height;
        u32 color_format;
        u32 layout;
        u32 pitch;
        u32 unused_nvmap;
        u32 offset;
        u32 kind;
        u32 block_height_log2;
        u32 scan;
        u32 second_field_offset;
        u64 flags;
        u64 size;
        u32 unk[6];
    };
    static_assert(sizeof(NvSurfaceRaw) == 0x58);

    struct NvGraphicBufferRaw {
        NvNativeHandle header;
        s32 unk0;        /* -1 */
        s32 nvmap_id;    /* nvmap object id  <- the prize */
        u32 unk2;
        u32 magic;       /* 0xDAFFCAFF */
        u32 pid;
        u32 type;
        u32 usage;
        u32 format;
        u32 ext_format;
        u32 stride;      /* in PIXELS */
        u32 total_size;  /* bytes */
        u32 num_planes;
        u32 unk12;
        NvSurfaceRaw planes[3];
        u64 unused;
    };
    static_assert(sizeof(NvGraphicBufferRaw) == 0x150);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, nvmap_id) == 0x10);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, magic)    == 0x18);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, planes)   == 0x40);

    constexpr u32 NvGraphicBufferMagic = 0xDAFFCAFF;

    /* Scan a parcel for a flattened NvGraphicBuffer; returns nullptr if absent. */
    const NvGraphicBufferRaw *FindGraphicBuffer(const void *parcel, size_t size);

    /* Log every field we care about. */
    void LogGraphicBuffer(const char *tag, const NvGraphicBufferRaw *gb);

}
