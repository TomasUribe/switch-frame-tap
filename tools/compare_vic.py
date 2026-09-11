#!/usr/bin/env python3
"""Check the VIC's hardware blit against a software de-swizzle of the same bytes.

applet-mitm dumps two files from ONE frozen read, so they are the same pixels:

  applet-mitm-strip.bin  983,040 B  block-linear, 1920x128 (one block-row)
  applet-mitm-vic.bin     65,536 B  linear, 512 px stride, 480x32 used

This de-swizzles the strip in software, box-downscales it 4x, and diffs the
result against what the hardware produced. The VIC's scaler is a real filter
with its own phase and taps, so exact equality is not expected - a few LSB of
mean error means the surface layout is right. A large error means the config is
wrong (kind, block height, stride or pitch), which is the thing worth catching.

Usage: python3 tools/compare_vic.py strip.bin vic.bin [out_prefix]
"""
import sys, os, zlib, struct

SRC_W, SRC_H = 1920, 128
OUT_W, OUT_H, OUT_STRIDE = 480, 32, 512
SCALE = 4
BPP = 8 // 2


def gob_offset(x, y):
    return ((x // 32) * 256) + ((y // 2) * 64) + (((x % 32) // 16) * 32) + ((y % 2) * 16)


def deswizzle_strip(data):
    """One block-row -> SRC_H rows of SRC_W*4 bytes."""
    pitch = SRC_W * BPP
    rows = [bytearray(pitch) for _ in range(SRC_H)]
    for block_x in range(pitch // 64):
        base = block_x * 8192
        for gob_y in range(16):
            gob = base + gob_y * 512
            for yy in range(8):
                y = gob_y * 8 + yy
                if y >= SRC_H:
                    continue
                for xg in (0, 16, 32, 48):
                    src = gob + gob_offset(xg, yy)
                    x = block_x * 64 + xg
                    rows[y][x:x + 16] = data[src:src + 16]
    return rows


def box_downscale(rows):
    out = [bytearray(OUT_W * BPP) for _ in range(OUT_H)]
    for oy in range(OUT_H):
        for ox in range(OUT_W):
            acc = [0, 0, 0, 0]
            for dy in range(SCALE):
                r = rows[oy * SCALE + dy]
                for dx in range(SCALE):
                    i = (ox * SCALE + dx) * BPP
                    for c in range(4):
                        acc[c] += r[i + c]
            o = ox * BPP
            for c in range(4):
                out[oy][o + c] = acc[c] // (SCALE * SCALE)
    return out


def vic_rows(data):
    stride = OUT_STRIDE * BPP
    return [bytearray(data[y * stride: y * stride + OUT_W * BPP]) for y in range(OUT_H)]


def write_png(path, rows, w):
    out = []
    for r in rows:
        r = bytearray(r)
        r[3::4] = b"\xff" * (len(r) // 4)
        out.append(r)
    raw = b"".join(b"\x00" + bytes(r) for r in out)
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, len(out), 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def score(soft, hard):
    tot = mx = n = 0
    for y in range(OUT_H):
        for x in range(OUT_W):
            i = x * BPP
            for c in range(3):          # alpha carries no picture
                d = abs(soft[y][i + c] - hard[y][i + c])
                tot += d; n += 1
                mx = max(mx, d)
    return tot / n, mx


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    strip = open(sys.argv[1], "rb").read()
    vics = sys.argv[2:]
    print(f"strip {len(strip):,} B (expect 983,040)\n")

    full = deswizzle_strip(strip)
    soft = box_downscale(full)
    write_png("cmp_soft.png", soft, OUT_W)
    write_png("cmp_full.png", full, SRC_W)

    results = []
    for path in vics:
        data = open(path, "rb").read()
        if len(data) < OUT_STRIDE * BPP * OUT_H:
            print(f"  {os.path.basename(path):<34} SHORT ({len(data):,} B) - skipped")
            continue
        hard = vic_rows(data)
        mean, mx = score(soft, hard)
        name = os.path.basename(path).replace("applet-mitm-vic-", "").replace(".bin", "")
        write_png(f"cmp_{name}.png", hard, OUT_W)
        results.append((mean, mx, name))

    results.sort()
    print(f"  {'variant':<16} {'mean err':>9} {'max':>5}   verdict")
    for mean, mx, name in results:
        v = "*** LAYOUT CORRECT ***" if mean < 24 else ("close" if mean < 45 else "wrong")
        print(f"  {name:<16} {mean:9.2f} {mx:5d}   {v}")

    if results and results[0][0] < 24:
        print(f"\nWINNER: {results[0][2]}")
    elif results:
        print(f"\nNone matched. Best was {results[0][2]} at {results[0][0]:.1f} "
              f"- the layout field we need is not in this sweep.")
    print("\nwrote cmp_soft.png (software reference) and cmp_<variant>.png for each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
