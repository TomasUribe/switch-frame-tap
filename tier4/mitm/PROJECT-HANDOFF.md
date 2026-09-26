# switch-frame-tap: project handoff (after M89)

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

**Current build: M89, run on hardware (Run P, 2026-09-25).** Live mode: the
module streams native 1280x720 H.264 (IDR + P) over USB whenever the PC
viewer is reading and a game is presenting, and reattaches by itself when the
viewer, the game or the game title changes. Tested with MK8D (60 fps) and
BOTW (30 fps): 0 lost, 0 undecodable, 0 stale and 0 torn frames in 6965;
~22 ms console + ~24 ms PC latency. The user plays from the PC window. The
README's quick start is the install procedure; `STATUS.md` has every run.

---

## 1. The goal and where it stands

**Goal: 1080p at 60 fps** - the game's native docked output, streamed to a
PC. **The handheld half is done** (native 720p60 over USB). What remains for
the goal: a 1080p NVENC setup and a network transport for docked play
(questions 3 and 4 below); bitrate tuning and audio alongside.

**The live pipeline (M86-M89)**, entered from `TryDebugCapture` when `live` is
armed (`RunLive`):

```
wait for a viewer (UsbViewerPresent: an empty SFTR header taken within 300 ms)
  and a game (pm:dmnt application pid, and queueBuffer moving)
  v
snapshot the game's geometry (setPreallocatedBuffer: slots, size, format)
  v
DebugActiveProcess -> FindSwapchainBySize(the game's real total size)
  -> drain + ContinueDebugEvent -> DebugPumpStart (applet_mitm_dbgpump.cpp)
  v
TryNvencStream(live):
  per present: snapshot slot + acquire fence (seqlock g_queue_seq),
  wait for the fence (nvhost-ctrl SYNCPT_READ), ReadDebugProcessMemory the slot
  (MK8/BOTW handheld: the 1280x720 corner only), VIC -> BT.709 NV12 16-row
  block-linear, NVENC IDR every nvgop frames else P (grc's jobs, refs and
  MEPRED ping-pong), SFTR header (flags: H.264, IDR; stride = console age us)
  + payload over usb:ds, double-buffered
  v
until the viewer goes (USB timeout, cancelled), the game goes (read fails /
pump sees ExitProcess), or NVENC stalls (parks for the boot)
  -> DebugPumpStop, CloseHandle, back to waiting
```

Live mode deliberately skips the old probe setup's `FROM_ID` import of the
game's buffer and its aruid adoption (M86b: they kept an exited game's
resources alive and the next launch hung), and the vi:m indirect-layer probe.

## 2. Proven vs inferred

| claim | status | evidence |
|---|---|---|
| `vi:u` mitm is invisible to the game at 60 fps | **proven** | M20s-M67, sustained |
| Debug-SVC capture reads a presented 1080p slot in ~5-7 ms, game running | **proven** | M56, M67 (60 s stream), M82 Run H |
| VIC blit, scale and block-linear input are byte-exact | **proven** | M43-M45 (0.00 error on an unscaled crop) |
| VIC output block height is log2 GOBs (1 = 16 rows, 2 = 32 rows) | **proven** | M82 Run H: the VIC's bytes are smooth only at h=2 when 2 was set |
| VIC colour-matrix arithmetic (the law in `tools/vic_csc.py`) | **proven on the measured points** | Run H: 528/528 values exact |
| The M83 BT.709 matrix produces real BT.709 | **proven** (luma; chroma via the probe) | Runs I and J: Y error 0.98 steps against a float conversion of the game's pixels; `bt709` probe 200,40,40 -> 80/112/199 |
| NVENC encodes grc's IDR job from our channel | **proven** | Runs F, G, H (129 jobs, 0 ucode errors) |
| NVENC reads `block_height` 2 as 16-row blocks | **proven** | Run H: decode = VIC bytes read at h=1, 42-48 dB; every other layout <= 9.5 dB |
| M83's fix (VIC writes h=1) makes the decode match | **proven** | Runs I and J: 43.7-49.1 dB against the VIC's planes |
| `error_status` 2 is routine and meaningless for us | **proven** (as a pattern) | set on all 129 of our jobs and on grc's own frames; its meaning is unknown |
| Constant QP (RCMODE 0) works; QP comes from setup byte 0x73 (I) / 0x71 (P) | **proven** for IDR | Run G c, Run H (QP 16/20/24 reported as avgQP) |
| The whole path fits 60 fps at 720p IDR-only | **proven without USB** | Run H: 12.7 ms average work, 60.5 fps |
| USB 2.0 carries ~37 MB/s from this module | **proven** | M70 sweep |
| The game keeps running while we stay attached | **proven** with the M84 pump | Run J: 59.2-60.0 fps presented in every window, 2 thread events continued (63 us hold) |
| USB carries the H.264 stream at 60 fps | **proven** | Run J: 3600/3600 frames, 59.9 fps at the PC, 0 lost; 98 Mbps IDR-only at QP 20 |
| P frames from grc's P setup and P job decode without drift | **proven** | Run K: stream frame 59 P after its IDR at 42.0 dB against the console's picture; nvp 30/30 at 42.0 dB. (Run J's 19.2 dB was the probe's off-by-one.) |
| Live mode survives viewer close, game exit, relaunch and a title switch | **proven** | Runs L-P: up to 4 sessions per boot; M86b fixed the relaunch hang |
| Games beyond MK8D stream | **proven for BOTW** | Run N: vi:u command 1 forwarded verbatim, 2 slots, correct colours |
| The capture reads only finished frames | **proven** | Run P: `sft_tool artifacts` 0 stale / 0 torn in 6965 (Run N: 29 / 22 per 3000) |
| Latency | **measured** | console (present -> sent) ~22 ms incl. the fence wait; PC (arrival -> on screen, 2 decode threads) ~24 ms |
| The PC decodes the stream in real time | **proven** on the user's 20-core PC: 12.3 ms/frame IDR-only on 1 thread | raw-view defaults to 2 decode threads (one frame of delay); one per core added ~250 ms |
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
  - **live mode** (`RunLive`, `FindSwapchainBySize`) and the per-game
    geometry (`CaptureGameSurface`, `NvfSession::SetGeometry`);
  - the grc observer (`TryGrcScan`) and the older NVENC/NVJPG probes.
