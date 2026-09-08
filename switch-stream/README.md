# switch-stream

A low-latency Nintendo Switch → PC screen streamer, in two programs:

- **`switch/`** — an Atmosphère sysmodule that reads the console's hardware H.264
  video + PCM audio from the `grc:d` service and ships it over USB or TCP.
- **`receiver/`** — a PC app (C, FFmpeg + SDL2 + libusb) that decodes and displays
  the stream with as little buffering as possible.

This is essentially a from-scratch, latency-tuned re-implementation of
[SysDVR](https://github.com/exelix11/SysDVR). If you only want a working tool,
use SysDVR. Use this if you want to modify the pipeline.

---

## What you actually get (hard limits)

| Property | Value | Why |
|---|---|---|
| Resolution | **1280×720** | fixed `grc:d` encoder config |
| Frame rate | **30 fps** | fixed `grc:d` encoder config |
| Codec | H.264 (Baseline/Main, no B-frames) | fixed |
| Bitrate | ~6–8 Mbps (attempted bump in config, OS may clamp) | fixed pipeline |
| Audio | 48 kHz, 16-bit, stereo PCM | fixed |
| Input passthrough | **none** — keep using the Switch's own controller | out of scope, on purpose |
| First image after connect | 1–10 s (waiting for the encoder's first IDR) | no API to force a keyframe |

There is **no supported homebrew path to 1080p or 60 fps** screen capture. Don't
expect console-native latency; expect "good enough for single-player and slower
games", not "ranked shooters".

## Rough latency budget (estimates, not measured on your gear)

| Transport | Console state | Extra latency vs. a real dock | Notes |
|---|---|---|---|
| USB-C cable to PC | handheld only | ~50–90 ms | lowest and most stable |
| Wired LAN (dock + USB-Ethernet adapter) | docked | ~60–110 ms | best "wireless dock" trade-off; stays docked & charged |
| 5 GHz Wi-Fi, same room | either | ~90–180 ms + jitter | playable for slower games |
| 2.4 GHz / congested Wi-Fi | either | 150 ms+++ | not recommended |

Component breakdown (approx): 30 fps cadence ~33 ms · HW encode ~1 frame ·
`grc:d` read + packetize <5 ms · transport (USB 2–5 / LAN 2–10 / Wi-Fi 10–60+) ·
PC decode 5–15 ms · present (vsync off) 0–8 ms.

## Recommended setup for a "wireless dock"

Switch **docked** (so it charges and can still drive the TV if you want) +
official **USB-LAN adapter** on the dock + PC on the **same wired switch/router**,
ideally nothing else sharing that path. Run the sender in `tcp` mode. This gets
you close to the USB latency floor without a cable to the PC and without Wi-Fi
jitter. Pure USB mode is a hair faster but forces handheld mode.

---

## Requirements

**Switch**
- Atmosphère CFW (tested target: recent AMS / firmware ≥ 12.x; older should work)
- devkitPro with `devkitA64` + `libnx` (`pacman -S switch-dev`)
- Games must support the capture button (nearly all retail titles do)

**PC (Linux/macOS/Windows)**
- CMake ≥ 3.16, a C11 compiler
- FFmpeg dev libs (`libavcodec`, `libavutil`), SDL2, libusb-1.0
  - Debian/Ubuntu: `sudo apt install build-essential cmake pkg-config libavcodec-dev libavutil-dev libsdl2-dev libusb-1.0-0-dev`
  - macOS: `brew install ffmpeg sdl2 libusb`
- For USB mode on Linux: a udev rule so you can open the device without root
  (see `receiver/70-switch-stream.rules`).

---

## Build

### Switch sysmodule
```bash
cd switch
make            # -> switch-stream.nsp
```
Install:
```
sdmc:/atmosphere/contents/0100000000000B00/exefs.nsp        <- the built .nsp
sdmc:/atmosphere/contents/0100000000000B00/flags/boot2.flag <- empty file, autostart
```
Create the sender config at `sdmc:/config/switch-stream/mode.txt` containing one line:
- `usb`            — stream over the USB-C port (handheld, cable to PC)
- `tcp`            — listen on port 9899
- `tcp:5000`       — listen on a custom port

Reboot. The module starts at boot and idles until a receiver connects.

### PC receiver
```bash
cd receiver
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## Run

USB:
```bash
./receiver/build/switch-stream-recv --usb
```
TCP:
```bash
./receiver/build/switch-stream-recv --tcp 192.168.1.50        # console IP
```
Useful flags: `--vsync` (smoother, +~1 frame), `--hw` (try hardware decode),
`--buffer N` (N-frame jitter buffer, default 0), `--no-audio`, `--fullscreen`.
Press `Esc` / `q` to quit, `f` to toggle fullscreen, `s` to print stats.

---

## Status / caveats

This code targets the real `libnx` / FFmpeg / SDL2 APIs and is structured to
build, but it has **not been run on hardware here**. Expect to iterate on:

- `grcdRead` semantics on your libnx version (older releases call it
  `grcdTransfer`; SysDVR pokes the IPC directly). See `switch/source/capture.c`.
- The npdm/kernel-capability list in `switch/switch-stream.json`.
- USB endpoint addresses / max packet size on the receiver
  (`receiver/src/source_usb.c`) if `usb_comms` descriptors differ.
- Bitrate: the `grc:d` "set params" call is version-dependent and may be a no-op.

See `switch-stream.json` and inline `TODO(hw)` comments.
