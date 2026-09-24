#!/usr/bin/env python3
"""
nvjpg_dec_control.py - build (and later check) the M76 NVJPG decode positive
control.

Why a DECODE, when the project wants an encoder: every probe of NVENC and NVJPG
so far (M68-M74) submitted a job whose configuration was itself unverified, so
a stall could never be attributed - was it the engine, the clock, our submit
path, or the config? This job's configuration is NOT a guess. The T210 NVJPG
decode picture-info record was reverse-engineered by averne for oss-nvjpg,
which decodes JPEGs on this exact hardware under Horizon. If this job stalls,
the config is not the reason.

  generate:  python3 tools/nvjpg_dec_control.py gen
             -> tier4/applet-mitm/source/nvjpg_dec_control.h   (committed)
             -> tools/nvjpg_dec_control.ref.png                  (reference)
  check:     python3 tools/nvjpg_dec_control.py check sdmc-copy/nvjpg-dec.rgba
             compares the engine's output with Pillow's decode of the same JPEG

The record layout (0xB2C bytes) is written from these offsets, which match the
struct in oss-nvjpg's include/nvjpg/nv/registers.hpp (static_assert 0xB2C):

  0x000  4 x Huffman table   <- JPEG class 0 (DC) tables, by table id
  0x4D0  4 x Huffman table   <- JPEG class 1 (AC) tables, by table id
         each: u32 bits[16], u8 zero[80], u8 huffval[162], pad to 308
  0x9A0  4 x component       {u8 h, u8 v, u8 quant_id, u8 ac_id, u8 dc_id, pad}
  0x9C0  4 x quant table     64 bytes each, in the file's zigzag order
  0xAC0  u32 restart_interval, width, height, num_mcu_h, num_mcu_v,
         num_components, scan_data_offset, scan_data_size,
         scan_samp_layout, out_samp_layout, out_surf_type,
         out_luma_pitch, out_chroma_pitch, alpha
  0xAF8  u32 yuv2rgb[6]      16.16 fixed: Y gain, VR, UG, VG, UB, Y offset
  0xB10  u32 tile_mode, gob_height, memory_mode, downscale_log2, reserved[3]

Note the class placement. oss-nvjpg's names say the first array is "AC", but
its DHT parser stores class-0 (DC) tables there, and that is the code that works
on hardware - so this follows the bytes, not the names.
"""
import io
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "tier4/applet-mitm/source/nvjpg_dec_control.h"
REFPNG = REPO / "tools/nvjpg_dec_control.ref.png"

W, H = 64, 64
PITCH = 256              # RGBA row, aligned to 0x100 as oss-nvjpg's Surface does
QUALITY = 90

SAMP = {"mono": 0, "420": 1, "422": 2, "440": 3, "444": 4}
SURF_RGBA = 3
MEMORY_MODE_PLANAR = 3   # what oss-nvjpg uses for an RGBA Surface


def make_image():
    from PIL import Image
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            if y < H // 2 and x < W // 2:
                px[x, y] = (220, 30, 30)            # red
            elif y < H // 2:
                px[x, y] = (30, 200, 60)            # green
            elif x < W // 2:
                px[x, y] = (40, 60, 220)            # blue
            else:
                g = (x - W // 2) * 255 // (W // 2 - 1)
                px[x, y] = (g, g, g)                # grey ramp
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=QUALITY, subsampling=0, optimize=False, progressive=False)
    return buf.getvalue()


