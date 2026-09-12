#!/usr/bin/env python3
"""Turn an NV12 dump from the VIC into a PNG, to check the conversion is real.

NV12 is semi-planar YUV 4:2:0:
    Y plane   w*h bytes,      one luma sample per pixel
    UV plane  w*(h/2) bytes,  interleaved U,V at half resolution in BOTH axes

The VIC produces this directly from the game's block-linear RGBA, which is a
2.67x bandwidth reduction for free and the input format NVENC requires.

Usage:  python3 tools/nv12topng.py applet-mitm-nv12.bin out.png [width height]
"""
import sys, zlib, struct


def nv12_to_rgb(data, w, h):
    ysize = w * h
    if len(data) < ysize + (w * h // 2):
        raise SystemExit(f"short file: {len(data)} B, need {ysize + w*h//2} for {w}x{h}")
    y_plane, uv = data[:ysize], data[ysize:]
    rows = []
    for j in range(h):
        row = bytearray(w * 3)
        uvrow = (j >> 1) * w
        for i in range(w):
            y = y_plane[j * w + i]
            k = uvrow + (i & ~1)
            u = uv[k] - 128
            v = uv[k + 1] - 128
            # BT.601 limited-range is what the VIC emits by default
            c = y - 16
            r = (298 * c + 409 * v + 128) >> 8
            g = (298 * c - 100 * u - 208 * v + 128) >> 8
            b = (298 * c + 516 * u + 128) >> 8
            row[i*3+0] = 0 if r < 0 else (255 if r > 255 else r)
            row[i*3+1] = 0 if g < 0 else (255 if g > 255 else g)
            row[i*3+2] = 0 if b < 0 else (255 if b > 255 else b)
        rows.append(bytes(row))
    return rows


def write_png(path, rows, w, h):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "nv12.png"
    w = int(sys.argv[3]) if len(sys.argv) > 4 else 640
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 360
    data = open(src, "rb").read()
    print(f"{src}: {len(data):,} B, decoding as {w}x{h} NV12 "
          f"(expect {w*h*3//2:,})")
    rows = nv12_to_rgb(data, w, h)
    write_png(out, rows, w, h)
    # a flat grey image means the engine wrote nothing useful
    ys = data[:w*h]
    print(f"wrote {out}  luma min={min(ys)} max={max(ys)} "
          f"distinct={len(set(ys))}")
    if len(set(ys)) < 8:
        print("  WARNING: luma is nearly constant - the VIC likely did not write real pixels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