- `source/applet_mitm_clk.*`: engine clocks through `mm:u` and `clkrst`
  (`ClockEnsure`).
- `source/applet_mitm_dbgpump.*`: the debug event pump (M84) that keeps an
  attached game running.
- `source/applet_mitm_service.*`: the `vi:u` mitm (commands 0 and 1), the
  binder intercept, and the queueBuffer slot/fence publication (seqlock).
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
| `sft_tool.py` | recorded streams (`raw-view --record`): `stats` (gaps, IDR/P sizes, console latency), `last` (the drift test against the console's picture), `artifacts` (stale and torn frames), `head` (cut a sample) |
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

**The live arm file:** `vic exec dbg usb nvgop=60 live wait=20`.

**Arm-file flags that matter now:**

| flag | does | needs |
|---|---|---|
| `vic` | brings up the VIC worker at `wait=` seconds | - |
| `exec` | maps the VIC buffers and runs the regression jobs (without it no VIC job runs) | `vic` |
| `dbg` | debug-attaches to the game and finds its swapchain | `vic` |
| `usb` | brings up the USB device (`1209:5f1e`) at boot | - |
| `csc` | the 12 VIC colour-matrix probes -> `vic-csc.bin` | `vic exec` |
| `nvframe[=N]` | frame 0 at 3 QPs + an N-frame timed loop, all saved (`nvframe-*.bin`) | `vic exec dbg` |
| `live` | **the product mode:** stream whenever a viewer and a game are there, for the whole boot (implies `nvstream`) | `vic exec dbg usb` |
| `nvgop=N` | an IDR every N frames, P frames between (0 = IDR-only); the live arm file uses 60 | `nvstream` or `live` |
| `nvstream[=N]` `nvqp=Q` | the one-shot H.264 stream: N frames (default 3600), QP Q (default 20) | `vic exec dbg usb` |
| `nvp[=N]` | IDR + (N-1) P frames saved (`nvp-*.bin`); opt-in, runs last | `vic exec dbg` |
| `grcscan` | read-only observer of grc's NVENC jobs | `vic` |
| `wait=S` | when the VIC worker starts (default 120 s; live uses 20) | - |
| **`grc`** | **the grc IPC interceptor: known dangerous. Never arm it.** | - |

## 5. Open questions

1. **Bitrate.** P frames are ~60% of an IDR in a fast race at QP 20 (Run K:
   IDR 196 KB, P 117 KB). grc's own GOP is 15 and its P frames are small;
   suspects: the zeroed first MEPRED input, ME settings in the setup, QP[P]
   equal to QP[I]. ~40-55 Mbps fits USB 2.0 easily, so this matters most for
   a network transport.
2. **Stutters.** A handful of frames per minute take 50-200 ms (reads,
   VIC, NVENC all slow at once, around loading). System-wide contention;
   NVENC is shared with grc's background recording. Not yet attributed.
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
   the port. 1080p60 with P frames suggests Wi-Fi or a LAN adapter, and
   `switch-stream/` has a TCP receiver to start from.
5. **The VIC's completion check can be fooled.** It shares syncpoint 12 with
   the compositor, so a fence can fire before our job finished. No artifact
   has been attributed to it since M89 (0 torn in 6965), but a marker the
   VIC writes last would make it certain.
6. **Other games.** The geometry path handles any single-object, block-linear
   RGBA swapchain up to 1080p; games with one nvmap object per buffer, other
   formats or other sizes are untested. Only A8B8G8R8 colours are verified.
7. **Cheats and other debuggers.** Live mode holds a debug handle on the game
   for the whole session; dmnt's cheat engine presumably cannot attach at
   the same time. Untested.
8. **Audio** has not been touched.
9. **`error_status` 2.** Its meaning is unknown and not needed.

## 6. Roadmap

1. **Done:** handheld native 720p60 H.264 over USB 2.0, live mode, MK8D and
   BOTW, frame-exact capture, measured latency (M83-M89).
2. **Bitrate tuning:** better P frames (question 1), then QP against quality.
3. **Audio.**
4. **Docked 1080p60 over the network** (questions 3 and 4).
5. **Robustness and reach:** more games, a Windows viewer, stutter
   attribution, the VIC completion marker.

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

- **An attached debugger stops the game on every thread start/exit** until
  it continues (M83 Run I froze at a loading screen). Keep the pump running
  whenever a debug handle is held.
- **Never hold the game's graphics resources past its exit.** `FROM_ID` on its
  buffer and `SetAruidWithoutCheck(game aruid)` kept an exited game alive in
  nvservices; the relaunch hung on a black screen (M86 Run L, forced
  power-off). Live mode uses only its own buffers.
- **queueBuffer is not "frame finished".** Wait for its acquire fence, and
  take the slot and fence as one snapshot (M88/M89).
- **Stream recordings are large** (~0.5-1 GB a minute). Record only what will
  be analysed; keep `sft_tool head` samples, not whole files.

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
