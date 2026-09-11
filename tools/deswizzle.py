#!/usr/bin/env python3
"""Turn a raw Tegra block-linear slot dump from applet-mitm into a PNG.

The Switch stores the swapchain block-linear (kind 0xFE, Generic_16BX2), not as
rows of pixels. Layout, for a 1920x1080 A8B8G8R8 surface with block_height_log2=4:

  GOB          64 bytes wide x 8 rows            = 512 B
  block        16 GOBs stacked vertically        = 8192 B, covers 64 B x 128 rows
  block-row    120 blocks left to right          = 983,040 B, full width x 128 rows
  slot         9 block-rows                      = 8,847,360 B, 1920x1152 padded

Bytes per pixel are R,G,B,A - A8B8G8R8 as a little-endian 32-bit word - which is
already PNG's RGBA order, so no channel swap is needed.

Usage:  python3 tools/deswizzle.py applet-mitm-frame.bin [out.png]
"""
import sys, zlib, struct

W, H, PADDED_H, BPP = 1920, 1080, 1152, 4
PITCH        = W * BPP          # 7680
GOB_W, GOB_H = 64, 8
BLOCK_H      = 16               # 1 << block_height_log2
BLOCK_SZ     = GOB_W * GOB_H * BLOCK_H      # 8192
BLOCKS_ROW   = PITCH // GOB_W               # 120
BLOCK_ROW_SZ = BLOCKS_ROW * BLOCK_SZ        # 983040


def gob_offset(x, y):
    """Byte offset of (x,y) within one 64x8 GOB."""
    return ((x // 32) * 256) + ((y // 2) * 64) + (((x % 32) // 16) * 32) + ((y % 2) * 16)


def deswizzle(data, height):
    rows = [bytearray(PITCH) for _ in range(height)]
    nrows = min(height, (len(data) // BLOCK_ROW_SZ) * (GOB_H * BLOCK_H))
    for block_y in range((nrows + GOB_H * BLOCK_H - 1) // (GOB_H * BLOCK_H)):
        for block_x in range(BLOCKS_ROW):
            base = (block_y * BLOCKS_ROW + block_x) * BLOCK_SZ
            for gob_y in range(BLOCK_H):
                gob = base + gob_y * (GOB_W * GOB_H)
                for yy in range(GOB_H):
                    y = block_y * (GOB_H * BLOCK_H) + gob_y * GOB_H + yy
                    if y >= height:
                        continue
                    dst = rows[y]
                    for xg in (0, 16, 32, 48):
                        src = gob + gob_offset(xg, yy)
                        if src + 16 > len(data):
                            continue
                        x = block_x * GOB_W + xg
                        dst[x:x + 16] = data[src:src + 16]
    return rows


def write_png(path, rows, opaque=True, crop_w=None):
    """Write RGBA rows as a PNG.

    opaque: the Switch render target's alpha channel is NOT a display alpha -
    only ~54% of pixels carry 0xFF - so compositing it washes the image out.
    Force it to 255 unless the caller really wants to inspect it.
    crop_w: keep only the left crop_w pixels of each row.
    """
    out = []
    for r in rows:
        r = bytearray(r[:crop_w * BPP]) if crop_w else bytearray(r)
        if opaque:
            r[3::4] = b"\xff" * (len(r) // 4)
        out.append(r)
    rows = out
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]) // BPP, len(rows), 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "frame.png"
    data = open(src, "rb").read()
    print(f"{src}: {len(data):,} bytes ({len(data)/BLOCK_ROW_SZ:.2f} block-rows)")
    rows = deswizzle(data, H)
    crop = None
    if "--crop" in sys.argv:
        cw, ch = sys.argv[sys.argv.index("--crop") + 1].lower().split("x")
        crop, rows = int(cw), rows[: int(ch)]
    write_png(out, rows, opaque="--keep-alpha" not in sys.argv, crop_w=crop)
    nz = sum(1 for r in rows for b in r if b)
    print(f"wrote {out}  ({crop or W}x{len(rows)}, {nz:,} nonzero bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
