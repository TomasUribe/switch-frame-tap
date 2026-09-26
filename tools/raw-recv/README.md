# raw-view and raw-recv

**`raw-view`** is the PC viewer for the switch-frame-tap stream: libusb +
libavcodec + SDL2 in one process. It waits for the console to appear on USB
(`1209:5f1e`), decodes the H.264 stream live, and shows the frame rate, the
bitrate, frames lost, and two latencies in the title bar: `console` (the game
presenting a frame -> its header leaving the Switch) and `pc` (the header
arriving -> the frame on screen).

```
sudo apt install libusb-1.0-0-dev libsdl2-dev libavcodec-dev libavutil-dev
make                                   # must say "raw-view built WITH H.264"
./raw-view                             # live
./raw-view --record s.sft              # also save the stream (large: ~0.5-1 GB/min)
./raw-view --file s.sft                # replay a recording
./raw-view --threads N                 # decode threads (default 2; 0 = one per core)
./raw-view --low-latency               # one decode thread, frames out immediately
```

`python3 ../sft_tool.py stats|last|artifacts|head` inspects recordings.

---

The original raw-frame tooling follows.

# raw-recv

Reads raw captured frames from `switch-frame-tap` over USB bulk and writes them
to disk. This is the **option A** transport: no codec, no protocol negotiation.

The sysmodule sends a 32-byte header then the raw Tegra block-linear slot; this
tool writes the payload to a `.bin` that `tools/deswizzle.py` turns into a PNG.

`switch-stream/receiver/` is a different thing — it speaks `sw_hdr_t` and pipes
through libavcodec, so it expects an **encoded** stream. That is option B and
needs NVENC.

## Build

```
cc -O2 -Wall -Wextra -o raw-recv raw-recv.c $(pkg-config --cflags --libs libusb-1.0)
```

## Permissions (one-time, avoids sudo)

```
sudo cp 99-switch-frame-tap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Use

Start it before the probe fires — the sysmodule posts a chunk and waits for the
host to drain it. If nothing is listening it times out after 5 s and logs
`completion wait timed out`; the game is already resumed by then, so a mistimed
run costs a cycle and nothing worse.

```
until lsusb -d 1209:5f1e >/dev/null 2>&1; do sleep 1; done
./raw-recv -o /tmp/sft
python3 ../../tools/deswizzle.py /tmp/sft_000.bin /tmp/frame.png
```

Device is `1209:5f1e` (pid.codes open range), interface 0, bulk IN `0x81`.
The header layout must stay byte-identical to `SftHdr` in
`tier4/applet-mitm/source/applet_mitm_nv.cpp`.

## raw-view — live viewer (M60)

libusb + SDL2 in one process. No pipe, no codec: an external player would add
latency, which is the thing this prototype exists to minimise.

```
cc -O2 -o raw-view raw-view.c $(pkg-config --cflags --libs libusb-1.0 sdl2)
./raw-view --scale 2        # window is 2x the stream size
./raw-view --swap           # flip R and B if colours look wrong
```

Start it BEFORE the probe fires. The window opens on the first frame and its
title shows live fps and worst frame gap.

Pixel order: M60 sends the capture's native bytes (R,G,B,A), so the default is
`SDL_PIXELFORMAT_ABGR8888`. `--swap` selects ARGB for a VIC-produced stream,
which transposes R and B.

## raw-view — the H.264 stream (M83, `nvstream`)

Build (needs `libusb-1.0-0-dev libsdl2-dev libavcodec-dev libavutil-dev`):

```
make                      # says "raw-view built WITH H.264 (libavcodec)"
make test                 # replays real NVENC output through raw-view (no console needed)
```

Start it **before** the console's probe fires (the module waits up to 5 s for
a host, then gives up on the stream):

```
until lsusb -d 1209:5f1e >/dev/null 2>&1; do sleep 1; done
./raw-view --record /tmp/run.sft --h264 /tmp/run.h264
```

- Each SFTR packet with `flags & 2` is a complete H.264 access unit: SPS +
  PPS + one IDR slice, 1280x720, BT.709 limited range (the SPS's VUI says so,
  and the viewer sets SDL's YUV conversion to BT.709). `kind` is the
  console's frame number; gaps are counted as lost.
- `--record FILE` keeps every packet (header + payload) for later replay with
  `--file FILE`; `--h264 FILE` keeps the elementary stream, which any player
  opens (`ffplay FILE`).
- Decoding an IDR-only stream at ~150-170 Mbps is heavy. By default the
  viewer uses FFmpeg's frame threads (throughput, a few frames of latency);
  `--low-latency` uses one thread and shows each frame the moment it decodes.
- The last stdout line is machine-readable:
  `packets=N frames=N undecoded=N lost=N`.

The old `raw-view` binary that used to be committed here predates all of
this; build from source.
