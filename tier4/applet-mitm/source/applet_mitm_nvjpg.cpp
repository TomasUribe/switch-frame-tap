/*
 * applet-mitm - M73: NVJPG, the hardware JPEG encoder.
 *
 * WHY THIS ENGINE AND NOT NVENC
 *
 * The goal is compression over USB 2.0, which M70 measured at ~37 MB/s. NVENC
 * (H.264) has resisted four probes and its failure is still not isolated.
 * NVJPG is a far smaller target for the same job:
 *
 *   - Its whole encoder interface is ~10 methods. NVENC's job is 39 words with
 *     reference pictures, IO history and four control arrays at offsets.
 *   - It has NO magic/version word, so the entire "which firmware generation"
 *     ambiguity that stalled M68-M71 simply does not exist here.
 *   - JPEG is all-intra by construction: no reference frames, no reordering,
 *     no rate-control feedback loop. That is the lowest-latency compression
 *     there is, which is exactly what this project wants.
 *
 * Bandwidth: 1080p60 MJPEG at quality ~85-90 runs roughly 200-400 KB/frame,
 * i.e. 12-24 MB/s, comfortably inside the measured 37 MB/s. Against raw
 * packed-420's 186.6 MB/s, that is the difference between 800x450 and native.
 *
 * THE M71 FLAW, FIXED HERE
 *
 * M71's ladder shared one channel across five submits, so the first job to
 * carry EXECUTE could hang the engine and every later job stalled behind it -
 * which is why its conclusions had to be retracted. Every attempt below opens
 * its OWN channel and closes it again, so a hung job cannot poison the next.
 */
#include "applet_mitm_nv.hpp"
#include "applet_mitm_nvjpg.hpp"
#include "applet_mitm_log.hpp"
#include "vic40_config.hpp"
#include "nvjpg_drv.h"
#include <cstring>

namespace ams::mitm::applet {

    constinit bool g_nvjpg_armed   = false;
    constinit u32  g_nvjpg_quality  = 85;
    constinit u32  g_nvjpg_attempts = 90;

    namespace {

        /* ---- standard JPEG tables (ITU T.81 Annex K) --------------------- */

        constexpr u8 StdQtLuma[64] = {
            16, 11, 10, 16, 24, 40, 51, 61,   12, 12, 14, 19, 26, 58, 60, 55,
            14, 13, 16, 24, 40, 57, 69, 56,   14, 17, 22, 29, 51, 87, 80, 62,
            18, 22, 37, 56, 68,109,103, 77,   24, 35, 55, 64, 81,104,113, 92,
            49, 64, 78, 87,103,121,120,101,   72, 92, 95, 98,112,100,103, 99,
        };
        constexpr u8 StdQtChroma[64] = {
            17, 18, 24, 47, 99, 99, 99, 99,   18, 21, 26, 66, 99, 99, 99, 99,
            24, 26, 56, 99, 99, 99, 99, 99,   47, 66, 99, 99, 99, 99, 99, 99,
            99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
            99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
        };

