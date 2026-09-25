# switch-frame-tap: project handoff (after M83)

This is for whoever continues the project cold, most likely a local session
with the SD card and the console at hand. It covers:

- what the project is trying to do;
- what works and how sure we are;
- how to build, test and run it;
- what is still open;
- the roadmap;
- the traps that already cost hardware cycles.

`tier4/mitm/STATUS.md` has the full evidence behind each item, newest
milestone first. This page is the map.

**Current build: M83. It is built and every PC-side check passes; it has not
been run on hardware yet.** The next hardware run is `HANDOFF-M83-RUNI.md`.

---

## 1. The goal and where it stands

**Goal:** capture the Switch's display at native resolution and 60 fps and
stream it to a PC, over USB or Wi-Fi.

**The pipeline as built in M83** (handheld, 1280x720 = native):

```
game (MK8D) presents a swapchain slot
  | vi:u mitm: binder queueBuffer tells us WHICH slot           (M20s-M30s, proven)
  v
svcReadDebugProcessMemory of that slot into our heap            (M56, proven, ~1.5 GB/s)
  | handheld: only the 1280x720 corner, 3.9 MB                  (M83, new)
  v
VIC: RGBA block-linear -> NV12, BT.709 matrix,                  (layout M82/M83, matrix M83)
     16-row block-linear, into an arena shared with NVENC
  v
NVENC: H.264 IDR, constant QP, grc's own setup and job          (M80-M82 proven; input fix M83)
  v
usb:ds bulk: SFTR header + SPS/PPS + slice, double-buffered     (transport M57-M71 proven; H.264 framing M83)
  v
PC: tools/raw-recv/raw-view: libusb + libavcodec + SDL2         (M83, tested on the PC)
```

**Proven on hardware:** every stage separately, and capture -> VIC -> NVENC
together at 60.5 fps (M82 Run H). Run H's encode had the wrong block height,
so the decoded picture was scrambled. That is diagnosed and fixed in M83.

**Not yet on hardware:**

- the fixed layout;
- the BT.709 matrix;
- USB carrying H.264 end to end;
- P frames.

Run I tests all four, in that order.

## 2. Proven vs inferred

| claim | status | evidence |
|---|---|---|
| `vi:u` mitm is invisible to the game at 60 fps | **proven** | M20s-M67, sustained |
| Debug-SVC capture reads a presented 1080p slot in ~5-7 ms, game running | **proven** | M56, M67 (60 s stream), M82 Run H |
| VIC blit, scale and block-linear input are byte-exact | **proven** | M43-M45 (0.00 error on an unscaled crop) |
| VIC output block height is log2 GOBs (1 = 16 rows, 2 = 32 rows) | **proven** | M82 Run H: the VIC's bytes are smooth only at h=2 when 2 was set |
| VIC colour-matrix arithmetic (the law in `tools/vic_csc.py`) | **proven on the measured points** | Run H: 528/528 values exact |
| The M83 BT.709 matrix produces real BT.709 | **inferred** from the law (within 0.5 steps) | Run I's `bt709` probe and nvframe source check measure it |
| NVENC encodes grc's IDR job from our channel | **proven** | Runs F, G, H (129 jobs, 0 ucode errors) |
| NVENC reads `block_height` 2 as 16-row blocks | **proven** | Run H: decode = VIC bytes read at h=1, 42-48 dB; every other layout <= 9.5 dB |
| M83's fix (VIC writes h=1) makes the decode match | **inferred** (same bytes read the same way) | Run I nvframe: PSNR > 30 dB expected |
| `error_status` 2 is routine and meaningless for us | **proven** (as a pattern) | set on all 129 of our jobs and on grc's own frames; its meaning is unknown |
| Constant QP (RCMODE 0) works; QP comes from setup byte 0x73 (I) / 0x71 (P) | **proven** for IDR | Run G c, Run H (QP 16/20/24 reported as avgQP) |
| The whole path fits 60 fps at 720p IDR-only | **proven without USB** | Run H: 12.7 ms average work, 60.5 fps |
| USB 2.0 carries ~37 MB/s from this module | **proven** | M70 sweep |
| USB carries the H.264 stream at 60 fps (~20 MB/s at QP 20) | **inferred** | Run I nvstream |
| P frames from grc's P setup and P job decode without drift | **inferred**: grc's setups and jobs are consistent (Run E) | Run I nvp |
| The PC can decode IDR-only 720p60 at ~165 Mbps | **measured on a 4-vCPU cloud box**: 32 ms/frame on one thread, 10 ms with frame threads | raw-view defaults to frame threads |
| SuperSpeed | **not working**: descriptors and BOS accepted, the link trains at High speed | M71; the cable is the untested variable |

