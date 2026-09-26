#!/usr/bin/env python3
"""
sft_tool.py - look inside a recorded SFTR stream (raw-view --record FILE).

A minute of the M83 H.264 stream is over a gigabyte, too big to commit. This
summarises a recording and cuts a small sample of it:

  python3 tools/sft_tool.py stats FILE [--decode N]
      packets, payload kinds, frame-number gaps, sizes, the bitrate at 60 fps;
      with --decode N, decodes the first N H.264 packets one by one and
      reports any that fail
  python3 tools/sft_tool.py head FILE OUT N
      copies the first N packets into OUT (commit that, not the recording)
  python3 tools/sft_tool.py last FILE DIR
      M85 drift test: decodes the whole recording and compares its last frame
      with the console's VIC picture of it (DIR/nvstream-last-y.bin, -uv.bin);
      its neighbours are compared too, so an off-by-one reads as one
  python3 tools/sft_tool.py artifacts FILE [N]
      M88: counts frames that look like an OLDER frame (k-2/k-3) than the one
      before them, and frames whose bottom half is older than their top (a
      tear) - what reading a slot before the GPU finished it produces
  python3 tools/sft_tool.py selftest
"""
import struct
import sys
from pathlib import Path

HDR = struct.Struct("<IHHIIIIII")
MAGIC = 0x52544653


def packets(path):
    with open(path, "rb") as f:
        while True:
            h = f.read(HDR.size)
            if len(h) < HDR.size:
                return
            magic, version, flags, w, hgt, stride, length, bh, kind = HDR.unpack(h)
            if magic != MAGIC:
                raise ValueError(f"bad magic {magic:#x} at offset {f.tell() - HDR.size}")
            payload = f.read(length)
            if len(payload) < length:
                return
            yield dict(version=version, flags=flags, w=w, h=hgt, length=length, kind=kind, raw=h + payload,
                       payload=payload, stride=stride)


def stats(path, decode=0, out=print):
    n = h264 = other = gaps = lost = 0
    sizes, last = [], None
    first = None
    key_sizes, ages = [], []
    for p in packets(path):
        n += 1
        if p["flags"] & 2:
            h264 += 1
            sizes.append(p["length"])
            if last is not None and p["kind"] > last + 1:
                gaps += 1
                lost += p["kind"] - last - 1
            last = p["kind"]
            first = p["kind"] if first is None else first
            if p["flags"] & 4:                       # M85: IDR
                key_sizes.append(p["length"])
            if 0 < p["stride"] < 10_000_000:         # M85: console-side age, us
                ages.append(p["stride"])
        else:
            other += 1
    out(f"{path}: {n} packets ({h264} H.264, {other} other)")
    if sizes:
        avg = sum(sizes) / len(sizes)
        out(f"   frame numbers {first}..{last}: {lost} missing in {gaps} gap(s)")
        out(f"   payload avg {avg / 1024:.0f} KB, min {min(sizes) / 1024:.0f} KB, max {max(sizes) / 1024:.0f} KB "
            f"-> {avg * 8 * 60 / 1e6:.0f} Mbps / {avg * 60 / 1e6:.1f} MB/s at 60 fps")
        if key_sizes:
            other_n = len(sizes) - len(key_sizes)
            other_avg = (sum(sizes) - sum(key_sizes)) / other_n if other_n else 0
            out(f"   {len(key_sizes)} IDR avg {sum(key_sizes) / len(key_sizes) / 1024:.0f} KB, "
                f"{other_n} P avg {other_avg / 1024:.0f} KB")
        if ages:
            out(f"   console latency (present -> header): avg {sum(ages) / len(ages) / 1000:.1f} ms, "
                f"max {max(ages) / 1000:.1f} ms")
    if decode:
        import av
        ctx = av.CodecContext.create("h264", "r")
        ok = bad = 0
        fmt = None
        for i, p in enumerate(packets(path)):
            if i >= decode:
                break
            if not p["flags"] & 2:
                continue
            try:
                frames = ctx.decode(av.Packet(p["payload"]))
                ok += len(frames)
                if frames and fmt is None:
                    f0 = frames[0]
                    fmt = f"{f0.width}x{f0.height} colorspace {int(f0.colorspace)} range {int(f0.color_range)}"
            except Exception as e:  # noqa: BLE001
                bad += 1
                out(f"   packet {i} (frame {p['kind']}): decode error {type(e).__name__}: {e}")
        ok += len(ctx.decode(None))
        out(f"   decoded {ok} frame(s) from the first {decode} packets, {bad} error(s); {fmt}")
    return n, h264, lost


