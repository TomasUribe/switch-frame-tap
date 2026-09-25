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
                       payload=payload)


def stats(path, decode=0, out=print):
    n = h264 = other = gaps = lost = 0
    sizes, last = [], None
    first = None
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
        else:
            other += 1
    out(f"{path}: {n} packets ({h264} H.264, {other} other)")
    if sizes:
        avg = sum(sizes) / len(sizes)
        out(f"   frame numbers {first}..{last}: {lost} missing in {gaps} gap(s)")
        out(f"   payload avg {avg / 1024:.0f} KB, min {min(sizes) / 1024:.0f} KB, max {max(sizes) / 1024:.0f} KB "
            f"-> {avg * 8 * 60 / 1e6:.0f} Mbps / {avg * 60 / 1e6:.1f} MB/s at 60 fps")
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
    print("selftest OK")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "selftest":
        selftest()
    elif len(a) >= 2 and a[0] == "stats":
        stats(a[1], int(a[a.index("--decode") + 1]) if "--decode" in a else 0)
    elif len(a) == 4 and a[0] == "head":
        head(a[1], a[2], int(a[3]))
    else:
        print(__doc__)