## 3. Repository map (the parts that matter)

**`tier4/applet-mitm/`** is the module (Atmosphère sysmodule, TID
`0100000000000C20`).

- `source/applet_mitm_main.cpp`:
  - boot and arm-file parsing (every flag is logged on the `ARMED FLAGS:`
    line);
  - the USB device side (`usb:ds`, `UsbSendBuffer` / `UsbPostAsync` /
    `UsbWaitAsync`);
  - the heartbeat thread and the `vi:u` mitm registration.
- `source/applet_mitm_nv.cpp` is everything nvdrv:
  - the VIC (`RunOneJob`, `FillBlitConfig`, `FillOutputConfig`) and the CSC
    probes (`TryCscSweep`);
  - the debug capture (`TryDebugCapture`), which hosts `nvframe`, `nvstream`
    and `nvp` (`NvfSession`, `NvfEncode`, `TryNvencRealFrames`,
    `TryNvencStream`, `TryNvencPFrames`);
  - the raw M67 stream (`StreamFrames`);
  - the grc observer (`TryGrcScan`) and the older NVENC/NVJPG probes.
- `source/applet_mitm_clk.*`: engine clocks through `mm:u` and `clkrst`
  (`ClockEnsure`).
- `source/applet_mitm_armfile.hpp`: the arm-file tokenizer (whole tokens).
  Host-tested by `test/armfile_test.cpp`, which carries every run's exact arm
  file.
- **Generated headers.** Never edit them; regenerate them.
  - `nvenc_grc_idr.h`, `nvenc_grc_p.h` and `nvenc_grc_hdrs.h` come from
    `tools/nvenc_replay.py gen`.
  - `vic_csc_bt709.h` comes from `tools/vic_csc.py gen`.
  - `nvjpg_dec_control.h` comes from `tools/nvjpg_dec_control.py gen`.
- **Vendored headers:**
  - `nvenc_drv_h264.h`, NVIDIA's MIT NVENC driver interface;
  - `nvjpg_drv.h`;
  - `vic40_config.hpp`, verified field for field against libdrm.
- `build.sh` builds inside `ref/Atmosphere` with the `devkitpro/devkita64`
  docker image (see `ref/README.md`).

**`tools/`** is the PC side:

| tool | what |
|---|---|
| `run_pc_tests.sh` | **every check that needs no console**, in one command |
| `nvenc_replay.py` | grc setup parsing, SPS/PPS writer (byte-exact against libx264), header generation, M80/M81 checks |
| `vic_csc.py` | the VIC matrix law, `verify`, `design`, `gen` |
| `nvframe_check.py` | real-frame run checks: deswizzle, layout scoring, decode, PSNR, "which layout did NVENC read", colour check |
| `nvp_check.py` | P-frame GOP decode and the drift test |
| `sft_stream_test.py` | builds the console's USB stream byte for byte and replays it through raw-view |
| `raw-recv/` | `raw-view` (live viewer: raw, packed-4:2:0 and H.264), `raw-recv` (dumper), Makefile, udev rule |
| `nvrec.py`, `nvsetup-dump.c` | decode the grc observer's records and NVENC setups |
| `deswizzle.py`, `nv12topng.py`, `compare_vic.py` | older frame tools |

**`tier4/mitm/`**:

- `STATUS.md`: analysis per milestone, newest first.
- `WRITEUP.md`: the long-form why.
- `HANDOFF-M*-RUN*.md`: one per hardware run, the instructions.
- `RESULT-M*-RUN*.md`: one per hardware run, the verbatim report.

**`logs/`** holds each run's log, files and PC check output, committed as
evidence.

## 4. Build, test, install

```
git clone --recursive https://github.com/Atmosphere-NX/Atmosphere ref/Atmosphere   # once
git -C ref/Atmosphere checkout 1.11.2                                              # match the console
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh     # -> tier4/applet-mitm/applet-mitm.nsp, "OK"
bash tools/run_pc_tests.sh                               # everything that needs no console
make -C tools/raw-recv                                   # the viewer
```

PC dependencies:

- python3 with `av` (PyAV), `numpy` and `pillow`;
- `libusb-1.0-0-dev`, `libsdl2-dev`, `libavcodec-dev` and `libavutil-dev`;
- the udev rule in `tools/raw-recv/`.

