/*
 * Host test for applet_mitm_png.hpp (M99 screenshots): swizzle a known picture
 * into the game's block-linear layout the way tools/nvframe_check.py does,
 * de-swizzle + write it as PNG with the module's code, then decode the PNG with
 * real zlib and compare every pixel.
 *
 *   g++ -std=c++20 -Wall -Wextra -I../source png_test.cpp -lz -o png_test && ./png_test
 */
#include "applet_mitm_png.hpp"
#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace png = ams::mitm::applet::png;

static int g_fail = 0;

static uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* the GOB layout from tools/nvframe_check.py gob_offsets/bl_index, written out
 * independently of the header's formula */
static size_t RefOffset(uint32_t xb, uint32_t y, uint32_t wb, uint32_t bh) {
    const uint32_t rows = 8u << bh, bbytes = 512u << bh, bpr = (wb + 63) / 64;
    const uint32_t x = xb % 64, yy = y % 8;
    const uint32_t g = (x / 32) * 256 + (yy / 2) * 64 + ((x % 32) / 16) * 32 + (yy % 2) * 16 + (x % 16);
    return size_t(y / rows) * bpr * bbytes + size_t(xb / 64) * bbytes + size_t((y % rows) / 8) * 512 + g;
}

static void RunCase(uint32_t sw, uint32_t sh, uint32_t bh, uint32_t w, uint32_t h, const char *out_path, bool bgra = false) {
    /* the surface: sw x sh RGBA, rows padded to whole blocks */
    const uint32_t wb = sw * 4, rows = ((sh + (8u << bh) - 1) / (8u << bh)) * (8u << bh);
    std::vector<uint8_t> surf(size_t(((wb + 63) / 64) * 64) * rows, 0);
    auto pix = [](uint32_t x, uint32_t y, int c) -> uint8_t { return uint8_t((x * 7 + y * 13 + c * 91 + (x ^ y)) & 0xFF); };
    for (uint32_t y = 0; y < sh; ++y) {
        for (uint32_t x = 0; x < sw; ++x) {
            /* memory order R,G,B,A - or B,G,R,A for a bgra surface */
            for (int c = 0; c < 4; ++c) { const int mc = (bgra && c != 3) ? 2 - c : c; surf[RefOffset(x * 4 + mc, y, wb, bh)] = c == 3 ? 0xFF : pix(x, y, c); }
        }
    }
    std::vector<uint8_t> file, row(1 + w * 3);
    const bool ok = png::WritePng(w, h, row.data(),
        [&](const uint8_t *d, size_t n) { file.insert(file.end(), d, d + n); return true; },
        [&](uint32_t y, uint8_t *rgb) { png::DeswizzleRowRgb(surf.data(), surf.size(), y, w, wb, bh, rgb, bgra); });
    if (!ok) { std::printf("FAIL WritePng returned false\n"); ++g_fail; return; }
    if (file.size() != png::PngSize(w, h)) { std::printf("FAIL PngSize(%u, %u) = %llu, file is %zu B\n", w, h, (unsigned long long)png::PngSize(w, h), file.size()); ++g_fail; return; }

    /* parse it back: chunks, CRCs, IDAT concatenation, inflate */
    if (file.size() < 8 || std::memcmp(file.data(), "\x89PNG\r\n\x1a\n", 8) != 0) { std::printf("FAIL signature\n"); ++g_fail; return; }
    std::vector<uint8_t> z;
    uint32_t pw = 0, ph = 0;
    bool iend = false;
    for (size_t p = 8; p + 12 <= file.size();) {
        const uint32_t len = Be32(&file[p]);
        const uint8_t *type = &file[p + 4];
        const uint32_t crc = Be32(&file[p + 8 + len]);
        if (crc != png::Crc32Update(0, type, 4 + len) || crc != uint32_t(crc32(0, type, 4 + len))) { std::printf("FAIL chunk CRC\n"); ++g_fail; return; }
        if (!std::memcmp(type, "IHDR", 4)) { pw = Be32(type + 4); ph = Be32(type + 8); }
        if (!std::memcmp(type, "IDAT", 4)) { z.insert(z.end(), type + 4, type + 4 + len); }
        if (!std::memcmp(type, "IEND", 4)) { iend = true; }
        p += 12 + len;
    }
    if (!iend || pw != w || ph != h) { std::printf("FAIL IHDR %ux%u / IEND %d\n", pw, ph, iend); ++g_fail; return; }
    std::vector<uint8_t> raw(size_t(h) * (1 + w * 3));
    uLongf rn = raw.size();
    if (uncompress(raw.data(), &rn, z.data(), z.size()) != Z_OK || rn != raw.size()) { std::printf("FAIL inflate (adler or blocks)\n"); ++g_fail; return; }
    size_t bad = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *r = &raw[size_t(y) * (1 + w * 3)];
        if (r[0] != 0) { ++bad; }
        for (uint32_t x = 0; x < w; ++x) { for (int c = 0; c < 3; ++c) { if (r[1 + x * 3 + c] != pix(x, y, c)) { ++bad; } } }
    }
    if (bad) { std::printf("FAIL %zu wrong bytes (%ux%u from %ux%u, bh %u)\n", bad, w, h, sw, sh, bh); ++g_fail; return; }
    std::printf("ok   %ux%u from a %ux%u surface (bh %u): %zu B\n", w, h, sw, sh, bh, file.size());
    if (out_path) { if (FILE *f = std::fopen(out_path, "wb")) { std::fwrite(file.data(), 1, file.size(), f); std::fclose(f); } }
}

/* v0.4.1: a mirrored row, odd and even widths */
static void MirrorCase(uint32_t w) {
    std::vector<uint8_t> row(w * 3), want(w * 3);
    for (uint32_t x = 0; x < w; ++x) { for (int k = 0; k < 3; ++k) { row[x * 3 + k] = uint8_t(x * 7 + k); want[(w - 1 - x) * 3 + k] = uint8_t(x * 7 + k); } }
    png::MirrorRowRgb(row.data(), w);
    if (row != want) { std::printf("FAIL MirrorRowRgb(%u)\n", w); ++g_fail; return; }
    std::printf("ok   mirrored row, %u px\n", w);
}

int main(int argc, char **argv) {
    RunCase(1920, 1080, 4, 1920, 1080, argc > 1 ? argv[1] : nullptr);   /* MK8 docked / ReverseNX */
    RunCase(1920, 1080, 4, 1280, 720, nullptr);                        /* the handheld corner */
    RunCase(1280, 720, 4, 1280, 720, nullptr);                         /* a 720p swapchain */
    RunCase(100, 37, 2, 100, 37, nullptr);                             /* odd sizes, another block height */
    RunCase(1920, 1080, 5, 1920, 1080, nullptr, true);                 /* v0.1.1b: 32-row blocks, B,G,R,A (Run AE's 1080p app) */
    MirrorCase(1920); MirrorCase(101); MirrorCase(1);
    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
