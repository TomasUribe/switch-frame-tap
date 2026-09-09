# switch-stream-project

Low-latency Nintendo Switch → PC screen streaming, and the research to push it
past what existing tools can do. Homebrew, for the author's own console, on
Atmosphère CFW.

## The short version

**SysDVR** already streams the Switch screen to a PC. It is capped at **720p30,
game layer only**, because it reads the `grc:d` game-recording encoder, whose
config is fixed in firmware.

This project set out to do better — native resolution, higher frame rate — and
worked down through every layer the Switch exposes. The result:

- A **plain sysmodule cannot beat SysDVR.** `nvdrv`, `apm`, and `caps:sc` live
  capture are all off-limits from that context on firmware 22.5.0. (`tier4/recon`)
- An **Atmosphère mitm module can.** By interposing on `vi:u` → the graphics
  binder, a module sees **every frame the game presents at 60 fps** and gets the
  exact memory layout of each one. It can then open the game's framebuffers
  (25 MB, native 1080p for Mario Kart 8) and reach the **VIC** (hardware
  block-linear→linear converter) and **NVENC** (hardware H.264 encoder) —
  a complete GPU-free video pipeline. (`tier4/applet-mitm`)
- Making that mitm work required **patching a capability into libstratosphere**
  that upstream does not have: forwarding for sub-objects returned from a mitm
  command on a **non-domain** session. (`tier4/applet-mitm/patch_libstrat.py`)

Full story: **[tier4/mitm/WRITEUP.md](tier4/mitm/WRITEUP.md)**.
Current state and the plan from here: **[tier4/mitm/STATUS.md](tier4/mitm/STATUS.md)**.

## What's in here

| Path | What it is | State |
|---|---|---|
| `switch-stream/` | A from-scratch low-latency USB/TCP streamer (sysmodule + PC receiver), written before settling on SysDVR as the base. The **PC receiver** (FFmpeg + SDL2, hardware decode, near-zero buffering) is reusable as the client for anything here. | receiver builds & runs; sysmodule untested |
| `tier4/recon/` | `tier4-recon` — a read-only, opt-in diagnostic sysmodule that mapped exactly what a plain sysmodule can and cannot reach. | done, its job is finished |
| `tier4/stream-oc/` | `stream-oc` — a charger-gated overclock companion sysmodule (docked clocks while on the official 39 W adapter). Runs alongside stock SysDVR. | builds; untested on hardware |
| `tier4/applet-mitm/` | The mitm module. Wraps `vi:u` → `IApplicationDisplayService` → `IHOSBinderDriver`, parses `NvGraphicBuffer`, opens the game's nvmap, surveys the nv engines. | **access layer verified on hardware; VIC/NVENC pipeline not yet built** |
| `tier4/DESIGN.md`, `tier4/FINDINGS.md` | Historical working notes from the research phase. Superseded by WRITEUP.md + STATUS.md but kept for the trail. | historical |
| `ref/` | git-ignored. SysDVR clone, Atmosphère tree (for the mitm build), switchbrew wikitext. | not committed |

## Building

Everything Switch-side builds in the `devkitpro/devkita64` Docker image — no
local toolchain needed.

```bash
# recon / stream-oc: plain libnx sysmodules
cd tier4/recon && docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkita64:latest make

# applet-mitm: libstratosphere module, built inside an Atmosphère tree checkout
#   (build.sh clones/uses ref/Atmosphere, applies patch_libstrat.py, builds,
#    copies the .nsp back). First build recompiles libstratosphere (~15 min).
cd tier4/applet-mitm && bash build.sh

# PC receiver: FFmpeg + SDL2 + libusb
cd switch-stream/receiver && cmake -B build && cmake --build build -j
```

## Installing a sysmodule

```
sdmc:/atmosphere/contents/<TITLE_ID>/exefs.nsp        <- the built .nsp, renamed
sdmc:/atmosphere/contents/<TITLE_ID>/flags/boot2.flag <- empty file
```

Title IDs: `tier4-recon` = `0100000000000C00`, `stream-oc` = `0100000000000C10`,
`applet-mitm` = `0100000000000C20`.

Recovery from a bad module: boot holding **Volume Up** (Atmosphère skips
`contents` sysmodules), or delete the folder from the SD card on a PC. Nothing
here touches NAND; a Hekate NAND backup covers the worst case regardless.

## Status: research milestone, not a finished tool

The mitm path has proven every access question needed for native-resolution
capture. What remains — driving the VIC and NVENC via host1x channel submission,
and wiring the transport — is substantial implementation work but contains no
open unknowns. See STATUS.md.