**Install:** card in a reader, never MTP.

- `atmosphere/contents/0100000000000C20/exefs.nsp` is the module.
- `.../flags/boot2.flag` must be an empty file.
- **Delete `.../mitm.lst` if it exists** (see the traps).
- `sdmc:/applet-mitm.armed` is the arm file: whitespace-separated tokens,
  ASCII, no BOM.
- The module writes `sdmc:/applet-mitm.log`. It truncates the log on every
  boot, so copy it off before the next boot.

**Arm-file flags that matter now:**

| flag | does | needs |
|---|---|---|
| `vic` | brings up the VIC worker at `wait=` seconds | - |
| `exec` | maps the VIC buffers and runs the regression jobs (without it no VIC job runs) | `vic` |
| `dbg` | debug-attaches to the game and finds its swapchain | `vic` |
| `usb` | brings up the USB device (`1209:5f1e`) at boot | - |
| `csc` | the 12 VIC colour-matrix probes -> `vic-csc.bin` | `vic exec` |
| `nvframe[=N]` | frame 0 at 3 QPs + an N-frame timed loop, all saved (`nvframe-*.bin`) | `vic exec dbg` |
| `nvstream[=N]` `nvqp=Q` | **the H.264 stream**, N frames (default 3600), QP Q (default 20) | `vic exec dbg usb` |
| `nvp[=N]` | IDR + (N-1) P frames saved (`nvp-*.bin`); opt-in, runs last | `vic exec dbg` |
| `grcscan` | read-only observer of grc's NVENC jobs | `vic` |
| `wait=S` | when the VIC worker starts (default 120 s) | - |
| **`grc`** | **the grc IPC interceptor: known dangerous. Never arm it.** | - |

## 5. Open questions

1. **Run I's answers:**
   - the matrix on hardware;
   - the layout fix;
   - the stream's real fps and loss;
   - whether P frames decode without drift.
