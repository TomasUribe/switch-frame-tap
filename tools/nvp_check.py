#!/usr/bin/env python3
"""
nvp_check.py - M83: check the P-frame probe (the "nvp" flag).

The console encodes 1 IDR + N-1 P frames of real game frames with grc's own
P setup and P job, and saves:

  nvp-index.bin          u32 count, then u32 byte length of each frame
  nvp-stream-K.bin       the GOP's slices, concatenated, split into <= 1 MB files
  nvp-last-y/-uv.bin     the VIC's picture for the LAST frame (BT.709 NV12,
                         16-row block-linear)

This decodes the GOP behind SPS/PPS built from grc's setup (NVENC writes
slices only), checks every frame decoded and which were P, and compares the
last decoded frame - a P frame predicted through the whole chain - with the
VIC's own picture. High PSNR there means the references are right: no drift.

  python3 tools/nvp_check.py DIR
  python3 tools/nvp_check.py selftest
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nvenc_replay as nr  # noqa: E402
import nvframe_check as nf  # noqa: E402


def load(d):
    d = Path(d)
    idx = d / "nvp-index.bin"
    if not idx.exists():
        return None, None
    raw = idx.read_bytes()
    n = struct.unpack_from("<I", raw, 0)[0]
    lens = list(struct.unpack_from(f"<{n}I", raw, 4))
    data = b""
    k = 0
    while (d / f"nvp-stream-{k}.bin").exists():
        data += (d / f"nvp-stream-{k}.bin").read_bytes()
        k += 1
    return lens, data


def decode_all(stream):
    import av
    ctx = av.CodecContext.create("h264", "r")
    frames = []
    for pkt in list(ctx.parse(stream)) + list(ctx.parse(None)):
        frames += ctx.decode(pkt)
    frames += ctx.decode(None)
    return frames


def planes(frame):
    a = frame.to_ndarray(format="yuv420p")
    h, w = frame.height, frame.width
    return a[:h], a[h:h + h // 4].reshape(h // 2, w // 2), a[h + h // 4:h + h // 2].reshape(h // 2, w // 2)


def check(d, out=print):
    d = Path(d)
    lens, data = load(d)
    if lens is None:
        out("nvp-index.bin: missing")
        return False
    out(f"nvp: {len(lens)} frames, {len(data)} B of slices (index says {sum(lens)} B)")
    if len(data) != sum(lens):
        out("   *** stream length does not match the index ***")
        return False
    have = {n[0] & 0x1F for _, n in nr.split_nals(data[:lens[0]]) if n}
    stream = data if {7, 8} <= have else nr.stream_headers() + data
    off = 0
    for i, n in enumerate(lens):
        nals = [nal for _, nal in nr.split_nals(data[off:off + n])]
        types = sorted({nal[0] & 0x1F for nal in nals if nal})
        out(f"   frame {i:2}: {n:7} B  NAL types {types}")
        off += n
    frames = decode_all(stream)
    def kind(f):
        t = getattr(f, "pict_type", None)
        name = getattr(t, "name", None)
        return name[0] if name else {1: "I", 2: "P", 3: "B"}.get(int(t) if t is not None else 0, "?")
    kinds = "".join(kind(f) for f in frames)
    out(f"decoded {len(frames)} of {len(lens)} frames; picture types {kinds}")
    ok = len(frames) == len(lens)
    ref = nf.Run(d, nf.M83_LAYOUT, "bt709", out)
    yb, uvb = d / "nvp-last-y.bin", d / "nvp-last-uv.bin"
    if frames and yb.exists() and uvb.exists():
        vy = nf.deswizzle(yb.read_bytes(), nf.W, ref.luma_rows, ref.layout)[:nf.H]
        vuv = nf.deswizzle(uvb.read_bytes(), nf.W, ref.chroma_rows, ref.layout)[:nf.H // 2]
        y, u, v = planes(frames[-1])
        py, pu, pv = nf.psnr(y, vy), nf.psnr(u, vuv[:, 0::2]), nf.psnr(v, vuv[:, 1::2])
        drift_ok = py > 30
        out(f"last frame against the VIC's picture: Y {py:.1f} dB, U {pu:.1f} dB, V {pv:.1f} dB -> "
            + ("P FRAMES DECODE, NO DRIFT" if drift_ok else "*** the last frame does not match: references or setup wrong ***"))
        ok = ok and drift_ok
        png = d / "nvp-last-decoded.png"
        if nf.save_png(png, nf.yuv_to_rgb(y, u, v, "bt709")):
            out(f"   -> {png}")
    if len(lens) > 1:
        out(f"IDR {lens[0]} B, P frames avg {sum(lens[1:]) / (len(lens) - 1):.0f} B "
            f"({sum(lens[1:]) / (len(lens) - 1) / lens[0] * 100:.0f}% of the IDR)")
    return ok


def selftest():
    """x264 stands in for NVENC: a moving picture, 1 IDR + 9 P, written the
    way the console writes it; the check must decode all ten and match the
    last frame against the 'VIC' planes."""
    import av
    import tempfile
    W, H = nf.W, nf.H
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = W, H, "yuv420p"
    enc.options = {"x264-params": "keyint=100:bframes=0:ref=1:scenecut=0", "qp": "20"}
    yy, xx = np.mgrid[0:H, 0:W]
    bits, lens, last = b"", [], None
    for i in range(10):
        y = ((xx + 8 * i) // 3 % 200 + 20 + (yy // 40) % 2 * 20).astype(np.uint8)
        u = np.full((H // 2, W // 2), 110 + i, np.uint8)
        v = np.full((H // 2, W // 2), 140 - i, np.uint8)
        fr = av.VideoFrame(W, H, "yuv420p")
        fr.planes[0].update(y.tobytes()); fr.planes[1].update(u.tobytes()); fr.planes[2].update(v.tobytes())
        fr.pts = i
        for p in enc.encode(fr):
            b = bytes(p)
            bits += b
            lens.append(len(b))
        last = (y, u, v)
    for p in enc.encode(None):
        b = bytes(p)
        bits += b
        lens.append(len(b))
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "nvp-index.bin").write_bytes(struct.pack(f"<I{len(lens)}I", len(lens), *lens))
        (d / "nvp-stream-0.bin").write_bytes(bits)
        y, u, v = last
        lr, cr = nf.rows_for(H, nf.M83_LAYOUT), nf.rows_for(H // 2, nf.M83_LAYOUT)
        yp = np.zeros((lr, W), np.uint8); yp[:H] = y
        uvp = np.zeros((cr, W), np.uint8); uvp[:H // 2, 0::2], uvp[:H // 2, 1::2] = u, v
        (d / "nvp-last-y.bin").write_bytes(nf.swizzle(yp, nf.M83_LAYOUT, W * lr))
        (d / "nvp-last-uv.bin").write_bytes(nf.swizzle(uvp, nf.M83_LAYOUT, W * cr))
        lines = []
        ok = check(d, out=lines.append)
        print("\n".join(lines))
        assert ok and any("P FRAMES DECODE, NO DRIFT" in ln for ln in lines)
    print("selftest OK")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) >= 2:
        sys.exit(0 if check(sys.argv[1]) else 1)
    else:
        print(__doc__)