def parse_jpeg(data):
    """Minimal baseline parser: exactly what the record needs, nothing else."""
    assert data[:2] == b"\xff\xd8", "not a JPEG"
    i = 2
    q = {}
    dht = {0: {}, 1: {}}
    comps = []
    scan_sel = {}
    restart = 0
    width = height = 0
    scan_off = None
    while i < len(data):
        assert data[i] == 0xFF, f"marker expected at {i:#x}"
        m = data[i + 1]
        i += 2
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            continue
        seglen = struct.unpack(">H", data[i:i + 2])[0]
        seg = data[i + 2:i + seglen]
        if m == 0xDB:                                   # DQT
            j = 0
            while j < len(seg):
                pq, tq = seg[j] >> 4, seg[j] & 15
                assert pq == 0, "16-bit quant tables not handled"
                q[tq] = seg[j + 1:j + 65]
                j += 65
        elif m == 0xC4:                                 # DHT
            j = 0
            while j < len(seg):
                tc, th = seg[j] >> 4, seg[j] & 15
                bits = list(seg[j + 1:j + 17])
                n = sum(bits)
                dht[tc][th] = (bits, bytes(seg[j + 17:j + 17 + n]))
                j += 17 + n
        elif m == 0xC0:                                 # SOF0
            assert seg[0] == 8
            height, width = struct.unpack(">HH", seg[1:5])
            for k in range(seg[5]):
                cid, hv, tq = seg[6 + 3 * k:9 + 3 * k]
                comps.append({"id": cid, "h": hv >> 4, "v": hv & 15, "tq": tq})
        elif m in (0xC1, 0xC2, 0xC3):
            raise SystemExit("only baseline JPEG is supported")
        elif m == 0xDD:                                 # DRI
            restart = struct.unpack(">H", seg[:2])[0]
        elif m == 0xDA:                                 # SOS
            for k in range(seg[0]):
                cid, sel = seg[1 + 2 * k:3 + 2 * k]
                scan_sel[cid] = (sel >> 4, sel & 15)    # (DC table, AC table)
            scan_off = i + seglen
            break
        i += seglen
    assert scan_off is not None and comps
    return dict(q=q, dht=dht, comps=comps, sel=scan_sel, restart=restart,
                width=width, height=height, scan=data[scan_off:])


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fixed16(f):
    """oss-nvjpg's float_to_fixed: (int)(f * 65536.0f + 0.5f), in float32."""
    return int(f32(f32(f32(f) * 65536.0) + 0.5)) & 0xFFFFFFFF   # int() truncates like the C cast


def build_record(j):
    rec = bytearray(0xB2C)

    def put_huff(base, idx, bits, vals):
        off = base + idx * 308
        struct.pack_into("<16I", rec, off, *bits)
        rec[off + 64 + 80:off + 64 + 80 + len(vals)] = vals

    for th, (bits, vals) in j["dht"][0].items():
        put_huff(0x000, th, bits, vals)
    for th, (bits, vals) in j["dht"][1].items():
        put_huff(0x4D0, th, bits, vals)

    for k, c in enumerate(j["comps"]):
        dc, ac = j["sel"][c["id"]]
        struct.pack_into("<5B", rec, 0x9A0 + 8 * k, c["h"], c["v"], c["tq"], ac, dc)
    for tq, tab in j["q"].items():
        rec[0x9C0 + 64 * tq:0x9C0 + 64 * tq + 64] = tab

    hmax = max(c["h"] for c in j["comps"])
    vmax = max(c["v"] for c in j["comps"])
    c0 = j["comps"][0]
    if len(j["comps"]) == 1:
        samp = SAMP["mono"]
    elif c0["h"] == 2 and c0["v"] == 2:
        samp = SAMP["420"]
    elif c0["v"] == 2:
        samp = SAMP["440"]
    elif c0["h"] == 2:
        samp = SAMP["422"]
    else:
        samp = SAMP["444"]

    mcu_w, mcu_h = 8 * hmax, 8 * vmax
    # bt601 "ex" = JFIF full range, what oss-nvjpg uses by default
    kernel = [fixed16(1.0), fixed16(1.402), fixed16(-0.34416), fixed16(-0.71415), fixed16(1.772), 0]

    struct.pack_into("<14I", rec, 0xAC0,
                     j["restart"], j["width"], j["height"],
                     (j["width"] + mcu_w - 1) // mcu_w, (j["height"] + mcu_h - 1) // mcu_h,
                     len(j["comps"]), 0, len(j["scan"]),
                     samp, samp, SURF_RGBA, PITCH, 0, 0xFF)
    struct.pack_into("<6I", rec, 0xAF8, *kernel)
    struct.pack_into("<4I", rec, 0xB10, 0, 0, MEMORY_MODE_PLANAR, 0)
    return bytes(rec), samp