def head(path, out_path, count):
    k = 0
    with open(out_path, "wb") as o:
        for p in packets(path):
            if k >= count:
                break
            o.write(p["raw"])
            k += 1
    print(f"wrote {k} packets to {out_path}")


def last(path, d, out=print):
    """Decode everything; compare the last frame (and its neighbours) with the
    VIC's planes the console saved for it."""
    import av
    import numpy as np
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import nvframe_check as nf
    d = Path(d)
    ref = nf.Run(d, nf.M83_LAYOUT, "bt709", lambda *_: None)
    vy = nf.deswizzle((d / "nvstream-last-y.bin").read_bytes(), nf.W, ref.luma_rows, ref.layout)[:nf.H]
    vuv = nf.deswizzle((d / "nvstream-last-uv.bin").read_bytes(), nf.W, ref.chroma_rows, ref.layout)[:nf.H // 2]
    ctx = av.CodecContext.create("h264", "r")
    keep, kinds, bad = {}, [], 0

    def take(frames):
        for f in frames:
            a = f.to_ndarray(format="yuv420p")
            h, w = f.height, f.width
            keep[f.pts] = (a[:h], a[h:h + h // 4].reshape(h // 2, w // 2), a[h + h // 4:h + h // 2].reshape(h // 2, w // 2))
            for k in [k for k in keep if k < f.pts - 3]:
                del keep[k]
    for p in packets(path):
        if not p["flags"] & 2:
            continue
        kinds.append(p["kind"])
        pk = av.Packet(p["payload"])
        pk.pts = p["kind"]
        try:
            take(ctx.decode(pk))
        except Exception:  # noqa: BLE001
            bad += 1
    take(ctx.decode(None))
    n = kinds[-1]
    since = 0
    for p in packets(path):
        if p["flags"] & 4:
            since = 0
        elif p["flags"] & 2:
            since += 1
    out(f"{path}: {len(kinds)} H.264 packets, {bad} decode error(s); last frame #{n}, {since} frame(s) after its IDR")
    best = None
    for k in sorted(keep):
        y, u, v = keep[k]
        py, pu, pv = nf.psnr(y, vy), nf.psnr(u, vuv[:, 0::2]), nf.psnr(v, vuv[:, 1::2])
        out(f"   frame #{k} against the VIC's last picture: Y {py:.1f} dB, U {pu:.1f} dB, V {pv:.1f} dB")
        if k == n:
            best = py
    verdict = best is not None and best > 30
    out("   -> " + ("THE LAST FRAME MATCHES: no drift along the P chain" if verdict
                    else "*** the last frame does not match its VIC picture ***"))
    return verdict


def artifacts_in(frames, out=print):
    """frames: luma arrays in order. Returns (regressions, tears)."""
    import numpy as np
    back = torn = 0
    ex = []
    for k in range(3, len(frames)):
        y, p1, p2, p3 = frames[k], frames[k - 1], frames[k - 2], frames[k - 3]
        d1 = np.abs(y - p1).mean()
        if d1 < 2:
            continue                    # static, or the same frame re-sent
        old = min(np.abs(y - p2).mean(), np.abs(y - p3).mean())
        h = y.shape[0] // 2
        t1 = np.abs(y[:h] - p1[:h]).mean()
        b1 = np.abs(y[h:] - p1[h:]).mean()
        b_old = min(np.abs(y[h:] - p2[h:]).mean(), np.abs(y[h:] - p3[h:]).mean())
        if old < 0.6 * d1:
            back += 1
            ex.append(k)
        if b_old < 0.5 * b1 and t1 < b1:
            torn += 1
    out(f"   {len(frames)} frames: {back} look like an older frame than the one before them, "
        f"{torn} have a bottom half older than the top; first: {ex[:10]}")
    return back, torn


def artifacts(path, n=3000, out=print):
    import av
    import numpy as np
    ctx = av.CodecContext.create("h264", "r")
    frames = []
    for p in packets(path):
        if not p["flags"] & 2:
            continue
        for f in ctx.decode(av.Packet(p["payload"])):
            frames.append(f.to_ndarray(format="gray")[::4, ::4].astype(np.float32))
        if len(frames) >= n:
            break
    out(f"{path}:")
    return artifacts_in(frames, out)


def selftest():
    import tempfile
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import sft_stream_test as t
    hdrs, sl = t.console_sps_pps(), t.nvenc_frames()
    with tempfile.TemporaryDirectory() as td:
        f = Path(td) / "s.sft"
        with open(f, "wb") as o:
            for i in range(10):
                if i != 4:
                    o.write(t.pack(i, hdrs + sl[i % 4]))
        lines = []
        n, h264, lost = stats(f, decode=9, out=lines.append)
        print("\n".join(lines))
        assert (n, h264, lost) == (9, 9, 1)
        assert any("decoded 9 frame(s)" in ln and "1280x720 colorspace 1 range 1" in ln for ln in lines), lines
        head(f, Path(td) / "h.sft", 3)
        assert stats(Path(td) / "h.sft", out=lambda *_: None)[0] == 3
        # M85: the drift test on Run J's real single frame (46.4 dB) as a
        # one-packet stream, and the same frame against a wrong picture
        j = Path(__file__).resolve().parent.parent / "logs" / "m84-runJ"
        if (j / "nvframe-last-bits.bin").exists():
            import nvenc_replay as nr
            (Path(td) / "nvstream-last-y.bin").write_bytes((j / "nvframe-last-y.bin").read_bytes())
            (Path(td) / "nvstream-last-uv.bin").write_bytes((j / "nvframe-last-uv.bin").read_bytes())
            pl = bytes(nr.stream_headers()) + (j / "nvframe-last-bits.bin").read_bytes()
            g = Path(td) / "k.sft"
            g.write_bytes(HDR.pack(MAGIC, 2, 6, 1280, 720, 9000, len(pl), 0, 7) + pl)
            lines = []
            assert last(g, td, out=lines.append), lines
            assert any("#7" in ln and "Y 46.4 dB" in ln for ln in lines), lines
            (Path(td) / "nvstream-last-y.bin").write_bytes((j / "nvframe-0-y.bin").read_bytes()[::-1])
            assert not last(g, td, out=lambda *_: None)
            print("drift test: Run J frame matches at 46.4 dB, a wrong picture fails")
    # M88: the artifact detector on synthetic motion - a clean pan, then the
    # same pan with a stale frame and a torn frame spliced in
    import numpy as np
    xs = np.arange(400, dtype=np.float32)
    base = np.tile(128 + 100 * np.sin(xs / 25.0), (180, 1)) + np.arange(180, dtype=np.float32)[:, None] * 0.2
    pan = [base[:, 3 * k:3 * k + 320].copy() for k in range(20)]
    assert artifacts_in(pan, out=lambda *_: None) == (0, 0)
    bad = [f.copy() for f in pan]
    bad[10] = pan[7].copy()                             # an old frame
    bad[15][90:] = pan[12][90:]                         # bottom half from 3 back
    back, torn = artifacts_in(bad, out=lambda *_: None)
    assert back >= 1 and torn >= 1, (back, torn)
    print("artifact detector: a clean pan is clean; a stale and a torn frame are caught")
    print("selftest OK")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "selftest":
        selftest()
    elif len(a) >= 2 and a[0] == "stats":
        stats(a[1], int(a[a.index("--decode") + 1]) if "--decode" in a else 0)
    elif len(a) >= 2 and a[0] == "artifacts":
        artifacts(a[1], int(a[2]) if len(a) > 2 else 3000)
    elif len(a) == 3 and a[0] == "last":
        sys.exit(0 if last(a[1], a[2]) else 1)
    elif len(a) == 4 and a[0] == "head":
        head(a[1], a[2], int(a[3]))
    else:
        print(__doc__)
