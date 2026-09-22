/*
 * applet-mitm - M73: NVJPG hardware JPEG encode.
 *
 * The table building, parameter filling and JPEG container live in
 * applet_mitm_nvjpg.cpp; the channel plumbing lives in applet_mitm_nv.cpp
 * next to the nvdrv helpers it shares with the VIC.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    extern bool g_nvjpg_armed;
    extern u32  g_nvjpg_quality;

    struct NvjpgLayout {
        u32 param_off, status_off, bits_off, bits_size;
        u32 luma_off, chroma_off, w, h, quality;
    };

    /* memory_mode: 0 = semi-planar (NV12, what the VIC makes), 1 = planar.
     * input_type is undocumented in the public header, so the probe sweeps it. */
    void NvjpgFillParams(u8 *base, const NvjpgLayout &L, u32 memory_mode, u32 input_type);

    /* Returns the number of command words written. */
    u32 NvjpgBuildCmdbuf(u32 *w, u32 param_addr, u32 status_addr,
                         u32 bits_addr, u32 luma_addr, u32 chroma_u_addr, u32 chroma_v_addr);

    /* Wraps the engine's entropy-coded scan in a viewable baseline JPEG.
     * Returns total bytes written to dst. */
    u32 NvjpgWriteContainer(u8 *dst, u32 w, u32 h, u32 quality, const u8 *scan, u32 scan_len);

}
