#!/usr/bin/env python3
"""
nvframe_check.py - check the real-frame NVENC runs (the "nvframe" flag).

The console reads a presented frame out of the game, has the VIC convert it
to 1280x720 NV12 in GPU block-linear layout, encodes that as an H.264 IDR
frame, and saves:

  nvframe-0-y.bin / -0-uv.bin          frame 0 as the VIC wrote it (block-linear)
  nvframe-0-src.bin                    (M83) the game's own pixels for the top
                                       128 rows of the picture, block-linear
                                       RGBA as read, so the colour conversion
                                       can be checked on real content
  nvframe-q16/q20/q24-status/-bits.bin frame 0 encoded at QP 16, 20 and 24
  nvframe-last-y/-uv/-status/-bits.bin the last frame of the timed loop (QP 20)

Two settings describe a run, and default to M83's:
  --layout N    the VIC's output block height, log2 GOBs: M83 writes 1 (16-row
                blocks, what NVENC reads); M82 wrote 2
  --colour C    bt709 (M83: the VIC converts to BT.709 limited-range YCbCr) or
                passthrough (M82: no matrix, luma = B, U = R, V = G)

For each saved picture it deswizzles the VIC planes, scores every layout for
smoothness (a wrong one shows up as a number), and writes <tag>-vic.png. For
each encode it decodes (SPS/PPS built from grc's setup when the stream has
none) and compares with the VIC's planes, per plane, in PSNR. When that
comparison fails it also finds the layout NVENC actually read the VIC's bytes
in - which is how M82 Run H's mismatch was diagnosed:

  python3 tools/nvframe_check.py logs/m82-runH --layout 2 --colour passthrough
  -> "NVENC read the VIC's bytes as block-linear h=1"

  python3 tools/nvframe_check.py DIR [--layout N] [--colour bt709|passthrough]
  python3 tools/nvframe_check.py selftest
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nvenc_replay as nr  # noqa: E402
import vic_csc  # noqa: E402

W, H = 1280, 720
M83_LAYOUT = 1                            # what NVENC reads (M82 Run H)


def rows_for(rows, bh_log2):
    bh = 8 << bh_log2
    return (rows + bh - 1) // bh * bh


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


def layouts(data, width_bytes, rows):
    """The plane read every way the bytes could be laid out."""
    out = {f"block-linear h={b}": deswizzle(data, width_bytes, rows_for(rows, b), b)[:rows] for b in range(5)}
    buf = np.frombuffer(data, np.uint8)
    n = min(len(buf), width_bytes * rows)
    pitch = np.zeros((rows, width_bytes), np.uint8)
    pitch.reshape(-1)[:n] = buf[:n]
    out["pitch"] = pitch
    return out


def layout_report(data, width_bytes, rows):
    cands = layouts(data, width_bytes, rows)
    scores = {k: roughness(v) for k, v in cands.items()}
    return min(scores, key=scores.get), scores, cands


def yuv_to_rgb(y, u, v, colour):
    """Full-resolution RGB for display. passthrough undoes M67's packing
    (luma = B, U = R, V = G); bt709 is the limited-range inverse."""
    up = lambda p: np.repeat(np.repeat(p, 2, 0), 2, 1)[:y.shape[0], :y.shape[1]]  # noqa: E731
    if colour == "passthrough":
        return np.dstack([up(u), up(v), y])
    kr, kb = vic_csc.STANDARDS[colour]
    kg = 1 - kr - kb
    yf = (y.astype(np.float64) - 16) * 255 / 219
    cb = (up(u).astype(np.float64) - 128) * 255 / 224
    cr = (up(v).astype(np.float64) - 128) * 255 / 224
    r = yf + 2 * (1 - kr) * cr
    b = yf + 2 * (1 - kb) * cb
    g = (yf - kr * r - kb * b) / kg
    return np.clip(np.dstack([r, g, b]) + 0.5, 0, 255).astype(np.uint8)


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


class Run:
    def __init__(self, d, layout, colour, out):
        self.d, self.layout, self.colour, self.out = Path(d), layout, colour, out
        self.luma_rows, self.chroma_rows = rows_for(H, layout), rows_for(H // 2, layout)

    def vic_planes(self, tag):
        d, out = self.d, self.out
        yp, uvp = d / f"nvframe-{tag}-y.bin", d / f"nvframe-{tag}-uv.bin"
        if not yp.exists() or not uvp.exists():
            out(f"nvframe-{tag}-y/uv.bin: missing")
            return None
        yb, uvb = yp.read_bytes(), uvp.read_bytes()
        want = f"block-linear h={self.layout}"
        best, scores, _ = layout_report(yb, W, H)
        out(f"{yp.name}: {len(yb)} B; roughness by layout: "
            + ", ".join(f"{k} {v:.1f}" for k, v in sorted(scores.items(), key=lambda kv: kv[1]))
            + f"  -> smoothest: {best}" + ("  (as configured)" if best == want else f"  *** configured {want} ***"))
        y = deswizzle(yb, W, self.luma_rows, self.layout)[:H]
        uv = deswizzle(uvb, W, self.chroma_rows, self.layout)[:H // 2]
        u, v = uv[:, 0::2], uv[:, 1::2]
        if self.colour == "passthrough":
            out(f"   luma(=B) mean {y.mean():.1f}; U(=R) mean {u.mean():.1f}, V(=G) mean {v.mean():.1f}")
        else:
            out(f"   Y mean {y.mean():.1f} range {y.min()}..{y.max()}; U mean {u.mean():.1f} range {u.min()}..{u.max()}; "
                f"V mean {v.mean():.1f} range {v.min()}..{v.max()}")
        png = d / f"nvframe-{tag}-vic.png"
        if save_png(png, yuv_to_rgb(y, u, v, self.colour)):
            out(f"   -> {png}")
        return (y, u, v), (yb, uvb)

    def source_check(self, planes):
        """The game's own RGBA for the top 128 rows, converted in float, against
        what the VIC wrote. Only meaningful when the VIC converts colour."""
        sp = self.d / "nvframe-0-src.bin"
        if not sp.exists() or planes is None or self.colour == "passthrough":
            return
        src = sp.read_bytes()
        rows = 128
        rgba = deswizzle(src, W * 4, rows, 4)
        r, g, b = (rgba[:, c::4].astype(np.float64) for c in range(3))
        kr, kb = vic_csc.STANDARDS[self.colour]
        kg = 1 - kr - kb
        yl = kr * r + kg * g + kb * b
        y_i = 16 + 219 / 255 * yl
        cb_i = 128 + 224 / 255 * (b - yl) / (2 * (1 - kb))
        cr_i = 128 + 224 / 255 * (r - yl) / (2 * (1 - kr))
        y, u, v = planes
        ey = np.abs(y[:rows].astype(np.float64) - y_i)
        avg = lambda p: (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2]) / 4  # noqa: E731
        eu_avg = np.abs(u[:rows // 2] - avg(cb_i))
        ev_avg = np.abs(v[:rows // 2] - avg(cr_i))
        eu_dec = np.abs(u[:rows // 2] - cb_i[0::2, 0::2])
        ev_dec = np.abs(v[:rows // 2] - cr_i[0::2, 0::2])
        self.out(f"{sp.name}: the game's pixels for rows 0-{rows - 1}, converted to {self.colour} in float, against the VIC's planes:")
        self.out(f"   Y: mean error {ey.mean():.2f}, 99th percentile {np.percentile(ey, 99):.2f} steps")
        self.out(f"   U/V against a 2x2 average: mean {eu_avg.mean():.2f}/{ev_avg.mean():.2f}; "
                 f"against the top-left sample: mean {eu_dec.mean():.2f}/{ev_dec.mean():.2f} steps")
        ok = ey.mean() < 1.0 and min(eu_avg.mean(), eu_dec.mean()) < 1.5 and min(ev_avg.mean(), ev_dec.mean()) < 1.5
        self.out("   -> THE VIC WRITES REAL " + self.colour.upper() + " YUV" if ok else "   -> *** colour does not match ***")

    def encode_check(self, tag, ref, raw):
        d, out = self.d, self.out
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
        stream = bits if {7, 8} <= have else nr.headers_from_setup(nr.parse_setup(nr.SETUP.read_bytes())) + bits
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
        match = None
        if ref is not None:
            ry, ru, rv = ref
            py, pu, pv = psnr(y[:H], ry), psnr(u[:H // 2], ru), psnr(v[:H // 2], rv)
            match = bool(py > 30)
            msg += (f"; PSNR against the VIC's planes: Y {py:.1f} dB, U {pu:.1f} dB, V {pv:.1f} dB -> "
                    + ("NVENC ENCODED THE PICTURE THE VIC WROTE" if match else "*** decoded picture does not match the VIC's ***"))
        out(msg)
        if match is False and raw is not None:
            yb, _ = raw
            scores = {k: psnr(y[:H], img) for k, img in layouts(yb, W, H).items()}
            best = max(scores, key=scores.get)
            out(f"   what NVENC read the VIC's luma bytes as: "
                + ", ".join(f"{k} {v:.1f} dB" for k, v in sorted(scores.items(), key=lambda kv: -kv[1])))
            out(f"   -> NVENC read the VIC's bytes as {best}" + (" - the VIC must write that layout" if scores[best] > 30 else ""))
        png = d / f"nvframe-{tag}-decoded.png"
        if save_png(png, yuv_to_rgb(y[:H], u[:H // 2], v[:H // 2], self.colour)):
            out(f"   -> {png}")
        return match


def check(d, layout=M83_LAYOUT, colour="bt709", out=print):
    run = Run(d, layout, colour, out)
    out(f"== settings: VIC block height h={layout}, colour {colour} ==")
    out("\n== frame 0, as the VIC wrote it ==")
    r0 = run.vic_planes("0")
    ref0, raw0 = (r0 if r0 else (None, None))
    run.source_check(ref0)
    results = []
    for q in (16, 20, 24):
        out(f"\n== frame 0 at QP {q} ==")
        results.append(run.encode_check(f"q{q}", ref0, raw0))
    out("\n== the last frame of the timed loop (QP 20) ==")
    r1 = run.vic_planes("last")
    ref1, raw1 = (r1 if r1 else (None, None))
    results.append(run.encode_check("last", ref1, raw1))
    return results


# ------------------------------------------------------------------ selftest

def synth_run(d, layout, colour, nvenc_layout, bits_for):
    """A directory as the console writes it: a synthetic game strip, converted
    by the VIC law (or passed through), written in `layout`, encoded from the
    bytes as NVENC would read them in `nvenc_layout`."""
    yy, xx = np.mgrid[0:H, 0:W]
    r = ((xx * 255) // W).astype(np.uint8)
    g = ((yy * 255) // H).astype(np.uint8)
    b = ((((xx // 40 + yy // 40) % 2) * 160 + 48 + (xx * 7 // 16) % 50 + (yy * 3 // 8) % 30) % 256).astype(np.uint8)
    if colour == "passthrough":
        y, u, v = b, r[0::2, 0::2], g[0::2, 0::2]
    else:
        m = vic_csc.design(colour)
        # the law, vectorised: rows (Y, V, U), inputs (B, G, R) at 10 bits
        ins = [b.astype(np.int64) << 2, g.astype(np.int64) << 2, r.astype(np.int64) << 2]
        rows = [(np.clip(((m[i][0] * ins[0] + m[i][1] * ins[1] + m[i][2] * ins[2]) >> vic_csc.DESIGN_SHIFT) + m[i][3] >> 8, 0, 1023) >> 2).astype(np.uint8)
                for i in range(3)]
        y = rows[0]
        v = ((rows[1][0::2, 0::2].astype(np.uint16) + rows[1][1::2, 0::2] + rows[1][0::2, 1::2] + rows[1][1::2, 1::2] + 2) // 4).astype(np.uint8)
        u = ((rows[2][0::2, 0::2].astype(np.uint16) + rows[2][1::2, 0::2] + rows[2][0::2, 1::2] + rows[2][1::2, 1::2] + 2) // 4).astype(np.uint8)
        # the source strip, block-linear RGBA, 16-GOB blocks, first 128 rows
        rgba = np.zeros((128, W * 4), np.uint8)
        rgba[:, 0::4], rgba[:, 1::4], rgba[:, 2::4], rgba[:, 3::4] = r[:128], g[:128], b[:128], 255
        (d / "nvframe-0-src.bin").write_bytes(swizzle(rgba, 4, 80 * 8192))
    lr, cr = rows_for(H, layout), rows_for(H // 2, layout)
    yplane = np.zeros((lr, W), np.uint8)
    yplane[:H] = y
    uv = np.zeros((cr, W), np.uint8)
    uv[:H // 2, 0::2], uv[:H // 2, 1::2] = u, v
    ybl, uvbl = swizzle(yplane, layout, W * lr), swizzle(uv, layout, W * cr)
    for tag in ("0", "last"):
        (d / f"nvframe-{tag}-y.bin").write_bytes(ybl)
        (d / f"nvframe-{tag}-uv.bin").write_bytes(uvbl)
    # what NVENC would read
    ny = deswizzle(ybl, W, rows_for(H, nvenc_layout), nvenc_layout)[:H]
    nuv = deswizzle(uvbl, W, rows_for(H // 2, nvenc_layout), nvenc_layout)[:H // 2]
    bits = bits_for(ny, nuv[:, 0::2], nuv[:, 1::2])
    for tag in ("q16", "q20", "q24", "last"):
        (d / f"nvframe-{tag}-bits.bin").write_bytes(bits)
        st = bytearray(0x1000)
        struct.pack_into(nr.STATUS_FMT, st, 0, 0x4D383300, 2, len(bits) * 8, 0, 3, 1, 0, 20, 0, 0, 0, len(bits), 3600, 0)
        (d / f"nvframe-{tag}-status.bin").write_bytes(st)


def x264_bits(y, u, v):
    import av
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = W, H, "yuv420p"
    enc.options = {"x264-params": "keyint=1:bframes=0", "qp": "20"}
    fr = av.VideoFrame(W, H, "yuv420p")
    fr.planes[0].update(np.ascontiguousarray(y).tobytes())
    fr.planes[1].update(np.ascontiguousarray(u).tobytes())
    fr.planes[2].update(np.ascontiguousarray(v).tobytes())
    return b"".join(bytes(p) for p in enc.encode(fr)) + b"".join(bytes(p) for p in enc.encode(None))


def selftest():
    import tempfile
    # the layout round trip
    plane = (np.arange(H * W) % 251).astype(np.uint8).reshape(H, W)
    for bh in range(5):
        assert np.array_equal(deswizzle(swizzle(np.vstack([plane, np.zeros((rows_for(H, bh) - H, W), np.uint8)]), bh,
                                                W * rows_for(H, bh)), W, rows_for(H, bh), bh)[:H], plane)
    all_lines = []
    # 1) M83: VIC writes h=1 BT.709, NVENC reads h=1 -> match, colour verified
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        synth_run(d, 1, "bt709", 1, x264_bits)
        lines = []
        res = check(d, out=lines.append)
        all_lines += lines
        assert res == [True] * 4, res
        assert sum("(as configured)" in ln for ln in lines) == 2
        assert any("THE VIC WRITES REAL BT709 YUV" in ln for ln in lines)
        for tag in ("0-vic", "q20-decoded"):
            assert (d / f"nvframe-{tag}.png").exists()
    # 2) M82 Run H's situation: VIC writes h=2, NVENC reads h=1 -> mismatch, diagnosed
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        synth_run(d, 2, "passthrough", 1, x264_bits)
        lines = []
        res = check(d, layout=2, colour="passthrough", out=lines.append)
        all_lines += lines
        assert res == [False] * 4, res
        assert sum("NVENC read the VIC's bytes as block-linear h=1" in ln for ln in lines) == 4
    print("\n".join(all_lines))
    print("selftest OK")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "selftest":
        selftest()
    elif args:
        layout, colour = M83_LAYOUT, "bt709"
        if "--layout" in args:
            layout = int(args[args.index("--layout") + 1])
        if "--colour" in args:
            colour = args[args.index("--colour") + 1]
        check(args[0], layout, colour)
    else:
        print(__doc__)