2. **P frames in the stream.** If nvp passes, `nvstream` should send IDR + P
   (a GOP of 30-60; grc uses 15). Frame N's `OUT_REF_PIC` becomes N+1's
   `IN_REF_PIC0`, and the MEPRED buffers swap. Per frame only frame_num (u16
   at 0x17C) and POC (0x17E, = 2 x frame_num) change, modulo 256. Two things
   are unknown:
   - the MEPRED buffer size (grc's are 128 KB apart; nvp gives 256 KB);
   - whether the first P frame's zeroed MEPRED input costs quality.
3. **Docked 1080p** needs a 1080p NVENC setup. grc's setup is 1280x720
   (Switch video clips are 720p), so grc does not hand us one. The fields
   that scale with size are:
   - width/height at 0x04;
   - the SPS size fields;
   - `hist_buf_size` (0x19C) and `bitstream_buf_size` (0x1A0);
   - the slice control's `max_slice_size` (MBs);
   - the surface configs.

   Build it on the PC with `nvsetup-dump` side by side, then test it with
   nvframe first.
4. **Docked transport.** USB device mode is impossible docked: the dock owns
   the port. 1080p60 with P frames at ~30-60 Mbps suggests Wi-Fi or a LAN
   adapter, and `switch-stream/` has a TCP receiver to start from.
5. **The VIC's completion check can be fooled.** It shares syncpoint 12 with
   the compositor, so a fence can fire before our job finished. At worst
   NVENC encodes a half-written frame (a visible tear). A marker the VIC
   writes last, or a VIC job the NVENC job waits on, would fix it.
6. **Game-agnostic capture.** The swapchain finder expects an exact 3 x
   8,847,360 B device-shared region (1920x1080 RGBA, 128-row blocks). That is
   MK8D's; other games may differ. The binder parcels carry each game's real
   geometry (`g_game_surface`).
7. **Latency** has not been measured end to end. The obvious method: a
   frame-number overlay or a light sensor.
8. **Audio** has not been touched.
9. **`error_status` 2.** Its meaning is unknown and not needed.

## 6. Roadmap to native resolution at 60 fps over USB 2.0

1. **Run I (M83).** Confirm the fix, the colour and the stream. Success
   means a live, compressed, native-720p handheld stream: the project's goal
   for handheld mode.
2. **P frames in the stream.** Roughly a quarter of IDR-only's bitrate, which
   frees the link and the PC's decoder. Add a periodic IDR (every 60 frames)
   for recovery.
3. **Tuning.** QP against the link (`nvqp`), measure latency, and consider
   `--low-latency` decoding on a fast PC.
4. **Docked 1080p60.** The 1080p setup (question 3) and a network transport
   (question 4). Raw 1080p60 is 186.6 MB/s, far past USB 2.0 (and USB is not
   available docked anyway), so this path is compressed by necessity.
5. **Robustness.** Reconnect on the receiver, a game-agnostic swapchain
   finder, and the VIC completion marker.

## 7. The SuperSpeed lossless option (later, handheld only)

Once USB 3.0 trains, handheld 720p60 could go uncompressed:

- NV12 through the VIC is 83 MB/s with no encoder and no encode latency.
- The BT.709 matrix is now solved, so that means real YUV rather than M67's
  packed RGB.
- Device side: the descriptors and BOS are accepted and the device matches
  haze exactly, but the link negotiates High speed.
- The untested variable is the cable: try a known USB 3.0 C-to-C or C-to-A
  cable first.
- `raw-view` already shows NV12-shaped payloads as packed 4:2:0. A real-YUV
  raw mode would be a small addition (an IYUV/NV12 texture with the BT.709
  conversion mode).

## 8. The traps (each cost at least one hardware cycle)

- **A zero address hangs the VIC**, and a hung VIC takes the compositor, and
  with it the console, down. `RunOneJob` refuses zero addresses; every new
  engine path must check its own pins (`NvfSession::Open` does).
- **Tearing down a channel with a stuck job wedges the compositor** (M72-M74:
  three forced power-offs). On a stall, leave the channel open, set
  `g_engine_wedged`, and do no more engine work that boot.
- **Substring arm-file matching.** `"grcscan"` contains `"grc"` and armed the
  dangerous interceptor (M75); `"sweep"` fed `sw=` (numbers). Now whole
  tokens only, and `test/armfile_test.cpp` must carry every run's exact arm
  file. Add yours before the run.
- **A stale `mitm.lst`.** With `atmosphere/contents/0100000000000C20/mitm.lst`
  on the card, boot2 declares a future `nvdrv:s` mitm, and every `nvdrv:s`
  client (vi included) waits for us to register. The module now registers a
  pass-through, but delete the file at every install anyway.
- **Unchecked and uncached writes.**
  - `fs::WriteFile` results were discarded once, and a dump from uncached
    nvmap memory silently failed (M41/M42, again in M77).
  - Stage engine output through cached memory and write with
    `WriteSdVerified` / `WriteEngineOutputToSd`. They read back, compare, and
    log an FNV-1a that the PC checks.
- **Logging inside a timed loop is the measurement.** M59's "119 ms VIC" was
  SD-flushing log calls (M63). Use `g_vic_quiet` in loops.
- **Core 3 is shared with the IPC thread that answers the game.** A busy loop
  froze the game (M60). Loops must sleep or block.
- **Syncpoint 12 belongs to the compositor too.** Rescue increments in a loop
  froze the game (M63b), and fences can fire early (question 5).
- **Static memory.**
  - Large `.bss` fataled other sysmodules; the heap lives in the Applet pool.
  - `handle_table_size` must be 512.
  - Nothing blocking may run on the binder thread.
- **Engine clocks.** NVJPG was simply unclocked, and a raised clock decays
  when nobody holds it (M76/M77). Ensure the clock right before the first
  submit, and re-check it in long runs.
- **Units differ between engines.** The VIC's block height is log2 GOBs;
  NVENC's value 2 is 16 rows (M82). Measure, do not assume.
- **Guessing packed fixed-point formats wastes runs.** The VIC matrix took
  three guesses (M64-M66); an 11-probe sweep in one run settled it. Probe
  with distinct, well-separated values.
- **The tools can be wrong too.** `parse_setup` read pic_control at the wrong
  offset for three milestones (fixed in M83). Cross-check against NVIDIA's
  struct by compiling it.
- **Early "findings" were artifacts** more than once (M59's VIC speed, M73's
  matrix theory). Write the evidence next to the claim, label the
  confidence, and let the next run decide.

## 9. The working method that has held up

- **One build, one hardware run, one verbatim result file.** The run handoff
  says exactly what to copy. The person at the console reports facts and
  does not analyse. Analysis happens afterwards, in `STATUS.md`.
- **Every file the console writes is checked on the PC.** Its FNV-1a is
  compared with the console's own `sd(...)` log line.
- **Every PC tool has a selftest on synthetic data.** Where possible it also
  has a regression check on a real run's files. `tools/run_pc_tests.sh` runs
  them all.
- **New engine work runs last in a boot, riskiest last.** A failure then
  costs only itself.