        constexpr u8 DcLumaBits[16]   = { 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
        constexpr u8 DcChromaBits[16] = { 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
        constexpr u8 DcVals[12]       = { 0,1,2,3,4,5,6,7,8,9,10,11 };

        constexpr u8 AcLumaBits[16]   = { 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
        constexpr u8 AcLumaVals[162] = {
            0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
            0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
            0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
            0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
            0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
            0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
            0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
            0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
            0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
            0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
            0xf9,0xfa,
        };
        constexpr u8 AcChromaBits[16] = { 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
        constexpr u8 AcChromaVals[162] = {
            0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
            0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
            0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
            0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
            0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
            0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
            0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
            0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
            0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
            0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
            0xf9,0xfa,
        };

        /* Canonical Huffman code assignment, T.81 Annex C. Produces, for each
         * symbol, the code length and the code word - which is exactly the
         * {length, value} pair nvjpg's tables want. */
        void BuildHuff(const u8 *bits, const u8 *vals, u32 nvals, huffman_symbol_s *out, u32 out_len, bool ac) {
            std::memset(out, 0, out_len * sizeof(huffman_symbol_s));
            u32 code = 0, k = 0;
            for (u32 len = 1; len <= 16; ++len) {
                for (u32 i = 0; i < bits[len - 1]; ++i, ++k) {
                    if (k >= nvals) { break; }
                    const u8 sym = vals[k];
                    /* DC: index by the magnitude category directly (0..11).
                     * AC: symbol is (run << 4) | size; the table is 16 runs x
                     * 11 sizes = 176 entries, size 0 being EOB (run 0) and
                     * ZRL (run 15). */
                    const u32 idx = ac ? (static_cast<u32>(sym >> 4) * 11u + static_cast<u32>(sym & 0xF))
                                       : static_cast<u32>(sym);
                    if (idx < out_len) {
                        out[idx].length = static_cast<unsigned short>(len);
                        out[idx].value  = static_cast<unsigned short>(code);
                    }
                    ++code;
                }
                code <<= 1;
            }
        }

        /* JPEG quality scaling, the libjpeg convention. */
        void ScaleQt(const u8 *base, u32 quality, u8 *out) {
            const u32 s = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
            for (u32 i = 0; i < 64; ++i) {
                s32 v = static_cast<s32>((base[i] * s + 50) / 100);
                if (v < 1)   { v = 1; }
                if (v > 255) { v = 255; }
                out[i] = static_cast<u8>(v);
            }
        }

        /* ---- JPEG container ---------------------------------------------
         * NVJPG emits only the entropy-coded scan. Wrapping it here rather
         * than on the PC means the file that lands on the SD card is a
         * directly viewable .jpg, which makes verifying a probe trivial. */
        u32 WriteJpegHeaders(u8 *p, u32 w, u32 h, const u8 *qtl, const u8 *qtc) {
            static constexpr u8 ZigZag[64] = {
                 0, 1, 8,16, 9, 2, 3,10,  17,24,32,25,18,11, 4, 5,
                12,19,26,33,40,48,41,34,  27,20,13, 6, 7,14,21,28,
                35,42,49,56,57,50,43,36,  29,22,15,23,30,37,44,51,
                58,59,52,45,38,31,39,46,  53,60,61,54,47,55,62,63,
            };
            u32 n = 0;
            auto b  = [&](u8 v) { p[n++] = v; };
            auto b16 = [&](u32 v) { p[n++] = static_cast<u8>(v >> 8); p[n++] = static_cast<u8>(v); };

            b(0xFF); b(0xD8);                                    /* SOI */

            b(0xFF); b(0xDB); b16(2 + 2 * 65);                   /* DQT, both tables */
            b(0x00); for (u32 i = 0; i < 64; ++i) { b(qtl[ZigZag[i]]); }
            b(0x01); for (u32 i = 0; i < 64; ++i) { b(qtc[ZigZag[i]]); }

            b(0xFF); b(0xC0); b16(17);                           /* SOF0, baseline */
            b(8); b16(h); b16(w); b(3);
            b(1); b(0x22); b(0);                                 /* Y,  4:2:0 */
            b(2); b(0x11); b(1);                                 /* Cb */
            b(3); b(0x11); b(1);                                 /* Cr */

            struct { u8 id; const u8 *bits; const u8 *vals; u32 nv; } ht[4] = {
                { 0x00, DcLumaBits,   DcVals,       12  },
                { 0x10, AcLumaBits,   AcLumaVals,   162 },
                { 0x01, DcChromaBits, DcVals,       12  },
                { 0x11, AcChromaBits, AcChromaVals, 162 },
            };
            for (u32 t = 0; t < 4; ++t) {
                b(0xFF); b(0xC4); b16(3 + 16 + ht[t].nv);
                b(ht[t].id);
                for (u32 i = 0; i < 16; ++i) { b(ht[t].bits[i]); }
                for (u32 i = 0; i < ht[t].nv; ++i) { b(ht[t].vals[i]); }
            }

            b(0xFF); b(0xDA); b16(12);                           /* SOS */
            b(3); b(1); b(0x00); b(2); b(0x11); b(3); b(0x11);
            b(0); b(63); b(0);
            return n;
        }

        /* nvjpg method table - the SINGLE-CORE layout, NVIDIA's cle7d0.h.
         *
         * M76: M73/M74 took these from clc9d1.h, which is a MULTI-core NVJPG.
         * That generation inserts SET_TOTAL_CORE_NUM at 0x704 and a per-core
         * block starting with SET_CORE_INDEX at 0x710, which pushes every
         * surface one register later. Tegra X1's NVJPG is single-core, and the
         * register map reverse-engineered from working Switch homebrew
         * (averne's oss-nvjpg, which decodes on this hardware) matches
         * cle7d0.h exactly: 0x708 setup, 0x70C status, 0x710 bitstream, 0x714
         * luma, 0x718 chroma U, 0x71C chroma V.
         *
         * So the M73/M74 command buffer wrote SET_CORE_INDEX = 0 into the
         * BITSTREAM register - a zero output address, the exact thing M27
         * proved hangs an engine - and handed it the bitstream as the luma
         * plane. It never mattered because the engine never ran (cycles=0),
         * but it would have faulted the moment it did.
         *
         * "Host1x method offsets are stable across generations" is true for
         * the common block (0x200 / 0x300 / 0x700-0x70C are identical in both
         * headers); it is not true past the point a generation adds methods. */
        namespace nvjpg {
            constexpr u32 HOST1X_CLASS_NVJPG   = 0xC0;
            constexpr u32 SET_APPLICATION_ID   = 0x00000200;
            constexpr u32 APPLICATION_ID_DEC   = 1;
            constexpr u32 APPLICATION_ID_ENC   = 2;
            constexpr u32 EXECUTE              = 0x00000300;
            constexpr u32 SET_CONTROL_PARAMS   = 0x00000700;
            constexpr u32 SET_PICTURE_INDEX    = 0x00000704;
            constexpr u32 SET_IN_DRV_PIC_SETUP = 0x00000708;
            constexpr u32 SET_OUT_STATUS       = 0x0000070C;
            constexpr u32 SET_BITSTREAM        = 0x00000710;
            constexpr u32 SET_CUR_PIC          = 0x00000714;
            constexpr u32 SET_CUR_PIC_CHROMA_U = 0x00000718;
            constexpr u32 SET_CUR_PIC_CHROMA_V = 0x0000071C;
        }

    }

    /* Fill the driver picture parameters for one baseline 4:2:0 encode.
     *
     * M76 caveat, UNVERIFIED FOR THIS CHIP: nvjpg_drv_pic_param_s is the
     * register-image layout of a later NVJPG. The one T210 layout known to work
     * on this console - the DECODE picture info in averne's oss-nvjpg - is a
     * completely different, higher-level struct (Huffman tables as BITS/HUFFVAL
     * arrays, a 0xB2C-byte record the firmware parses). The T210 encoder's
     * record is very likely the same style, not this one. Until it is known,
     * an encode reaching a running engine is expected to fail on the setup,
     * not the method table. The decode positive control (jpgdec) does not
     * depend on this struct at all. */
    void NvjpgFillParams(u8 *base, const NvjpgLayout &L, u32 memory_mode, u32 input_type) {
        auto *pp = reinterpret_cast<nvjpg_drv_pic_param_s *>(base + L.param_off);
        std::memset(pp, 0, sizeof(*pp));

        const u32 mcu_w = (L.w + 15) / 16;   /* 4:2:0 -> 16x16 MCU */
        const u32 mcu_h = (L.h + 15) / 16;

        pp->enc_top_ctl.chroma_format = 3;   /* 420 */
        pp->enc_top_ctl.mcu_width     = mcu_w - 1;
        pp->enc_top_ctl.mcu_height    = mcu_h - 1;
        pp->enc_top_ctl.stuff_disable = 0;
        pp->enc_restart_interval.restart_interval = 0;

        u8 qtl[64], qtc[64];
        ScaleQt(StdQtLuma,   L.quality, qtl);
        ScaleQt(StdQtChroma, L.quality, qtc);
        /* the engine wants reciprocals: (1<<15)/q */
        for (u32 i = 0; i < 64; ++i) {
            pp->enc_quant_tab.quantLumaFactor[i]   = static_cast<unsigned short>(32768u / qtl[i]);
            pp->enc_quant_tab.quantChromaFactor[i] = static_cast<unsigned short>(32768u / qtc[i]);
        }

        BuildHuff(DcLumaBits,   DcVals,       12,  pp->enc_huff_tab.hfDcLuma,   DCVALUEITEM, false);
        BuildHuff(AcLumaBits,   AcLumaVals,   162, pp->enc_huff_tab.hfAcLuma,   ACVALUEITEM, true);
        BuildHuff(DcChromaBits, DcVals,       12,  pp->enc_huff_tab.hfDcChroma, DCVALUEITEM, false);
        BuildHuff(AcChromaBits, AcChromaVals, 162, pp->enc_huff_tab.hfAcChroma, ACVALUEITEM, true);

        pp->pdma_pic_info.input_type   = input_type;
        pp->pdma_pic_info.chroma_mode  = 3;              /* 420 */
        pp->pdma_pic_info.width_in_8x8  = (L.w + 7) / 8;
        pp->pdma_pic_info.height_in_8x8 = (L.h + 7) / 8;
        pp->pdma_pic_info1.memory_mode = memory_mode;    /* 0 semi-planar, 1 planar */
        pp->pdma_pic_info1.tiling_mode = 0;              /* pitch linear */
        pp->luma_stride   = L.w;
        pp->chroma_stride = L.w;

        pp->bitstream_offset = 0;
        pp->bitstream_size   = L.bits_size;
    }

    u32 NvjpgWriteContainer(u8 *dst, u32 w, u32 h, u32 quality, const u8 *scan, u32 scan_len) {
        u8 qtl[64], qtc[64];
        ScaleQt(StdQtLuma,   quality, qtl);
        ScaleQt(StdQtChroma, quality, qtc);
        u32 n = WriteJpegHeaders(dst, w, h, qtl, qtc);
        std::memcpy(dst + n, scan, scan_len);
        n += scan_len;
        dst[n++] = 0xFF; dst[n++] = 0xD9;   /* EOI */
        return n;
    }

    u32 NvjpgBuildCmdbuf(u32 *w, u32 param_addr, u32 status_addr,
                         u32 bits_addr, u32 luma_addr, u32 chroma_u_addr, u32 chroma_v_addr) {
        u32 n = 0;
        auto m = [&](u32 method, u32 value) {
            w[n++] = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;
            w[n++] = method >> 2;
            w[n++] = value;
        };
        w[n++] = vic::Host1xOpcodeSetClass(0, nvjpg::HOST1X_CLASS_NVJPG, 0);
        m(nvjpg::SET_APPLICATION_ID,   nvjpg::APPLICATION_ID_ENC);
        m(nvjpg::SET_CONTROL_PARAMS,   0);
        m(nvjpg::SET_PICTURE_INDEX,    0);
        m(nvjpg::SET_IN_DRV_PIC_SETUP, param_addr  >> 8);
        m(nvjpg::SET_OUT_STATUS,       status_addr >> 8);
        m(nvjpg::SET_BITSTREAM,        bits_addr     >> 8);
        m(nvjpg::SET_CUR_PIC,          luma_addr     >> 8);
        m(nvjpg::SET_CUR_PIC_CHROMA_U, chroma_u_addr >> 8);
        if (chroma_v_addr != 0) { m(nvjpg::SET_CUR_PIC_CHROMA_V, chroma_v_addr >> 8); }
        m(nvjpg::EXECUTE, 1u << 8);   /* AWAKEN */
        return n;
    }

}
