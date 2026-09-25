#!/usr/bin/env python3
"""
sft_stream_test.py - M83: the H.264 stream path, tested end to end on the PC.

What the console's nvstream sends, built here byte for byte the way
TryNvencStream builds it:
  - the SPS + PPS array compiled into the module (parsed out of
    tier4/applet-mitm/source/nvenc_grc_hdrs.h, so this tests the real bytes),
  - followed by real NVENC slices (M82 Run H's IDR frames, cut to the status
    block's bit count, as the console cuts them),
  - each frame as an SFTR packet: 32-byte header (version 2, flags 2 = H.264,
    1280x720, kind = frame number) then the payload.

It then replays that stream through tools/raw-recv/raw-view (--file
--headless --h264) and checks that every packet decoded, that the one
deliberately skipped frame number is counted as lost, and that the H.264
elementary stream raw-view wrote decodes again with PyAV as BT.709 1280x720.
Hardware is the only thing not covered: the USB transfer itself.

  python3 tools/sft_stream_test.py        (or: make -C tools/raw-recv test)
"""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HDRS = REPO / "tier4/applet-mitm/source/nvenc_grc_hdrs.h"
VIEW = REPO / "tools/raw-recv/raw-view"
RUN_H = REPO / "logs/m82-runH"
STATUS_FMT = "<IIIIHHHHIiIIHH"


def console_sps_pps():
    txt = HDRS.read_text()
    body = txt[txt.index("SpsPps["):]
    n = int(re.search(r"SpsPps\[(\d+)\]", body).group(1))
    vals = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
    assert len(vals) == n, (len(vals), n)
    return bytes(vals)


def nvenc_frames():
    """Real NVENC IDR slices, cut as the console cuts them."""
    out = []
    for tag in ("q16", "q20", "q24", "last"):
        st = (RUN_H / f"nvframe-{tag}-status.bin").read_bytes()
        total_bits = struct.unpack_from(STATUS_FMT, st, 0)[2]
        bits = (RUN_H / f"nvframe-{tag}-bits.bin").read_bytes()
        out.append(bits[:total_bits // 8])
    return out


def pack(frame_no, payload):
    return struct.pack("<IHHIIIIII", 0x52544653, 2, 2, 1280, 720, 0, len(payload), 0, frame_no) + payload


def main():
    if not VIEW.exists():
        subprocess.run(["make", "-C", str(VIEW.parent), "raw-view"], check=True)
    hdrs = console_sps_pps()
    slices = nvenc_frames()
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        sft, es = td / "stream.sft", td / "stream.h264"
        n, skipped = 0, 0
        with open(sft, "wb") as f:
            for i in range(41):
                if i == 17:          # a frame the console skipped (encode error)
                    skipped += 1
                    continue
                f.write(pack(i, hdrs + slices[i % len(slices)]))
                n += 1
        r = subprocess.run([str(VIEW), "--file", str(sft), "--headless", "--h264", str(es)],
                           capture_output=True, text=True, timeout=120)
        last = r.stdout.strip().splitlines()[-1]
        print(f"raw-view: {last}")
        got = dict(kv.split("=") for kv in last.split())
        assert got == {"packets": str(n), "frames": str(n), "undecoded": "0", "lost": str(skipped)}, (got, r.stderr)

        import av
        ctx = av.CodecContext.create("h264", "r")
        frames = []
        data = es.read_bytes()
        for pkt in list(ctx.parse(data)) + list(ctx.parse(None)):
            frames += ctx.decode(pkt)
        frames += ctx.decode(None)
        assert len(frames) == n, len(frames)
        f0 = frames[0]
        assert (f0.width, f0.height) == (1280, 720) and int(f0.colorspace) == 1 and int(f0.color_range) == 1
        print(f"the elementary stream raw-view wrote: {len(frames)} frames, 1280x720, BT.709 limited range, "
              f"{len(data) / n / 1024:.0f} KB/frame")
    print("sft_stream_test OK")


if __name__ == "__main__":
    sys.exit(main())
