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
    def __init__(self, d, layout, colour, out, w=None, h=None, prefix="nvframe", headers=None):
        """w, h, prefix and headers default to the 720p nvframe run; M90's
        1080p probe writes the same files as nv1080-* at 1920x1080."""
        self.d, self.layout, self.colour, self.out = Path(d), layout, colour, out
        self.w, self.h, self.p = w or W, h or H, prefix
        self.headers = headers
        self.luma_rows, self.chroma_rows = rows_for(self.h, layout), rows_for(self.h // 2, layout)

    def vic_planes(self, tag):
        d, out = self.d, self.out
        W, H = self.w, self.h
        yp, uvp = d / f"{self.p}-{tag}-y.bin", d / f"{self.p}-{tag}-uv.bin"
        if not yp.exists() or not uvp.exists():
            out(f"{self.p}-{tag}-y/uv.bin: missing")
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
        png = d / f"{self.p}-{tag}-vic.png"
        if save_png(png, yuv_to_rgb(y, u, v, self.colour)):
            out(f"   -> {png}")
        return (y, u, v), (yb, uvb)

    def source_check(self, planes):
        """The game's own RGBA for the top 128 rows, converted in float, against
        what the VIC wrote. Only meaningful when the VIC converts colour."""
        sp = self.d / f"{self.p}-0-src.bin"
        if not sp.exists() or planes is None or self.colour == "passthrough":
            return
        W = self.w
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
        W, H = self.w, self.h
        st_path, bits_path = d / f"{self.p}-{tag}-status.bin", d / f"{self.p}-{tag}-bits.bin"
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
        hdrs = self.headers if self.headers is not None else nr.headers_from_setup(nr.parse_setup(nr.SETUP.read_bytes()))
        stream = bits if {7, 8} <= have else hdrs + bits
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
        png = d / f"{self.p}-{tag}-decoded.png"
        if save_png(png, yuv_to_rgb(y[:H], u[:H // 2], v[:H // 2], self.colour)):
            out(f"   -> {png}")
        return match


def check(d, layout=M83_LAYOUT, colour="bt709", out=print, w=None, h=None, prefix="nvframe", headers=None, last=True):
    run = Run(d, layout, colour, out, w, h, prefix, headers)
    out(f"== settings: VIC block height h={layout}, colour {colour} ==")
    out("\n== frame 0, as the VIC wrote it ==")
    r0 = run.vic_planes("0")
    ref0, raw0 = (r0 if r0 else (None, None))
    run.source_check(ref0)
    results = []
    for q in (16, 20, 24):
        out(f"\n== frame 0 at QP {q} ==")
        results.append(run.encode_check(f"q{q}", ref0, raw0))
    if last:
        out("\n== the last frame of the timed loop (QP 20) ==")
        r1 = run.vic_planes("last")
        ref1, raw1 = (r1 if r1 else (None, None))
        results.append(run.encode_check("last", ref1, raw1))
    return results


# ------------------------------------------------------- M90: 1080p

W1080, H1080 = 1920, 1080


def headers_1080():
    return nr.headers_from_setup(nr.parse_setup(nr.setups_1080()[0]), vui=True)


def check_gop(d, prefix, w, h, headers, layout=M83_LAYOUT, out=print):
    """IDR + P frames as the console encoded them, decoded in order; the last
    against the VIC's picture of it (the drift test)."""
    d = Path(d)
    idx = d / f"{prefix}-gop-index.bin"
    if not idx.exists():
        out(f"{idx.name}: missing")
        return None
    raw = idx.read_bytes()
    n = struct.unpack_from("<I", raw, 0)[0]
    lens = list(struct.unpack_from(f"<{n}I", raw, 4))
    data = (d / f"{prefix}-gop.bin").read_bytes()
    out(f"{prefix} GOP: {n} frames, {len(data)} B (index says {sum(lens)} B); sizes " + ", ".join(str(x) for x in lens))
    if len(data) != sum(lens):
        out("   *** stream length does not match the index ***")
        return False
    import av
    ctx = av.CodecContext.create("h264", "r")
    frames = []
    for pkt in list(ctx.parse(headers + data)) + list(ctx.parse(None)):
        frames += ctx.decode(pkt)
    frames += ctx.decode(None)
    kinds = "".join({1: "I", 2: "P", 3: "B"}.get(int(f.pict_type) if f.pict_type is not None else 0, "?") for f in frames)
    out(f"   decoded {len(frames)} of {n} frames at {frames[0].width}x{frames[0].height}: {kinds}" if frames else "   nothing decoded")
    ok = len(frames) == n
    yb, uvb = d / f"{prefix}-gop-last-y.bin", d / f"{prefix}-gop-last-uv.bin"
    if frames and yb.exists() and uvb.exists():
        vy = deswizzle(yb.read_bytes(), w, rows_for(h, layout), layout)[:h]
        vuv = deswizzle(uvb.read_bytes(), w, rows_for(h // 2, layout), layout)[:h // 2]
        a = frames[-1].to_ndarray(format="yuv420p")
        fh, fw = frames[-1].height, frames[-1].width
        y, u, v = a[:fh], a[fh:fh + fh // 4].reshape(fh // 2, fw // 2), a[fh + fh // 4:fh + fh // 2].reshape(fh // 2, fw // 2)
        py, pu, pv = psnr(y[:h], vy), psnr(u[:h // 2], vuv[:, 0::2]), psnr(v[:h // 2], vuv[:, 1::2])
        drift_ok = py > 30
        out(f"   last frame against the VIC's picture: Y {py:.1f} dB, U {pu:.1f} dB, V {pv:.1f} dB -> "
            + ("P FRAMES DECODE, NO DRIFT" if drift_ok else "*** the last frame does not match ***"))
        ok = ok and drift_ok
    return ok


def check_ladder(d, out=print):
    """M91: the size ladder - each rung's IDR decoded with the SPS/PPS of its
    own setup and compared with the VIC's planes at that size. Returns
    {rung: True/False/None (not run)}."""
    res = {}
    for name, w, h, lv in nr.LADDER:
        if not (Path(d) / f"nv1080-r{name}-bits.bin").exists():
            out(f"\n== rung {name} ({w}x{h}, level {lv / 10}): not run ==")
            res[name] = None
            continue
        out(f"\n== rung {name} ({w}x{h}, level {lv / 10}) ==")
        hdrs = nr.headers_from_setup(nr.parse_setup(nr.ladder_setup(w, h, lv)))
        run = Run(d, M83_LAYOUT, "bt709", out, w, h, "nv1080", hdrs)
        r = run.vic_planes(f"r{name}")
        ref, raw = (r if r else (None, None))
        res[name] = run.encode_check(f"r{name}", ref, raw)
    return res


def check_1080(d, out=print):
    """M90/M91's nv1080 probe: the size ladder, frame 0 at three QPs (and the
    colour, docked), then the GOP's drift test. The timed loop is in the log."""
    hdrs = headers_1080()
    out("== M91 size ladder ==")
    ladder = check_ladder(d, out)
    out("\n== M90 nv1080: 1920x1080, VIC block height h=1, BT.709 ==")
    res = check(d, M83_LAYOUT, "bt709", out, W1080, H1080, "nv1080", hdrs, last=False)
    out("\n== the GOP (IDR + P) ==")
    res.append(check_gop(d, "nv1080", W1080, H1080, hdrs, out=out))
    return ladder, res


# ------------------------------------------------------------------ selftest

def synth_run(d, layout, colour, nvenc_layout, bits_for, w=None, h=None, prefix="nvframe"):
    """A directory as the console writes it: a synthetic game strip, converted
    by the VIC law (or passed through), written in `layout`, encoded from the
    bytes as NVENC would read them in `nvenc_layout`."""
    W, H = w or globals()["W"], h or globals()["H"]
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
        (d / f"{prefix}-0-src.bin").write_bytes(swizzle(rgba, 4, 128 * W * 4))
    lr, cr = rows_for(H, layout), rows_for(H // 2, layout)
    yplane = np.zeros((lr, W), np.uint8)
    yplane[:H] = y
    uv = np.zeros((cr, W), np.uint8)
    uv[:H // 2, 0::2], uv[:H // 2, 1::2] = u, v
    ybl, uvbl = swizzle(yplane, layout, W * lr), swizzle(uv, layout, W * cr)
    for tag in ("0", "last"):
        (d / f"{prefix}-{tag}-y.bin").write_bytes(ybl)
        (d / f"{prefix}-{tag}-uv.bin").write_bytes(uvbl)
    # what NVENC would read
    ny = deswizzle(ybl, W, rows_for(H, nvenc_layout), nvenc_layout)[:H]
    nuv = deswizzle(uvbl, W, rows_for(H // 2, nvenc_layout), nvenc_layout)[:H // 2]
    bits = bits_for(ny, nuv[:, 0::2], nuv[:, 1::2])
    for tag in ("q16", "q20", "q24", "last"):
        (d / f"{prefix}-{tag}-bits.bin").write_bytes(bits)
        st = bytearray(0x1000)
        struct.pack_into(nr.STATUS_FMT, st, 0, 0x4D383300, 2, len(bits) * 8, 0, 3, 1, 0, 20, 0, 0, 0, len(bits), 3600, 0)
        (d / f"{prefix}-{tag}-status.bin").write_bytes(st)


def x264_bits(y, u, v):
    import av
    H, W = y.shape
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
    # 3) M90: the same at 1920x1080 with nv1080 names, plus a synthetic GOP
    #    (x264 IDR + 9 P of a moving picture) for the drift test
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        synth_run(d, 1, "bt709", 1, x264_bits, W1080, H1080, "nv1080")
        import av
        enc = av.CodecContext.create("libx264", "w")
        enc.width, enc.height, enc.pix_fmt = W1080, H1080, "yuv420p"
        enc.options = {"x264-params": "keyint=60:bframes=0:scenecut=0:annexb=1", "qp": "20"}
        yy, xx = np.mgrid[0:H1080, 0:W1080]
        pkts, last = [], None
        for k in range(10):
            y = ((np.sin((xx + 12 * k) / 40.0) + np.cos(yy / 55.0)) * 50 + 128).astype(np.uint8)
            fr = av.VideoFrame(W1080, H1080, "yuv420p")
            fr.planes[0].update(y.tobytes())
            fr.planes[1].update(np.full((H1080 // 2, W1080 // 2), 128, np.uint8).tobytes())
            fr.planes[2].update(np.full((H1080 // 2, W1080 // 2), 128, np.uint8).tobytes())
            pkts += [bytes(p) for p in enc.encode(fr)]
            last = y
        pkts += [bytes(p) for p in enc.encode(None)]
        # x264's slices need x264's own SPS/PPS (grc's differ), so the test
        # stream keeps them in its first frame and the check adds no headers
        slices = [b"".join(b"\0\0\0\1" + n for _, n in nr.split_nals(p) if n and n[0] & 0x1F in (1, 5, 7, 8)) for p in pkts]
        (d / "nv1080-gop.bin").write_bytes(b"".join(slices))
        (d / "nv1080-gop-index.bin").write_bytes(struct.pack(f"<I{len(slices)}I", len(slices), *[len(x) for x in slices]))
        lr, cr = rows_for(H1080, 1), rows_for(H1080 // 2, 1)
        yplane = np.zeros((lr, W1080), np.uint8)
        yplane[:H1080] = last
        uv = np.full((cr, W1080), 128, np.uint8)
        (d / "nv1080-gop-last-y.bin").write_bytes(swizzle(yplane, 1, W1080 * lr))
        (d / "nv1080-gop-last-uv.bin").write_bytes(swizzle(uv, 1, W1080 * cr))
        lines = []
        res = check(d, 1, "bt709", lines.append, W1080, H1080, "nv1080", nr.headers_from_setup(nr.parse_setup(nr.setups_1080()[0]), vui=False), last=False)
        res.append(check_gop(d, "nv1080", W1080, H1080, b"", out=lines.append))
        all_lines += lines
        assert res == [True] * 4, (res, lines)
        assert any("THE VIC WRITES REAL BT709 YUV" in ln for ln in lines)
        assert any("decoded 10 of 10 frames at 1920x1080: IPPPPPPPPP" in ln for ln in lines), lines
        # M91: a ladder rung as the console writes it (x264 standing in for
        # NVENC, so its own SPS/PPS ride in the bits); absent rungs read "not run"
        synth_run(d, 1, "bt709", 1, x264_bits, 1920, 720, "nv1080")
        for f in ("y", "uv", "bits", "status"):
            (d / f"nv1080-r1920x720-{f}.bin").write_bytes((d / f"nv1080-{'0' if f in ('y', 'uv') else 'q20'}-{f}.bin").read_bytes())
        lad = check_ladder(d, lines.append)
        assert lad == {"720l32": None, "720l42": None, "1920x720": True, "1920x1088": None, "1080": None}, lad
    print("\n".join(all_lines))
    print("selftest OK")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "selftest":
        selftest()
    elif len(args) >= 2 and args[0] == "1080":
        check_1080(args[1])
    elif args:
        layout, colour = M83_LAYOUT, "bt709"
        if "--layout" in args:
            layout = int(args[args.index("--layout") + 1])
        if "--colour" in args:
            colour = args[args.index("--colour") + 1]
        check(args[0], layout, colour)
    else:
        print(__doc__)
