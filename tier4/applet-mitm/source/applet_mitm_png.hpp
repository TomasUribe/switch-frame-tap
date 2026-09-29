/*
 * M99: native-resolution screenshots - block-linear RGBA -> PNG.
 *
 * Pure functions, no libstratosphere, so tier4/applet-mitm/test/png_test.cpp
 * compiles exactly this code and checks the result with real zlib.
 *
 * The game's swapchain is block-linear (Generic_16Bx2, 64-byte x 8-row GOBs,
 * 2^bh_log2 GOBs per block, blocks in rows across the surface - the layout
 * tools/nvframe_check.py bl_index de-swizzles on the PC). Pixels are
 * A8B8G8R8: R, G, B, A in memory.
 *
 * The PNG is written as it is de-swizzled, a row at a time, with no buffer
 * for the picture: 8-bit RGB, filter 0, and the zlib stream made of STORED
 * deflate blocks (one per row). No compression - a 1080p shot is 6.2 MB - but
 * nothing to get wrong, nothing slow, and the pixels exactly as the game drew
 * them. Every PNG reader opens it.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ams::mitm::applet::png {

    /* offset of byte x (0..width_bytes) of row y in a block-linear surface */
    inline size_t BlockLinearOffset(uint32_t x, uint32_t y, uint32_t width_bytes, uint32_t bh_log2) {
        const uint32_t block_rows   = 8u << bh_log2;
        const uint32_t block_bytes  = 512u << bh_log2;
        const uint32_t blocks_per_row = (width_bytes + 63) / 64;
        const uint32_t gx = x % 64, gy = y % 8;
        const uint32_t in_gob = (gx / 32) * 256 + (gy / 2) * 64 + ((gx % 32) / 16) * 32 + (gy % 2) * 16 + (gx % 16);
        return static_cast<size_t>(y / block_rows) * blocks_per_row * block_bytes
             + static_cast<size_t>(x / 64) * block_bytes
             + static_cast<size_t>((y % block_rows) / 8) * 512 + in_gob;
    }

    /* one row of RGBA (block-linear, `stride_bytes` wide) -> w pixels of RGB.
     * 16-byte runs (4 pixels) are contiguous in a GOB, so it copies by run. */
    inline void DeswizzleRowRgb(const uint8_t *src, size_t src_size, uint32_t y, uint32_t w,
                                uint32_t stride_bytes, uint32_t bh_log2, uint8_t *rgb) {
        for (uint32_t x = 0; x < w; x += 4) {
            const size_t off = BlockLinearOffset(x * 4, y, stride_bytes, bh_log2);
            const uint32_t n = (w - x) < 4 ? (w - x) : 4;
            for (uint32_t k = 0; k < n; ++k) {
                const size_t o = off + k * 4;
                uint8_t *d = rgb + (x + k) * 3;
                if (o + 3 < src_size) { d[0] = src[o]; d[1] = src[o + 1]; d[2] = src[o + 2]; }
                else { d[0] = d[1] = d[2] = 0; }
            }
        }
    }

    inline uint32_t Crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) { c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
                table[i] = c;
            }
            ready = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); }
        return ~crc;
    }

    /* the exact size WritePng produces, so the file can be created at its
     * final size in one allocation (M99b: growing it 128 KB at a time took
     * 2.6-5 s for 6 MB in Run AB) */
    inline uint64_t PngSize(uint32_t w, uint32_t h) {
        const uint64_t row_len = 1 + uint64_t(w) * 3;
        return 8 + (12 + 13) + uint64_t(h) * (12 + 5 + row_len) + 2 + (12 + 4) + 12;
    }

    /* Writes a PNG through `sink(data, len)` (return false to abort).
     * row(y, rgb) fills row y's w*3 bytes. Returns false if a write failed. */
    template<typename Sink, typename RowFn>
    bool WritePng(uint32_t w, uint32_t h, uint8_t *row_buf /* 1 + w*3 */, Sink &&sink, RowFn &&row) {
        auto be32 = [](uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
        /* a chunk: length, type, data (possibly in two parts), CRC over type + data */
        auto chunk = [&](const char *type, const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
            uint8_t hdr[8];
            be32(hdr, static_cast<uint32_t>(an + bn));
            std::memcpy(hdr + 4, type, 4);
            uint32_t crc = Crc32Update(0, hdr + 4, 4);
            crc = Crc32Update(crc, a, an);
            if (bn) { crc = Crc32Update(crc, b, bn); }
            uint8_t c[4];
            be32(c, crc);
            return sink(hdr, 8) && (an == 0 || sink(a, an)) && (bn == 0 || sink(b, bn)) && sink(c, 4);
        };
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        if (!sink(sig, 8)) { return false; }
        uint8_t ihdr[13];
        be32(ihdr, w); be32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGB, deflate, filter 0, no interlace */
        if (!chunk("IHDR", ihdr, 13, nullptr, 0)) { return false; }

        const uint32_t row_len = 1 + w * 3;      /* filter byte + pixels; one stored block each (< 65536) */
        uint32_t a1 = 1, a2 = 0;                 /* Adler-32 of the uncompressed stream */
        for (uint32_t y = 0; y < h; ++y) {
            row_buf[0] = 0;
            row(y, row_buf + 1);
            for (uint32_t i = 0; i < row_len; ++i) { a1 = (a1 + row_buf[i]) % 65521u; a2 = (a2 + a1) % 65521u; }
            uint8_t pre[7];
            size_t pn = 0;
            if (y == 0) { pre[pn++] = 0x78; pre[pn++] = 0x01; }            /* zlib header: deflate, 32K window, no dict */
            pre[pn++] = (y == h - 1) ? 1 : 0;                              /* BFINAL on the last row, BTYPE 00 = stored */
            pre[pn++] = row_len & 0xFF; pre[pn++] = row_len >> 8;
            pre[pn++] = ~row_len & 0xFF; pre[pn++] = (~row_len >> 8) & 0xFF;
            /* one IDAT per row: the stored-block header and the row */
            uint8_t both[7];
            std::memcpy(both, pre, pn);
            if (!chunk("IDAT", both, pn, row_buf, row_len)) { return false; }
        }
        uint8_t adler[4];
        be32(adler, (a2 << 16) | a1);
        if (!chunk("IDAT", adler, 4, nullptr, 0)) { return false; }
        return chunk("IEND", nullptr, 0, nullptr, 0);
    }

}
