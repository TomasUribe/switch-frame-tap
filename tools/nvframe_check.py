#!/usr/bin/env python3
"""
nvframe_check.py - M82: check the real-frame NVENC run (the "nvframe" flag).

The console reads a presented 1920x1080 frame out of the game, has the VIC
scale it to 1280x720 NV12 in GPU block-linear layout (32-row blocks, the
layout grc's NVENC input uses), encodes that as an H.264 IDR frame, and saves:

  nvframe-0-y.bin / -0-uv.bin          frame 0 as the VIC wrote it (block-linear)
  nvframe-q16/q20/q24-status/-bits.bin frame 0 encoded at QP 16, 20 and 24
  nvframe-last-y/-uv/-status/-bits.bin the last frame of the timed loop (QP 20)

Colour is the VIC's pass-through, not YUV: luma = B, U = R, V = G (the
packed-420 stream's mapping). This tool undoes that for the PNGs.

For each saved picture it:
  - deswizzles the VIC planes and reports which layout makes the picture
    smooth (block heights 0-4 and pitch), so a wrong layout is visible as a
    number rather than as a judgement about a PNG
  - writes <tag>-vic.png
For each encode it:
  - prints the status, lists the NAL units, adds SPS/PPS built from grc's
    setup when the stream has none, and decodes
  - compares the decoded planes with the VIC's planes (PSNR per plane): high
    means NVENC read the same picture the VIC wrote, in the same layout
  - writes <tag>-decoded.png

  python3 tools/nvframe_check.py DIR
  python3 tools/nvframe_check.py selftest
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nvenc_replay as nr  # noqa: E402

W, H = 1280, 720
LUMA_ROWS, CHROMA_ROWS = 736, 384        # whole 32-row blocks
BH_LOG2 = 2                              # grc's input block height


def gob_offsets():
    """Byte offset of (x, y) inside one 64x8 GOB (the Generic_16Bx2 layout)."""
    x = np.arange(64)[None, :]
    y = np.arange(8)[:, None]
    return (x // 32) * 256 + (y // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)


def bl_index(width_bytes, rows, bh_log2):
    """For every (row, byte) of a width_bytes-wide plane, its offset in the
    block-linear buffer."""
    bh = 8 << bh_log2
    y = np.arange(rows)[:, None]
    x = np.arange(width_bytes)[None, :]
    blocks_per_row = (width_bytes + 63) // 64
    block_bytes = 512 << bh_log2
    g = gob_offsets()
    return ((y // bh) * blocks_per_row * block_bytes + (x // 64) * block_bytes
            + ((y % bh) // 8) * 512 + g[y % 8, x % 64])


def deswizzle(data, width_bytes, rows, bh_log2):
    buf = np.frombuffer(data, np.uint8)
    idx = bl_index(width_bytes, rows, bh_log2)
    out = np.zeros(idx.shape, np.uint8)
    ok = idx < len(buf)
    out[ok] = buf[idx[ok]]
    return out


def swizzle(plane, bh_log2, total):
    out = np.zeros(total, np.uint8)
    idx = bl_index(plane.shape[1], plane.shape[0], bh_log2)
    out[idx] = plane
    return out.tobytes()


def roughness(img):
    """Mean absolute difference between neighbouring pixels, vertically plus
    horizontally: small for a picture, larger for one read in the wrong
    layout (rows land in the wrong place, and 16-byte runs from different
    rows sit side by side)."""
    a = img.astype(np.int16)
    return float(np.abs(a[1:] - a[:-1]).mean() + np.abs(a[:, 1:] - a[:, :-1]).mean())


def layout_report(data, width_bytes, rows):
    cands = {f"block-linear h={b}": deswizzle(data, width_bytes, rows, b) for b in range(5)}
    buf = np.frombuffer(data, np.uint8)
    n = min(len(buf), width_bytes * rows)
    pitch = np.zeros((rows, width_bytes), np.uint8)
    pitch.reshape(-1)[:n] = buf[:n]
    cands["pitch"] = pitch
    scores = {k: roughness(v) for k, v in cands.items()}
    best = min(scores, key=scores.get)
    return best, scores, cands


def packed_to_rgb(y, u, v):
    """Pass-through NV12 -> RGB: luma = B, U = R, V = G."""
    r = np.repeat(np.repeat(u, 2, 0), 2, 1)[:y.shape[0], :y.shape[1]]
    g = np.repeat(np.repeat(v, 2, 0), 2, 1)[:y.shape[0], :y.shape[1]]
    return np.dstack([r, g, y])


def save_png(path, rgb):
    try:
        from PIL import Image
    except ImportError:
        return False
    Image.fromarray(rgb.astype(np.uint8), "RGB").save(path)
    return True


def psnr(a, b):
    mse = float(((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean())
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def decode_planes(stream):
    import av
    ctx = av.CodecContext.create("h264", "r")
    frames = []
    for pkt in list(ctx.parse(stream)) + list(ctx.parse(None)):
        frames += ctx.decode(pkt)
    frames += ctx.decode(None)
    if not frames:
        return None
    f = frames[0]
    a = f.to_ndarray(format="yuv420p")
    h, w = f.height, f.width
    y = a[:h]
    u = a[h:h + h // 4].reshape(h // 2, w // 2)
    v = a[h + h // 4:h + h // 2].reshape(h // 2, w // 2)
    return y, u, v


def vic_planes(d, tag, out=print):
    yp, uvp = d / f"nvframe-{tag}-y.bin", d / f"nvframe-{tag}-uv.bin"
    if not yp.exists() or not uvp.exists():
        out(f"nvframe-{tag}-y/uv.bin: missing")
        return None
    yb, uvb = yp.read_bytes(), uvp.read_bytes()
    best, scores, cands = layout_report(yb, W, LUMA_ROWS)
    out(f"{yp.name}: {len(yb)} B; roughness by layout: "
        + ", ".join(f"{k} {v:.1f}" for k, v in sorted(scores.items(), key=lambda kv: kv[1]))
        + f"  -> smoothest: {best}" + ("  (as configured)" if best == f"block-linear h={BH_LOG2}" else "  *** NOT the configured layout ***"))
    y = deswizzle(yb, W, LUMA_ROWS, BH_LOG2)[:H]
    uv = deswizzle(uvb, W, CHROMA_ROWS, BH_LOG2)[:H // 2]
    u, v = uv[:, 0::2], uv[:, 1::2]
    out(f"   luma mean {y.mean():.1f} range {y.min()}..{y.max()}; U(=R) mean {u.mean():.1f}, V(=G) mean {v.mean():.1f}")
    png = d / f"nvframe-{tag}-vic.png"
    if save_png(png, packed_to_rgb(y, u, v)):
        out(f"   -> {png}")
    return y, u, v


def encode_check(d, tag, ref, setup, out=print):
    st_path, bits_path = d / f"nvframe-{tag}-status.bin", d / f"nvframe-{tag}-bits.bin"
    if st_path.exists():
        st = st_path.read_bytes()
        (pic_index, err_word, total_bits, _t1, pic_type, num_slices, _act, avg_qp,
         _cyc, _hrd, _bs, last_valid, intra, inter) = struct.unpack_from(nr.STATUS_FMT, st, 0)
        out(f"status: picture_index={pic_index:#x} error_status={err_word & 3} ucode_error_status={err_word >> 2:#x} "
            f"{total_bits // 8} B pic_type={pic_type} slices={num_slices} avgQP={avg_qp} intra/inter={intra}/{inter}")
    if not bits_path.exists():
        out(f"{bits_path.name}: missing")
        return None
    bits = bits_path.read_bytes()
    for line in nr.describe_nals(bits):
        out(line)
    have = {n[0] & 0x1F for _, n in nr.split_nals(bits) if n}
    stream = bits if {7, 8} <= have else nr.headers_from_setup(setup) + bits
    try:
        planes = decode_planes(stream)
    except Exception as e:  # noqa: BLE001
        out(f"decode FAILED: {type(e).__name__}: {e}")
        return None
    if planes is None:
        out("no picture decoded")
        return None
    y, u, v = planes
    msg = f"decoded {y.shape[1]}x{y.shape[0]}"
    if ref is not None:
        ry, ru, rv = ref
        py, pu, pv = psnr(y[:H], ry), psnr(u[:H // 2], ru), psnr(v[:H // 2], rv)
        verdict = "NVENC encoded the picture the VIC wrote" if py > 30 else "*** decoded picture does not match the VIC's ***"
        msg += f"; PSNR against the VIC's planes: Y {py:.1f} dB, U {pu:.1f} dB, V {pv:.1f} dB -> {verdict}"
    out(msg)
    png = d / f"nvframe-{tag}-decoded.png"
    if save_png(png, packed_to_rgb(y[:H], u[:H // 2], v[:H // 2])):
        out(f"   -> {png}")
    return planes


def check(d, out=print):
    d = Path(d)
    setup = nr.parse_setup(nr.SETUP.read_bytes())
    out("== frame 0, as the VIC wrote it ==")
    ref0 = vic_planes(d, "0", out)
    for q in (16, 20, 24):
        out(f"\n== frame 0 at QP {q} ==")
        encode_check(d, f"q{q}", ref0, setup, out)
    out("\n== the last frame of the timed loop (QP 20) ==")
    ref1 = vic_planes(d, "last", out)
    encode_check(d, "last", ref1, setup, out)


def selftest():
    import av
    import tempfile
    # a picture with structure in every plane
    yy, xx = np.mgrid[0:H, 0:W]
    r = ((xx * 255) // W).astype(np.uint8)
    g = ((yy * 255) // H).astype(np.uint8)
    b = ((((xx // 40 + yy // 40) % 2) * 160 + 48 + (xx * 7 // 16) % 50 + (yy * 3 // 8) % 30) % 256).astype(np.uint8)
    y = b
    u = r[0::2, 0::2]
    v = g[0::2, 0::2]
    # round trip of the layout
    ybl = swizzle(np.vstack([y, np.zeros((LUMA_ROWS - H, W), np.uint8)]), BH_LOG2, W * LUMA_ROWS)
    uv = np.zeros((CHROMA_ROWS, W), np.uint8)
    uv[:H // 2, 0::2], uv[:H // 2, 1::2] = u, v
    uvbl = swizzle(uv, BH_LOG2, W * CHROMA_ROWS)
    assert np.array_equal(deswizzle(ybl, W, LUMA_ROWS, BH_LOG2)[:H], y)
    best, _, _ = layout_report(ybl, W, LUMA_ROWS)
    assert best == f"block-linear h={BH_LOG2}", best
    # x264 stands in for NVENC: the planes go in as YUV, as they do on the console
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = W, H, "yuv420p"
    enc.options = {"x264-params": "keyint=1:bframes=0", "qp": "20"}
    fr = av.VideoFrame(W, H, "yuv420p")
    fr.planes[0].update(y.tobytes())
    fr.planes[1].update(u.tobytes())
    fr.planes[2].update(v.tobytes())
    bits = b"".join(bytes(p) for p in enc.encode(fr)) + b"".join(bytes(p) for p in enc.encode(None))
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        for tag in ("0", "last"):
            (d / f"nvframe-{tag}-y.bin").write_bytes(ybl)
            (d / f"nvframe-{tag}-uv.bin").write_bytes(uvbl)
        for tag in ("q16", "q20", "q24", "last"):
            (d / f"nvframe-{tag}-bits.bin").write_bytes(bits)
            st = bytearray(0x1000)
            struct.pack_into(nr.STATUS_FMT, st, 0, 0x4D383200, 2, len(bits) * 8, 0, 3, 1, 0, 20, 0, 0, 0, len(bits), 3600, 0)
            (d / f"nvframe-{tag}-status.bin").write_bytes(st)
        lines = []
        check(d, out=lines.append)
        print("\n".join(lines))
        assert sum("NVENC encoded the picture the VIC wrote" in ln for ln in lines) == 4, "PSNR check"
        assert sum("(as configured)" in ln for ln in lines) == 2
        for tag in ("0-vic", "q20-decoded"):
            assert (d / f"nvframe-{tag}.png").exists()
    print("selftest OK")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) >= 2:
        check(sys.argv[1])
    else:
        print(__doc__)