def block_means(rgb_rows):
    """8x8 block means of an RGB image, as u8 - robust to IDCT rounding."""
    out = []
    for by in range(H // 8):
        for bx in range(W // 8):
            for ch in range(3):
                s = 0
                for y in range(by * 8, by * 8 + 8):
                    for x in range(bx * 8, bx * 8 + 8):
                        s += rgb_rows[y][x][ch]
                out.append((s + 32) // 64)
    return bytes(out)


def c_array(name, data, ctype="u8"):
    lines = [f"    constexpr {ctype} {name}[{len(data)}] = {{"]
    for k in range(0, len(data), 16):
        lines.append("        " + ", ".join(f"0x{b:02x}" for b in data[k:k + 16]) + ",")
    lines.append("    };")
    return "\n".join(lines)


def gen():
    from PIL import Image
    jpeg = make_image()
    j = parse_jpeg(jpeg)
    assert (j["width"], j["height"]) == (W, H)
    rec, samp = build_record(j)

    ref = Image.open(io.BytesIO(jpeg)).convert("RGB")
    ref.save(REFPNG)
    rows = [[ref.getpixel((x, y)) for x in range(W)] for y in range(H)]
    means = block_means(rows)

    hdr = f"""/*
 * nvjpg_dec_control.h - GENERATED by tools/nvjpg_dec_control.py. Do not edit.
 *
 * M76 positive control: one {W}x{H} baseline JPEG (4:4:4, quality {QUALITY}),
 * its NVJPG decode picture-info record in the T210 layout used by working
 * Switch homebrew, and the expected 8x8 block means of the decoded RGB.
 * Regenerate with: python3 tools/nvjpg_dec_control.py gen
 */
#pragma once

namespace ams::mitm::applet::nvjpg_dec_control {{

    constexpr u32 Width  = {W};
    constexpr u32 Height = {H};
    constexpr u32 Pitch  = {PITCH};     /* RGBA, 0x100-aligned */
    constexpr u32 SamplingLayout = {samp};

{c_array("PictureInfo", rec)}
    static_assert(sizeof(PictureInfo) == 0xB2C);

{c_array("ScanData", j["scan"])}

    /* expected[(by * {W // 8} + bx) * 3 + ch], ch = R,G,B */
{c_array("ExpectedBlockMeans", means)}

}}
"""
    HEADER.write_text(hdr)
    print(f"wrote {HEADER.relative_to(REPO)}: record 0xB2C, scan {len(j['scan'])} B, sampling {samp}")
    print(f"wrote {REFPNG.relative_to(REPO)}")


def check(path):
    from PIL import Image
    raw = Path(path).read_bytes()
    need = PITCH * H
    if len(raw) < need:
        raise SystemExit(f"{path}: {len(raw)} bytes, expected at least {need}")
    ref = Image.open(REFPNG).convert("RGB")
    worst = 0
    total = 0
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            o = y * PITCH + 4 * x
            r, g, b = raw[o], raw[o + 1], raw[o + 2]
            row.append((r, g, b))
            er = ref.getpixel((x, y))
            d = max(abs(r - er[0]), abs(g - er[1]), abs(b - er[2]))
            worst = max(worst, d)
            total += d
        rows.append(row)
    out = Image.new("RGB", (W, H))
    for y in range(H):
        for x in range(W):
            out.putpixel((x, y), rows[y][x])
    png = Path(path).with_suffix(".png")
    out.save(png)
    print(f"max per-pixel channel diff {worst}, mean {total / (W * H):.2f}  -> {png}")
    print("MATCH" if worst <= 8 else "MISMATCH (see the png)")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        gen()
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        check(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
