# applet-mitm — status & resume point

Console: Mariko, FW **22.5.0**, Atmosphère **1.11.2**. Module TID
`0100000000000C20`. 79 hardware test cycles (through M82 Run H). Current build: **M83** (not yet run).

**Picking this up cold?** Read [`PROJECT-HANDOFF.md`](PROJECT-HANDOFF.md) first: what works, what is
proven vs inferred, the roadmap, and the traps. `bash tools/run_pc_tests.sh` runs every check that needs
no console.

Read **[WRITEUP.md](WRITEUP.md)** first for the why. This file is the what-now.

---

# CURRENT STATE (read this first)

## What works, verified on hardware

1. **A transparent `vi:u` mitm** wrapping `GetDisplayService` →
   `IApplicationDisplayService` → `GetRelayService` → `IHOSBinderDriver`.
   Sustained **60.0 fps**, invisible to the game. Needs the
   `patch_libstrat.py` fix for non-domain mitm sub-object forwarding.
2. **Complete frame-pipeline visibility** — every `NvGraphicBuffer` parsed from
   the binder traffic; `queueBuffer` slot index decoded past Android's
   `writeInterfaceToken`.
3. **A full VIC pipeline in a sysmodule, byte-exact.** heap alloc →
   `svcSetMemoryAttribute(uncached)` → nvmap `CREATE`/`ALLOC` → `MAP_CMD_BUFFER`
   pin → host1x cmdbuf (`SETCL` + methods + `INCR_SYNCPT`) → `CHANNEL_SUBMIT` →
   syncpoint wait → cache invalidate → read back. A fill and a real blit both
   reproduce their expected output **byte for byte**. Output is `AV_PIX_FMT_ARGB`.
   NVENC (`/dev/nvhost-msenc`) is open.
4. **`nvdrv:t` gives the full `0xFFFFFFFF` permission mask** — every engine node
   except TSEC, and IOVAs outside the restricted window.
5. **The whole indirect-layer object graph is constructible from `vi:m`**:
   `CreateIndirectLayer` → `CreateIndirectProducerEndPoint` →
   `CreateIndirectConsumerEndPoint`, three undocumented ABIs guessed correctly
   by analogy with `viCreateManagedLayer`.
6. **NVJPG decodes from this process** (M77 Run C, file verified in M78 Run D),
   once its clock is requested through `mm:u` right before the submit.
7. **NVENC encodes H.264 from this process** (M80 Run F). grc's own IDR job,
   replayed from our msenc channel with our buffers, completes in 2.1 ms; the
   stream decodes on the PC to the input, and the reconstructed picture matches
   it exactly. `error_status` 2 on every frame is routine (M81 Run G: grc's own
   frames carry it too) and says nothing about the frame.
8. **NVENC's constant-QP mode works** (M81 Run G, variant c): RCMODE 0 takes
   the QP from the setup's I-frame QP, touches no rate-control state, and
   decodes correctly.
9. **Real game frames through VIC and NVENC at 60 fps** (M82 Run H): 120/120
   frames read out of the game, converted by the VIC and encoded, 60.5 fps
   against the game's 60.0, work 12.7 ms avg of the 16.7 ms budget, 0 errors.
   The encode was of the wrong pixel order (a layout mismatch, found and fixed
   in M83 - see below), but the engine path and its speed are proven.
10. **The VIC's colour matrix is understood** (M82 Run H): an exact model
   reproduces all 528 measured values; `tools/vic_csc.py`.
11. **Frames come from the game's memory, not from graphics** (M56-M67): the
   debug-SVC route (`svcDebugActiveProcess` + `ReadDebugProcessMemory`) reads
   the presented swapchain slot at ~1.5 GB/s, game running. This is the frame
   source everything above uses; the graphics routes below stay closed.

## What is blocked, and why — BOTH GRAPHICS ROUTES CLOSED

**No graphics API hands a sysmodule another process's frame.** The project gets
frames from the game's memory through the kernel debug SVCs instead (item 11
above). These are the routes that do not work:

| route | verdict |
|---|---|
| **Read the game's swapchain** (`FROM_ID` + `MAP_CMD_BUFFER`) | `phys=0`, silently. Survives `is_compr`, `MAP_CMD_BUFFER_EX`, relocs, the **full permission mask**, and the game's **exact aruid adopted before any `Open`**. At `Initialize` nvservices is handed `CUR_PROCESS_HANDLE` and maps client memory through *that*; the game's pages are in the game's process, so `FROM_ID` yields only a refcounted reference and the IOVA allocator never advances. **Structural.** |
| **`vi` indirect layers** (`GetIndirectLayerImageMap`) | `0x60A ams::sf::PreconditionViolation`. The PID descriptor, type-0x46 buffer and aruid are all *accepted* — the layer is simply **empty**. We can build layer + producer endpoint + consumer endpoint, but nothing attaches to the producer. Wiring an *application's* layer to an indirect layer is **AM's** job (`GetIndirectLayerConsumerHandle`, `PartialForegroundWithIndirectDisplay` — swkbd's inline keyboard is the only documented user), and a sysmodule cannot drive AM. |

Also ruled out along the way:
- **Relocs are inert** on Horizon — the command buffer is never patched, so
  addresses must be inlined from `MAP_CMD_BUFFER`.
- **The display controller has no readback** — `nvdisp-disp0` is `FLIP` /
  `SET_MODE` / `GET_WINDOW`; `nvdcutil` is DSI/EDID test plumbing. Grepping the
  entire nvdrv doc for `READBACK|CAPTURE|GET_FRAME|SCANOUT` returns nothing.
- **`caps` `CaptureRawImage` is `[1.0.0]`** — removed long before 22.5.0.

**This is very likely why SysDVR is stuck at 720p30 via `grc:d`.** It is not
that nobody tried; the platform does not let a sysmodule reach another
process's framebuffer.

## Hard-won constraints (do not relearn these)

- **Never take large static memory.** A 4 MB array fataled *another* sysmodule
  with `0x10801 LimitReached` at boot (M27), and 8 MB held from boot did it again
  (M50). `.bss` lives at ~1.45 MB, and the heap ceiling *was* **2 MB** for every
  run up to M53 — see the correction below for why it no longer is.
  **M54/M55 correction:** there are *two* gates, and they are not the same one.
  `pool_partition` picks the **physical pool** — now `1` (Applet).
  `application_type` picks the **resource-limit group** — now `2`
  (`ProgramInfoFlag_Applet`), and `LimitReached` is *that* gate, the one that
  killed `am`. M54 moved only the pool and I wrongly called that safe; it took
  **both**. With both on Applet the probe-time budget is **411,260 KB free**
  rather than 3,748 KB, so the 2 MB ceiling above is historical.
  **M56 demonstrated it:** 16 MB granted on the first rung, Applet `+16,384 KB`
  exactly, System flat to the KB, no fatal. The 2 MB ceiling is dead — but the
  memory is **not free**: a 16 MB heap makes the one-time VIC setup ~6x slower
  and every `SetMemoryAttribute`/`nvmapOwn` ~10x slower (5-8 ms -> 61-79 ms).
  Take what a frame needs, not what the pool allows, until that curve is
  understood.
- **`handle_table_size` must be 512**, not the default 16 — `ams_mitm` uses 512.
  16 exhausted mid-run and made `smGetService("vi:m")` return `0xD201
  OutOfHandles`, which reads exactly like a permission refusal and is not one.
- **Nothing blocking may run on the binder thread.** It blocks `queueBuffer`,
  which wedges `vi`, which freezes the console, which forces a power-off, which
  truncates the log. All engine work runs on a worker thread.
- **A zero address handed to the VIC hangs the engine**, and a hung VIC takes the
  compositor down with it. Every address is checked before submit.
- **Read the SD in a card reader**, not over MTP.
- **A device-shared region is not a surface.** Regions are merged by the kernel
  when state/permission/attribute match, so several nvmap objects appear as one
  block. Never assume a region's base is an allocation's base — find the
  allocation by content.
- **Debug SVCs need an NPDM `debug_flags` capability, not just the syscall bits.**
  Granting `svcDebugActiveProcess` in `syscalls` is necessary and not sufficient;
  `kern_svc_debug.cpp:38` also wants `force_debug`. M33 shipped without it.

## *** M83: the first H.264 stream - real frames, real YUV, over USB (built, not yet run) ***

### M82 Run H, read: evidence first

Facts in `RESULT-M82-RUNH.md`. All 13 files verify.

- 11/11 colour probes and 120/120 real-frame encodes completed, 0 errors.
- The loop ran at 60.5 fps against the game's 60.0.
- Per frame, the work averaged 12.7 ms (max 26.2 ms): read+flush 7.7, VIC 0.7,
  NVENC 4.3.
- IDR frames at QP 20 averaged 344 KB, which is 165 Mbps / 20.6 MB/s at 60 fps.
- The PC check said the decoded pictures did not match the VIC's planes
  (Y 9.5 dB).

**1. The mismatch is a block-height disagreement between the VIC and NVENC.**
*Confidence: high (measured).*

- **PSNR was identical at QP 16, 20 and 24** (9.5 / 9.9 / 11.7 dB). A
  quantisation problem would move with QP; this is spatial.
- **Reading the VIC's raw bytes every possible way** and comparing each
  reading with the decode (`tools/nvframe_check.py --layout 2 --colour
  passthrough`, output in `logs/m82-runH/recheck-m83.txt`):

| VIC bytes read as | QP 16 | QP 20 | QP 24 | last frame |
|---|---|---|---|---|
| block-linear h=1 (16-row blocks) | **47.9** | **45.1** | **42.0** | **44.4** |
| block-linear h=2 (what the VIC wrote) | 9.5 | 9.5 | 9.5 | 7.9 |
| h=0, h=3, h=4, pitch | <= 9.5 | <= 9.5 | <= 9.5 | <= 8.2 |

  Chroma agrees: U/V are 45.2 dB at h=1, QP 16. At h=1, PSNR falls with QP
  exactly as quantisation should.
- **The VIC's own output is smoothest read at h=2**, as configured. So the VIC
  wrote 32-row blocks and **NVENC read the same bytes as 16-row blocks**.
- **grc's `block_height` field value 2 means 16 rows to NVENC.** The VIC's
  `OutBlkHeight` is log2 GOBs, so M82 wrote 2 (32 rows) believing the two
  fields shared a unit. grc's chroma allocation, 368 rows (360 rounded to 16),
  was the clue all along.
- **M83 fix:** the VIC writes `OutBlkHeight` 1. The planes become luma 720
  rows and chroma 368.
- Whether NVENC's field counts GOBs or is log2 of something else is not
  settled. It does not matter: value 2 is 16 rows, as measured.

**2. `[nf:vic0] changed=0/65536 (untouched)` was a diagnostic bug, not an
engine fault.** *Confidence: high (code reading).* RunOneJob's
post-job check always inspected `g_vic_dst_buf`, the VIC's own scratch
output. nvframe's VIC job wrote into the NVENC arena instead, and
`nvframe-0-y.bin` holds its real output. M83 gives `JobCtx` an
`out_is_dst_buf` flag. A caller-owned output is now logged as such and not
inspected.

**3. The VIC colour matrix, solved.** *Confidence: high for the law (exact
on every measured value); the designed BT.709 matrix is checked through the
law, and Run I measures it on the card.*

What each probe showed:

| probe | result |
|---|---|
| `none`, `out_k8` (K=256, shift 0), `out_k16_s8` (K=65536, shift 8) | identical pass-through: Y = B, U = R, V = G at gain 1 |
| K >= 4096 at shift 0 (`out_k12`, `k16`, `k19`, `dense`, `slot_k16`) | saturate at 255 |
| `out_off` (offsets 256/512/768 alone) | 0 |
| `out_neg` (row 1 = -65536) | **V** = 0: row 1 is the Cr plane |
| `m64_bt601` | Y 4, U/V 32: M64's constant, reproduced |

`tools/vic_csc.py` implements the law below. `verify` shows it reproduces all
528 values (11 probes x 16 patches x 3 planes) with zero error. The second-best
variant (rounding instead of truncation) misses 5.

```
inputs  in = (B, G, R)       the card's bytes R,G,B,A, declared A8R8G8B8 (as the game)
        in10 = in8 << 2
row i   acc = (sum_j c[i][j] * in10[j]) >> matrix_r_shift  +  c[i][3]
        out10 = clamp(acc >> 8, 0, 1023);  out8 = out10 >> 2
rows -> planes (Y, Cr, Cb)
```

So a coefficient has 8 fraction bits (256 = 1.0), `matrix_r_shift` divides
the products only, and an offset is in 1/256ths of a 10-bit step.

**What M64-M66 got wrong, all at once:**

- **Scale.** Coefficients were chosen for "shift 8 means /256" on top of the
  8 fraction bits the hardware already has. 66/129/25 at shift 8 are worth
  66/65536: about 0, hence the constant.
- **Offsets.** They were in 8-bit units. 16 needs 16 x 4 x 256 = 16384; M64's
  4096 is exactly the observed 4.
- **Order.** Rows were Y, Cb, Cr and columns R, G, B. The hardware, for our
  declared format, is rows Y, Cr, Cb and columns B, G, R.
- M66's slot matrix obeys the same law: `slot_k16` saturates like `out_k16`.

**The M83 matrix** (`vic_csc.py design` / `gen` -> `vic_csc_bt709.h`) is
BT.709 limited range at shift 8, for 16 fraction bits. The law was only
measured at shifts 0 and 8, and 8 it is. Rows (Y, Cr, Cb) x columns (B, G, R,
offset):

```
Y   4064  40254  11966   16896      (16 x 1024 + 512 rounding)
Cr -2639 -26145  28784  131584      (128 x 1024 + 512)
Cb 28785 -22189  -6596  131584
```

Through the law, over the whole RGB cube, it lands within 0.50 steps of the
float BT.709 conversion (rounding only). Chroma rows sum to zero, so grey
stays neutral.

**4. `error_status` 2, across Runs F-H.** *Confidence: high that it carries no
per-frame information for us; low on what it actually is.*

- It was set on every job we ran (1 + 5 + 3 + 120 = 129) and on all 3 of
  grc's own live frames. That held whatever the rate control (18, 0), HRD,
  two-pass setting, frame size (0.5 to 385 KB), content or QP.
- What all of those share: the firmware, grc's setup, and the `0x1100` bits
  of SET_CONTROL_PARAMS (FORCE_OUT_PIC, GPTIMER_ON). Those are the
  candidates if anyone ever cares.
- A job counts as good when:
  - the engine wrote our picture index back;
  - `ucode_error_status` is 0;
  - the bit count is non-zero.

**5. The budget, measured.**

- 12.7 ms of work in a 16.7 ms frame.
- The biggest item is the 7.7 ms slot read+flush. In handheld only 3.9 of
  the slot's 8.8 MB are picture (the top-left 1280x720: 6 block-rows x 80 of
  120 GOB columns, contiguous per block-row). M83 reads just that, so ~3.5 ms
  is expected.
- IDR-only at QP 20 is 165 Mbps. That is inside the ~290 Mbps link M70
  measured, but not by much, and real YUV should shrink it. In Run H the
  chroma planes carried R and G at full energy; real Cb/Cr carry far less.

**6. Also found and fixed while reading Run H.** `tools/nvenc_replay.py`'s
`parse_setup` read the pic_control bitfield word at 0xC8. That is the start of
a reference-list array. The word is at 0x1A8 (offsets from NVIDIA's struct,
compiled):

| | IDR | P |
|---|---|---|
| byte 0xC8 (old read) | 0xFE (pic_type 3, by coincidence) | 0x00 |
| word 0x1A8 (real) | 0x9C: type 3, ref 1 | 0x90: type 0, **ref 1** |

`ref_pic_flag` was therefore misread for P frames. They are reference frames,
so grc's reference ping-pong is coherent. That matters for P frames (below).

### What M83 builds

All PC-side, all tested before any hardware run: `bash tools/run_pc_tests.sh`.

**Console (`applet-mitm M83`):**

- **The VIC -> NVENC input is fixed.** `OutBlkHeight` 1, planes 720 / 368
  rows. The arena is mapped into both channels as in M82.
- **Real YUV.** Every nvframe / nvstream / nvp VIC job runs the BT.709
  matrix. The csc sweep gains a 12th probe, `bt709`: the production matrix
  measured on the card.
- **Handheld reads only the picture.** After the first full read finds
  handheld content, later frames read 3.9 MB (6 x 655,360 B) instead of
  8.8 MB.
- **nvframe, re-run as the check of the fixes:**
  - frame 0 at QP 16/20/24 plus the last of a 120-frame loop, as in M82;
  - new: `nvframe-0-src.bin`, the game's own pixels for the picture's top
    128 rows, as read, so the PC checks the colour conversion on real content.
- **nvstream, the stream:** game frame -> VIC -> NVENC IDR -> USB bulk.
  - **Framing.** Each frame is one SFTR packet: the 32-byte header (version
    2, flags 2 = H.264, kind = frame number) in its own transfer, then SPS +
    PPS + the IDR slice, posted asynchronously. Frame N's transfer overlaps
    frame N+1's capture and encode, which is M67's double-buffering.
  - **SPS/PPS.** They are built from grc's setup with a VUI (BT.709 limited,
    60 fps, no reordering) and generated into `nvenc_grc_hdrs.h`. Every
    frame is self-contained, so the receiver can join anywhere.
  - **idr_pic_id** alternates 0/1, as H.264 requires of back-to-back IDRs.
    grc's own IDR setups differ exactly there.
  - **Per-frame checks.** A bad encode is skipped and counted, not sent.
    Every 600 frames the NVENC clock is read and re-ensured if it fell
    below 400 MHz, and a progress line is logged.
  - **Stopping.** The stream stops cleanly if the host stops reading.
  - **Arm file.** `nvstream` (3600 frames = 60 s by default, `nvstream=N`),
    `nvqp=N` (default 20), plus `usb`.
- **nvp, the P-frame probe:** opt-in, and runs last.
  - It encodes 1 IDR + 29 P from real frames with grc's own P setup
    (`nvenc_grc_p.h`, generated from Run E's setup_2). The generator checks
    that it differs from the IDR setup in exactly the 23 bytes grc's P setups
    do.
  - Each P job adds grc's P-only methods (IN/OUT_MEPRED, IN_REF_PIC0) in
    grc's order.
  - References and MEPRED buffers ping-pong as grc's do.
  - Per frame, only frame_num / POC change, as in grc's setups 2, 6, 5.
  - It saves the GOP and the last frame's VIC picture. The PC decodes the
    GOP and compares the last P frame with the VIC's picture: the drift test.
- **Safety guards kept everywhere:**
  - zero-address refusal;
  - a fresh msenc channel per probe;
  - a stalled channel left open, with engine work stopped for the boot;
  - the clock ensured before the first submit;
  - every SD write verified by read-back and FNV-1a;
  - the grc IPC interceptor is never armed.

**PC:**

- **`tools/vic_csc.py`**: the law (`law`), `verify`, `design` (BT.709/601),
  and `gen`. Its selftest reproduces M64 and checks Run H.
- **`tools/nvframe_check.py`**:
  - `--layout` / `--colour`, defaulting to M83's h=1 / BT.709;
  - a diagnosis of which layout NVENC read, when the decode does not match;
  - the real-content colour check against `nvframe-0-src.bin`.
- **`tools/nvp_check.py`**: GOP decode and the drift test.
- **`tools/raw-recv/raw-view`**:
  - an H.264 mode through libavcodec, with BT.709 display;
  - `--record` / `--file` for replay and `--h264` for an elementary stream;
  - `--headless` for tests, and frame threads by default (`--low-latency`
    for one thread);
  - a Makefile. The stale prebuilt binary is removed from git.
- **`tools/sft_stream_test.py`** packs a stream byte for byte the way the
  console does, from the console's own SPS/PPS array and real NVENC slices.
  It replays the stream through raw-view and re-decodes what raw-view wrote.
  **The whole receive path is tested; only the USB wire is not.**
- **`tools/run_pc_tests.sh`**: every check above in one command.

### Run I - `vic exec dbg usb csc nvframe nvstream nvp wait=60`, handheld, `raw-view` running on the PC

| reading | meaning |
|---|---|
| csc `bt709` probe: "REAL BT709 YUV", worst <= 1 step | the matrix works on hardware as designed |
| nvframe: layout "(as configured)" h=1, PSNR > 30 dB at every QP, "THE VIC WRITES REAL BT709 YUV" | **the input fix and the colour are confirmed**: real frames encode correctly |
| nvstream: raw-view shows the game live; the log's fps near 60, `%u presents skipped` small | **the first compressed stream: native 720p over USB 2.0.** Next: P frames for bitrate, then docked 1080p |
| nvstream fps well under 60 | the log's per-stage averages say where the time goes (read, VIC, NVENC, copy, USB) |
| raw-view shows lost frames or stalls | the log's `nvstream: stopped:` line and the viewer's counters say which side |
| nvp: "P FRAMES DECODE, NO DRIFT" | **P frames work**: the next build streams IDR + P (GOP 30-60), roughly a quarter of the bitrate |
| nvp stalls | P needs more than grc's setup and bindings (buffer sizes, the first MEPRED input); engine work stops, and everything before it already counted |

**Risk:**

- The VIC's block-linear output changes only in height. M82 ran it 121 times.
- The stream holds the debug attach for about 60 s, which M67 did.
- USB stream transfers are M67's.
- The new risk is nvp, the first non-IDR job since M71, and it runs last.

## *** M82: real game frames into NVENC, and the VIC colour matrix measured (Run H: 60 fps, wrong block height, matrix solved) ***

### M81 Run G, read

Facts in `RESULT-M81-RUNG.md`. All 15 files verify: each FNV-1a equals the
console's `sd(...)` line.

**`error_status` 2 is routine. It does not react to anything we can change.**

- **grc's own frames carry it.** The observer found three of grc's live
  status blocks. All are P frames (`pic_type` 0) of 18-24 KB at avgQP 13,
  close to grc's 5 Mbps / 30 fps budget of 20.8 KB a frame, and all three
  report `error_status` 2 with `ucode_error_status` 0. grc's recordings play
  back.
- **Every variant carries it**, and each one removed a candidate cause:

| variant | what it removed | result |
|---|---|---|
| a | (Run F again) | 2; byte-identical output to Run F (`nvenc-a-bits.bin` FNV `e869e7fa` both runs) |
| b | "the frame is too small for the budget": 345,793 B at avgQP 21, QP 8..26, **about 17x over** the per-frame budget | 2 |
| c | rate control: RCMODE 0, constant QP 24 | 2 |
| d | HRD verification: setup `hrd_type` 0 | 2; bitstream and RC state byte-identical to a |
| e | the two-pass flag: setup `two_pass_rc` 0 | 2; bitstream and RC state byte-identical to a |

So M81's leading hypothesis, an HRD overflow verdict, is **refuted**: an
over-budget frame, an HRD-off job and a job with no rate control at all flag
the same 2. So are the zeroed-RC-state and the two-pass hypotheses. What all
of them share with grc's frames is the firmware, grc's setup, and the
`0x1100` bits of SET_CONTROL_PARAMS (FORCE_OUT_PIC, GPTIMER_ON). Isolating
one of those buys nothing. **From here on a job counts as good when the
engine wrote our picture index, `ucode_error_status` is 0 and the bit count
is non-zero. `error_status` is logged and ignored.**

**Also learned:**

- **Constant QP works.** Variant c took avgQP 24, which is grc's I-frame QP
  from the setup. It wrote nothing to the RC-process buffer; that is why
  `nvenc-c-rc.bin` is absent (the module saves the buffer only when something
  in it is non-zero). This is the mode a stream wants: quality set directly,
  no rate-control state to carry.
- **The RC-process state is 242 bytes.** After an IDR under RCMODE 18 it
  holds:
  - a 45-entry byte array at +0x58: one QP per macroblock row (720 / 16 =
    45). Flat stripes give all 8; noise gives 8 ramping to 26 down the frame;
  - a second 45-entry array at +0x88;
  - a few counters: +0x54 is the average QP, and +0xf0 is 256 in both.

  That is what a P frame would carry over. Constant QP never needs it.
- **Two setup bytes changed nothing.** d and e ran without complaint and
  produced grc's exact output, the first setups differing from grc's since
  M71.
- **The engine is fast.** The status appeared 28 us (c) to 2.7 ms (b) after
  the submit returned, with 60-700 us between the last clock read and the submit returning. Our
  jobs queue behind grc's on the same engine, which explains most of the
  spread. A flat 720p IDR frame took about 0.1 ms; the noisy one, under 3 ms.
- **grc runs no VIC job.** No VIC SETCL anywhere in grc's memory; its YUV
  input is made elsewhere (the compositor side). So grc cannot teach us the
  RGB->YUV matrix. M82 measures it directly instead.
- **Nothing was disturbed.**
  - Continuous `txn` through the log.
  - grc's video capture saved.
  - No crash report.
  - The brief hitch the user noticed at the Capture button press (~120 s)
    came 50 s after our last engine job (67.6 s). The log shows presents
    continuing through it. Saving a clip is grc's own work; nothing of ours
    was running.

### What M82 builds

Two probes, both in one boot. The arm file is `vic exec dbg csc nvframe
wait=60`. `exec` is back on: it maps the VIC buffers, which both probes need.
It also runs the fill and self-blit regression jobs that have completed in
every VIC run since M13.

**1. `csc`: the VIC colour matrix, measured.** M64 (output matrix) and M66
(slot matrix) both produced a constant: the offset column landed and every
coefficient term came out as zero. The field layout is the one Ryujinx and
NVIDIA's header agree on. What is unknown is the arithmetic: what one
coefficient unit is worth. So instead of a fourth guess, 11 VIC jobs each
program one matrix:

- **Input**: a 64x64 card of 16 flat patches. The bytes are R,G,B,A like the
  game's surface, declared exactly as the game path declares it, so the
  answer applies to game frames unchanged.
- **Output**: NV12, pitch.
- **`tools/vic_csc.py`** fits every output plane as `a*R + b*G + c*B + d`
  over the patch centres.

| probe | measures |
|---|---|
| `none` | the known pass-through (Y = B, U = R, V = G) |
| `out_k8`, `out_k12`, `out_k16`, `out_k19` | one coefficient per row, K = 2^8 to 2^19-1, shift 0. Which K gives gain 1 is the coefficient unit. Row -> plane and input -> channel fall out too |
| `out_k16_s8` | whether `matrix_r_shift` divides the products |
| `out_off` | the offset column's unit |
| `out_neg` | whether a negative coefficient works (two's complement) |
| `out_dense` | all nine coefficients distinct: is every field where the struct says? |
| `slot_k16` | the slot matrix under the same law |
| `m64_bt601` | M64's exact matrix. It must reproduce M64's constant (Y 4, U/V 32), or the harness is not measuring what M64 did |

The selftest makes a file under a made-up law and recovers it. That law has
16 fraction bits in the coefficients and offsets in 10-bit units, and under
it M64's matrix produces exactly the constant M64 saw (4, 32, 32). So "the
coefficients were 2^16 too small" fits every observation so far. The sweep
settles it.

**2. `nvframe`: NVENC on real game frames.** Inside the debug capture, with
the game running:

1. Read the presented 1920x1080 slot out of the game (the M56 route).
2. Look outside the top-left 1280x720: all zero means handheld, where MK8
   renders 720p into the corner of the 1080p surface (M37). In that case the
   VIC copies the corner **1:1**, native resolution. Docked, it scales
   1920x1080 down to 1280x720.
3. The VIC writes **NV12 block-linear, 32-row blocks** into an arena that is
   mapped into both the VIC and the NVENC channels. NVENC's surface config
   has no pitch mode, and grc's input is GPU block-linear with block height
   2, so this is the one layout it takes. Both planes round up to whole
   blocks: luma 736 rows, chroma 384. M81's arena had only 368 chroma rows,
   which this layout would have overrun.
4. NVENC encodes it as an IDR frame with grc's setup, **RCMODE 0**, the QP
   patched into the setup.

Colour stays the VIC's pass-through (Y = B, U = R, V = G). The PC undoes it.
Fixing it is what `csc` is for.

- **Frame 0** is encoded at QP 16, 20 and 24. Its VIC output and all three
  encodes are saved.
- **Then 120 frames run back to back at QP 20**: read, flush, VIC, encode,
  with no SD or logging in the loop. The module logs, per stage, the average
  and worst time, the bytes per frame, the achieved fps against the game's
  own present rate, and the NVENC clock after. The last frame is saved.
  **This is the first measurement of the whole capture -> encode path at
  speed.**
- **Housekeeping for this path:**
  - The NVENC pushbuffer lives in the arena, not in the VIC's. The VIC's
    completion is judged on syncpoint 12, which the compositor also advances,
    so reusing the VIC's pushbuffer for NVENC after a fence that fired early
    could feed NVENC methods to the VIC.
  - The arena is zeroed and flushed before any engine writes it, so no dirty
    cache line can later land on engine output.
  - The older one-shot `dbg` steps are skipped on an nvframe run: the strip
    dump and the 120-frame read-only loop.
- **`tools/nvframe_check.py`**:
  - deswizzles the VIC planes and scores every block height (0-4) and pitch
    for smoothness, so a wrong layout shows up as a number;
  - decodes each encode (behind SPS/PPS built from grc's setup) and computes
    PSNR per plane against the VIC's own planes. High PSNR means NVENC read
    the picture the VIC wrote, in the same layout;
  - writes PNGs of both, colours remapped.

  Its selftest swizzles a test picture into the layout, encodes the planes
  with x264 in NVENC's place, and requires the right layout to win and the
  PSNR to pass.

### Run H - `vic exec dbg csc nvframe wait=60`, handheld

Handheld, so the frame is native 720p and the VIC copies it 1:1. Docked also
works, scaled.

| reading | meaning |
|---|---|
| a K where the gain is 1, the offsets' unit known, `m64_bt601` = 4/32/32 | **the colour matrix is solved.** M83 programs BT.601 with that law, checks it on the card, and uses it for real frames |
| every coefficient probe constant, `m64_bt601` = 4/32/32 | the coefficient path is dead at every scale, not mis-scaled. Colour stays pass-through (the PC remaps it) and the matrix is parked |
| layout check says h=2, and PSNR against the VIC > 30 dB at all three QPs | **real frames encode.** The loop's timings say whether 720p all-intra keeps up with 60 fps; the sizes say the bitrate. M83 sends it over USB: a compressed native-720p stream |
| the VIC picture is right but the decode is scrambled (low PSNR) | NVENC reads a different block-linear variant; the setup's `input_bl_mode` / block height are the knobs |
| the VIC picture is scrambled in every layout | the VIC's block-linear output config is off |
| an NVENC stall | channel left open, engine work stops for the boot; frame 0's files say how far it got |

**Risk:** the VIC's block-linear output is a new configuration. The
addresses are all checked, but a VIC that hangs takes the compositor with it
(see "Hard-won constraints"), and that would mean a forced power-off. The debug
attach stops the game briefly while the swapchain is found (2 ms in the last
logged run).
The loop reads 8.8 MB a frame from the game for ~2 s, which M67 did at 60 fps
for a minute.

## *** M81: what `error_status` 2 reacts to (Run G: nothing - it is routine) ***

### M80 Run F, read

Facts in `RESULT-M80-RUNF.md`. The three files verify: each FNV-1a equals the
console's `sd(...)` line.

- **NVENC encoded our frame, correctly.** The job was grc's, submitted from
  our channel. The fence was reached, and the engine wrote our picture index
  into our status block 2.1 ms after the submit, with NVENC at 979.2 MHz.
  - The 516-byte slice decodes, behind an SPS/PPS built from grc's setup, to
    all 23 stripes at their exact values.
  - The engine's reconstructed luma matches the input at 0.0 deviation.
  - This is the first NVENC job this project has seen complete. M68-M71's
    hand-built jobs never did. What differed is known; which difference
    mattered is not, and no longer needs to be.
- **Nothing was disturbed.** No stutter, and `txn` climbed without a gap to
  the end of the log. grc's own video capture saved afterwards. No crash
  report, and a normal shutdown.
- **The whole status block, field by field.** All 128 bytes were written by
  the engine: the block was poisoned with 0xA5, and none of it survived.

| field | value | reading |
|---|---|---|
| `error_status` (2 bits) | **2** | the question |
| `ucode_error_status` (30 bits) | 0 | the firmware's own error enum (BAD_MAGIC / INVALID_INPUT / ...) says none |
| `total_bit_count` / `last_valid_byte_offset` | 4128 / 516 | agree with each other and with the slice the PC parsed |
| `type1_bit_count` | 4096 | |
| `pic_type` / `num_slices` | 3 / 1 | IDR, as set |
| `avgQP`, `actual_min/max_qp_used` | 8, 8..8 | grc's setup asks I-QP 24 in 0..51: the rate control chose 8 by itself |
| `intra_mb_count` / `inter_mb_count` | 3600 / 0 | every MB of 1280x720 |
| `total_intra_cost` | 2176 | a flat picture is almost free |
| `total_inter_cost` | 235,926,000 | = 3600 x 65535: every MB's inter cost saturated, as it must be without a reference |
| `hrdFullness`, `complexity`, `cycle_count` | 0, 0, 0 | `cycle_count` needs DumpCycleCount; the other two are discussed below |
| ME/perf/SSD/SSIM fields, reserved | 0 | |

### What `error_status` 2 is, and is not

**It is not a failed encode.** The bitstream decodes to the input. The
reconstruction is exact. The ucode's own error field is 0. The value is not
left over from before: the whole block was poisoned.

**It is not documented.** NVIDIA's header gives the field two bits and the
comment "report error if any". Nothing names its values. NVJPG's status struct
has a field of the same name with the same comment, which does not help.

**The rate control is the only part of this job that passes a verdict.**
Everything else either ran or did not. grc's setup turns picture-level rate
control on with VCL HRD:

- `hrd_type` 1, `vcl_cpb_size` 5,000,000 bits, `vcl_bitrate` 5,000,000.
- `framerate` 7680, which is 30 x 256.
- `R` 46, which is 5 Mbps / 30 / 3600 MBs = 46.3 bits per macroblock per
  frame.
- The header's own comment on the RC struct: picture-level RC "will also
  perform HRD verification".

Against that budget, our frame is tiny:

- The per-frame budget is 166,667 bits. The frame took 4128, 2.5% of it.
- In leaky-bucket terms, a frame that small lets the coded-picture buffer
  gain 162 kbit it cannot drain. That is an HRD *overflow*. A CBR encoder
  pads it with filler data; a VBR encoder ignores it.
- A two-bit verdict with values 0 / 1 / 2 would fit none / underflow /
  overflow.

**That is the leading hypothesis, and only a hypothesis.** Two others fit the
same facts:

- **Our RC state starts zeroed.** The RC-process buffer is state the engine
  carries from frame to frame. We zero it; grc's may be seeded by the driver.
  A zeroed state could itself trip the check. `hrdFullness` 0 and
  `complexity` 0 would fit either reading.
- **The flag is routine for this configuration.** It could be set on grc's
  frames too, which encode correctly as well.

A fourth oddity is noted, not ranked: grc's setup has `two_pass_rc` 1, which
the header calls "first pass of 2 pass rc". No second pass is ever submitted.

**It does not block anything.** A decoder never sees HRD state, and the
stream path will not keep grc's rate control anyway:

- grc's settings are 5 Mbps / 30 fps for its recordings.
- The USB link carries ~290 Mbps (see README), so the stream wants high
  quality at 60 fps. That will be constant QP or a much larger bitrate, set by
  us.
- So the flag matters only if it marks something that breaks the *next*
  frame, a P frame that reads this frame's RC and history state.

Pinning it down costs one run, and M81 makes that the same run as the next
useful step.

### What M81 builds

**`nvgrc`: one channel, five independent IDR jobs, one change each.** Before
every job, all of these are reset:

- the status, RC-process, bitstream and history buffers are zeroed;
- the reference output is zeroed;
- grc's setup is copied in fresh;
- the status block is poisoned.

So every job is a first frame. Each gets its own picture index
(`0x4D383000` + n), and each is judged, as in M80, by the fence *and* its own
index being written back.

| variant | change from Run F | tests |
|---|---|---|
| **a** | none | is the 2 reproducible? |
| **b** | input: the same stripes plus noise in [-24, 24], mirrored within each 32-row band so every band keeps its exact mean in any layout | a frame that costs bits: does the verdict move with the frame's size? |
| **c** | SET_CONTROL_PARAMS `0x00001103`: RCMODE 0 | no rate control at all |
| **d** | setup `rate_control.hrd_type` 1 -> 0 (byte 0x70) | rate control on, HRD verification off |
| **e** | setup `rate_control.two_pass_rc` 1 -> 0 (byte 0xBA) | the two-pass flag |

d and e are the only variants that change grc's setup, so they run last.
M68-M71's hand-built setups stalled, and a stall ends engine work for the
boot. Both offsets come from `offsetof` on NVIDIA's struct and are
static-asserted against the offsets nvsetup-dump uses. The build also asserts
that grc's setup holds 1 at both.

**Per variant, on the SD card:**

- `nvenc-X-status.bin` and `nvenc-X-bits.bin`.
- `nvenc-X-rc.bin`: the RC-process buffer, up to its last non-zero byte. The
  offset of that byte is logged. It went in zeroed, so every non-zero word is
  RC state the engine wrote. If `error_status` is an HRD verdict, the fullness
  it judged should be in there. For the P frames after this, it is the state
  that has to carry over.
- Variant a also writes `nvenc-a-recon-y.bin`.
- The log line now includes QP min..max and `hrdFullness`.
- A bitstream over 1 MB, the stage buffer's limit, is saved truncated, and
  the log says so.

**The grc observer (`grcscan`) now looks for three more things, and runs
first.** It touches no engine. Running it before our jobs means it sees grc
before anything of ours has shared NVENC with grc.

- **grc's own status blocks.** grc has to read its status blocks to know
  each frame's size, so they are in its memory. The signature is strict:
  - intra + inter MBs = 3600;
  - `pic_type` 0-3 and 1-64 slices;
  - a non-zero bit count that fits the bitstream buffer;
  - a `last_valid_byte_offset` that fits too.

  Every match gets a NOTE (up to 32), and the log adds a histogram of
  `error_status` 0/1/2/3 and a count of non-zero ucode errors. If grc's
  frames carry 2 as well, the flag is routine for this configuration.
- **grc's VIC jobs.** A VIC SETCL (class 0x5D) now also counts when it is
  followed by the THI 0x0B write. Up to 4 windows of 1 KB are dumped, and
  `nvrec.py` names the VIC 4.0 methods. This is the next step's question.
  Real frames reach NVENC as NV12, and our VIC's RGB->YUV conversion is still
  wrong (the packed-4:2:0 stream carries B/R/G, not Y/U/V). If grc converts
  with the VIC, its config struct shows how.
- **The SETCL rule is fixed.** M78's rule wanted register 0x10 after SETCL.
  grc writes THI 0x0B there (Run E), so grc's NVENC SETCLs now count too.

**PC side:**

- `nvenc_replay.py check` reads whichever set is on the card: the M80 names,
  or `nvenc-a-*` to `nvenc-e-*`. For each variant it:
  - prints the status with QP range and `hrdFullness`;
  - lists the NAL units, decodes, and checks the bands;
  - writes `nvenc-X-decoded.png`;
  - lists the non-zero words of `nvenc-X-rc.bin`.
- The check ran on Run F's files (same result as in Run F) and on a
  synthetic a-e set. That set used x264 encodes of the flat and the noisy
  input, generated with the console's exact noise sequence, and all five
  variants decoded to the stripes.
- `nvrec.py` reproduces Run E's decode exactly: only output paths differ,
  and the setups are byte-identical.

### Run G - `vic nvgrc grcscan wait=60`

In a race, as before.

| reading | meaning |
|---|---|
| a = 2 | reproducible; the rest of the table applies |
| a = 0 | Run F's 2 was not a property of the job; look at what differed (grc's recording state, timing) |
| c = 0 and d = 0 | **the HRD check sets it.** b says which way: if b's bigger frame clears it or turns it into another value, it is an overflow / underflow verdict. Harmless for a stream; the stream sets its own RC |
| c = 0, d = 2 | rate control sets it, but not through `hrd_type`; the RC dump and e narrow it |
| c = 2 | not rate control; e and grc's histogram are what is left |
| e = 0, others 2 | the two-pass flag |
| grc's histogram mostly 2 | routine for grc's configuration, whatever it means |
| d or e stalls | that setup byte is load-bearing. Engine work stops for the boot, a to c still count, and the observer has already run |
| the observer finds VIC SETCLs | the next build reads grc's VIC config for the RGB->YUV setup |

**Risk:** five jobs instead of one. a to c change nothing in grc's setup that
Run F did not already run. d and e each change one byte, which is the first
time since M71 that a setup differs from grc's. The observer's risk is Run D
and Run E's (both clean).

## *** M80: grc's IDR job, replayed from our own channel (Run F: encoded correctly, with error_status 2) ***

### M79 Run E, read

Facts in `RESULT-M79-RUNE.md`. `grc-scan.bin` verified here too: its FNV-1a
equals the console's `sd(...)` line (`796a134f`).

- **The command buffers are a ring inside grc's own data.** All 326
  `SET_IN_DRV_PIC_SETUP` writes sit in one 32 KB region
  (`0x62a37cd000+0x8000`, state 0x4). A P job is 0xC8 bytes (50 words); the
  IDR job is 0xA4 (41 words), which is exactly the three method writes it
  leaves out. 70 jobs were decoded across the eight dumps (four windows,
  each dumped twice).
- **Every job has the same shape**, and there is a SETCL after all. M78's
  SETCL rule wanted the next word to write register 0x10, but grc's next word
  writes THI register 0x0B, so the rule was too strict. The method-write search
  found the jobs anyway:

```
SETCL 0x21 ; INCR THI 0x0B = 0
SET_CONTROL_PARAMS   0x12001103   H.264, FORCE_OUT_PIC, GPTIMER_ON, RCMODE 18
SET_PICTURE_INDEX    n
SET_APPLICATION_ID   1
SET_IN_DRV_PIC_SETUP / SET_OUT_ENC_STATUS / SET_IO_RC_PROCESS / SET_OUT_BITSTREAM /
SET_IOHISTORY / SET_IN_CUR_PIC / SET_IN_CUR_PIC_CHROMA_U / SET_OUT_REF_PIC_LUMA
[P frames only: SET_IN_MEPRED_DATA / SET_OUT_MEPRED_DATA / SET_IN_REF_PIC0_LUMA]
EXECUTE 0x100 ; INCR_SYNCPT OP_DONE, syncpoint 14
```

- **The IDR job binds no ME or reference inputs.** Among the 70 jobs, the
  one with picture index 660 (= 44 x 15, and grc's GOP is 15) goes from
  `SET_OUT_REF_PIC_LUMA` straight to `EXECUTE`. Every P job around it binds
  all three.
- **Buffers:**
  - Setup, status and bitstream alternate between two sets (ping-pong).
  - The reference and ME buffers ping-pong as well.
  - The input picture rotates.
  - RC-process and IO-history are one shared buffer each.
  - From the IOVA spacing: bitstream 0x160000, history 0xC0000, input luma
    1280x736 then chroma (so it is 32-row aligned, which matches the setup's
    `block_height` 2).
- **SET_CONTROL_PARAMS was M71's biggest gap.** M71 sent only the codec bits
  (`3`). grc also sets FORCE_OUT_PIC, GPTIMER_ON and RCMODE 18, and binds
  IO_RC_PROCESS, which M71 never did.
- **The IDR setups** (slots 3, 8, 9) agree with each other except for
  `idr_pic_id`. Against the P setups, 24 bytes differ: picture type,
  reference-list entries, frame_num / POC, and the inter-mode enables in the
  MD control.
- **The two "implausible" setups** (setup_0, setup_1) are the heap objects
  seen in Run D. They are ignored.
- **grc was not disturbed.** The observer stayed attached 382 ms, drained 0
  debug events, and a video capture afterwards saved fine. It skipped 3
  regions over 8 MB (86 MB of frame and bitstream storage).

### What M80 builds: `nvgrc`

One IDR frame, submitted from our own msenc channel. It is the job grc
submits, with our buffers:

- **The setup is grc's**, byte for byte: `setup_8.bin`, IDR, frame_num 0,
  idr_pic_id 0. `tools/nvenc_replay.py gen` generates `nvenc_grc_idr.h` from
  it and checks its fields first. The setup holds no addresses; every surface
  is bound by a method.
- **The command buffer is grc's IDR job**, word for word: SETCL, the THI 0x0B
  write, `0x12001103`, the twelve methods in grc's order, EXECUTE, and an
  OP_DONE increment.
- **Buffers mirror grc's sizes** in one 5.1 MB cached nvmap arena, flushed
  before the submit and after completion. The RC-process and history buffers
  start zeroed, which is what grc's must look like before its first frame.
- **The input is 32-row luma stripes** (32 + 8k) on neutral chroma. A 32-row
  band is one contiguous byte range both in pitch-linear and in grc's 32-row
  block-linear input, so no swizzle is needed, and every stage can be checked
  band by band.
- **The clock:** ClockEnsure on NVENC, then a final read immediately before
  the submit, as for jpgdec.
- **Completion is judged by the status buffer.** Our channel gets syncpoint 14,
  the same one grc's jobs increment (as ours shares 12 with the compositor on
  the VIC), so a grc job can advance the fence. The job carries its own
  picture index (`0x4D383000`) and counts as done only when the engine has
  written that index into our poisoned status block.
- **On completion:** the status fields are logged, the bitstream's first bytes
  and the reconstructed luma's stripe means are checked on the console, and
  `nvenc-status.bin`, `nvenc-bits.bin` and `nvenc-recon-y.bin` are written
  through `WriteEngineOutputToSd`. On a stall the channel is left open and
  engine work stops for the boot, as for jpgdec.
- **`tools/nvenc_replay.py check`** prints the status and lists the NAL units.
  NVENC writes slices; grc writes SPS/PPS itself, so if the stream has none,
  the tool builds them from grc's setup. It then decodes with PyAV and
  compares each stripe with the input. Its `selftest` proves the SPS/PPS
  writer: it rewrites libx264's headers byte for byte from parsed fields, and
  decodes an x264 encode of the same stripes to the exact band values. The
  full check path ran on a synthetic output directory.
- `stage-buffer` users now also check that the heap actually reaches the stage
  buffer. `armfile_test` gains the case that matters here: `nvgrc` must never
  arm the `grc` interceptor.

### Run F - `vic nvgrc wait=60`

In a race, as before; grc keeps running its own encodes alongside ours.

| reading | meaning |
|---|---|
| status WRITTEN, error 0, bits > 0, stripes match on the console, and `check` decodes to the stripes | **NVENC encodes from this process.** The encoder door is open; M81 feeds it real frames (game -> VIC -> NV12 -> NVENC -> USB) |
| status written with a non-zero `error_status` / `ucode_error_status` | the engine ran our job and rejected something; the ucode error names what (the NVC5B7 error enum) |
| status not written in 1 s | treated as a stall: channel left open, no more engine work this boot |
| submit rejected | nothing reached the engine |

**Risk:** our job shares NVENC with grc's. M68-M71's NVENC stalls never
stopped the game from presenting; the compositor wedges (M73/M74) were NVJPG
and happened at channel teardown, which a stall here does not do. A stall could
still break grc's recording for that boot.

## *** M79: grc's live encoder config is in hand - now its command buffer and an intra frame (Run E: both captured) ***

### M78 Run D, read

Facts in `RESULT-M78-RUND.md`. Verified here as well: the decode file's
FNV-1a equals the console's `sd(...)` line (`3c391345`), and the check tool
reports diff 0, so NVJPG's output matches libjpeg's to the byte for this
image. `grc-scan.bin` matches too (`60525240`).

The observer attached to grc (pid 138), resumed it, scanned 98,356 KB across
52 regions, and **stopped at the 96 MB cap**. It found five NVENC 5.0 magics
and **no NVENC SETCL at all**.

- **Three of the five are grc's live per-frame setups.** Each sits at the
  start of its own 12 KB region (`0x55c7959000`, `...c000`, `...f000`, state
  0xd). They differ in exactly two fields, `frame_num` and
  `pic_order_cnt_lsb` (1/2, 2/4, 2/4), so they look like a ring of setups for
  consecutive frames.
- The other two are in a 532 KB heap region: the magic followed by
  pointer-like words. Some other grc object, not a setup.
- **This is the configuration NVENC accepts on this firmware**, decoded with
  NVIDIA's header:

| field | grc | M71 (ours) |
|---|---|---|
| magic | 0xd0b70006 (NVENC 5.0) | same |
| size / input | 1280x720, pitch 1280, block_height 2 | 256x128, pitch-linear |
| ref / output pictures | tiled 16x16, chroma at +0xE10 x 256 B | tiled, separate chroma |
| profile / level | High (100) / 3.2, CABAC, 8x8 transform | Baseline (66) / 4.2, CAVLC |
| GOP | 15, P frames, 1 reference, POC type 2 | infinite, every frame IDR |
| rate control | hrd_type 1, 5 Mbps, VBV 5 Mb, QP 24/28/24, two_pass_rc | hrd_type 2, constant QP 26 |
| framerate | 7680 = 30 << 8 (8.8 fixed point) | 60 (i.e. 0.23 fps) |
| rhopbi | 0,0,0 | 256,256,256 |
| max_slice_size, e4byteStartCode | 3600, 1 | 256 KB, 0 |
| ME / MD / quant controls | fully populated | ME: one field set; MD: intra modes only; quant: zeroed |
| hist / bitstream buffers | 706,560 / 1,382,400 B | 64 KB / 256 KB |

  Any of those differences could have kept M68-M71's job from completing.
  None has to be guessed any more: the replay starts from grc's bytes.
- **The command buffer is still missing.** No NVENC SETCL turned up in
  96 MB. The likely reason is the one oss-nvjpg already shows: on Horizon
  userspace does not send SETCL, and nvservices sets the class. M78's search
  keyed on SETCL, so it could not have found grc's command buffers, and the
  cap may have kept it out of the right regions anyway. The command buffer
  holds what the setup does not: the `SET_CONTROL_PARAMS` value, and which
  surfaces grc binds (rate-control data, IO history, status, reference
  pictures).
- **Every setup caught was a P frame.** The first job to replay should be an
  intra frame, which references nothing.

### What M79 builds

- **Command buffers found by what they do.** A hit is a method-offset write
  (INCR, NONINCR or MASK to register 0x10, or the IMM form) whose method is
  `SET_IN_DRV_PIC_SETUP` (0x710 >> 2). Each gets a 2 KB window starting
  0x200 before the hit; at most two per region and twelve in all.
- **No blind spots from the cap.** Every readable region up to 8 MB is
  scanned in full, with an overall cap of 256 MB. Larger regions (frame and
  bitstream storage) are skipped, and each is recorded as a note.
- **The setup ring is watched for up to 3 s** after the scan, with grc
  running. Each new (slot, frame_num, pic_type) is dumped. Intra frames
  (pic_type 2 or 3) are always kept, P frames at most three. The first time
  an intra frame appears, the command-buffer windows are dumped again at that
  moment. Each pass drains any debug event, so grc is never left halted.
- `tools/nvrec.py`: observer CMDBUF records carry the hit offset. The words
  before it are printed raw (the window may start mid-command) and decoding
  starts at the hit. The class shows as `?` when no SETCL was seen.
  Round-tripped on the host with a synthetic file; M78's file still decodes.

### Run E - `vic grcscan wait=60`

No engine probe, and no clock requests: this run only reads grc.

| reading | next |
|---|---|
| SET_IN_DRV_PIC_SETUP writes decoded, plus an intra setup | **M80 replays grc's intra frame**: its setup bytes and its method list, in our own NVENC job at 1280x720, from a VIC-made NV12 input, over the submit path jpgdec proved, with ClockEnsure on NVENC first |
| writes decoded, no intra frame | M80 can still start from a P setup rewritten to IDR (pic_type 3, frame_num 0, POC 0). Less faithful |
| no writes found | the command buffers are in a skipped (> 8 MB) region or use an encoding this search misses; the skipped-region notes say where to look next |

**Risk:** the attach lasts up to ~3 s instead of under 1 s. grc keeps running
the whole time. In Run D, a video capture right after a sub-second attach
saved and played back fine.

## *** M78: the dump that could not have worked, and grc's encoder job read out (Run D: both files verified, grc's setups captured) ***

### M77 Run C: this process drove a non-VIC engine for the first time

Facts in `RESULT-M77-RUNC.md` and `logs/m77-runC-applet-mitm.log`. The line
that matters:

```
[63.377] jpgdec: submit rc=0x0 nverr=0 fence 1/1 REACHED after 314 us | NVJPG 422400000 Hz read 163 us before
         submit returned, 422400000 Hz after | status used=292 mcu=0x0 result=0 | 16384/16384 output bytes written
[63.496] jpgdec: worst 8x8 block-mean deviation 0 (block 0) -> *** NVJPG RAN AND DECODED CORRECTLY ***
```

- **The first completed non-VIC job since M68.** The config was known-good,
  the clock was confirmed at the submit, and the output matched libjpeg's
  decode, block for block. Sampled pixels are within 1 count of the source
  image, with byte order R, G, B, A.
- **So our submit path is sound.** Channel, nvmap fd, MAP_CMD_BUFFER pins,
  SETCL, method writes, OP_DONE syncpoint, and the aruid adopted from the game
  all work for an engine other than the VIC. What M73/M74 lacked was the clock
  (M76), plus the right method offsets (also M76).
- **For NVENC that settles the order of work.** Run A showed its clock was
  running all along, and our submit path is now proven, so what remains for
  NVENC is its job configuration. The best source for that is grc's own job.
- No freeze, no stutter, and `txn` climbed without a gap to 38,357 at 363 s.

What the clock watch showed:

```
53.976  held+2s    NVJPG=652.8  VIC=652.8  NVDEC=979.2
59.382  clk-watch  NVDEC 979.2 -> 0.0            <- nothing of ours was running
60.718  node /dev/nvhost-nvjpg opened and closed (node survey)
60.807  clk-watch  NVJPG, VIC 652.8 -> 422.4     <- the next sample after it
62.986  clk-ensure #1 re-set max -> 422.4 MHz    (not 652.8)
```

- NVJPG's raised rate fell within one sample (89 ms) of the node survey
  closing an NVJPG channel. With Run B that is two runs consistent with
  "closing an engine channel resets its clock", now with the timing to back it.
  It is still an inference, not a documented behaviour.
- NVDEC fell on its own, 1.3 s before anything of ours touched a device, so
  something outside this module also moves these clocks. Any long-lived engine
  user should re-check its clock rather than trust a request.
- Re-issuing `SetAndWait(max)` did not restore 652.8 MHz. It left NVJPG at the
  domain's 422.4, which was enough to run. ClockEnsure stops at any non-zero
  rate by design. For an encoder where throughput matters, the next change is
  to escalate until the rate reaches the requested maximum. Not needed yet.
- Unlike Runs A and B, NVJPG (422.4) and NVDEC (460.8) were non-zero *before*
  any request, matching their domains' base rates. clkrst appears to report 0
  while a module's clock is disabled and the domain rate while it is enabled.
  Why they started enabled in this boot is unknown.

### The `.rgba` file was never the engine's output, and it could not have been

The local session traced it: the file held recycled FAT clusters (a line of
this run's own log, Mario Kart asset names). The cause is in the kernel.
`jpgdec` passed `fs::WriteFile` the **uncached** engine buffer. Mesosphere's
IPC buffer setup (`kern_k_page_table_base.cpp`, the `test_attr_mask` for
Ipc, NonSecureIpc and NonDeviceIpc alike) refuses any buffer with the
`Uncached` attribute. `CreateFile` had already allocated the clusters, so a
16 KB file existed and nothing said the write had failed, because its Result
was discarded.

This project learned exactly this in M41/M42, and the strip-dump code says
so in a comment. M76 wrote the jpgdec dump without it; that was my bug. The
M73 JPEG-encode dump had the same flaw and has never run with a working engine.

**Fix:** `WriteSdVerified` / `WriteEngineOutputToSd`. Engine output is copied
into the cached stage buffer first. Every fs Result is checked and logged. The
file is read back and compared byte for byte, with one retry. The log line
carries the FNV-1a of what was written, and `tools/nvjpg_dec_control.py check`
prints the FNV-1a of the file it was given. Equal hashes mean the file is what
the console wrote. Both dumps use the new path. The hash was checked against
standard test vectors in C++ and Python.

### The next step: read grc's NVENC job out, read-only

The M75 observer attaches to grc, resumes it at once, scans its memory for an
NVENC setup magic or an NVENC SETCL, and detaches. It touches no channel and
submits nothing. It has never actually run: M75's own run was hijacked by the
parser bug. M78 makes one run of it count:

- **Every hit is dumped, not just located.** 4 KB from each setup magic and
  1 KB of command words from each NVENC SETCL, read while still attached, go
  into `sdmc:/grc-scan.bin` as records in the M72 recorder's format. A note
  record before each gives the full address and its memory region.
  `tools/nvrec.py` names every method in the command buffers, and
  `tools/nvsetup-dump` decodes each setup with NVIDIA's header. Round-tripped
  on the host with a synthetic record file.
- **Fewer false positives.** A SETCL now counts only when the next word
  writes the method-offset register (INCR, NONINCR or MASK to 0x10). M75
  accepted any word whose opcode was <= 4.
- The file goes through `WriteSdVerified`, so it is checked the same way.

### Run D - `vic clk jpgdec grcscan wait=60`

jpgdec runs first, as a regression control that should now leave a file
verifiable on the PC. The grc observer runs after it. `grc` must **not** be
armed and `mitm.lst` must be absent: the observer uses the debug SVCs, not the
M72 IPC interceptor.

| reading | meaning |
|---|---|
| `sd(sdmc:/nvjpg-dec.rgba): ... fnv1a32=X` and the PC check prints the same X plus MATCH | the decode is verified independently of the console |
| setup hits whose decode shows a sane H.264 config (a real resolution, profile, rate control) | we have grc's configuration; M79 replays it in our own NVENC job |
| NVENC SETCL hits only | grc's method sequence (and any method we never send) plus the setup's IOVA; the setup itself then needs locating |
| attach or resume fails | the `rc` values say why; nothing else was touched |
| no hits | grc was idle, or its buffers are past the 96 MB cap or unreadable; the scanned-KB and region counts say which |

**Risk:** attaching stops grc until `ContinueDebugEvent`, which is the next
call. If grc objects, recording breaks for the rest of the boot. The game's
picture does not depend on grc.

## *** M77: establish NVJPG's clock at the submit, and find out what cleared it (Run C: NVJPG decoded correctly) ***

Picks up from `HANDOFF-M76.md`. No hardware cycle yet.

### What Run B's log shows, beyond the summary

Between the hold (52.4 s) and the jpgdec check (62.9 s) exactly three things
happened:

```
52.461  mm id 7 (NVJPG) SetAndWait(max) -> 652.8 MHz
54.635  clk[held+2s]   VIC=652.8  NVJPG=652.8  NVDEC=979.2   <- still up
61.008  vb:4b_node_survey - opens and CLOSES every engine node:
61.152    /dev/nvhost-msenc   61.167 /dev/nvhost-nvdec   61.199 /dev/nvhost-nvjpg
61.669  /dev/nvhost-vic opened (the VIC worker's own channel)
62.927  clk[jpgdec-pre] VIC=422.4  NVJPG=0.0    NVDEC=979.2   <- NVJPG gone
```

- **VIC fell with NVJPG** (652.8 -> 422.4). Run A's release shows the same
  pairing: dropping the NVJPG request put VIC back to 422.4, while NVENC and
  NVDEC rose and fell together. By the rates, {NVJPG, VIC} and {NVENC, NVDEC}
  behave like two shared clock domains. That is an inference from four
  numbers, not a documented fact.
- So in Run B the NVJPG request's **effect vanished entirely**, VIC included.
  This was not NVJPG idling on its own. Something undid the rate mm:u had set.
- **Prime suspect: the node survey's open and close of `/dev/nvhost-nvjpg`.**
  The last close of an engine channel is the natural place for nvservices to
  drop that engine's clock. The evidence against it: NVDEC's node was opened
  and closed in the same survey and NVDEC stayed at 979.2. So this is a
  suspect, not a finding.
- **The order was backwards anyway.** Both working drivers open the engine
  channel first and request the clock after (oss-nvjpg: `channel.open`, map,
  then `mmuRequestInitialize`; nvtegra: open at device init, `SetAndWait` at
  decode init). M76 requested the clock ten seconds before a channel existed.

The bug that turned this into a refusal is the one the handoff names:
`ClocksHoldForEngines` returned `true` from its idempotency guard, so
`held=1` meant "a request exists", not "the clock is running".

### The fix

- **`ClockEnsure(module)`** re-applies the mm:u request for that engine and
  reads the clock back through clkrst. It escalates until the clock is non-zero
  or six attempts are spent, and logs every attempt:
  1. `SetAndWait(max)` on the existing request
  2. `SetAndWait(0)` then `SetAndWait(max)`, which defeats a cached "no change"
  3. `FinalizeWithId`, fresh `InitializeWithId`, `SetAndWait(max)`
  `ClocksHoldForEngines` now documents that it only creates requests.
- **jpgdec order:** open the channel -> syncpoint -> nvmap fd -> map buffers
  -> prepare record, scan, cmdbuf and submit args -> `ClockEnsure` ->
  breadcrumb -> **final clkrst read -> submit**, with nothing but a timestamp
  between the read and the ioctl. The result line carries the NVJPG rate read
  at the submit, how many microseconds before the submit returned, and the
  rate afterwards.
- If the clock cannot be brought up, jpgdec unmaps, closes (nothing was
  submitted, so teardown is safe) and refuses.
- **`clk-watch`:** while the survey thread holds clocks for an engine probe,
  it samples NVJPG, VIC and NVDEC every 200 ms and logs only changes, until
  jpgdec finishes (or 45 s). That places the drop against the node survey's
  timestamps.

Which ensure step brings NVJPG back is itself a result:

| step that works | meaning |
|---|---|
| 1, re-set max | the rate was overwritten underneath mm:u; a fresh request re-applies it |
| 2, 0 then max | mm:u caches the rate and skipped a same-value request; pcv had been changed behind it |
| 3, fresh request | the request itself was dropped server-side |
| none | this process cannot hold NVJPG's clock with a channel open; stop and rethink |

### Run C - `vic clk jpgdec wait=60`

Same arm file as Run B, in a game. Read, in order:

1. `clk-watch` lines from ~54.7 s: when NVJPG and VIC drop, against
   `node /dev/nvhost-nvjpg` (~61.2 s) and the `/dev/nvhost-vic` open.
2. `clk-ensure(jpgdec)` lines: which step brought NVJPG up.
3. `jpgdec: submit ... NVJPG <Hz> read <us> before submit returned`:

| result | meaning |
|---|---|
| REACHED, block means match | this process drives a non-VIC engine and the decode record is right. NVJPG's M73/M74 failures were the clock plus the offsets M76 fixed. Run `tools/nvjpg_dec_control.py check` on `nvjpg-dec.rgba` |
| REACHED, mismatch | the engine ran; the output needs comparing on the PC |
| STALLED, NVJPG non-zero at submit | the clock was necessary but not sufficient; something in our submit path is still wrong. Next suspects: the SETCL oss-nvjpg does not send on Horizon, adopting the game's aruid before opening nvmap, the `nvdrv:t` session |
| refused | the watch and ensure lines say why |

**Risk:** unchanged from Run B. A stall can still wedge the compositor like
M74 did, even though the channel is left open on a stall.

### NVENC

Run A settled one thing: NVENC ran at 460.8 MHz before anything was
requested, so the clock does not explain M68-M71. That leaves configuration or
the submit path, and Run C tells them apart for free. If jpgdec completes, our
submit path is proven and NVENC is purely a config problem, best answered by
grc's own job (the M75 observer, which has never actually run). If jpgdec
stalls with a clock, NVENC would stall for the same reason, so the submit path
comes first.

## *** M76: nobody ever asked for the clock - and the NVJPG table was one register off ***

No hardware cycle. This milestone is a desk review of M68-M75 against the two
open-source drivers known to run Tegra engines on this console under Horizon:
averne's **oss-nvjpg** (NVJPG decode) and averne's **FFmpeg nvtegra** hwaccel
(NVDEC + NVJPG + VIC). It found three defects and one hole in the roadmap, and
it builds the probes that test them. Nothing below is verified on hardware yet.

### 1. The engines were never clocked

The facts every engine run agrees on: the VIC works; NVENC and NVJPG accept a
submit, issue a fence, and never execute - NVJPG's status says `cycles=0`, i.e.
the engine did not run for a single clock. M73 named the one difference (the
VIC is kept running by nvnflinger) and guessed power gating. It never found the
mechanism, and M74 spent a forced power-off trying to wake the engine with
screenshots.

On Horizon, the clock for NVDEC / NVENC / NVJPG is not nvservices' business. A
client asks the multimedia service **`mm:u`** for a frequency on the engine it
is about to use. Both working drivers do exactly that before submitting:

| driver | what it does |
|---|---|
| oss-nvjpg `Decoder::initialize` | `mmuRequestInitialize(MmuModuleId_Nvjpg, 8, false)` |
| FFmpeg nvtegra device init + DFS | `mmuRequestInitialize` for NVDEC and NVJPG, then `mmuRequestSetAndWait(max)` - commented "reproduces official code". It also notes the channel `SET_CLK_RATE` ioctl exists on HOS but is reset on sleep. |

This module has never opened `mm:u`, never listed it in the NPDM, and never
issued `SET_CLK_RATE`. Grep says so. An engine with no clock behaves exactly
like M68-M74: channel real, submit accepted, fence issued, zero cycles.

Two cautions, stated before the run:

- **NVENC may not be the same story.** grc continuously records the last 30
  seconds for titles that support capture (MK8D does), so grc may keep NVENC
  clocked during gameplay. If the survey shows NVENC already running before we
  ask, the clock was not NVENC's problem and its config is.
- **The mm:u module ids are ambiguous.** libnx names 5 NVENC and 6 NVDEC;
  nvtegra, by the same author and newer, passes `(MmuModuleId)5` for NVDEC. So
  M76 requests 5, 6 and 7 and lets clkrst say which id moved which engine.

### 2. The NVJPG method table came from the wrong generation

M73 took NVJPG's methods from `clc9d1.h`, a **multi-core** NVJPG. That header
inserts `SET_TOTAL_CORE_NUM` at 0x704 and starts a per-core block with
`SET_CORE_INDEX` at 0x710, pushing every surface one register later. Tegra X1's
NVJPG is single-core. The single-core header, `cle7d0.h` (it was in
`ref/open-gpu-doc` all along), agrees register-for-register with the map
oss-nvjpg drives on this hardware:

| offset | M73 wrote (C9D1) | T210 / E7D0 means |
|---|---|---|
| 0x704 | TOTAL_CORE_NUM = 1 | PICTURE_INDEX |
| 0x710 | CORE_INDEX = **0** | **BITSTREAM** |
| 0x714 | bitstream | CUR_PIC (luma) |
| 0x718 | luma | CHROMA_U |
| 0x71C | chroma U | CHROMA_V |

So M73/M74 handed the engine **a zero bitstream address** - the thing M27
proved hangs an engine - and the bitstream as its luma plane. It never mattered
only because the engine never ran. "Method offsets are stable across
generations" holds for the common block (0x200, 0x300, 0x700-0x70C are the same
in both headers); it does not hold past the point a generation adds methods.
Fixed.

### 3. The NVJPG encode setup struct is probably not this chip's format either

`nvjpg_drv_pic_param_s` from open-gpu-doc is a register-image layout. The one
T210 NVJPG record known to work - oss-nvjpg's decode picture info - is a
completely different, higher-level 0xB2C-byte record (Huffman tables as JPEG
BITS/HUFFVAL arrays, quant tables in file order). The T210 encoder almost
certainly takes a record in that style, and nobody has published it. So even
with the clock and the method table fixed, **NVJPG encode is not the quick path
M73 thought it was**, and `jpg` should not be armed until its record is known.

### 4. The M75 parser bug class survived in ArmFileNumber

M75 fixed `ArmFileContains` and declared the prefix-bug class closed.
`ArmFileNumber` still used `strstr`: with `sweep stream sw=768` it read `sw`
out of "sweep", found no digits, and silently used the default. Both now share
one tokenizer (`applet_mitm_armfile.hpp`) with a host test
(`tier4/applet-mitm/test/armfile_test.cpp`, 24 checks including M75's exact
failure).

The boot banner also printed `jpg=off` *before* the `jpg` flag was parsed, and
called every build a read-only observer. It now names only what it knows; the
flag dump that follows it is the record.

### 5. Docked 1080p cannot go over USB at all

The Switch has one USB-C port. Docked, the dock owns it and the console is the
USB **host**; `usb:ds` device mode needs a direct cable to the PC, i.e.
handheld. So every USB stream so far (M58-M71) was necessarily handheld, where
MK8D renders 1280x720 into its 1920x1080 surface (M47).

- **SuperSpeed can only ever help handheld**, where native is 720p. Handheld
  720p60 packed-420 is 83 MB/s: over USB 2.0's measured 37, comfortably inside
  USB 3.0. So a SuperSpeed link would give native handheld with no encoder.
- **Docked native 1080p60 needs a network transport** (Wi-Fi, or a USB LAN
  adapter in the dock), and neither is expected to beat the USB 2.0 link. So
  docked native resolution needs compression, with no way around it.

### What M76 builds

- **`clk` - clock survey, no engine contact.** Fires at uptime `wait - 10`
  (so its "before" reading precedes any engine probe at `wait`).
  Reads the real clocks of VIC / NVENC / NVJPG / NVDEC / HOST1X through
  `clkrst` four times, then opens `mm:u` and requests the maximum on ids 5, 6
  and 7, logs what each request reports, reads the clocks twice more, and
  releases (unless an engine probe in the same run needs them). NPDM gains
  `mm:u` and `clkrst`. Both are hand-rolled IPC with explicit command ids.
- **`jpgdec` - the first positive control this project has ever had.** One
  NVJPG **decode** of a 64x64 JPEG whose picture-info record is generated on the
  PC (`tools/nvjpg_dec_control.py`) in oss-nvjpg's T210 layout and was checked
  **byte-for-byte against that project's own struct and parser: 0 diffs in
  2,860 bytes**. The method sequence is oss-nvjpg's, plus the SETCL an L4T
  kernel would insert. Unlike every job since M68, a stall here cannot be blamed
  on the config. Guards: it will not submit unless clkrst shows NVJPG clocked
  after the request; on a stall it leaves the channel **open** (M74's wedge was
  in teardown) and refuses all later engine work that boot. On success it
  compares 8x8 block means against libjpeg's decode on the console and writes
  `sdmc:/nvjpg-dec.rgba` for the PC check.
- `nvenc` / `jpg` probes now refuse to run after a stuck job, and hold the
  clocks first when `clk` is armed.
- **A boot hang closed off.** Since M73 the grc interceptor registers only when
  `grc` is armed - but if `mitm.lst` from M72-M75 was still on the card, boot2
  had already declared a future `nvdrv:s` mitm, and with nothing registering,
  every `nvdrv:s` client (vi included) would wait forever. Now `mitm.lst`
  without `grc` registers a pass-through that accepts nobody, and says so in
  the log.

## M76 Run A result: NVJPG confirmed unclocked; NVENC confirmed NOT unclocked

Clean run, no crash, no freeze, game played normally for 230+ s at steady 60 fps
throughout (queueBuffer incrementing ~300/5s the entire time). Zero engine
channel touched - pure clkrst + mm:u survey, exactly as designed.

    clk[before]    VIC=422.4  NVENC=460.8  NVJPG=0.0    NVDEC=0.0    HOST1X=81.6  MHz
    clk[before+] x3 identical - stable baseline before we touch anything
    clk[held]      VIC=652.8  NVENC=979.2  NVJPG=652.8  NVDEC=979.2  HOST1X=81.6  MHz
    clk[held+2s]   identical - the hold is stable, not a transient blip
    clk[released]  VIC=422.4  NVENC=460.8  NVJPG=0.0    NVDEC=0.0    HOST1X=81.6  MHz

Two results, matching both branches of the PR's own decision table at once:

- **NVJPG: 0.0 MHz before, 652.8 MHz when held.** Exactly the M76 prediction.
  NVJPG was clock-gated in every prior probe (M73, M74) and mm:u raises it.
  This justifies Run B (jpgdec).
- **NVENC: 460.8 MHz BEFORE we ever call mm:u.** Non-zero baseline, present
  through the whole survey window, unrelated to our request. Per the PR's own
  table: "NVENC non-zero before we ask -> grc keeps it running; NVENC's
  problem is config, not clock." So the four M68-M71 NVENC "accepted, never
  executed" results are NOT explained by an unclocked engine - NVENC had a
  clock the entire time (almost certainly grc's background 30 s capture
  buffer, which MK8D supports). NVENC's stall is a genuine configuration
  problem and needs its own investigation, separate from and unrelated to the
  clock-gating fix.

Side note: requesting NVENC/NVDEC/NVJPG clocks also raised VIC's reported clock
(422.4 -> 652.8 MHz) even though VIC was not requested. Consistent with a
shared DVFS voltage/frequency table across the video engine complex.

Cleanly released. Next: Run B (`vic clk jpgdec`), the one-submit decode
control, is now justified by this data - proceed only as a deliberate step,
per the PR's stated residual risk (a stall could still wedge the compositor
even with the channel left open).

## M76 Run B result: refused to submit - NVJPG's clock decayed within ~8 s of being raised

No crash, no freeze. The guard worked exactly as designed: it refused rather
than gambling on an uncertain submit, so `nvjpg-dec.rgba` was never written and
the decode is still untested. But the refusal itself is the finding.

Full timeline, `clk` armed with `keep_holding=true` (jpgdec is armed this run):

    50.4   clk:1_survey
    50.5-52.2  clk[before] x4, all identical: VIC=422.4 NVENC=460.8 NVJPG=0.0 NVDEC=0.0 HOST1X=81.6
    52.3   clk:2_hold - ClocksHoldForEngines("survey") - mm:u ids 5,6,7 all SetAndWait(max) rc=0x0
    52.6   clk[held]     VIC=652.8 NVENC=979.2 NVJPG=652.8 NVDEC=979.2 HOST1X=81.6
    54.6   clk[held+2s]  VIC=652.8 NVENC=979.2 NVJPG=652.8 NVDEC=979.2 HOST1X=81.6   (stable)
    54.7   "holding for the engine probes armed in this run" - NOT released, g_holding stays true
    ...
    62.9   clk[jpgdec-pre]  VIC=422.4 NVENC=979.2 NVJPG=0.0  NVDEC=979.2 HOST1X=81.6
    63.0   clk[jpgdec]      VIC=422.4 NVENC=979.2 NVJPG=0.0  NVDEC=979.2 HOST1X=81.6
    63.05  jpgdec: NOT submitting - hold=1 clkrst readable=1 NVJPG=0 Hz

**NVENC and NVDEC held their requested 979.2 MHz rock-steady for the full ~8-10
second gap with zero engine work on either.** NVJPG did not: it decayed from
652.8 back to 0.0 in the same window, also with zero engine work. All three
were requested via the identical `MmSetAndWait(id, max, -1)` call and none was
ever released (`FinalizeWithId` was never called - `g_engine_wedged` is false,
`keep_holding` is true throughout). So NVJPG specifically has some auto-idle or
timeout behavior on this firmware that ignores or overrides the `mm:u`
performance-mode request when the engine sees no actual submission, while
NVENC/NVDEC do not exhibit it, at least not within the window measured here.

**A bug in our own code compounded this.** `ClocksHoldForEngines` is
idempotent, guarded by `if (g_holding) { return true; }`. Once the initial
survey's hold succeeds, every later call - including jpgdec's own, ~10 s
later - short-circuits to `true` without re-issuing `SetAndWaitWithId` for
anything, NVJPG included. So `held=1` in the refusal log means "we successfully
requested a hold at some point," not "the clock is elevated right now" - and
for NVJPG on this firmware those turned out not to be the same thing. Fixing
this needs re-requesting (or at minimum re-checking and re-requesting) the
NVJPG clock immediately before the submit, not relying on a hold from up to
ten seconds earlier - the guard that saved this run from an uncertain submit is
also the guard that is currently unable to ever pass for NVJPG when `clk`'s own
timing model (survey at wait-10, submit at wait) is used as designed.

### Test plan

Before either run: delete `atmosphere/contents/0100000000000C20/mitm.lst` if
it is still there from M72-M75 (M76 survives it, but the run should not carry
the interceptor's registration at all).

**Run A - `clk wait=60`**, in a game. No channel, no submit. The survey lines
(`clk[before]`, `clk[held]`, ...) decide the next step:

| reading | meaning |
|---|---|
| NVJPG 0 before, non-zero when held | the clock is what M73/M74 lacked -> Run B |
| NVENC non-zero *before* we ask | grc keeps it running; NVENC's problem is config, not clock |
| NVENC 0 before, non-zero held | NVENC was unclocked too; the M68-M71 results need re-reading |
| clkrst errors | report it; the survey needs a different read path |

**Run B - `vic clk jpgdec wait=60`**, in a game, only if Run A shows NVJPG
clocked when held. One decode. Copy `sdmc:/nvjpg-dec.rgba` off the card and run
`python3 tools/nvjpg_dec_control.py check nvjpg-dec.rgba`. Completes and
matches: this process can drive a non-VIC engine and encoding is purely a
config problem. Stalls with the clock held: the problem is how this process
reaches the engine. **Risk:** if it stalls, a M74-style compositor wedge is
possible even with the channel left open; it would cost a forced power-off.

Do **not** arm `jpg` or `nvenc` in either run.
## *** M75: the observer never ran - a substring match re-armed the broken interceptor ***

M75 was meant to be the safe route: attach to grc, read its memory, detach.
No channel, no cmdbuf, no engine contact - the same mechanism already used on
the game's framebuffer at 60 fps without incident. The arm file was
`vic grcscan wait=90`.

The console would not launch a game, and needed a fourth forced power-off.

### The cause, from the log alone

```
[10.715] applet-mitm ... (jpg=off grc=ARMED)      <- believed off
[44.644] hb:11 ... vic=waiting                    <- last heartbeat
[45.407] nvdrv:s accept program=0100000000000035  <- the M72 interceptor, live
[46.285] indirect:probe_done                      <- last line of any kind
```

`ArmFileContains` was `strstr`. **"vic grcscan wait=90" contains "grc"**, so
`g_grc_armed` came up true and the M72 nvdrv:s IPC interceptor registered - the
one whose `Open` handler was already known to be broken from M72b's fatal. grc's
session was accepted at 45.4 s, grc called `Open`, the handler died, and the
game launch blocked behind grc. The observer's own first log line never appears
because the process was gone before it was reached.

**So the observer has still never been tested.** M75 re-ran M72's known bug by
accident, and the run says nothing whatever about reading grc's memory.

### What this changes about M72-M74

Nothing about their conclusions - those runs armed `grc` deliberately, or not at
all, and their evidence stands. But it does change the count: of the four forced
power-offs, **three were engine teardown and one was this**, and they are
unrelated faults. Lumping them together as "probing engines is dangerous" would
have been wrong.

### The fix

Whole-token matching, split on whitespace, with `key=value` matching on `key`.
Verified on the host against 18 cases including the exact failure
(`"vic grcscan wait=90"` + `"grc"` -> false) before going anywhere near the
console.

Any flag that is a prefix of another was a trap for the old parser: `grc` and
`grcscan`, `q` and `qN`, `jpg` and `jpgN`. The class is now closed rather than
the one instance patched.

Two guards added alongside, because a silent mis-arm is what made this expensive:

- **Every flag is dumped at boot**, by name and value, on one line. A mismatch
  between what was intended and what was parsed is now visible in the first
  second of the log instead of inferred from a hang.
- **The grc IPC interceptor refuses to register without `mitm.lst`.** boot2 only
  declares a future mitm for services listed there, so registering late is
  incoherent - and the failure mode is a console that will not launch a game.

### The lesson worth keeping

Four of the last five runs failed on something other than the hypothesis being
tested. Three on engine teardown, one on argument parsing. The hypotheses were
sound; the delivery kept failing. A probe that cannot be trusted to run only the
thing it claims to run is not a cheap experiment, it is an expensive one - and
the boot-time flag dump exists so that the next run proves what it armed before
it does anything else.
## *** M74: one failed submit is enough to wedge the compositor ***

M73 left one hypothesis standing: NVENC and NVJPG sit idle and are therefore
clock/power-gated, while the VIC is driven continuously by nvnflinger. That is
testable without guessing an ioctl - the system encodes every screenshot with
NVJPG - so M74 submitted one well-formed job per second for 90 seconds while
the user took screenshots.

The test did not get to run. **Two attempts completed in 21 seconds**, not the
90 planned, and the console wedged during the second.

### The timing, which is the real finding

```
63.387  attempt 1 submitted -> STALLED
63.5    hb:17  txn=3025
66.6    hb:18  txn=3377   (+352 in 3 s - the game is running normally)
69.7    hb:19  txn=3745   (+368)
72.8    hb:20  txn=4103   (+358)
75.8    hb:21  txn=4467   (+364)
79.1    hb:22  txn=4610   (+143)  <- the game starts to stall
82.1    hb:23  txn=4610   (  +0)  <- the game is frozen
84.354  attempt 2 returns -> STALLED
```

Two things follow, and both matter more than the gating question:

1. **Each attempt takes ~21 seconds, not the ~1.4 s designed.** The extra 20 s
   is inside channel teardown: `NvClose` blocks waiting for a job that will
   never finish, until nvservices' own default timeout fires and aborts. M74
   deliberately removed the 1000 ms `SET_SUBMIT_TIMEOUT` that M73 set, on the
   theory that frequent aborts were the danger. That was wrong - it did not
   avoid the abort, only delayed it.

2. **One stalled submit is enough.** The game froze at ~79 s, i.e. during the
   teardown that followed attempt 1, long before attempt 2 returned. The retry
   loop did not cause the damage; it only made it obvious. M73's four attempts
   and M74's two produce the same outcome.

### What this closes off

Any submit to these engines that does not complete leaves the channel in a state
whose teardown takes the compositor down with it. host1x is shared, and
nvnflinger is on the other side of it. **So these engines cannot be probed this
way at all** - not with a better config, not with fewer attempts, not with a
different timeout. Three forced power-offs across M72b, M73 and M74 are three
instances of the same mechanism.

The gating hypothesis is neither confirmed nor refuted. Attempt 1 landed at
63.4 s and attempt 2 at 84.4 s, and there is no way to know whether either
coincided with a screenshot. The experiment was sound; the delivery mechanism
made it unrunnable.

### Correction: the S5.16 colour-matrix theory was wrong

M73's writeup floated S5.16 coefficients as the likely cause of the three failed
colour-matrix attempts, based on `OCsc0MatCoeff[3][4]` in NVIDIA's CEB6 header.
**That does not apply to this chip.** CEB6 is a much later VIC; VIC 4.0's
`MatrixStruct`, which is what this console has, is a 20-bit signed coefficient
with a separate 4-bit `matrix_r_shift` - which is exactly the form the code
already uses. So the scale error I proposed is not there, and the matrix failure
remains unexplained.

Worth recording precisely because it was stated to the user as a likely root
cause before being checked against the right generation's header.
## *** M73: NVJPG fails identically to NVENC - the blocker is not the config ***

The compression path needs a hardware encoder. NVENC had resisted four probes
with its cause still unisolated, so M73 switched engines rather than keep
guessing at a 512-byte bitfield struct. NVJPG is a much smaller target for the
same job: ~10 methods against NVENC's 39-word job, no reference pictures, no IO
history, and **no magic/version word at all** - which removes the entire
ambiguity that stalled M68-M71. JPEG is also all-intra by construction, so it is
the lowest-latency compression available, and 1080p60 at quality 85 is roughly
12-24 MB/s against the measured 37 MB/s link.

### The result

```
nvjpg mem_mode=0 input_type=0 words=33 rc=0x0 nverr=0 fence 0/1 STALLED err=0 bytes=0 mcu=0x0 cycles=0
nvjpg mem_mode=0 input_type=1 words=33 rc=0x0 nverr=0 fence 1/2 STALLED err=0 bytes=0 mcu=0x0 cycles=0
nvjpg mem_mode=0 input_type=2 words=33 rc=0x0 nverr=0 fence 2/3 STALLED err=0 bytes=0 mcu=0x0 cycles=0
```

**Identical to NVENC in every respect**: submit accepted (`rc=0 nverr=0`), a
fence issued, the engine never completes, and the status buffer never written
(`cycles=0` is decisive - the engine did not burn a single cycle).

Each attempt used a **fresh channel**, which is the fix for the flaw that
invalidated M71. So this is not one hung job poisoning the rest.

### What this establishes, and what it kills

Two completely different engines, with completely different configuration
structures, one of which has no version word to get wrong, fail the same way.
**The blocker is not our NVENC configuration.** Everything from M68 to M71 that
treated it as a config problem was looking in the wrong place.

Note the fence column: 0/1, then 1/2, then 2/3. The syncpoint advances by
exactly one between attempts, never during them. That is consistent with
nvhost force-incrementing on channel teardown to release waiters, not with the
engine completing late.

### The remaining difference between VIC and these engines

Our VIC jobs complete reliably. VIC, NVENC and NVJPG all take the same host1x
submit path, and all three classes even declare the same `CTX_SAVE_AREA` /
`CTX_SWITCH` methods, which we set for none of them. So that is not the
difference.

What *is* different: **nvnflinger drives the VIC continuously.** It is powered,
clocked, firmware-booted and context-initialised by the compositor before we
ever touch it. NVENC and NVJPG sit idle, and an idle engine on Tegra is
clock/power-gated. A gated engine would behave exactly like this: the channel is
real, the syncpoint allocates, the submit is accepted, and nothing executes.

This is a hypothesis, not a finding. It has not been tested.

### Two bugs of mine in this run

- **The display wedged on the fourth attempt.** That case was `memory_mode=1`
  (planar) while `chroma_v_addr` stayed 0, so `SET_CUR_PIC_CHROMA_V` was never
  emitted - a planar encode with no V plane. M27's lesson was that a zero
  address does not fail politely on the VIC; it applies to every engine, and I
  did not apply it. The console needed a forced power-off. No fatal report was
  written, so the module hung rather than aborting.
- `SET_SUBMIT_TIMEOUT` was set to 1000 ms, so every stalled attempt ended in a
  channel abort. Repeated engine resets are a plausible contributor to the
  compositor corruption and should not be used while probing.

### What did work

The JPEG container and tables were verified on the PC *before* the run, which
is why the run cost nothing extra to diagnose:

- Wrapping libjpeg's own scan data in our headers decodes **pixel-identical**
  (max abs diff 0). That validates the quantisation tables, the quality scaling,
  zigzag order, DHT segments and marker structure together.
- The Huffman codes we hand the engine match the canonical standard values, and
  the AC symbol-to-index mapping is collision-free across all 176 entries.

So the moment any engine executes, the output is a correct, viewable JPEG.
## *** M72: the grc recorder - copy the one client that works (built, not yet run) ***

### First, M71 was over-read

M71 wrote up the ladder as killing the "missing surfaces" hypothesis and
leaving "Falcon firmware not booted" as the only survivor. That does not
follow. In every run the **first job carrying EXECUTE is the only informative
one**: if its config hangs the engine, every later job queues behind the hang
and stalls identically. That is also the better explanation of M69, where a
deliberately invalid magic behaved exactly like the real ones: the first
submit hung the engine and the other four never had a chance.

What is actually established: **the engine never completed the first job we
gave it.** "Firmware not running" and "our config hangs it" are
indistinguishable from where we have been measuring. And the firmware theory
has a fact against it: grc (`0100000000000035`), the system game recorder that
SysDVR reads from, drives this same engine on this same firmware.

### The design

Stop guessing at a 512-byte bitfield struct and record the client that works.

- **Logging-only mitm on `nvdrv:s`**, `ShouldMitm` true for grc only and only
  when the arm file contains `grc`. Every request is forwarded unchanged.
- **`mitm.lst`** (`nvdrv:s`) in the contents directory: boot2's
  `DetectAndDeclareFutureMitms` declares it before launching anything, so grc's
  first session waits for us instead of racing past. Price: every `nvdrv:s`
  client, vi included, waits until we register, so registration is the first
  thing `main` does after `LogInit`. We use `nvdrv:t` ourselves, so there is no
  self-deadlock.
- **Own ServerManager, four LoopProcess threads**, separate from `vi:u`. A grc
  call blocked in nvservices (a syncpoint wait) ties up one grc thread and can
  never stall the game's binder traffic.
- Explicitly forwarded (so the reply is visible): `Open` (fd to path), every
  msenc ioctl, nvmap `CREATE` (handle to size) and `ALLOC` (handle to grc VA).
  Everything else goes back with `ResultShouldForwardToSession`, i.e. native
  forwarding. `Ioctl2`/`Ioctl3` are recorded on msenc fds and forwarded natively.
- **On each of the first 8 msenc submits**: attach to grc
  (`DebugActiveProcess`, drain, `ContinueDebugEvent`), read each cmdbuf via
  handle to VA, replay the host1x register writes to find method 0x710
  (`SET_IN_DRV_PIC_SETUP`), translate that IOVA through the recorded
  `MAP_CMD_BUFFER` results to a grc VA, and read 0x1000 bytes of setup. Detach
  by closing the handle. grc is blocked in our handler throughout.
- Records go to a 256 KB RAM buffer; the heartbeat thread appends them to
  `sdmc:/nvenc-grc.bin` every 3 s, so the IPC path never touches the SD card.
- Decoding: `tools/nvrec.py` (timeline, every method named from `clc5b7.h`)
  and `tools/nvsetup-dump.c` (the setup struct, via NVIDIA's own header; the
  size asserts pass on x86-64). Both smoke-tested on a synthetic record file.

### Risks, stated before the run

- `Ioctl2`/`Ioctl3` buffer layouts are taken from libnx's `nvIoctl2`/`nvIoctl3`
  (In,In,Out and In,Out,Out). If grc uses a different shape, sf rejects the
  request, grc gets an error, and a Nintendo sysmodule may abort on it, i.e. a
  fatal naming grc. Recoverable, and informative.
- If the module dies before registering, every `nvdrv:s` client waits forever:
  a boot that never reaches the home menu. Recovery as always: delete
  `atmosphere/contents/0100000000000C20/` from a PC.

## *** M71: the NVENC ladder answers it - the engine never completes ***

M69's magic sweep taught nothing because every case, including a deliberately
invalid control, behaved identically. That is the signature of a job that never
runs, and varying the CONTENTS of a job that never runs cannot explain why.

So M71 stopped varying content and varied **structure**: five submits on one
channel, each adding exactly one layer, the first four ending with an IMMEDIATE
syncpoint increment that host1x performs as it retires the opcode regardless of
the engine. A fence that moves means "host1x got this far".

```
L0  bare INCR_SYNCPT (no class, no engine)   words= 2  fence 4009/4009  REACHED
L1  + SETCL class 0x21                       words= 3  fence 4011/4011  REACHED
L2  + SET_APPLICATION_ID                     words= 6  fence 4013/4013  REACHED
L3  + full surfaces + EXECUTE                words=39  fence 4015/4015  REACHED
L4  same, INCR on OP_DONE                    words=39  fence 4015/4017  STALLED
```

L3 and L4 are the **same 39 words** and differ only in the increment condition.
L3 retires; L4 never fires. So:

- host1x runs our cmdbufs on the msenc channel, and the class switch, the
  methods and the full job are all accepted and retired.
- The engine receives fully-formed work and **never signals OP_DONE**.

### Two of the three M68 hypotheses are now dead - **OVER-READ, CORRECTED IN M72**

> The conclusions below do not follow from the ladder. The first job carrying
> EXECUTE is the only informative one in any run; if its config hangs the
> engine, every later job stalls behind it identically. All that is
> established is that the engine never completed the first job it was given.
> See M72.

L3/L4 carried a complete H.264 all-intra IDR setup at 256x128 - populated
sps/pps/rc/pic_control, slice+ME+MD+quant control blocks at their offsets, and
every surface the firmware can dereference (input NV12, reference luma and
chroma, IO history, bitstream, status). Bitstream stayed all-zero, `bits=0`.

- **(3) "the engine validates surfaces before starting" is DISPROVEN.** Giving
  it every surface changed nothing at all.
- **(2) "EXECUTE is encoded wrongly" is unlikely.** The identical word retires
  fine through host1x; a malformed EXECUTE would be a method write like any
  other, and L2 proves method writes land.
- **(1) "the Falcon microcode is not booted" is what is left**, and it now fits
  every observation: a live channel, a live host1x path, and an engine behind it
  running no ucode, which therefore can never produce an OP_DONE.

`SET_UCODE_STATE` (0x50C) is still unused. The next experiment is firmware boot,
not job configuration.

### SuperSpeed: the BOS was missing, and it was still not enough

M58 declared USB 3.0 device and endpoint descriptors and the link kept training
to High; I read that as a cable or console limit. It was neither in the sense I
meant: USB 3.0 enumeration requires a Binary Object Store carrying a SuperSpeed
Device Capability descriptor, and `usbDsSetBinaryObjectStore` was **never called
at all**. Atmosphere's haze calls it at usb_session.cpp:220 and negotiates SS.

Added, accepted, and the link **still trains to High**:

```
usbDsSetBinaryObjectStore rc=0x0
*** NEGOTIATED SPEED = 3 (High(480Mbps)) *** rc=0x0
```

Our descriptor set now matches haze's, so the device side is no longer a
candidate. That leaves the cable (or the port), which is the one variable this
project has never controlled. **Untested, and cheap to test.**

### 800x450 corrupts, and the reason is alignment

The link-speed picker chose 800x450 (the largest size M70's 37 MB/s wall
supports at 60 fps) and it ran at **59.7 fps** - the frame rate target is met.
But the picture tore into vertical bands and horizontal stripes.

768 and 1280 were both clean. 768 = 64 x 12, 1280 = 64 x 20, and **800 = 64 x
12.5**. The Tegra GOB is 64 bytes wide, so a width that is not a multiple of 64
puts the VIC's output stride out of step with the copy. This is a width
constraint the pipeline has always had and never had to notice, because every
size tried until now happened to satisfy it.

**Every stream width must be a multiple of 64.** Nearest legal sizes bracketing
the 60 fps budget: 768x432 (497,664 B, 29.9 MB/s) and 832x468 (584,064 B,
35.0 MB/s, marginal). The honest best-known-good 60 fps config stays 768x432.
## *** M70: the USB 2.0 ceiling, measured - raw pixels are finished ***

One run, five resolutions, 300 frames each, live gameplay:

| resolution | B/frame | fps | usb wait | effective |
|---|---|---|---|---|
| 768x432 | 497,696 | **58.0** | 140 us | 28.9 MB/s |
| 896x504 | 677,408 | 52.2 | 1,064 us | 35.4 MB/s |
| 960x540 | 777,632 | 46.4 | 1,925 us | 36.1 MB/s |
| 1152x648 | 1,119,776 | 32.1 | 7,823 us | 35.9 MB/s |
| 1280x720 | 1,382,432 | 27.4 | 12,796 us | 37.9 MB/s |

**The link saturates at ~37 MB/s.** M67's "usb wait = 120 us therefore >= 43
MB/s" was a lower bound from a wait that never blocked, and it was optimistic;
the true figure is lower. Watch `usb wait` go from 140 us to 12,796 us across
the sweep - at 720p a third of every frame is spent waiting on the cable.

The console side barely moves: read stays ~7 ms at every size, VIC 2.1 -> 3.0 ms.
Only `copy` grows (1,476 -> 5,282 us), and that is the memcpy out of the
UNCACHED VIC destination - uncached reads are slow, and it is worth making that
buffer cacheable.

### The 60 fps ceiling

60 fps needs <= ~33 MB/s sustained, i.e. ~550,000 B/frame, i.e. ~366,000 pixels:

```
768x432  497,664 B  29.9 MB/s  OK      832x468  584,064 B  35.0 MB/s  over
800x450  540,000 B  32.4 MB/s  OK      854x480  614,880 B  36.9 MB/s  over
```

So **~800x450 is the maximum at 60 fps**, 1.09x the pixels of what already
ships. There is no meaningful resolution left to win this way.

And at 768x432 we are no longer transport-limited at all: 11.3 ms of work
against a 16.7 ms budget, so the 58 fps IS the game's own rate. The pipeline has
headroom it cannot spend.

### The picture quality problem, named

The user's word for 1280x720 was "pixelated", and that is a real artifact of the
packed-420 trick rather than of the resolution. **We subsample R and G, not
chroma.** Proper 4:2:0 discards colour detail the eye barely registers; we are
discarding two thirds of the red and green signal, which it very much does.

So the colour matrix is not only a correctness question - it is worth real
perceived quality at identical bandwidth. Three attempts failed (M64 output
matrix, M66 slot matrix, both collapsing luma to a constant), and the constants
4096>>10=4 and 32768>>10=32 say the offset column lands while every coefficient
term reads as zero. That is a coefficient-encoding problem, still unsolved.

### Where native resolution has to come from

Raw 1080p60 packed-420 is 186.6 MB/s against a measured 37. Nothing about the
capture, the engine or the CPU is in the way - 1080p60 H.264 all-intra at
50 Mbps is 6.25 MB/s, a sixth of what the cable already carries.

Two doors, both still shut:
- **NVENC** - accepts cmdbufs, never executes (M68-M69). Next test: give it a
  real input surface, reference pictures and IO history, on the theory that it
  validates all surfaces before starting.
- **SuperSpeed** - descriptors accepted, link still negotiates High. Untested
  against a known USB 3.0 cable.
## *** M68-M69: NVENC accepts cmdbufs but does not execute ***

NVIDIA's `nvenc_drv.h` is MIT, so the H.264 half is **vendored verbatim** rather
than transcribed - `nvenc_h264_drv_pic_setup_s` is 512 bytes of bitfields with
five sub-structures at offsets, and a hand-copy error would surface only as a
firmware rejection with no clue which field was wrong. Every struct size matches
NVIDIA's own comments under our compiler:

```
surface_cfg 32   rc 88   slice_control 128   me_control 192
md_control 128   quant_control 192   pic_control 276   drv_pic_setup 512
```

### The probe, and why the control mattered

M68 filled the setup struct with nothing but the magic, submitted on the msenc
channel with the class-0x21 method table, and read back:

```
fence NOT reached;  error_status=1  ucode_error_status=0x00000000
```

and logged *"NONE - the engine accepted the job"*. **That line was wrong.** An
unwritten status buffer reads as zero too, and the fence had not moved, so the
zero meant nothing. Same failure of reasoning as the 119 ms VIC claim: treating
an absence of evidence as evidence.

M69 swept every candidate magic **plus a deliberately invalid control**:

| magic | fence | ucode |
|---|---|---|
| 5.0 `0xd0b70006` | NOT reached | 0 |
| 6.0 `0xc1b70006` | NOT reached | 0 |
| 1.0 `0xc0b70006` | NOT reached | 0 |
| MSENC 2.0 `0xa0b70006` | NOT reached | 0 |
| **CONTROL `0xDEADBEEF`** | **NOT reached** | **0** |

The invalid control behaves **identically** to every real candidate. The
firmware never reaches the magic check, so the job is not executing at all and
the sweep says nothing about which generation this engine is.

### What is actually established

- The msenc channel accepts our submits: `rc=0 nverr=0`, a fence is issued.
- The engine never signals completion - the syncpoint does not advance.
- Probing is **safe**: five submits with garbage configs, and the game presented
  continuously throughout (`txn` 3991 -> 6167 across the sweep). Unlike the VIC,
  a malformed NVENC job does not take the compositor down.

### Candidate explanations, untested

1. **The Falcon firmware is not booted.** NVENC runs microcode that nvservices
   loads when a real client opens the channel; opening `/dev/nvhost-msenc` and
   submitting may not be enough. `SET_UCODE_STATE` (0x50C) exists in the method
   table and is unused here.
2. **EXECUTE is encoded wrongly.** `1u << 8` was copied from the VIC convention;
   NVC5B7_EXECUTE has its own NOTIFY/AWAKEN field layout.
3. **The engine validates all surfaces before anything else**, so a job with no
   input picture, no reference pictures and no IO history never starts. This is
   the most likely of the three, and the cheapest to test: point
   `SET_IN_CUR_PIC` at the NV12 the VIC already produces, give it reference and
   history buffers, and see whether the fence moves.

(3) is the next experiment. Note it needs NV12 input - which M67 already makes
at 2.5 ms/frame, so that dependency is satisfied.
## *** M67: A PLAYABLE STREAM - 768x432 at 59.6 fps ***

```
stream: 3600 frames sent in 60315 ms -> 59.6 fps  (18 presented frames dropped)
stream: avg per frame - read 7426 us, vic 2464 us, copy 1698 us, usb wait 120 us
stream: queue counter 6967 -> 10587 over the run; 0 stale iterations
stream: frame time best 6982 us, worst 23640 us  (60 fps budget = 16667 us)
```

A full minute of gameplay, locked to the game's own frame rate, with **0 stale
iterations** - we kept up with every frame the game presented. Reported by the
user as fully playable, with latency low enough to drive from.

| stage | cost | note |
|---|---|---|
| capture + 8.8 MB cache flush | 7,426 us | the flush is effectively free |
| VIC scale + pack | 2,464 us | 1920x1080 -> 768x432 |
| stage out of uncached nvmap | 1,698 us | |
| USB wait | 120 us | overlapped, nearly free |
| **total** | **11,708 us** | of 16,667 |

### What made it possible

Three things, all from M63-M66:

1. **The VIC is 1-2 ms, not 119 ms.** M59's figure was ~7 SD-flushing log calls
   and a 65,536-iteration checksum inside the timed region. With them gone the
   engine is cheap enough to run every frame.
2. **1.5 bytes/pixel instead of 4.** The VIC writes the NV12 plane layout, and
   although it performs no colour conversion the packing is recoverable: luma
   plane = B at full resolution, chroma even/odd = R/G at half. That is 4:2:0
   subsampled RGB, and the host undoes it for free. 2.67x on the wire for no
   console cost and no colour matrix.
3. **The stale bail had to be loosened.** At 240 iterations (~5 s) a Mario Kart
   loading screen ended the run: the queue counter moved 14 frames in 7.6 s and
   the stream stopped, which looked exactly like a freeze. A loading screen is
   not a dead game. Raised to ~25 s.

### The constraint is now USB, and only USB

497,664 B/frame x 59.6 = **29.7 MB/s**, against roughly 31 MB/s of usable USB
2.0 bulk. The console-side pipeline has **~5 ms of headroom per frame it cannot
spend**, because there is nowhere to put the bytes.

| what | bytes/frame | at 60 fps |
|---|---|---|
| 768x432 packed-420 (shipping) | 497,664 | 29.9 MB/s - at the ceiling |
| 1920x1080 packed-420 | 3,110,400 | 187 MB/s - 6x over |
| **1920x1080 H.264 all-intra @ 50 Mbps** | **~104,000** | **6.25 MB/s** |

Compression does not merely help: it makes the link a non-issue and hands the
spare milliseconds back to resolution. That is the whole remaining gap.

### Why all-intra is the right codec here

Every frame independent: no B-frames, no reordering, no inter-frame dependency,
so a dropped frame costs one frame rather than a GOP. That is the lowest-latency
H.264 configuration, and latency is the property this project is optimising.
It also removes reference-frame management from the NVENC setup entirely.
## *** M63: THE VIC WAS NEVER SLOW - I measured my own logging ***

M59 reported 119,023 us per full-frame VIC blit and concluded **"the VIC is
unusable as a per-frame stage."** That went into this file, the README, the
commit log and the GBAtemp thread as a property of the hardware.

**It is wrong.** Here is what was inside the timed region, per job:

| cost | what it is |
|---|---|
| ~7 x `LogLine` | each one `fs::OpenFile` + `WriteFile(WriteOption::Flush)` + `CloseFile` **against the SD card**. An SD flush is 5-20 ms. |
| `NvOpen`/`NvClose` | `/dev/nvhost-ctrl` opened and closed **every job** |
| 65,536-iteration loop | byte-by-byte checksum of the output buffer |
| `memset` + hex format | poison-fill and a 32-byte hex dump |

Seven SD flushes alone is 35-140 ms. **That is the 119 ms.** The engine was
never measured at all.

The compositor starvation has the same cause. It was not contention for the VIC
or its syncpoint: it was blocking SD-card I/O in a tight loop on core 3, which
is where our own IPC thread also lives (M61). Two "hardware findings" collapse
into one instrumentation bug.

### The lesson, stated plainly

**Never time a region that contains a log write.** Every per-frame diagnostic in
this project writes to the SD card with an explicit flush, by design, because a
crash must not lose the tail. That design is right for one-shot probes and
catastrophic for anything measured in a loop.

`g_vic_quiet` now gates the whole diagnostic path. When false, the one-shot
probes behave exactly as before and still produce their evidence. When true,
the job is submit + wait and nothing else, `/dev/nvhost-ctrl` stays open for the
life of the process, and timings accumulate in memory to be logged once at the
end.

### What this reopens

The VIC is the natural RGBA -> NV12 converter, and NV12 is what NVENC requires
as input. M59's wrong conclusion had closed that door and forced the CPU
point-sampler; the door is open again.

## NVIDIA published the headers we needed

`ref/open-gpu-doc` (`git clone https://github.com/NVIDIA/open-gpu-doc.git`),
`classes/video/`:

**1. VIC output chroma offsets, confirmed rather than guessed.** libdrm defines
only `SET_OUTPUT_SURFACE_LUMA_OFFSET`, which is why M59 refused NV12 output - a
wrong register hangs the VIC and takes the compositor with it. `clceb6.h`
defines all three:

```
SET_OUTPUT_SURFACE_LUMA_OFFSET      0x720   <- identical to the value we proved on NVB0B6
SET_OUTPUT_SURFACE_CHROMA_U_OFFSET  0x724
SET_OUTPUT_SURFACE_CHROMA_V_OFFSET  0x728
```

The luma match matters: `NVCEB6` is many generations newer than our `NVB0B6`,
and its offset is byte-identical to one we verified on hardware. **The host1x
method ABI is stable across generations**, which is what licenses using the
other two, and by extension the NVENC table below.

**2. The complete NVENC method table** (`clc5b7.h`) - `SET_APPLICATION_ID`
(0x200, H264=1), `SET_CONTROL_PARAMS` (0x700, H264=3, CONSTQP=0),
`SET_IN_DRV_PIC_SETUP` (0x710), `SET_IN_CUR_PIC` (0x734) / `_CHROMA_U` (0x740),
`SET_OUT_BITSTREAM` (0x71C), `SET_OUT_ENC_STATUS` (0x718),
`SET_OUT_REF_PIC_LUMA` (0x730), `EXECUTE` (0x300), plus the full error enum.

**3. The driver structures for our generation** (`nvenc_drv.h`, 275 KB),
version-gated back to `NV_NVENC_1_0`. The magic encodes the class:

```
NV_NVENC_5_0  0xd0b70006      NV_NVENC_6_0  0xc1b70006
NV_NVENC_1_0  0xc0b70006      NV_MSENC_2_0  0xa0b70006
```

Tegra X1 is GM20B, so our engine is in that table. The firmware **validates the
magic and errors out** (`...EncErrorH264BadMagic`) rather than hanging, so
probing which version the Switch's firmware accepts is safe by construction.

`nvenc_h264_drv_pic_setup_s` is 512 bytes with sub-structs at offsets - the same
shape as the VIC config struct we already program correctly.

**4. NVJPG is a dead end here.** `nvjpg_drv.h` exists, but switchbrew's
`NV_services` lists `/dev/nvhost-nvjpg` on this firmware as **JPEG Decoder**.
Hardware JPEG *encode* arrives on Xavier, not X1. MJPEG would have been ideal
for latency - independent frames, no reordering - and it is simply not available.

## M63 build

- `g_vic_quiet`, persistent `/dev/nvhost-ctrl`, in-memory timing accumulators.
- **NV12 output**: `PIXFMT_Y8_U8V8_N420` (67), chroma plane dimensions in
  `FillOutputConfig`, and `SET_OUTPUT_SURFACE_CHROMA_U_OFFSET` emitted in the
  cmdbuf. Luma bytes must be 256-aligned because host1x carries addresses
  shifted right by 8; every resolution used here satisfies that.
- `VicDstSize` 1 MB -> 3.5 MB so a full 1920x1080 NV12 frame (3,110,400 B) fits.
- Heap ladder now reaches **24 MB**. M55 measured 411,260 KB free in the Applet
  resource-limit group, so 16 MB was never a ceiling, just the top rung.
- `bench` arm token: 60 quiet VIC jobs at 640x360 in RGBA, then again in NV12,
  reporting submit+wait average and worst, plus wall time per frame against the
  16,667 us budget. Dumps the NV12 result to `sdmc:/applet-mitm-nv12.bin`;
  `tools/nv12topng.py` renders it so correctness is checked on the PC rather
  than asserted on the console.

### What the bench decides

If quiet VIC jobs come back in single-digit milliseconds, the architecture
changes: VIC does RGBA -> NV12 (2.67x less data, for free, in hardware), and the
NVENC path has its required input format. If they are still ~119 ms with the
logging gone, then M59's conclusion was right for a reason I have not found and
the CPU path stays.

Either way the number is now being measured rather than inferred.
## *** M61-M62: the stream runs, and the bottleneck is now the cable ***

M61 is the run that made it usable, and the bug it fixed was ours, not the
platform's.

### M60's stream froze the game, and it was CPU starvation

```
[62.112] hb:17  txn=3259      game presenting normally
[63.861] st:1_loop            stream starts
[65.152] hb:18  txn=3511      frozen
[71.188] hb:20  txn=3511      still frozen, 8 s later
[72.209] stream stops
[74.208] hb:21  txn=3717      presenting again, instantly
```

The game stopped for exactly the duration of the loop and resumed the moment it
ended. Not a debug-event stall - `GAME FROZEN FOR 0 ms` and `drained 35 debug
events` show the resume worked.

**Every thread in this process is pinned to core 3** (`kernel_flags`
`lowest_cpu_id 3, highest_cpu_id 3`), *including the mitm IPC thread that
answers the game's `vi:u` calls*. The stream loop burned ~10 ms of solid CPU per
iteration at the **same priority** as that IPC thread and immediately looped, so
the game blocked on a binder call we were never scheduled to answer.

Three fixes:

1. **Worker priority = main + 4** (higher number is lower priority on Horizon),
   so IPC preempts the worker the instant a request arrives.
2. **An explicit 2 ms sleep per iteration**, guaranteeing a scheduling window.
3. **Removed `armDCacheFlush(g_ind_buf, FbSlotSize)`** - 8.8 MB, i.e. 138,240
   cache-line operations, 60 times a second. It existed so the VIC could see our
   writes through the SMMU; the CPU point-sampler reads its own cached writes
   coherently. Pure waste since M60.

Confirmed working on hardware at 480x270.

### M62: 640x360, and the honest ceiling

`StreamStageSize` raised 0x100000 -> 0x220000 so 960x540 (2,073,600 B) fits,
guarded by a `static_assert` against the 16 MB heap. `wait=0` starts the stream
on the game's first presented frame.

| output | B/frame | USB-limited | CPU-limited | measured |
|---|---|---|---|---|
| 480x270 (4x) | 518,400 | ~60 fps | ~85 fps | **59.4 fps** |
| 640x360 (3x) | 921,600 | 34-43 fps | ~65 fps | **lags** |
| 960x540 (2x) | 2,073,600 | ~15 fps | ~43 fps | not tried |

**USB 2.0 bulk is now the only constraint.** 518,400 B x 59.4 = 30.8 MB/s is
about what the link delivers, and no arrangement of raw pixels gets 720p60
(83 MB/s in NV12, 221 MB/s in RGBA) through it.

The point sampler needs an exact uniform divisor of 1920x1080, so only 4x, 3x
and 2x are available - that constraint is the sampler's, not the transport's.

### Where the remaining speed has to come from

Raw pixels are finished as a strategy. The two doors, neither opened yet:

- **SuperSpeed.** Descriptors are accepted (`rc=0x0`) and the link still
  negotiates High. The PC is ruled out - its root hubs report 10000/20000 Mbps -
  so it is the cable or the console's device-mode capability. One USB 3.0 device
  would settle it in thirty seconds.
- **Compression (NVENC).** H.264 at 20 Mbps is 2.4 MB/s, which fits USB 2.0
  sixteen times over. Class id 0x21 is known, the method table is not, and
  nouveau has no Tegra NVENC support - so REing nvservices is the route.

Note the awkward interaction: **NVENC wants NV12 input and the VIC is the
natural way to produce it, but M59 proved the VIC cannot be used per-frame**
without starving the compositor it shares. A CPU RGBA->NV12 conversion, or a
VIC job cheap enough not to contend, is an unsolved sub-problem of the encode
path.

## *** M58-M60: A WORKING 60 fps STREAM ***

```
stream: 480x270, 518400 B/frame
300 frames  59.4 fps  worst gap 44.4 ms
```

Live video from the console to the PC over USB, at **59.4 fps**, with the
console still running afterwards. Every stage is now proven together: debug-SVC
capture, downscale, USB transport, live display.

### M58: SuperSpeed is declared, and still negotiates High

`usb:ds` accepts the SuperSpeed descriptors and the endpoint companions
(`device/configuration descriptors (Full+High+Super) rc=0x0`), enumeration is
unaffected, and `usbDsGetSpeed` still reports `3 (High, 480 Mbps)`.

M52-M57 only ever declared Full and High, so the host had nothing better to
negotiate - the 480 Mbps ceiling was ours. haze declares Super
(`usb_session.cpp:144`), which proves the console supports USB 3.0 device mode.
The remaining variables are **the cable** (most USB-C cables are USB 2.0 only and
physically lack the SS pairs) and the console's device-mode capability. The PC is
ruled out: its root hubs report 10000/20000 Mbps. Untested because no second USB
3.0 device was available to prove the cable.

### M59: the VIC is unusable as a per-frame stage

The first streaming loop worked and was far too slow:

```
180 frames sent in 27036 ms -> 6.6 fps
avg per frame - read 5484 us, vic 119023 us, copy 1662 us, usb wait 602 us
frame time best 52947 us, worst 1622498 us
```

**119 ms per full-frame VIC blit** - 7x the entire 60 fps budget. And the worse
half: nvnflinger composites on the SAME engine and the SAME syncpoint 12 we
submit to, so a tight blit loop starved the compositor. The queue counter moved
1495 -> 1598 across 27 s, i.e. **the game fell to 3.8 fps and never recovered**;
the console needed a power cycle.

The risk was named in this file before the run and the loop was written anyway.
**The VIC is fine for a one-shot blit and must not be in a per-frame path.**

> **RETRACTED IN M63.** The 119 ms was ~7 SD-flushing `LogLine` calls and a
> 65,536-iteration checksum *inside the timed region*, plus an `NvOpen`/`NvClose`
> of `/dev/nvhost-ctrl` per job. The engine was never measured, and the
> compositor starvation was blocking SD I/O in a tight loop, not engine
> contention. See the M63 section at the top of this file.

### M60: CPU point-sample, no engine at all

At an exact 4x reduction the block-linear arithmetic is unusually kind: output
pixel x lands on byte 16*x, which is always a 16-byte group boundary, so each
output pixel is one aligned 4-byte read straight out of the capture. No engine,
no syncpoint, no contention.

```
480x270 from 1920x1080, 300 frames, 59.4 fps, worst gap 44.4 ms, console healthy
```

It is a point sample - no filtering, so it aliases. Frame rate first.

USB is now the bottleneck: 518,400 B x 59.4 = **30.8 MB/s**, near what USB 2.0
bulk realistically delivers. 480x270 RGBA is therefore about the most this
transport carries at 60 fps; 640x360 (921,600 B) lands near 40 fps.

### Three bugs worth not repeating

1. **The stream loop must sit BELOW `resume_game()`.** Placed above it, the game
   is still halted by `DebugActiveProcess`: the loop waited 514 ms for a frame
   that could not arrive, reported "game stopped presenting at frame 0", and was
   itself most of the 613 ms freeze it measured.
2. **Do not gate capture on `g_queue_count`.** Two runs died on that check while
   the heartbeat showed the game emitting ~109 binder txns/s across the same
   window. The counter's increment path is unconditional for `code == 7`, so the
   assumption about what it tracks was wrong, not the code. Capture now proceeds
   regardless and the counter is used only for slot choice and drop accounting.
3. **Header and payload must be SEPARATE URBs.** Combining them into one
   518,432-byte post produced `LIBUSB_ERROR_OVERFLOW` on the host: USB delivers
   one URB as 512-byte packets, so a 32-byte header read overflows. M57 worked
   because it posted them separately; the "optimisation" broke the framing.

Also: `vic` must stay in the arm file even though the VIC engine is no longer
used - `g_vic_armed` gates the probe trigger itself
(`applet_mitm_service.cpp:119`), so without it nothing fires at all.

### A tooling note, because it cost a build

Splicing a moved code block with `s[:start] + new + s[end:]` silently duplicates
`[end, start)` when `end < start` - which is exactly what happens after the block
has been moved earlier in the file. It duplicated 122 lines including the strip
capture and the sustained-capture loop, and only surfaced as a compile error.
**Assert the ordering before splicing, or match on unique anchors.**

## *** M57 RUN: a real frame left the console over USB ***

Capture, the 16 MB heap and the USB transport joined end to end for the first
time. `/tmp/sft_000.bin` arrived at **8,847,360 bytes - exactly FbSlotSize, not
one byte short**, so the transport is lossless. De-swizzled it is a clean
Mario Kart 8 Deluxe frame: correct colours, no channel swap, no swizzle
artefacts, HUD and minimap intact.

```
frame 0: 1920x1080 stride=7680 kind=0xfe blk_h_log2=4 payload=8847360 B
/tmp/sft_000.bin: 8,847,360 bytes (9.00 block-rows)
wrote /tmp/frame.png  (1920x1080, 3,683,587 nonzero bytes)
```

### The game was rendering 720p, not 1080p

Measured, not eyeballed: the non-black bounding box is **exactly 1280x720** in
the top-left of the 1920x1080 surface - a 0.667 ratio on both axes, far too
clean to be coincidence. MK8D renders 720p **undocked** into a swapchain
allocated at 1080p, and the compositor scales on output.

So M57 proves the pipeline, **not** native-res capture. What we capture is the
game's render target, and that is whatever the game chose. A docked re-run is
the test that settles it, and it needs no rebuild.

### Two verification lessons

**`strings` on a .nsp proves nothing.** All five M57 literals showed 0 hits in
the shipped NSP and 1-6 hits in the ELF. The NSO header reads `flags 0x3f` -
text, rodata and data all compressed. Same false alarm as M53's split banner,
opposite direction. **Check the ELF, never the NSP.**

**A grep against a missing binary reports success.** `aarch64-none-elf-objdump`
exists only inside the devkitPro container, so my host-side call-site check ran
against a failed command; `grep -c` printed `0` and exited 0, so the `||`
fallback never fired. I nearly read that as "the transport was dead-stripped".
Run objdump **in the container**:

```
docker run --rm -v $PWD/ref/Atmosphere:/ams devkitpro/devkita64:latest \
  bash -lc '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-objdump -d /ams/<elf> | grep bl'
```

It found the three real call sites - one `UsbReady` guard, two `UsbSendBuffer`
(header, then body).

### Transport plumbing

- `iface`/`ep_in`/`ep_out` were **locals** in `TryUsbEnumerate`, discarded on
  return. That is why M52/M53 could enumerate but never transmit. Now file-scope.
- `UsbReady()`/`UsbSendBuffer()` live outside the anonymous namespace so
  `applet_mitm_nv.cpp` can reach them - the `LogMemoryPools` lesson from M54.
- Send sequence follows haze (`usb_session.cpp:250,256-258`): `PostBufferAsync`
  -> wait `CompletionEvent` -> `eventClear` -> `GetReportData` -> `ParseReportData`.
- 256 KB chunks: a multiple of 0x1000, so every boundary stays aligned. A short
  completion is treated as fatal rather than silently misaligning the next post.
- `g_ind_buf` is normal **cached** memory (the uncached `SetMemoryAttribute`
  calls cover only the four VIC buffers below `VicBufsEnd`) and sits at
  `addr+0x30000`, satisfying both usbDs requirements. Sending from the uncached
  VIC buffers would fail the way `fs::WriteFile` did with `0xd401`.
- The game is already resumed before the transport runs, so the send costs it
  nothing.

## The 60 fps bandwidth problem, with numbers

This is the whole remaining question, so here is the arithmetic rather than
adjectives. One 1080p frame = 1920x1080 = 2,073,600 px.

| format | bytes/frame | 1080p60 needs |
|---|---|---|
| RGBA (what we send today) | 8,294,400 | **474 MiB/s** (3.98 Gbps) |
| NV12 / YUV420 (VIC can output this) | 3,110,400 | **178 MiB/s** |
| H.264 @ 20 Mbps | ~41,000 | **2.4 MiB/s** |

Against the transports:

| transport | realistic | verdict |
|---|---|---|
| USB 2.0 High Speed (what we have) | ~40 MB/s | RGBA 1080p = **5 fps**; NV12 1080p = 13 fps; **NV12 720p30 fits** |
| USB 3.0 SuperSpeed | ~350 MB/s | **NV12 1080p60 fits with 2x headroom** |
| 802.11ac Wi-Fi | 12-25 MB/s | **worse than USB 2.0** |
| H.264 over USB 2.0 | 2.4 MB/s needed | fits with ~16x headroom |

### Wi-Fi Direct is a downgrade, not an option

The console's 802.11ac tops out around 100-200 Mbps real-world, i.e. 12-25 MB/s
- **less than half** of what USB 2.0 already gives us, with worse latency and
jitter. `ldn` local wireless is more restricted still. Wi-Fi only becomes viable
*after* compression, and once you have compression USB 2.0 is already plentiful.
So it solves nothing that is not already solved by the thing it depends on.

### Two real paths, and one of them is cheap to test

**Path A - USB 3.0 + NV12, no encoder.** We have only ever declared
`UsbDeviceSpeed_Full` and `UsbDeviceSpeed_High` in `TryUsbEnumerate`. We never
offered `UsbDeviceSpeed_Super`. **The 480 Mbps we measured may be our own
ceiling, not the platform's.** If SuperSpeed enumerates, NV12 1080p60 needs
178 MiB/s against ~350 MB/s available - and the VIC already does RGBA->NV12
conversion in hardware for free, which is a 2.67x reduction we are not taking.
Test cost: add Super descriptors + endpoint companion, boot, read
`/sys/bus/usb/devices/*/speed` for `5000`. Needs a USB 3.0 cable.

**Path B - NVENC.** Class id 0x21 is known; the method table is not, and there
is no public reference (nouveau has no Tegra NVENC support, as Souldbminer
pointed out - REing nvservices is the honest route). NVENC also *wants* NV12
input, so the VIC work in path A is a prerequisite either way.

Do A first: it is one descriptor change against an unknown-method-table research
problem, and it may remove the need for B entirely.

## *** M56 RUN: 16 MB held in a sysmodule - and it is not free ***

The ladder was `{16, 12, 10, 8, 4, 2}` MB, descending. It never descended:

```
SetMemoryHeapSize(16 MB) rc=0x0
[before heap grab] pool 1 Applet  used= 100740 KB  free= 411260 KB
[after  heap grab] pool 1 Applet  used= 117124 KB  free= 394876 KB   (+16384 = exactly 16 MB)
[before heap grab] pool 2 System  used= 229140 KB  free=   8588 KB
[after  heap grab] pool 2 System  used= 229140 KB  free=   8588 KB   (flat to the KB)
```

**A sysmodule is holding 16 MB — twice a 1080p frame — with the System pool and
the System resource limit untouched.** Console ran 259 s with the game live,
`txn=24099`, `vic=vb:released`, no fatal. VIC healthy: `job_fill` `OP_DONE` and
wrote our memory, `job_blit_self` submitted, `nvdrv:t` still full mask.

M50's claim is now properly retired, and it took all three of: pool_partition 1
(M54), application_type 2 (M55), and actually taking the memory (M56).

```
capture region 16192 KB vs one 1080p frame 8100 KB -> FITS
stage buf at +960 KB; full-frame capture would OVERLAP staging
```

### The cost, measured

A 16 MB heap makes the one-time VIC setup path **~6x slower**, and every
`SetMemoryAttribute`/`nvmapOwn` step **~10x slower**:

| | M54 (2 MB) | M55 (2 MB) | M56 (16 MB) |
|---|---|---|---|
| heap -> job_fill (total setup) | 0.706 s | 0.742 s | **4.359 s** |
| alloc_bufs -> open_vic | 0.071 s | 0.069 s | 0.684 s |
| nvmap/attr block (8 steps) | 41 ms | 44 ms | **382 ms** |
| per step | 5-8 ms | 5-9 ms | **61-79 ms** |
| binder txn rate | 98.3/s | 97.4/s | 92.7/s |

**The memset is not the cause.** 16 MB zeroed in <=26 ms (120.458 -> 120.484)
against ~9 ms for 2 MB — sub-linear, and irrelevant at this scale. Nor is it a
one-time cache writeback afterwards: the penalty repeats on *every* step
(61, 73, 79, 74, 74 ms), not just the first.

**Open question, not a conclusion.** Most likely the per-call cost of
`svcSetMemoryAttribute` scales with the containing heap region (kernel memory
block splitting/merging), but the log cannot distinguish that from alternatives.
Cheap test in M57: request 10 MB instead of 16 and see whether the per-step cost
tracks heap size. If it does, take only what a frame needs.

This is worker-thread setup cost, not per-frame, and nothing froze. But 4.36 s
is a window a user would feel, and the txn rate did dip ~5%, so it is recorded as
a cost rather than folded into the success.

### M57

Two things, and they are independent:

1. **Relayout.** `g_stage_buf = g_ind_buf + FbBlockRowStage` puts staging
   983,040 B into a region that must now hold 8,294,400 B. Move staging past a
   full frame — there is room (8,294,400 + 983,040 = 9,277,440 against
   16,580,608 available). This is our own constant, not a platform limit, and it
   is the only thing between here and a full-frame capture.
2. **Size/cost curve.** 10 MB vs 16 MB, to find whether the setup penalty is
   proportional to the heap.

## *** M55 RUN: application_type 2 - the memory ceiling is gone ***

One KAC capability: `{"type": "application_type", "value": 2}`
(`ProgramInfoFlag_Applet`). `pm_spec.cpp:145` reads exactly this field to pick
the resource-limit group, so it moves gate A from System to Applet.

```
M54  [probe, BEFORE heap grab] process total=  5472 KB used=1724 KB free=  3748 KB
M55  [probe, BEFORE heap grab] process total=412984 KB used=1724 KB free=411260 KB
```

**75x.** At boot it reads 511,936 KB, against M54's 14,060 KB. Our own `used` is
1,724 KB in both runs — nothing about us changed except which group we count
against.

It also confirms the `kern_k_process.cpp:868` semantics derived in M54, to the
kilobyte:

```
412,984 = 411,260 (Applet group free) + 1,724 (our used)
```

**A 1080p frame is 7,913 KB. Free at probe is 411,260 KB — about 52 frames.**

### Both gates are now on Applet

| gate | field | value | pool/group free at probe |
|---|---|---|---|
| physical pool | `pool_partition` | 1 | 411,260 KB |
| resource limit | `application_type` | 2 | 411,260 KB |

M54 moved the first and I wrongly called that the end of it. It took both.

### Still measure-only, on purpose

The ladder stayed capped at `{ 2_MB }`. Applet went 100,740 → 102,788 KB
(+2,048, the 2 MB), System flat at 229,140 KB both sides. Console survived
282 s, `txn=27515`, `vic=vb:released`, no fatal. VIC healthy: `job_fill` and
`job_blit_self` both wrote our memory. `nvdrv:t` still returns the full mask.

**An applet-group mitm was untested by anyone** — `memlet` declares
`application_type 2` but is not a mitm. It works, with no observable change to
the mitm, nvdrv permissions, or the VIC.

### What this does NOT prove

We measured the limit. **We did not take 8 MB.** "A 1080p frame now fits" is
what the resource limit reports, not something demonstrated on hardware. That
exact gap — a number that says yes versus hardware that does it — is what I got
wrong in M49 ("never a hard limit") and again in M54 ("structurally
impossible"). Both times I reasoned about a limit and spent against the
reasoning.

The difference now is the margin: 411 MB against 8 MB is 50x, and it is
measured rather than inferred. That is a reason to try it, not a reason to skip
verifying it.

### masagrator

His objection — *on 22.5.0 a sysmodule cannot hold a 1080p buffer* — is
genuinely retired, and he was right for every run up to M53. It stood as long as
it did because **two** independent gates had to move, and I kept moving one and
announcing the result. He is owed the numbers.

### M56

Raise the ladder to 8 MB and actually take it. Log both pools and both gates
either side. Success = a full 1080p frame resident in a sysmodule, System pool
and System resource limit untouched, no fatal. Then the capture path can read a
whole frame instead of 983,040 B block-rows.

## *** M54 RUN: the pool moved - and my "structurally impossible" was wrong ***

`pool_partition: 2 -> 1` (System -> Applet). One NPDM field. It does exactly
what the kernel source says, measured either side of the grab:

```
[before heap grab] pool 1 Applet       used=  100740 KB  free=   411260 KB
[before heap grab] pool 2 System       used=  229140 KB  free=     8588 KB
SetMemoryHeapSize(8 MB) rc=0x1003
SetMemoryHeapSize(6 MB) rc=0x1003
SetMemoryHeapSize(4 MB) rc=0x1003
SetMemoryHeapSize(2 MB) rc=0x0
[after  heap grab] pool 1 Applet       used=  102788 KB  free=   409212 KB
[after  heap grab] pool 2 System       used=  229140 KB  free=     8588 KB
```

Applet **+2,048 KB — exactly the 2 MB granted**. System **unchanged to the KB**.
Application and SystemUnsafe unchanged. Console ran 257 s with the game live,
`txn=25367`, `vic=vb:released`, no fatal. VIC healthy: `job_fill` and
`job_blit_self` both `OP_DONE`, engine wrote our memory.

### The chain, verified in source *before* the run

Not inferred from a successful allocation — that is precisely the M49 mistake:

- `ldr_process_creation.cpp:464` — `pool_partition 1` → `CreateProcessFlag_PoolPartitionApplet`
- `kern_k_page_table_base.cpp:513` — `KProcess::Initialize`'s `pool` → `m_allocate_option`
- same file, line 1930 — `SetHeapSize` allocates through `m_allocate_option`
- `kern_k_shared_memory.cpp:40` — `CreateSharedMemory` uses the same option

The heap inherits the pool for free. `memlet`'s `svcCreateSharedMemory`
machinery is **not** needed.

### The retraction

I told the user this made M50's failure "structurally impossible". **That is
wrong, and wrong in the same direction as M49 and M50.**

M50's fatal was `0x10801 LimitReached` — a **resource limit** error, not pool
exhaustion. Two independent gates:

| gate | set by | M54 |
|---|---|---|
| resource limit (`LimitReached`, what killed `am`) | `application_type` → `ResourceLimitGroup_System` | **unchanged** |
| physical pool | `pool_partition` | System → Applet |

`pool_partition` moves the *second*. M50 died on the *first*. Had M54's 8 MB
been granted it would have reserved 8 MB of the same shared System resource
limit `am` needs, and `am` could have died again. **`am` survived this run
because the request was refused, not because I made it safe.**

That is now three times — M49, M50, M54 — of reasoning about a limit and then
spending memory on the strength of the reasoning. Measure first.

### What `process total` actually is

`kern_k_process.cpp:868` — `GetTotalUserPhysicalMemorySize()` returns
`m_resource_limit->GetFreeValue(PhysicalMemoryMax)` **plus** our own used size.
That free value belongs to the **shared System resource-limit group**, not to
us. The process figures were never a per-process quota:

```
[boot ] THIS PROCESS: total=14060 KB used=1724 KB free=12336 KB
[probe] process       total= 5472 KB used=1724 KB free= 3748 KB
```

Nothing of ours changed between those lines — `used` is identical. The System
*group* drained as the game and other sysmodules claimed their share. Quoting
the boot number to justify a probe-time allocation, which is what I did when I
said a 1080p frame "fits", is the M49 error with fresh numbers.

**A 1080p frame is 7,913 KB. The whole budget at probe is 5,472 KB. It never
fit, and the Applet pool's 411 MB free is irrelevant to that.**

### masagrator, precisely

His objection survives M54, but the *reason* changed and the change matters: it
is not "the System pool is exhausted" (we have escaped that) but "the System
**resource limit group** is nearly exhausted once a game is resident". Owed to
him as a correction, not a rebuttal.

### Verifying an NPDM field actually changed

`npdmtool` silently accepts unknown fields — it printed ten "field not present"
notices and still exited 0. So prove it differentially: compile the same JSON
twice, differing only in the field, and diff.

```
offset 653: p1=005 p2=011      (octal 5 and 9)
```

`AcidFlag_PoolPartitionShift = 2`, so partition 1 → `1<<2 = 4`, partition 2 →
`2<<2 = 8`, plus the retail bit: `4|1 = 5`, `8|1 = 9`. Matches the kernel
definition exactly. **Use this for every NPDM field from now on.**

### Souldbminer's comment (GBAtemp), assessed

> the best way to do this is to simply rip the frames straight from the display
> controller with MMIO, you have access to it from a sysmodule

**The mechanism is real and corrects an assumption recorded in this file.** MMIO
is reachable from a sysmodule with no kernel or secmon patch:

- `boot.json` declares `{"address": "0x54200000", "size": "0x3000", "is_io": true}` — that is DISPLAY_A
- `boot_display.cpp:43,92` maps it via `dd::QueryIoMapping`
- we **already** declare `svcQueryIoMapping: 0x55`; only the `map` capability is missing
- `npdmtool` compiles a `map` entry for `0x54200000` (verified)
- `PhysicalMapAllowedMask = (1<<36)-1`, so the address is acceptable

**But the framing does not survive contact.** The DC holds no pixels; it is a
scanout engine reading DRAM through the **SMMU**. `boot_display.cpp:155-158`
hands it a framebuffer via `CreateDeviceAddressSpace` →
`AttachDeviceAddressSpace(DeviceName_Dc)` → `MapDeviceAddressSpaceAligned`, so
`WINBUF_START_ADDR` holds a **device virtual address** — meaningless without the
IOMMU page tables. We hold `0x56`/`0x57`/`0x5a` and could attach our own address
space to the DC, but nvservices already owns that attachment, and fighting it
for the live display engine is M50's class of move.

His NVENC point matches what we found independently: nouveau has no Tegra NVENC
support, so REing `nvservices` is the honest path.

### M55 (proposed, not run)

`application_type: 2` (`ProgramInfoFlag_Applet = (2 << 0)`) moves gate A from the
System group to the Applet group — `pm_spec.cpp:145` reads exactly this field,
`ldr_process_creation.cpp:171` computes it from the ACI KAC, and `memlet` is
shipped precedent for a sysmodule declaring it. `npdmtool` honours it (NPDM
1096 → 1100 B, one new capability word at offset 873, verified differentially).
It would also unlock `system_resource_size`, which `ldr_process_creation.cpp:521`
gates on `IsApplication(meta) || IsApplet(meta)`.

**Measure only.** Keep the ladder at 2 MB and just log `TotalMemorySize` at
probe. If the group moved it jumps from 5,472 KB to hundreds of MB. That answers
the question without allocating anything, and it breaks the pattern that
produced M50.

An applet-group **mitm** is untested by anyone — `memlet` is not a mitm — so it
is the user's call, not ours.

## *** M52 RUN: USB TRANSPORT WORKS - but SysDVR owns the bus ***

Every call succeeded, first attempt:

```
usbDsInitialize rc=0x0
string descriptors rc=0x0
device descriptors (Full+High) rc=0x0
usbDsRegisterInterface rc=0x0
configuration descriptors rc=0x0
endpoints IN=0x81 OUT=0x01 rc=0x0
EnableInterface rc=0x0
usbDsEnable rc=0x0
```

Interface registered, bulk endpoints in/out, device enabled. **The transport half
is written and functioning.**

### But the host does not see us

`lsusb` on the PC shows `18d1:4ee0`, whose product string is **"SysDVR"** and
manufacturer `https://github.com/exelix11/SysDVR`. Title `00FF0000A53BB665` is
installed on the card **with a `boot2.flag`**, one of nine auto-starting
sysmodules, alongside `config/sysdvr` and `switch/SysDVR-conf.nro`.

SysDVR enumerates first and owns the device presentation. Our descriptors are
simply not what the host is shown.

### Three corrections

1. **`usb:ds` exclusivity is at the BUS level, not the service level.** Both
   modules got `rc=0x0` from `usbDsInitialize`. The documented "one client"
   behaviour does not manifest as a failed acquire, so a zero return proves
   nothing about who the host actually talks to.
2. **The `*** USB DEVICE ENUMERATED ***` log line asserts more than it can
   know.** It fires on `usbDsEnable` returning zero, which says nothing about bus
   ownership. It should verify that the host sees *our* VID/PID.
3. **An earlier session reported "no SysDVR installed" after checking
   `contents/`.** That was wrong: SysDVR uses a `00FF…` title ID, which was not
   recognised while scanning for `0100…` game-style IDs. SysDVR has therefore
   been holding the bus during *every* run so far, and any earlier transport
   attempt would have failed the same way.

Also note `lsusb` mislabels it "Google Inc. Nexus/Pixel Device (fastboot)"
because it consults a VID/PID table rather than the device's own strings - which
is why an initial grep for `1209:5f1e` returned "not found" and was briefly read
as "nothing enumerated". `/sys/bus/usb/devices/*/product` tells the truth.

### NVENC: class ID settled, methods still unknown

`NV_VIDEO_ENCODE_NVENC_CLASS_ID = 0x21` (MSENC is an alias), from the T210
nvhost device table and `class_ids.h`. Cross-checked the only way that matters:
the same header gives `NV_GRAPHICS_VIC_CLASS_ID = 0x5D`, the value proven on
hardware in M16. So `SETCL(0, 0x21, 0)` is the encoder's equivalent - the field
that cost six runs on the VIC, settled with no console time.

**Deliberately not probed yet.** M16 also proved `INCR_SYNCPT` fires in *any*
class, so a `SETCL`-only probe cannot discriminate, and with no method table
anywhere local, writing methods would be aiming blind at an engine - the pattern
that froze the console twice.

### Next

One `mv` of SysDVR's `boot2.flag`, one boot with a cable attached, and confirm
`1209:5f1e` from the PC. Fully reversible, but it stops a working capture tool
while ours is unfinished - the user's call, not ours to make unilaterally.

## *** M50 FATALED THE CONSOLE - and M49's conclusion was wrong ***

```
Error Code: 2001-0132 (0x10801)
Program: 0100000000000023        <- am, the applet manager. NOT us.
```

`0x10801` decodes as kernel module 1, description 132: **`LimitReached`**. A
*different* sysmodule was killed because we took the memory it needed.

The log shows it plainly:

```
[boot] System pool now: used=231424 KB free=6304 KB  (we took 8192 KB of it)
```

8 MB out of a pool with 14,496 KB free left 6,304 KB, and `am` could not start.
**The allocation always succeeded - holding it is what broke the console.**

### The retraction

M49 concluded "the 2 MB ceiling was never a hard limit, it was an artefact of
asking late", and that was recorded here and told to the user. **It is false, and
the reasoning was backwards.**

8 MB was grantable at boot precisely *because* `am` had not allocated yet. M49
measured a **transient** and read it as headroom. The 2 MB seen at probe time is
not a measurement taken at the wrong moment - it is the System pool's honest
steady state once every sysmodule has claimed its share.

masagrator's original objection stands: **on 22.5.0 a sysmodule cannot hold a
1080p buffer.** The correction that was about to be posted to GBAtemp would have
been wrong. Worth telling him the test result rather than quietly dropping it.

### Second time for this pool

M27 over-drew the same pool with 4 MB of `.bss` and fataled a sysmodule the same
way. Two independent failures, same mechanism, ~30 milestones apart. The System
pool's free space is **shared across every sysmodule on the console**, and it is
not ours to spend.

### M51

- No heap taken at boot.
- The probe-time ladder is **capped at 2 MB** rather than starting at 8. Widening
  it is now a deliberate decision requiring `pool_partition` to change first, not
  an optimisation to retry.
- Capture stays strip-wise, which costs nothing structurally: the 983,040 B
  block-row is the natural unit of the block-linear layout anyway.

The pool survey stays - it is read-only `svcGetSystemInfo` and allocates nothing.

Remaining levers for NVENC's working set: `pool_partition` (Applet has 500 MB
free, though a sysmodule may not be permitted to use it), all-intra encoding to
eliminate reference frames, and reduced encode resolution.

## *** M49 RUN: 8 MB IS GRANTABLE - the ceiling was a timing artefact ***

```
[boot] process total=13076 KB used=1696 KB free=11380 KB
[boot] SetMemoryHeapSize(8 MB) rc=0x0
[boot] holding 8 MB: total=13076 KB used=9888 KB
[boot] RELEASED rc=0x0
*** LARGEST GRANTABLE HEAP AT BOOT: 8 MB ***
```

First try, no ladder needed, with ~3.2 MB still spare afterwards. The release
succeeded too, so nothing was held through boot and no other sysmodule was
starved.

**A whole 1080p frame is 7913 KB and now fits.** The strip-wise design becomes a
choice rather than a constraint, and NVENC gets room for an input surface and a
bitstream buffer.

### The 2 MB ceiling was never a hard limit

It was an artefact of *when* we asked. At probe time - game resident, transfer
memory committed - 4 MB returns `OutOfMemory`. At boot, 8 MB succeeds
immediately. **The fix is timing, not `pool_partition`**, which does not need to
be touched at all.

That also means the conclusion posted to GBAtemp needs correcting rather than
merely conceding. masagrator was right about the constraint we were operating
under, and our own failed allocations confirmed it - but "a sysmodule cannot hold
a 1080p buffer on 22.5.0" is **false**. It can, if it asks at boot.

### M50, and the risk it retires

M50 takes the 8 MB at startup and **holds** it, with the existing
`AllocVicHeap` early-out meaning the probe simply reuses it.

M49 grabbed and released immediately. Holding through a game launch is a
different proposition: the System pool has only 14 MB free in total, and M27
proved that starving it fatals a *different* sysmodule with `LimitReached`. So
this run's real question is not "does 8 MB allocate" - that is answered - but
**"does the console still launch a game while we hold it"**.

If MK8 fails to start, that is the cause. Recovery is deleting
`atmosphere/contents/0100000000000C20` from a PC, or booting with Volume Up.
Nothing touches NAND.

The run logs the System pool before and after the grab, so what we took from a
14 MB shared budget is visible rather than inferred.

## *** M48 RUN: we are in the one pool that is full ***

Boot-only test - no game, no dock, 20 seconds:

```
pool 0 Application   total= 3363840 KB  used=      0 KB  free= 3363840 KB
pool 1 Applet        total=  512000 KB  used=     64 KB  free=  511936 KB
pool 2 System        total=  237728 KB  used= 223232 KB  free=    14496 KB
pool 3 SystemUnsafe  total=   45184 KB  used=  43180 KB  free=     2004 KB
THIS PROCESS (pool_partition 2, at boot): total=13076 KB used=1696 KB free=11380 KB
```

**System is 94% used, with 14 MB free across every sysmodule on the console.**
Applet has **500 MB** free and Application **3.2 GB**. So `pool_partition` is not
a marginal lever - it is the whole question.

Two cautions on reading that table. Application's 3.2 GB is free only because no
game was running; with MK8 resident most of it is gone, so Applet's 500 MB is the
honest target. And a sysmodule may simply not be permitted to allocate from
those pools - untested.

### The contradiction worth resolving first

`THIS PROCESS ... total=13076 KB free=11380 KB` **at boot**, against M47's
`total=4720 KB free=976 KB` at probe time. Our own budget appears to *shrink*
once a game is resident - and 11,380 KB free at boot is already more than a whole
1080p frame needs (7913 KB).

If that headroom is real at the right moment, a frame fits, the strip-wise design
becomes a choice rather than a constraint, and **the NPDM change is unnecessary**.
Two readings fit the data and imply very different things: either M47's figure
was taken after the heap and transfer memory were already committed and measures
something else, or the budget genuinely varies with system load. They are
distinguishable by measurement, so measure rather than guess.

### M49

Three measurement points in one run - **boot**, **probe before the heap grab**,
**probe after** - plus the heap ladder retried **at boot**, when 11 MB appears
free.

The boot ladder grabs and then **immediately releases**. M27 already proved that
holding several MB from the System pool during boot fatals a *different*
sysmodule with `LimitReached`, and System has only 14 MB free in total. Learning
the ceiling is worth a run; risking an unbootable console to hold it is not. The
release `rc` is logged, so a failed release is visible rather than silent.

The ladder also drops 5 MB and 3 MB: they returned `0xca01` (kernel
`InvalidSize`) because `svcSetHeapSize` requires 2 MB granularity, so they were
never valid requests and only added noise.

`*** LARGEST GRANTABLE HEAP AT BOOT: N MB ***` is the line that decides the next
move. 8 MB means a whole frame fits and NVENC has room; 2 MB means the budget is
genuinely fixed and `pool_partition` becomes the next thing to try.

## *** M47 RUN: NVENC CHANNEL WORKS, and the memory budget is brutal ***

Handheld, so 1280x720 of content inside the usual 1920x1080 surface. The layout
is unchanged - `nvmap 1268, 1920x1080, pitch 7680, block_h_log2 4`, swapchain
still matched at exactly 26,542,080 B - so handheld costs content width, not
correctness.

### NVENC phase A passed

```
/dev/nvhost-msenc open fd=23461890
GET_SYNCPOINT rc=0x0 nverr=0 -> syncpt=14   (VIC uses 12)
SET_NVMAP_FD rc=0x0 nverr=0
CHANNEL_SUBMIT req=0xc0340001 sz=52 words=2 rc=0x0 nverr=0 -> fence=4691
WAIT nverr=0 syncpt=4691  *** NVENC CHANNEL USABLE ***
```

The encoder channel opens, carries its own syncpoint, accepts the same submit
ABI as the VIC, and the fence advances. No hang - the console ran on to txn
34,859. **Only the encode configuration is unknown now**, not the plumbing.

### The stutter fix worked

**4780 ms -> 67 ms.** Resuming immediately after the block-row read, instead of
after the blits and SD writes, removed 98.6% of the frozen window.

### The memory budget, measured

```
MEMORY BUDGET: total=4720 KB used=3744 KB free=976 KB | sysresource 0/0 KB
```

**The entire process gets 4.6 MB** - less than a single 1080p frame at 7913 KB.
masagrator argued a raw buffer would consume half the available sysmodule space;
in fact it does not fit at all, and NVENC's whole working set would have to live
in the ~976 KB that remains.

The ladder also explains its own shape: `0xca01` on 5 MB and 3 MB is kernel
`InvalidSize`, not out-of-memory - `svcSetHeapSize` requires 2 MB granularity, so
those requests were never valid. `0x1003` (`OutOfMemory`) on 8/6/4 MB is the real
signal. **The ceiling is exactly 2 MB because 4 MB is the next legal step.**

### A correction to M45's headline

M45 was reported here and to the user as "err 0.00 on all four lanes", implying
the blit matched outright. It did not. That 0.00 was the **per-lane** figure
after allowing a permutation; the harness score for ONE2ONE in M45 was 70.44.
The blit is bit-exact *under an R/B swap*, which is a materially weaker claim
than the one made.

M47 confirms the same thing honestly. ONE2ONE scores 53.63 overall, but per lane:

```
out0(A)=srcA 0.53   out1(R)=srcB 0.00
out2(G)=srcG 0.00   out3(B)=srcR 0.00   -> ABGR
```

Three lanes at exactly 0.00 and the same `ABGR` mapping. **Nothing regressed** -
the headline number is just the permutation being scored honestly.

### M48

Query **every** physical memory pool at boot via `svcGetSystemInfo`
(`Application=0, Applet=1, System=2, SystemUnsafe=3`), rather than inferring the
budget from a failed allocation. `0x6F GetSystemInfo` is verified granted in the
compiled KAC, so it cannot fail silently.

This answers whether `pool_partition` is a real lever - we are on 2 (System), and
if Applet or Application has headroom, a one-line NPDM change might buy the room
NVENC needs. Logged at **boot**, so every future memory question costs a ~20 s
boot instead of a three-minute race to reach the probe.

## External review: the downstream budget is tighter than assumed

masagrator raised two objections on the GBAtemp thread. Both are worth recording
because one is simply correct and changes the plan.

**Transport.** He read the 1100 MB/s figure as a transport rate. It is not - that
is `ReadDebugProcessMemory` pulling a frame out of the game's address space, a
memory read. Raw 1080p60 is **498 MB/s**. But correcting the number does not
rescue the point: 498 MB/s still exceeds practical USB 3, and `usb:ds` in normal
mode is USB 2.0, roughly 30-40 MB/s. SysDVR's own readme states the same wall -
*"Video quality is fixed to 720p @ 30fps with h264 compression, this is a
hardware limit"*. **Encoding is load-bearing, not an optimisation.**

**Memory, and he is right.** From our own log:

```
SetMemoryHeapSize(8 MB) rc=0x1003
SetMemoryHeapSize(6 MB) rc=0x1003
SetMemoryHeapSize(4 MB) rc=0x1003
SetMemoryHeapSize(2 MB) rc=0x0
```

2 MB is the ceiling and one 1080p frame is **7,913 KB - four times the entire
heap**. That is why everything works in 983,040 B block-rows, and M27 already
found the sharp edge: 4 MB of `.bss` fataled a *different* sysmodule at boot
with `LimitReached`, because `.bss` comes from the same shared system pool.

What was never quantified is NVENC's own working set on top - input surface,
bitstream buffer, and reference frames for inter-frame prediction.

### Consequences

1. **Measure the budget rather than infer it.** M47 asks the kernel directly via
   `svcGetInfo` (`TotalMemorySize`, `UsedMemorySize`, `SystemResourceSize*`)
   instead of deducing it from a failed `SetMemoryHeapSize`. The ladder is also
   finer (8/6/5/4/3/2 MB) to find the true ceiling rather than a power of two.
2. **All-intra becomes the default encoder plan, not a fallback.** I-frames need
   no reference frames, removing the largest consumer. It costs bitrate, and
   bitrate is the budget with more room to trade than memory.
3. **`pool_partition` has never been varied.** It is a one-line NPDM change
   (currently 2, system). Worth one test before concluding 2 MB is immovable.
4. **The screenshot tool is a real product.** Native 1080p capture works, has no
   bandwidth and no encoder problem, and is done. If NVENC will not fit a
   sysmodule's budget, that ships rather than nothing.

The capture findings stand regardless of what happens downstream: the kernel
debug-SVC route, the NPDM `force_debug` requirement, and why nvmap pinning of a
foreign handle cannot work.

## *** M45 RUN: THE VIC IS EXACT - err 0.00 on all four lanes ***

```
ONE2ONE:  out0(A)=srcR 0.00   out1(R)=srcG 0.00
          out2(G)=srcB 0.00   out3(B)=srcA 0.00
```

The unscaled 480x32 blit reproduces a software de-swizzle **bit for bit**. Not
"within a few LSB" - exactly. That settles three things at once:

- block-linear addressing, GOB tiling, stride and block height are **exact**;
- the 2-3 LSB residual on scaled variants is the VIC's polyphase scaler
  differing from a box filter, which is not an error;
- `tools/compare_vic.py` is correct, since it agrees perfectly with hardware.

**The VIC pipeline is finished.** It had been finished for several milestones;
what remained was only channel order, and I spent three docked runs on it.

**Channel order needs no further hardware.** Every format pair is a lossless
permutation - `S32O33` gives `ABGR`, a pure R<->B swap from identity. That is a
free fix on the PC, or by choosing NVENC's input format, which has to be
configured anyway. Spending a dock/undock cycle to untwist two constants would
be spending the expensive resource to save the cheap one.

### The stutter, measured

`dbg:1_find_pid` at 181.195, `ContinueDebugEvent` at 185.971: **the game was
frozen for 4.78 seconds.** That is the stutter felt on the console, and it is
entirely self-inflicted - the strip read, five VIC blits and 1.3 MB of SD writes
all happened before resuming. Only the 983 KB read needs the target halted;
everything after works on our own buffer.

M46 resumes immediately after the read and logs the frozen window, which should
drop to roughly 50 ms.

### M46

1. Stutter fix as above.
2. Format pair fixed at the best known (`S32O33`), sweep cut to two regression
   variants.
3. **NVENC phase A** - the M11 treatment: open `/dev/nvhost-msenc`, take its
   syncpoint, bind the nvmap fd, submit a command buffer that does nothing but
   increment that syncpoint. No SETCL, no method writes, so the engine cannot be
   aimed at a bad address and cannot hang. If the fence advances, the channel and
   submit ABI are usable and only the encode configuration remains unknown.

## *** M44 RUN: the blit is LOSSLESS - only R and B are transposed ***

My prediction failed and the failure was informative. `P34_rgba` scored 50.80,
not the "<5" I expected. But the per-lane mapping table is unambiguous: for every
format, **each output lane matches some source lane under 5 LSB, alpha
included.** Nothing is destroyed; the channels are merely shuffled.

`P32_argb`, the closest:

| output lane | matches source | err |
|---|---|---|
| out0 (A) | **A** | 1.11 |
| out1 (R) | B | 4.12 |
| out2 (G) | **G** | 4.52 |
| out3 (B) | R | 4.98 |

Alpha tracks source alpha at 1.11 - so it is **not** being forced to 255 by
`ConstantAlpha` after all - green is already correct, and **R and B are simply
transposed**. Sampling, scaling, block-linear addressing and alpha are all
correct.

The source's alpha channel (only 2 distinct values, 170/255) acts as a tracer:
it lands in L3 for `P33`, L1 for `P34`, and nowhere among L1-L3 for `P32`. That
is how the permutation per format was read off directly.

### Two claims retracted

- **M43's "`ConstantAlpha` destroys red".** Wrong. Nothing is lost in any
  format; I mistook "R is not where I expected" for "R is gone".
- **"identical to A - field inert".** Wrong three times over. All three M44 dumps
  have different md5s despite two sharing a byte sum - they are permutations. A
  byte sum is blind to reordering, which is precisely what these variants do.
  The label is deleted; sameness is now decided on the PC by md5.

### M45

The **output** format has been hardcoded `A8B8G8R8` since M19 and never swept -
and the 64x64 self-blit could not have revealed an R/B swap, because its painted
ramp differed in R and B only by a constant. So sweep the pair:

`S32O32`, `S32O33`, `S33O32`, `S33O33` - one must be the identity mapping.

Plus **`ONE2ONE`**: an unscaled 480x32 crop. The residual 4-5 LSB is either the
VIC's polyphase scaler differing from a box filter, or a small sampling offset.
An unscaled blit cannot have a filter error, so if it lands near zero the
residual is filtering and harmless. The harness compares it against a 1:1 crop
rather than a downscale, and now calls anything under 6 an identity mapping.

Also fixed: `StripSrc` carried only 7 of 9 initialisers after `rect_w`/`rect_h`
were added. C++ aggregate init zero-fills silently, so `src.rect_w - 1` would
underflow to `0xFFFFFFFF` into a 30-bit `SourceRectRight`. Unreachable in the
current flow - the sweep assigns `g_strip_src` before every submit - but a
garbage rect hangs the VIC and takes the console with it, so it is now
initialised properly and guarded by a `static_assert`.

## *** M43 RUN: it was never the layout - the source pixel format was wrong ***

The dump path fix worked: `7 dumped`, zero `0xd401`, and the file checksums now
agree with the engine's. So for the first time the VIC comparison is real data.

**The `G_pitchkind` control differed** (10347312 vs A's 10672437), so the engine
*is* reading our slot surface config. The "config ignored entirely" branch is
ruled out.

### First, a correction to M42

M42 concluded three fields were "inert" from identical **byte sums**. A byte sum
is blind to reordering - which is exactly what a layout change does. The md5s
tell the real story: A, B and E share one md5 (genuinely identical, so luma width
and cache width really are inert), but **C has A's byte sum with a different
md5**, and `sorted(A) == sorted(C)`. Same bytes, different order.
`SlotBlkHeight` is **live**; it permutes pixels. The on-console
"(identical to A - field inert)" line repeats that same mistake and should be
read as "same sum", nothing more.

### The actual fault

Per-lane comparison against a software de-swizzle of the same strip:

| | out[1] | out[2] | out[3] |
|---|---|---|---|
| aligned (out=A,R,G,B vs src R,G,B) | 7.15 | 21.02 | 195.71 |
| **shifted by one lane** | **1.87** | **1.69** | **0.70** |

Under 2 LSB on every lane once shifted - filter-difference magnitude. And
channel 0 is `min=255 max=255 distinct=1` in all seven variants.

So the engine's output is `[0xFF, G_src, B_src, A_src]`: it consumed the
source's **R byte as alpha**, slid R,G,B down one place, and forced alpha to max
via `ConstantAlpha=1 / PlanarAlpha=1023`.

**Geometry, scale and block-linear addressing were all correct - probably since
M41.** The game's surface is R,G,B,A in memory, which the VIC calls
**`PIXFMT_R8G8B8A8` (34)**, not the `A8B8G8R8` (33) we declared.

### Why this took two extra runs

M16's fill had already proved the VIC's **output** byte order is `A,R,G,B`
(`ff c0 80 40` from A=1023/R=768/G=512/B=256). `tools/compare_vic.py` assumed
`R,G,B,A`, so it drew byte 0 - alpha, pinned at 255 - as **red**. The
"scrambled, red-tinted" image that sent M42 and M43 hunting for a layout bug was
substantially a decoder artifact. The harness now reorders `A,R,G,B -> R,G,B,A`.

Re-scoring M43's data with the corrected harness gives **74.63**, worse than the
57.30 it printed before - because two misalignments were partially cancelling.
The number that matters is what `P34_rgba` scores next run, not this one.

### M44

Sweep the source pixel format with the layout fixed at what M43 proved correct
(pixels, blk_h 4, 64Bx4, kind GENERIC_16Bx2):

- `P34_rgba` - `R8G8B8A8`, predicted correct
- `P33_abgr` - `A8B8G8R8`, M43's control, expected to stay shifted one lane
- `P32_argb` - `A8R8G8B8`

If P34 lands under ~5 mean error, the VIC path is done and NVENC is next.

## M40-M42 RUNS: capture holds up; the VIC sweep was measuring garbage

**M40 (the good run).** With the probe delayed to 180 s so it lands mid-race, and
the honest verdict logic in place:

```
captured 120 full frames in 2001 ms -> 59 fps  (120 distinct, 0 missed)
per-frame read: min 7143 us  avg 9140 us  max 11803 us  (0 of 120 over budget)
slots read: [0]=40 [1]=40 [2]=40
game presented: 62 fps before, 59 during, 60 after -> -2 fps delta  [GAME UNAFFECTED]
```

Every M39 complaint came back clean: 120/120 distinct (the signature now hashes
block-row 0 rather than the padding below the image), an even 40/40/40 slot
split proving we follow the swapchain rotation, zero frames over budget, and a
2 fps cost measured against the average of before and after with a tolerance of
3. This held again in M42 (59 fps, 118 distinct, -2 fps).

**M41 (the VIC meets real pixels).** The strip blit completed and wrote 61,378 of
the 61,440 bytes expected for 480x32 - but the picture was scrambled: correct
brightness structure, no coherent image. Right byte count, wrong pixel order,
i.e. a source-LAYOUT fault rather than scale or crop.

**M42 (the sweep), and two faults of mine.**

*The dumps never contained the engine's output.* All six writes returned
`rc=0xd401` - kernel, description 106, `InvalidCurrentMemory`:

```
WriteBufToSd(sdmc:/applet-mitm-vic-A_px_bh4.bin) rc=0xd401   (x6)
swept 6 variants: 6 completed, 0 dumped
```

`g_vic_dst_buf` is **uncached** nvmap memory, and an uncached buffer cannot be
handed to `fs::WriteFile` - the IPC layer cannot map it for transfer. This is
also why M41 reported `strip.bin OK vic.bin FAILED`: the strip comes from the
CACHEABLE capture buffer, the VIC output does not.

The trap: `fs::CreateFile(path, len)` pre-allocates, so a failed write still
leaves a correctly-sized file. Six files of exactly 65,536 B existed with six
different md5s and plausible content - and none of their byte sums matched the
engine's own checksum for the same job (file A summed 11,031,532 against the
console's 11,470,945). **The PC-side ranking was comparing corrupted data and is
discarded.** It printed a "winner"; the winner meant nothing.

*The fields being swept are inert.* The on-console checksums are computed by the
module straight from the engine output, so unlike the files they can be trusted:

| variant | bytesum | |
|---|---|---|
| A  px, bh4, 64Bx4 | 11470945 | baseline |
| B  byt, bh4 | 11470945 | identical - luma width inert |
| C  px, bh0 | 11470945 | identical - block height inert |
| E  px, bh4, 16Bx16 | 11470945 | identical - cache width inert |
| D  byt, bh0 | 15114208 | differs |
| F  byt, bh1 | 15258556 | differs |

`SlotLumaWidth`, `SlotBlkHeight` and `SlotCacheWidth`, each changed **alone**, do
nothing. Only combinations moving luma width *and* block height together change
the output. So the field that controls the source layout is not in this sweep -
a far better explanation of "none matched" than "wrong values".

### M43

- Stage the engine output through normal cached heap past the nvmap'd block-row
  before writing it, fixing `0xd401`.
- Log each variant's byte sum directly, so the comparison survives a failed dump.
- Add a **`G_pitchkind` control**: the same blit with `SlotBlkKind = PITCH`. If
  G's checksum also matches A's, even the block-kind field is being ignored and
  the engine is not reading our slot surface config at all - a deeper problem
  worth knowing before sweeping anything further. If G differs, the config is
  live and the search narrows to how block-linear addressing is derived.

## M39 RUN: 120 frames captured, and three things to fix

```
captured 120 full frames in 2137 ms -> 56 fps  (31 distinct, 0 missed)
per-frame read: min 5861 us  avg 10292 us  max 19655 us   (60 fps budget 16667 us)
game presented: 59 fps before, 56 fps during, 61 fps after  [*** GAME UNAFFECTED ***]
```

**The loop works.** 120 consecutive native-resolution frames, zero missed, driven
off the `queueBuffer` intercept. Reading the slot the game just presented is
sound, and the debug handle survives a multi-second capture session.

But the honest reading of those numbers is worse than the verdict line claims,
and all three problems are mine:

**1. The game did slow, by about 4 fps.** 56 during, against 59 before and 61
after. The verdict printed `GAME UNAFFECTED` because I wrote the tolerance as
`fps_during + 6 >= fps_before`, which is loose enough to swallow a real dip. M40
compares against the average of before and after with a tolerance of 3, and
prints the signed delta so the number cannot hide behind a label.

**2. `31 distinct of 120` is a measurement bug, not a stale-slot bug.** The
signature hashed `g_ind_buf` *after* the strip loop — which holds the **last**
strip, rows 1024-1151. The image is 1080 rows, so that buffer is mostly padding
below the picture and barely changes. M40 hashes **block-row 0**, the top of the
frame, taken during the read. M40 also logs how many times each of the three
slots was read, so swapchain rotation is visible rather than inferred.

**3. Jitter is the real risk, not throughput.** min 5861 us, avg 10292 us, max
19655 us — the maximum is *over* the 16667 us budget, and the average is 54%
higher than the 6691 us measured standalone in M38. Reading while the game
renders costs more than reading while it idles, which is unsurprising: we are
competing for memory bandwidth with the thing we are capturing. M40 counts how
many frames exceed budget instead of reporting only min/avg/max.

### And the stutter had a specific cause

The SD dump took **2566 ms at 3 MB/s**, against M38's 280 ms at 31 MB/s — and the
game is frozen for all of it, because the dump happens before
`ContinueDebugEvent`. The game was loading at the time, competing for the same
card. That freeze is exactly the stutter felt on the console.

The dump is now **opt-in** (`dump` in the arm file). It proved the capture; it is
not something a streaming implementation would ever do.

### The probe was also firing far too early

`total > 300` binder transactions lands at ~50 s — still the title screen, which
is why all three captured frames are the MK8 logo. The trigger is now **elapsed
time**, set by `wait=N` in the arm file (default 120 s), so the capture can land
in an actual race. That also makes the measurement more honest: a race is a much
heavier scene than a title screen, and jitter is exactly where that will show.

## M38 run: native 1920x1080, clean

Docked, and the frame came out perfect: the Mario Kart 8 Deluxe title screen,
sharp lettering, lens flare, star field, no tearing and no wash.

```
*** SWAPCHAIN AT 0x37399e6000 (offset 0 into its region) - matched by EXACT SIZE ***
FULL SLOT into RAM: 9/9 strips, 6691 us total, 1322 MB/s -> *** FITS IN A 60 fps FRAME BUDGET ***
*** WROTE A WHOLE FRAME (game stopped - no tearing): 8847360 B, 280 ms ***
```

**Native resolution, confirmed by content.** The non-black bounding box is
exactly `x 0..1919, y 0..1079` and 87% of the buffer is nonzero — against 44%
and `x 0..1279, y 0..719` on the handheld run. Docked, MK8 really does render
1920x1080, and we get all of it. This is the resolution SysDVR cannot reach at
all.

**Dumping before `ContinueDebugEvent` killed the tearing**, exactly as predicted.
Mean luminance per 128-row band is now 78, 63, 71, 90, 51, 64, 72, 42, 28 —
varying with the picture's own light and shade, instead of M37's monotonic
191 → 254 fade ramp.

**And the number that decides streaming:** a whole native-resolution slot reads
into RAM in **6,691 us of a 16,667 us frame budget — 40%**, at 1322 MB/s, with
the game running. Roughly 10 ms per frame left over for encode and transport.

### M39 — does it hold up per frame, sustained?

One timing on a title screen is not a streaming capture. M39 wires the read to
the `queueBuffer` intercept that has been running at 60 fps since M9:

- the binder thread now records, per presented frame, a counter and the slot
  that frame went into — two relaxed atomic stores and a parse, nothing that can
  block, which is the rule that has held since M8 froze the console;
- the worker waits for each present, then reads **the slot the game just
  presented** rather than the one it is drawing into;
- 120 consecutive full frames, recording per-frame min/avg/max, how many frames
  were distinct (proof we are getting new pixels, not re-reading one stale
  slot), and the **game's own frame rate before, during and after**.

That last measurement is the one that matters. If the game's presentation rate
drops while we capture, full-rate native capture is not viable no matter how
good the per-frame number looks in isolation.

## M37 run: we have a frame

```
*** SWAPCHAIN AT 0x3f117e6000 (offset 0 into its region) - matched by EXACT SIZE ***
    slot0 +0x0000000: 33 34 3c ff  35 36 3d ff  39 3a 41 ff  3c 3d 44 ff
*** WROTE A WHOLE FRAME: sdmc:/applet-mitm-frame.bin (8847360 B, 135 chunks, 358 ms, 24 MB/s) ***
```

`tools/deswizzle.py` turned that dump into a 1920x1080 PNG on the PC, and it is
**Mario, his kart, and Rainbow Road** — read out of the game's own swapchain by a
sysmodule. Every route through the graphics stack refused us; the kernel debug
path delivered.

### Three things the decoded frame taught us

**1. The console renders 1280x720, not 1920x1080.** The non-black bounding box is
exactly `x 0..1279, y 0..719`, and nonzero bytes are 44.44% of the buffer —
precisely `1280x720 / 1920x1080`. MK8 in **handheld mode** renders 720p into a
1080p surface. Docking should give the full 1920x1080; that is worth a run, since
native resolution is the point of the project.

**2. The washed-out look is tearing, and the numbers prove it.** Mean luminance
per 128-row band — and a band is exactly one block-row, exactly our read unit:

| band | rows | mean luma | near-white |
|---|---|---|---|
| 0 | 0-127 | 191 | 29% |
| 1 | 128-255 | 223 | 86% |
| 2 | 256-383 | 237 | 100% |
| 3 | 384-511 | 249 | 100% |
| 4 | 512-639 | 254 | 100% |
| 5 | 640-719 | 251 | 100% |

A monotonic ramp down the image, in the order we read it. MK8 was mid
fade-to-white (the transition when a menu is skipped), and the SD dump took
358 ms — about 21 frames — so each band is a later moment in the fade. Not a
decode fault: alpha is 255 across 100% of sampled pixels, and the least-faded top
40 rows average RGB (159, 189, 219), a believable Rainbow Road sky.

**3. Byte order confirmed by content.** `sky (156, 213, 255)` and a red-dominant
Mario hat confirm **R,G,B,A** — A8B8G8R8 as a little-endian word. PNG's own RGBA
order, so no channel swap is needed anywhere.

### M38 — take a clean one

Dump the slot **before** `ContinueDebugEvent` rather than after. With the game
stopped nothing can write to it, so the frame is coherent; it costs ~400 ms
frozen, once, which is a fine trade for a screenshot. A streaming capture would
never do this — it would read into RAM at 1129 MB/s and not touch the SD card.

M38 also measures that directly: a whole slot read into RAM in nine block-row
strips, no SD in the loop, which is the true per-frame cost and the number that
decides whether 60 fps survives contact with a running game.

## M36 run: live capture is feasible at 60 fps

```
drained 44 debug events; ContinueDebugEvent rc=0x0 -> GAME RUNNING WHILE WE STAY ATTACHED
live sample over 120 ms: *** PIXELS CHANGED - the game is presenting while attached ***
    t0: ff ff ff ff ff ff ff ff
    t1: aa aa af ff ab ab af ff
read 983040 B of block-row in 776 us -> 1265 MB/s
=> one full 8,847,360 B slot would take ~6988 us; 60 fps needs <= 16667 us  [FEASIBLE]
```

Three separate results, and together they settle the architecture:

1. **`ContinueDebugEvent(ExceptionHandled | ContinueAll)` resumes the game with
   the debug handle still held.** Reading no longer requires freezing the target.
2. **The pixels move underneath us** — sampled 120 ms apart while attached, the
   corner went from flat white to `aa aa af ff`. The game is presenting live.
3. **1265 MB/s.** A whole 8,847,360 B slot reads in ~7.0 ms against a 16.67 ms
   frame budget. Full-resolution 60 fps capture has the headroom.

The exact-size fast path also worked: `5 big enough (1 exactly 26542080 B); 3
probe reads` — the search collapsed from ~24,000 reads to 3, and the frozen
window to ~43 ms, nearly all of it the region walk rather than the search.

ASLR confirmed a third time: the swapchain was at `0x1a3d5e6000` this run,
`0x10851e6000` the last.

### The one misleading number, and why it is not a problem

```
block-row stats: nonzero=516752/983040  distinct byte values=238  opaque pixels=0/245760
first 16 B: aa aa af ff  ab ab af ff  ac ac b0 ff  ae ae b2 ff
```

`opaque pixels=0/245760` looks alarming and is an artifact of my own code. At
the instant of detection the corner was **flat white**, so all four byte lanes
were `0xFF` and the scanner picked lane 0 for being first. The stat then counted
lane 0 — which is **red** — across the strip. The real alpha lane is **3**:
`aa aa af ff` is A8B8G8R8 stored little-endian as **R,G,B,A**.

A second bug of mine allowed it: `FbPixelLane` computed `varied` and never acted
on it, so a uniform block passed a test whose comment said it should be rejected.
The location was still correct because the exact-size match carried it.

The strip itself is unmistakably an image: **52.6% nonzero, 238 of 256 distinct
byte values**, and neighbouring pixels differing by one or two
(`aa aa af / ab ab af / ac ac b0 / ae ae b2`) — a smooth grey-blue gradient.

### M37 — put a real frame on the SD card

Enough measuring. M37 streams a whole slot to `sdmc:/applet-mitm-frame.bin` in
64 KB chunks while the game runs, writes the geometry beside it in
`applet-mitm-frame.txt`, and `tools/deswizzle.py` turns it into a PNG on the PC.
Block-linear de-swizzling is done off-console precisely so this step depends on
nothing we have not already proven.

Also fixed: the `varied == 0` hole, the alpha lane (now measured from the strip,
with all four lane counts logged instead of one guessed lane), and an
exactly-sized device-shared region is now accepted on size alone — the corner is
allowed to be flat white.

## M35 run: the swapchain is located

```
searchable region 1: base=0x10851e6000 size=26542080 (slack 0 B)
*** CANDIDATE 0: addr=0x10851e6000  alpha lane=0  varied=1 ***
    slot0 +0x0000000: ff fb ff ff  ff fb ff ff  ff fb ff ff  ff fc ff ff
    slot1 +0x0870000: ff f7 ff ff  ff f8 ff ff  ff f8 ff ff  ff f8 ff ff
    slot2 +0x10e0000: ff f4 ff ff  ff f4 ff ff  ff f4 ff ff  ff f5 ff ff
```

Four independent things agree, so this is not a guess:

1. The region is **exactly 26,542,080 B** — three 8,847,360 B slots, and the same
   size `NVMAP_IOC_PARAM(Size)` reported for handle 1268 back in M13.
2. It is device-shared, as nvmap-pinned memory must be.
3. The candidate is at **offset 0** of that region, so the region *is* the
   surface, not something containing it.
4. All three slots carry the opaque lane, and the varying lane differs per slot
   (`fb` / `f7` / `f4`) — three successive frames of a near-white corner.

**Exactly one candidate survived ~24,000 probed offsets.** The signature is as
selective as hoped.

### Two things to carry forward

- **Addresses are not stable.** The same 41.5 MB region sat at `0x14f5e6d000` on
  one boot and `0x107346d000` on the next. The swapchain must be located at
  runtime, every run. Never cache an address across boots.
- **A region's base is not an allocation's base.** M34 failed precisely here:
  it picked the first device-shared region >= 8,847,360 B, got the 41.5 MB one,
  and applied slot offsets from the wrong origin — reading heap bookkeeping
  (`0x14f5e00000`, a pointer back into that region) and calling it a framebuffer.

Also fixed in M36: M35 printed the candidate's offset against `big[0]` instead of
the region it was found in, so a true offset of 0 was reported as `0x11d79000`.

### M36 — read it live, with the game running

The remaining architectural question is not whether we can read, but whether we
can read *without freezing the target*. `DebugActiveProcess` stops the process;
`ContinueDebugEvent(ExceptionHandled | ContinueAll)` restarts it **with the debug
handle still held**, which is exactly how dmnt reads cheat addresses at 60 Hz.

M36 therefore: drains the queued attach events, calls Continue, and only then
does its measurements —

- samples the live slot twice 120 ms apart; **changing pixels prove the game is
  presenting while we stay attached**;
- reads one full block-row (983,040 B — the full 1920 px width by 128 rows) into
  the capture buffer, timed, and projects whether a whole 8,847,360 B slot fits
  in the 16,667 us that 60 fps allows;
- computes whether a whole block-row looks like an image: nonzero density,
  distinct byte values, and what fraction of pixels carry the opaque lane.

It also checks exactly-26,542,080-byte regions first, which turns the search from
24,000 reads into one and drops the frozen window from ~340 ms to ~1 ms.

## M34 run: the debug read works — and the region picker does not

The NPDM fix was right. On hardware:

```
DebugActiveProcess(pid=142) rc=0x0 ATTACHED (and already detached)
```

**`svcReadDebugProcessMemory` returns the game's memory to us.** After nvmap,
indirect layers and the display controller all refused, the kernel debug path is
open. That is the single result this whole project was blocked on.

What it read, however, was **not pixels**:

```
framebuffer candidate: base=0x14f5e6d000 size=41541632 (larger than one slot)
slot0 +0x0        nonzero=18/32  78 e0 6a d9 73 00 00 00 00 00 e0 f5 14 00 00 00
slot1 +0x870000   nonzero= 0/32  (all zero)
slot2 +0x10E0000  nonzero= 0/32  (all zero)
```

Those 16 bytes are two 64-bit words, `0x73d96ae078` and `0x14f5e00000` — the
second a pointer back into the same region. Heap bookkeeping. The selector took
the first device-shared region >= 8,847,360 B, which is **41,541,632** bytes, not
26,542,080: the swapchain sits *somewhere inside* that region, so the region base
is not the surface origin, and the slot offsets were applied from the wrong zero.

The `*** READ THE GAME'S FRAMEBUFFER ***` line in that log is my own message
being over-eager — it only ever proved that some bytes were readable.

**The search space is small.** Of the 24 regions logged, exactly one is big
enough to hold the swapchain, with 14,999,552 B of slack — 3,662 page-aligned
offsets to test. Note also the walk **hit its 1500-step cap**, so that map is
truncated and there may be further regions above `0x1507b8b000`.

Two side results from the same run: the VIC fill and self-blit are still
**byte-exact** (`ff c0 80 40`, and the ramp `ff 00 00 11 / ff 04 00 11 …`), so the
pipeline is healthy; and `2450` still returns `0x60A` with every handle shape, so
the indirect-layer route stays closed.

### M35 — find the surface instead of assuming where it starts

Scan inside each device-shared region >= 26,542,080 B, at 4 KB steps, for this
signature: the surface is A8B8G8R8 and a presented frame is **opaque**, so one of
the four byte lanes is `0xFF` in every pixel. The first 64 bytes of a
block-linear surface are the first GOB's first row — 16 consecutive pixels — so
the 4-byte period holds there.

Sixteen `0xFF` in one lane is weak alone. Requiring **the same lane at all three
slot offsets at once** is not: heap data does not reproduce that pattern at
exactly 8,847,360-byte spacing, twice. A uniform fill is rejected by requiring at
least one other lane to vary. Step cap raised to 4000 with an explicit
completion flag, read budget capped at 24,000 so the freeze stays bounded.

## The route itself: kernel debug SVCs (M32 -> M52)

Both graphics routes are closed, so go around the graphics stack. Atmosphere's
own cheat engine reads a running game's memory at 60 Hz this way:

```
pm:dmnt GetApplicationProcessId -> svcDebugActiveProcess
  -> svcQueryDebugProcessMemory  (find the framebuffer region)
  -> svcReadDebugProcessMemory   (read the pixels)
```

`TryDebugCapture()` in `applet_mitm_nv.cpp`. Attaches, walks the game's memory
map, selects the region by `MemoryAttribute_DeviceShared` / `device_count > 0`
(not by size - "biggest region" finds the heap), samples 32 bytes at each of the
three known slot offsets, detaches, and only *then* logs. Nothing writes to the
SD while the game is stopped.

### Read straight out of mesosphere - what this route does and does not hit

| question | file:line | answer |
|---|---|---|
| Does `DeviceShared` block the read, the way it blocked nvmap? | `kern_k_page_table_base.cpp:2743` | **No.** `ReadDebugMemory`'s primary gate checks state and permission with an attribute mask of `None`. Any user-readable mapped range qualifies. This is the whole reason the route is viable. |
| What does `DebugActiveProcess` require? | `kern_svc_debug.cpp:28,38` | `IsDebugMode() \|\| CanForceDebugProd()`, **and** `target->IsPermittedDebug() \|\| CanForceDebug() \|\| CanForceDebugProd()`. |
| What do Query/Read require? | `kern_svc_debug.cpp:232,276` | `IsDebugMode() \|\| CanForceDebugProd()`. |
| Could `svcMapProcessMemory` map the swapchain in directly instead? | `kern_svc_process_memory.cpp:92` | **No, permanently.** The source range must have `KMemoryAttribute_None` - *no* attributes set. nvmap-pinned pages always carry `DeviceShared`. Do not spend a build on this. |
| Can we stay attached without freezing the game? | `dmnt_cheat_debug_events_manager.cpp:87` | **Yes.** `ContinueDebugEvent(ExceptionHandled \| ContinueAll, nullptr, 0)` resumes all threads with the debug handle still held. Required for streaming; the one-shot probe does not use it. |
| Will dmnt already hold the debug handle? | `dmnt_cheat_api.cpp:783` | Only if cheats are enabled *and* a cheat file loads for the title. With no cheats installed it never attaches, so no contention. If the user has cheats on for the game, expect our attach to fail. |

### M34 - the bug M33 shipped with

**M33 could not have attached.** `applet-mitm.json` declared no `debug_flags`
capability at all, so gate 2 above reduced to `target->IsPermittedDebug()`,
which is not ours to control. M34 adds the flag `creport` and `dmnt.gen2` both
declare:

```json
{ "type": "debug_flags",
  "value": { "allow_debug": false, "force_debug_prod": false, "force_debug": true } }
```

Verified in the built NPDM by decoding its KAC: capability `0x0008FFFF`
(id_bits 16, payload bit 2 = ForceDebug), alongside 65 granted SVCs including
`0x60/0x63/0x64/0x69/0x6a` and **not** `0x6b WriteDebugProcessMemory`,
`0x62 TerminateDebugProcess`, `0x61 BreakDebugProcess`.

`force_debug` rather than `force_debug_prod` on purpose: `force_debug_prod`
would also satisfy gate 1 without relying on `IsDebugMode()`, but it sets
`IsForceDebugProd()` on the debug object, which narrows what
`CanReadWriteDebugMemory` will allow. Since Atmosphere runs with debug mode on
(dmnt's cheat engine declares no debug flags at all and still works on retail),
gate 1 is already satisfied, and `force_debug` is the less restrictive choice.

### What to read in the M34 log

```
pmdmntInitialize rc=0x0                          <- boot line; 0 means the pid lookup works
DebugActiveProcess(pid=...) rc=0x0 ATTACHED
  [ n] base=0x... size=  26542080 ... attr=0x4 devs=1   <== EXACT SWAPCHAIN SIZE
  *** READ THE GAME'S FRAMEBUFFER ***
```

`attr=0x4` is `MemoryAttribute_DeviceShared`. If the swapchain shows up as three
adjacent 8,847,360-byte regions instead of one 26,542,080-byte region, that is
fine - the hit table lists up to 24 regions, so the split is visible.

If it fails, the rc says which gate:
`ResultNotImplemented` = gate 1 (debug mode), `ResultInvalidState` = gate 2
(the flag did not take, or dmnt holds the handle), `ResultInvalidProcessId` =
no application running.

## The remaining speculative avenue

The `8200`-series shared-buffer commands on `IManagerDisplayService`:
`CreateSharedBufferStaticStorage` (8200), `BindSharedLowLevelLayerToIndirectLayer`
(8204), `ConnectSharedLowLevelLayerToSharedBuffer` (8208). If the game's layer
can be bound to our indirect layer this way, 2450 would populate. It is a chain
of five undocumented ABIs with no documentation to check against, and each
attempt costs a reboot — genuinely speculative, unlike everything above.

---

## Build & test loop

```bash
cd tier4/applet-mitm && bash build.sh          # applies patch_libstrat.py, builds in Docker
```
Then: copy `applet-mitm.nsp` → `atmosphere/contents/0100000000000C20/exefs.nsp`,
keep `flags/boot2.flag`, reboot, launch a game, read
`sdmc:/applet-mitm.log` and `sdmc:/applet-mitm.last` (the last-step breadcrumb).

Recovery: boot holding **Volume Up**, or delete the folder from a PC. Nothing
touches NAND.

## Verified on hardware (do not re-litigate)

| | |
|---|---|
| mitm framework, standalone libstratosphere module | loads, registers |
| `appletOE` | **not mitm-able** (one-session-only). Don't go back. |
| `vi:u` | mitm-able; transparent mitm is invisible to games |
| **libstratosphere patch** for non-domain sub-object forwarding | required, works — see WRITEUP §4 |
| wrap `GetDisplayService` → `IApplicationDisplayService` → `GetRelayService` → `IHOSBinderDriver` | all working, game runs normally |
| binder `TransactParcelAuto` (cmd 3) intercept | sees every frame, **60.0 fps** measured |
| `NvGraphicBuffer` parse from `setPreallocatedBuffer` (code 14) input parcel | nvmap 1268, 3× 1920×1080 A8B8G8R8 BlockLinear, kind 0xFE, block_h_log2 4, offsets 0/0x870000/0x10E0000, pitch 7680, 8,847,360 B/slot |
| hand-rolled `nvdrv:s` (force `__nx_nv_service_type = NvServiceType_System`) | `Initialize` + `Open(/dev/nvmap)` ok |
| `NVMAP_IOC_FROM_ID(1268)` | handle ok, `PARAM(Size)` = 26,542,080 = 3× slot = full swapchain |
| engine survey | `/dev/nvhost-vic`, `-msenc`, `-ctrl` **open**; `-gpu`/`-as-gpu`/`-ctrl-gpu` (0x30003) and `-nvdec`/`-nvjpg` (0x1000) denied — none needed |
| own nvmap buffer: `CREATE` + `ALLOC(kind=Pitch, our cpu_addr)` + `GET_ID` + CPU read-back | works — this is the VIC destination |
| games + homebrew unaffected | yes (probe must release the nvdrv session; it does) |
| **VIC channel usable** | `/dev/nvhost-vic` open, `GET_SYNCPOINT`→12, ctrl `SYNCPT_READ(12)`→~82, `SET_SUBMIT_TIMEOUT` — all clean |

### Do NOT re-try
- ~~`NVHOST_IOCTL_CHANNEL_MAP_CMD_BUFFER` crashed nvservices; use relocs~~
  **SUPERSEDED.** Both halves were wrong. It crashed because M7c called it
  without `SET_NVMAP_FD`, and relocs are *not* the answer on Horizon - a submit
  with relocs for unpinned handles returns InvalidState(8). MAP_CMD_BUFFER is
  the required step; see the Phase B section above.
- Channel devices are **one fd per session** — a leaked survey fd made the real
  open fail `nverr=4096`. The survey now closes each fd.

## *** M16: THE VIC WRITES OUR MEMORY *** — the bug was the missing SETCL

```
job_fill_SETCL   setcl=1  cmd[0]=00001740  changed=16384/65536  *** ENGINE WROTE OUR MEMORY ***
job_fill_plain   setcl=0  cmd[0]=10100002  changed=0/65536      (poison intact)
```

A clean A/B. `METHOD_OFFSET`/`METHOD_DATA` (0x10/0x11) are registers **of the
current host1x class**; nvservices' `CHANNEL_SUBMIT` does *not* set the class to
VIC for us, so without `SETCL(0, 0x5D, 0)` every method write landed on
meaningless registers. `INCR_SYNCPT` lives at register 0x00 in every class,
which is why `OP_DONE` fired for six runs while nothing was produced.

**Geometry is exactly right:** 64 px × 4 B = 256 B per row × 64 rows =
**16384 bytes changed** out of a 65536-byte buffer — the 64×64 image at a
1024-byte stride, precisely as configured.

Output half of the pipeline: **DONE.**

### Colour byte order — still to pin down
We asked for `A=1023 R=1023 G=0 B=0` and got `ff ff 00 00` per pixel: two
channels at max, two at zero, exactly as set — but alpha and red were both
`0xFF`, so it cannot say which byte is which. M17 fills with four **distinct**
levels (`A=1023→0xFF, R=768→0xC0, G=512→0x80, B=256→0x40`) to read the order
straight off the dump.

### The blit hung the engine and froze the console
`job_blit_SETCL` submitted fine but `WAIT nverr=5 … ENGINE DID NOT COMPLETE`.
The rescue restored syncpoint 12, but a **hung VIC still takes the compositor
with it** — the game stopped presenting at txn 492 and the console froze.

Cause: `src` pins to `phys=0`, so the reloc pointed the engine at
`0 + 0x10E0000`. **Pointing the VIC at a bad source address is not a cheap
mistake — it wedges the console.**

## M17 run — byte order pinned down, and relocs proven inert

**Output byte order (settled).** Asked `A=1023(0xFF) R=768(0xC0) G=512(0x80)
B=256(0x40)`, got `ff c0 80 40` repeating. So `OutPixelFormat = 33` writes
memory byte order **A, R, G, B** — i.e. `AV_PIX_FMT_ARGB` for the receiver.

**Relocs do nothing here.**
```
[job_fill]        resolved addrs: cfg=0x00e31c00 dst=0x00e31e00   <- values we wrote
[job_reloc_probe] resolved addrs: cfg=0x00000000 dst=0x00000000 src=0x00000000
```
All three still zero — including `cfg` and `dst`, whose handles pin perfectly.
So our command buffer is never patched.

That **reframes the M16 freeze**: it was not merely `src=0`. In the reloc path
`cfg` was 0 too, so the VIC read its *config struct* from address 0 — garbage
config, hung engine, dead compositor.

**Source pinning is refused three ways:** `is_compr=0`, `is_compr=1` and
`MAP_CMD_BUFFER_EX` all return `nverr=0` with `phys=0x0`. Plausibly deliberate:
addresses are disclosed for handles we own, withheld for another process's.

## *** M19: THE VIC BLIT WORKS, VERIFIED BYTE-FOR-BYTE ***

Source painted by CPU as `A=0xFF, R=x*4, G=y*4, B=0x11`; blitted 64×64 through
the VIC into our linear destination:

```
row  0 expected: ff 00 00 11 ff 04 00 11 ff 08 00 11 ff 0c 00 11 ...
row  0 got     : ff 00 00 11 ff 04 00 11 ff 08 00 11 ff 0c 00 11 ...   EXACT
row 32 expected: ff 00 80 11 ff 04 80 11        (G = 32*4 = 0x80)
row 32 got     : ff 00 80 11 ff 04 80 11                                EXACT
```

Source read, rect setup, `SlotConfig`, `SlotSurfaceConfig`, block-kind handling,
output write, cache maintenance — **the entire VIC pipeline is proven**. No
freeze; 8423 txns at 60 fps.

**One thing is left in the whole project: reaching the game's pixels.**

## M20 run — full permissions obtained, and the pin *still* refuses

`nvdrv:t` opened and the mask demonstrably changed, two independent ways:

```
using nvdrv:t rc=0x0
perm probe: /dev/nvhost-gpu rc=0x0 nverr=0 -> OPEN (bit0 set: full mask)
```
| | cfg | dst | self |
|---|---|---|---|
| `nvdrv:s` | `0xE31C0000` | `0xE31E0000` | `0xE3200000` | ← restricted `0xE0000000+` window |
| `nvdrv:t` | `0x01960000` | `0x01980000` | `0x019A0000` | ← bit 15 FullVaRange |

Self-blit still byte-exact at the new addresses. But the game's handle returns
`phys=0` for all three variants **with `0xFFFFFFFF` permissions**.

So it is not a permission bit. `FROM_ID` hands us a reference; the pages live in
the game's address space and nvservices will not map them into our channel.

## Reading the game's swapchain is STRUCTURALLY BLOCKED — route closed

Five builds established this, and every observation agrees:

| attempt | result |
|---|---|
| `nvdrv:s`, default identity | `phys=0` |
| `is_compr=1` | `phys=0` |
| `MAP_CMD_BUFFER_EX` (0x25) | `phys=0` |
| relocs (let nvservices resolve it) | inert — cmdbuf never patched |
| `nvdrv:t`, **full `0xFFFFFFFF` mask** (verified: `/dev/nvhost-gpu` opens, IOVAs leave the restricted window) | `phys=0` |
| **exact aruid 142 discovered and adopted** (`SetAruidWithoutCheck` rc=0 err=0) | `phys=0` |
| aruid adopted **before** any `Open`, fresh fd under that identity | `phys=0` |

**Why.** At `Initialize` we hand nvservices `CUR_PROCESS_HANDLE`, and it maps
client memory through *that* process handle. The game's swapchain pages live in
the **game's** process. nvservices has no route to map them for our client — so
`FROM_ID` succeeds (it is only a refcounted reference to the object) while
pinning is a silent no-op, and the **IOVA allocator does not even advance**. No
permission bit and no aruid can change that.

**Do not spend further builds on pinning a foreign nvmap handle.**

## What we own, and what that is worth

The hard, reusable half is finished and verified on hardware: a sysmodule that
allocates device memory, pins it, configures the VIC, submits host1x work, and
gets **byte-exact** de-swizzled / scaled / format-converted output back —
`AV_PIX_FMT_ARGB`, with NVENC (`/dev/nvhost-msenc`) also open. What is missing is
only a *source* we are allowed to read.

## M23 — survey what the full mask reaches (read-only)

`nvdrv:t` grants bit 8 (Display) and bit 0 (Gpu), both denied in every earlier
run. A **post-composition** source needs no foreign handle at all, and would
capture the home menu and system overlays — one of the original goals, and
something SysDVR cannot do. M23 opens and immediately closes each of
`/dev/nvhost-display`, `/dev/nvdisp-ctrl`, `/dev/nvdisp-disp0/1`,
`/dev/nvdcutil-disp0`, `/dev/nvcec-ctrl`, `-as-gpu`, `-ctrl-gpu`, `-msenc`,
`-nvdec`, `-tsec`, `-nvjpg` and logs which are reachable.

## M21 — the last untried mechanism: **aruid ownership**

nvmap objects are bound to an **AppletResourceUserId**. libnx's `nvInitialize`
does this for every normal client:
```c
u64 aruid = appletGetAppletResourceUserId();
if (aruid) _nvSetClientPID(aruid);
```
**Our session has never set one** — we are aruid 0, while the game's buffers
belong to the game's aruid. That asymmetry is the remaining explanation for a
silent `phys=0`.

- `SetAruidWithoutCheck` (cmd 7) takes a plain `u64` — **no PID descriptor**, so
  unlike `vi`'s `OpenLayer` it *is* expressible for us. It needs
  `NvDrvPermission` bit 10, which only `nvdrv:t` grants — which we now have.
- `NVMAP_IOC_IS_OWNED_BY_ARUID` (`0x40100113`) is a pure query, so sweeping
  candidate aruids costs nothing and risks nothing. `am` assigns them
  sequentially from boot, so the running application's is small; M21 sweeps
  1..256, adopts any hit, re-imports the handle under the new identity, and
  re-pins.

## Why the game's handle pins to 0 — it is a *permission*, not a bug

`FROM_ID` succeeds, `MAP_CMD_BUFFER` returns `nverr=0` with `phys=0`, and the
IOVA allocator **does not advance** across the attempt (cfg→dst +0x20000,
dst→self +0x20000), so the pin is a silent no-op.

The wiki's `NvDrvPermission` table maps exactly onto everything we have observed:

| bit | meaning | our result |
|---|---|---|
| 0 | Gpu | `/dev/nvhost-gpu` **denied** (0x30003) → clear |
| 3 | VIC | `/dev/nvhost-vic` **open** → set |
| 4 | VideoEncoder | `/dev/nvhost-msenc` **open** → set |
| 5 / 7 | VideoDecoder / JPEG | denied → clear |
| 15 | full VA range | our IOVAs are `0xE31xxxxx`, i.e. **the restricted `0xE0000000–0xFFFE0000` window** → clear |

And the masks are per **service name**:

| service | mask | notes |
|---|---|---|
| `nvdrv` (apps) | `0xA83B` | |
| `nvdrv:a` (applets) | `0x10A9` | |
| **`nvdrv:s` (sysmodules, what we used)** | **`0x439E`** | no bit 10, no bit 12, no bit 15 |
| **`nvdrv:t` (factory)** | **`0xFFFFFFFF`** | everything |

`nvdrv:s` *does* have bit 9 (ImportMemory) — which is why `FROM_ID` works — but
lacks **bit 12** (import *exported* handles) and **bit 10**
(`SetAruidWithoutCheck`). nvmap objects are bound to an **AppletResourceUserId**
(`EXPORT_FOR_ARUID`, `IS_OWNED_BY_ARUID`), and our session has never called
`SetAruid` at all.

**M20** opens `nvdrv:t` (already in our NPDM's `service_access`), falling back to
`nvdrv:s`, and proves which mask it got by opening `/dev/nvhost-gpu` — bit 0 is
clear for `nvdrv:s` and set for `nvdrv:t`. If the pin still returns 0, the next
step is `SetAruidWithoutCheck` with the game's aruid, which bit 10 permits.

## M18 froze the console — my mistake, but it settled the reloc question

`job_fill_reloc` passed **cfg and dst as relocs**, leaving both words zero in the
command buffer. M17 had already proven relocs never patch our buffer, so the VIC
was handed `SET_CONFIG_STRUCT_OFFSET = 0`, read its config struct from address 0,
and hung — taking the compositor and the console with it. Exactly the M16
mechanism. I reasoned "a fill has no source to aim anywhere bad" and missed that
in *that* job the **config struct address itself** was the reloc.

Destructive, but conclusive: **relocs are inert AND the engine really does
receive the unpatched zeros.** Nothing more to test — the reloc path is deleted.

### Structural guards added in M19
Two console freezes have now had the same root cause: an address of 0 reaching
the engine. That is no longer left to per-call-site reasoning.

- `RunOneJob` **refuses to submit** if `cfg_addr`, `dst_addr`, or (for a blit)
  `src_addr` is zero. One check, before every submit.
- `RunOneJob` returns whether the engine completed; the sequence **stops at the
  first failure**, since further submits on a wedged VIC only starve the
  compositor further.
- The whole reloc code path is gone.

### Testing note
A freeze forces a power-off, which truncates the `.log`, and MTP then tends to
return I/O errors on it. **Read the SD in a card reader** (mount it directly)
rather than over MTP — every clean log so far came from the card reader.

## M18 — separate "can we blit at all" from "can we reach the game's memory"

| job | purpose |
|---|---|
| `vb:job_fill_direct` | control, known good |
| `vb:job_fill_reloc` | same fill with cfg+dst as **relocs**. Says whether nvservices patches a private copy (fill works) or relocs are simply inert (nothing). Safe — a fill has no source to aim anywhere bad. |
| `vb:job_blit_self` | **a real blit from a 4th heap buffer WE own**, CPU-painted with a ramp (`A=FF, R=x*4, G=y*4, B=11`). Exercises `SlotConfig`, `SlotSurfaceConfig`, rects and the source read path against a known-good address. |
| `vb:job_blit_game` | still gated behind `src_addr != 0`. |

If `job_blit_self` reproduces the ramp, the VIC pipeline is **complete** and the
only remaining problem is reaching the game's pixels.

## M17 — learn the source address without ever running the engine on it

- `VicJob::RelocProbe`: identical to the blit but with **`EXECUTE` omitted**
  (syncpt cond `IMMEDIATE`). nvservices still patches the reloc addresses into
  our command buffer, so we read back what it resolved for the game's
  handle — with zero risk to the engine.
- Try `MAP_CMD_BUFFER` with `is_compr=0`, then `is_compr=1`, then
  `MAP_CMD_BUFFER_EX` (0x25). All diagnostics; none submit.
- **Hard safety gate:** the blit only runs when `src_addr != 0`. No guessing.

## M15 run — heap+uncached confirmed working, but NOT the fix

```
VIC heap at 0x83f400000: cfg=0x83f400000 cmd=0x83f404000 dst=0x83f410000
SetMemoryAttribute(0x83f400000, 0x4000, uncached) rc=0x0     (all three rc=0)
[vb:job_fill] dst sum32=11206656 changed=0/65536  (untouched - poison 0xAB intact)
```

The heap route and the uncached attribute both work. But the poison survived
**100% intact on all three jobs**, which is stronger than "read back zeros":
the engine is **not writing our memory at all**.

And the decisive detail — the pinned addresses did **not move** when the buffers
did:

| | cfg | dst |
|---|---|---|
| M13/M14, buffers in `.bss` | `0xE31C0000` | `0xE31E0000` |
| M15, buffers on heap `0x83F400000` | `0xE31C0000` | `0xE31E0000` |

Identical. So `MAP_CMD_BUFFER` returns an **SMMU IOVA allocated in request
order**, not a physical page address — the address was never wrong, and
cache/`.bss` was never the blocker either. (Both changes were still correct and
are kept.)

## M16 — the assumption nobody tested: the host1x **class**

`VIC_UCLASS_METHOD_OFFSET` (0x10) and `METHOD_DATA` (0x11) are registers **of the
current host1x class**. libdrm never emits `SETCL` because the DRM kernel driver
sets `job->class = HOST1X_CLASS_VIC` and emits it before the gather. Whether
nvservices' `CHANNEL_SUBMIT` does the same has been an untested assumption for
six runs.

If the class is not VIC:
- writes to 0x10/0x11 hit meaningless registers → engine does nothing
- `EXECUTE` likewise → no output
- but `INCR_SYNCPT` (register 0x00) exists in **every** class → **OP_DONE still fires**

That is exactly the behaviour observed every time.

M16 emits `SETCL(0, 0x5D, 0)` = `0x00001740` at the head of the cmdbuf and A/Bs it
in one run: `job_fill_SETCL`, `job_fill_plain` (control), `job_blit_SETCL`.
Reloc word indices shift by one when SETCL leads. `cmd[0..3]` is logged so the
encoding can be checked on-device.

## M14 run — all three jobs ran, all three wrote nothing

| job | submit | engine | dst |
|---|---|---|---|
| `vb:job_fill` (no source) | `nr=0` nverr=0 | OP_DONE fired | empty |
| `vb:job_blit_direct` | `nr=0` nverr=0 | OP_DONE fired | empty |
| `vb:job_blit_reloc` | **`nr=3` nverr=0** | OP_DONE fired | empty |

Two things settled:
- **Relocs are accepted once the buffers are pinned** — the earlier
  `InvalidState` really was "nothing was pinned", confirmed.
- **`fill` needs no source and failed too.** So the game buffer's `phys=0`
  is *not* what is blocking us. The fault is in the **output** half.

Verified by diffing against the real libdrm header, so these are no longer
suspects: all 9 config structs match field-for-field and bit-for-bit
(`SlotConfig` 65 fields/512 bits, `VicConfigStruct` 1552 B), and every
`NVB0B6_*` method offset matches.

## Root cause: our "device" buffers were ordinary cached `.bss`

libnx's `nvMapCreate` does one thing after `NVMAP_IOC_ALLOC` that we never did:

```c
rc = nvioctlNvmap_Alloc(fd, handle, 0, is_cpu_cacheable ? 1 : 0, align, kind, cpu_addr);
if (R_SUCCEEDED(rc) && !is_cpu_cacheable) {
    armDCacheFlush(m->cpu_addr, m->size);
    svcSetMemoryAttribute(m->cpu_addr, m->size, 8, 8);   /* MemoryAttribute_Uncached */
}
```

Note also that `flags` bit 0 is **cacheable**, not "read-write" as the wiki's
comment says — we pass 0 (non-cacheable), which obliges us to do the CPU-side
half above.

`svcSetMemoryAttribute` is only permitted on `MemoryState_Normal` (heap). Our
buffers were in `.bss`, which is code-mutable — so we had cached, non-coherent
memory standing in for device memory. That is consistent with every symptom:
the engine reports OP_DONE and the read-back is empty.

**M15:** allocate the VIC buffers from the real process heap via
`os::SetMemoryHeapSize` + `os::AllocateMemoryBlock` (2 MB, `svcSetHeapSize`
underneath), then flush + `svc::SetMemoryAttribute(..., Uncached)` on each.
`.bss` drops 1,531,648 → 1,445,632.

**Poison prefill:** dst is now filled with `0xAB`, not zero. "All zero" could not
distinguish *engine wrote zeros* from *engine never touched our memory*;
surviving `0xAB` proves the latter outright.

## M13 run — MAP_CMD_BUFFER works, and **the VIC engine executed**

| | |
|---|---|
| queueBuffer slot parse | **slot=2**, `src_off=0x10E0000` — fixed |
| `MAP_CMD_BUFFER(cfg)` | nverr=0 → **phys=0xE31C0000** (no crash — the M7c note really was wrong) |
| `MAP_CMD_BUFFER(dst)` | nverr=0 → **phys=0xE31E0000** |
| `MAP_CMD_BUFFER(src, game handle 1268)` | nverr=0 → **phys=0x0** ← the blocker |
| `SUBMIT` `nr=0` | rc=0 nverr=0, fence=608 |
| `SYNCPT_WAIT` / `READ` | nverr=0, **610 ≥ 608** |

`cond=OP_DONE` only increments when the **engine** finishes, so the VIC really
ran. It just read from `0x0 + 0x10E0000`, which is not where the game's frame
lives, so it produced nothing. No freeze; 60 fps throughout.

### Two things to fix
1. **`phys=0` for the imported handle.** `PARAM(3=BASE)` is documented as
   *"returns error"* on Horizon, so that route is closed. Options left: pin with
   `is_compr=1`, or let **nvservices resolve the address itself via a reloc** —
   which may well work now that everything is pinned (the earlier
   `InvalidState` was almost certainly *because* nothing was).
2. **No cache maintenance.** `g_vic_dst_buf` is CPU memory; the VIC writes it
   device-side. Without `armDCacheFlush` before the read-back, the CPU returns
   its own stale zeros **regardless of what the engine wrote**. This alone could
   have masked a working blit.

## M14 — three jobs per run
One reboot now answers everything. Each job zeroes + flushes dst, submits,
waits, rescues the syncpoint, invalidates, and checksums:

| job | what it isolates |
|---|---|
| `vb:job_fill` | **no source at all** (libdrm `vic40_fill`): config struct, dst address, EXECUTE, cache handling. Non-zero here = the whole output half works. |
| `vb:job_blit_direct` | source address inlined from MAP_CMD_BUFFER |
| `vb:job_blit_reloc` | source address left to nvservices via a reloc, everything pinned |

## Phase B first attempt — `InvalidState`, and it named the missing step

```
cmdbuf words=20  execute=1  incr_syncpt=12 cond=OP_DONE
SUBMIT req=0xC0700001 sz=112 nr=3  rc=0x0 nverr=8 -> fence(syncpt=12, val=0)
vb:11_SUBMIT_FAILED
```

`nverr=8` = **InvalidState** (switchbrew NV_services error table; the `nverr=5`
we saw earlier is Timeout, which fits too). Rejected cleanly *before* doing
anything — **no freeze**, game ran on to 8284 txns at 60 fps, clean exit. The
safety design did its job.

Phase A (`nr=0`) works, Phase B (`nr=3`) does not: the difference is **relocs**.
Per switchbrew, `NVHOST_IOCTL_CHANNEL_MAP_CMD_BUFFER` *"uses **nvmap_pin**
internally to pin nvmap handles to an appropriate device physical address"* and
returns `phys_addr_out`. **Horizon replaced Linux's per-submit pinning with
explicit pinning** — so a submit whose relocs name unpinned handles is exactly
`InvalidState`.

### Why MAP_CMD_BUFFER crashed in M7c (and why it should be safe now)
M7c called it **without `SET_NVMAP_FD`** — that ioctl only arrived in M8. With no
nvmap client bound to the channel, `nvmap_pin` had nothing valid to resolve
handles against. We now call `SET_NVMAP_FD` first (`nverr=0`, verified twice).
*The "do NOT re-try MAP_CMD_BUFFER" note below is therefore superseded.*

### M13 approach: pin, then inline the addresses
Because MAP_CMD_BUFFER *returns* the address, relocs are unnecessary:
pin cfg/dst/src, write `phys >> 8` straight into the cmdbuf, and submit with
**`num_relocs = 0`** — the exact 52-byte shape Phase A proved. Each pin is
separately breadcrumbed (`vb:8a/8b/8c`) so a crash names the guilty handle, and
all three are unpinned on the way out.

### queueBuffer slot parse — fixed
The parcel starts with `writeInterfaceToken`:
`[u32 strict_mode][u32 len][UTF-16 name, len+1 units, pad to 4]`. The `256` we
kept reading was `STRICT_MODE_PENALTY_GATHER`. For
`android.gui.IGraphicBufferProducer` (34 units) the slot is at parcel offset
**96**; now parsed properly rather than assumed.

## Phase A — **PASSED** (M11, verified on hardware)

```
cmdbuf words=5  execute=0  incr_syncpt=12 cond=IMMEDIATE
SUBMIT req=0xC0340001 sz=52 nr=0  rc=0x0 nverr=0 -> fence(syncpt=12, val=584)
SYNCPT_WAIT(id=12 thr=584)        rc=0x0 nverr=0      <- was nverr=5
SYNCPT_READ(id=12) -> value=586   (want >= 584)       <- passed the fence
```

No rescue fired, **no freeze**, and **59.2 fps sustained for 51 s** after the
submit (worst 3 s window 55.0 fps). The full host1x path is now proven:
channel open → `SET_NVMAP_FD` → `CHANNEL_SUBMIT` → syncpoint increment → wait.

Nothing about driving the VIC from a mitm sysmodule is unknown any more. What
remains is getting the VIC's own config right.

### Known bug (harmless for now)
The `queueBuffer` parcel slot parse returns **256**, not 0/1/2. `RunVicBlit`
clamps out-of-range to slot 0, which is a live framebuffer either way (the game
rotates all three), so the blit still sees real pixels. M12 dumps the parcel
head so the layout can be decoded properly.

### Phase B alignment fix
libdrm's `vic_image_new` aligns every VIC surface stride to **256 pixels**
(`align = 256; stride = ALIGN(width, align)`), pitch-linear included. The 64 px
dst stride (256 B pitch) was almost certainly under-aligned, so the dst is now
64×64 with a **256 px / 1024 B** stride (`DstSize` 0x10000).

## Phase A run — CHANNEL_SUBMIT ABI **verified**, and the freeze root-caused

The whole probe ran end to end on the worker thread, and the game kept
rendering through it. Everything up to the submit is now proven on hardware:

| step | result |
|---|---|
| `smGetService("nvdrv:s")`, tmem, `Initialize` | rc=0 |
| `Open(/dev/nvmap)` | fd, nverr=0 |
| `FROM_ID(1276)` | handle=1276 |
| our 3 nvmap buffers (cfg/cmd/dst) | handles 3948 / 3952 / 3956 |
| `Open(/dev/nvhost-vic)` + `GET_SYNCPOINT` | syncpt **12** |
| **`SET_NVMAP_FD`** | rc=0 **nverr=0** |
| **`NVHOST_IOCTL_CHANNEL_SUBMIT`** `req=0xC0340001 sz=52 nr=0` | **rc=0 nverr=0**, fence(syncpt=12, **val=586**) |
| `SYNCPT_WAIT(12, 586)` | nverr=5 (timeout) |
| `SYNCPT_READ(12)` | **585** — one short of the fence |

**The submit ABI is correct.** The bug: `syncpt_incrs` in the submit only tells
nvhost to raise the syncpoint's *max*; the increment itself must be **programmed
into the command stream**. Our cmdbuf never did, so:

- our own wait timed out (585 < 586), **and**
- syncpoint 12 is the **VIC's**, which **nvnflinger composites on** — so every
  later waiter stalled forever. That is what froze the console at the title
  screen, both times. One bug, both symptoms.

Fix (libdrm `drm_tegra_pushbuf_sync_cond`): append
`NONINCR(UCLASS_INCR_SYNCPT=0x0, 1)` + `cond << 8 | syncpt_id`.
VIC 4.0 reports version `0x21` → `cond_shift = 8`; `IMMEDIATE=0`, `OP_DONE=1`.
Phase A uses IMMEDIATE (no engine op), Phase B uses OP_DONE (after `EXECUTE`).

**Safety net added:** after the wait, if the syncpoint is still short of the
fence, force it up with `NVHOST_IOCTL_CTRL_SYNCPT_INCR`. A bad command stream
now costs the probe, not the console.

## The M8/M8b blackout — SOLVED (M9 observer run, verified)

The module was never broken. **Two symptoms, one cause:**

1. `TryVicBlit` ran **on the game's binder dispatch thread**. It blocked there,
   so `queueBuffer` never returned → `vi` wedged → whole system froze.
2. That forced a power-off, and **the power-off ate the log**. A forced cut
   loses the `.log` tail before FAT commits — leaving exactly the 150 bytes
   written at t≈9.3 s. "No logs" never meant "no logging".

Timing confirms it: the trigger condition `txn > 300` lands at **t≈40 s**, ~8 s
into rendering — the title screen, exactly where M8b froze.

**M9 observer run (clean shutdown) was perfect:** `sess=1 getdisp=1 relay=1
txn=6251`, heartbeat to 103 s, and a **rock-steady 60.0 fps for a full minute**
(120.4 / 119.0 / 120.9 … txn/s). All three swapchain slots captured: nvmap 1268,
1920×1080, pitch 7680, kind 0xFE, `block_h_log2=4`, pixfmt 33 (A8B8G8R8),
offsets `0x0` / `0x870000` / `0x10E0000`. The mitm chain is transparent.

**Rule going forward: nothing that can block runs on the binder thread.**

### Test protocol (learned the hard way)
Always **exit the game and power off from the menu**. If the console does wedge,
`.last` is the only reliable evidence — it is rewritten whole on every mark.

## M8/M8b blackout — what it was NOT (two hardware runs)

Both builds went **completely silent after `registered mitm server for vi:u`**
while the system wedged on `vi` (force power-off needed). Ruled out so far:

| Theory | Verdict |
|---|---|
| Build didn't pick up the sources | **No** — every new format string is in the shipped ELF (`strings`) |
| `.bss` growth tipped a memory limit | **No** — M7d `.bss` = 1,453,824; M8b = **1,441,536**, i.e. *smaller* than the known-good build |
| Our new code executed and hung | **Unlikely** — M8b does strictly *less* than M7d before frame 300, and never logged even `GetDisplayService` |
| MTP showed a stale copy | **No** — `.log` is byte-exactly the two boot lines (150 B) |

Why "no logs" is ambiguous by construction, and why the system wedges either way:
`sm` blocks **every** `vi:u` open on our mitm query port, so a dead `LoopProcess`
freezes the system identically to a hung handler — and neither writes a line.
A forced power-off can also lose the `.log` tail before FAT commits, so absence
of logs may not even mean absence of logging.

**M9 is the instrument for this**, not another guess: see below.

## M9 — heartbeat + opt-in arm gate

- **Heartbeat thread** (3 s, independent of the dispatch path) writes
  `hb:<n> sess=<n> getdisp=<n> relay=<n> txn=<n>` via `LogMark`, which rewrites
  `.last` whole — so it survives a hard power cut and bounds process lifetime.
- **Opt-in**: nothing touches nvdrv/VIC unless `sdmc:/applet-mitm.armed`
  contains `vic`. The default build is a pure observer == M7d behaviour.
- `main:heartbeat_started` / `main:LoopProcess` / `main:LoopProcess_RETURNED` marks.

Reading the result:

| `.last` after the run | Meaning |
|---|---|
| `LogInit` only | died before `Main` got going — logger or very early abort |
| `main:LoopProcess`, no `hb:` | heartbeat thread never ran |
| `hb:N` climbing, `sess=0` | **process alive, never receives a session** — the mitm/query routing is the problem, not our handlers |
| `hb:N`, `sess>0`, `getdisp>0`, `txn` climbing | module is fine; the `.log` loss was a flush/power-cut artifact |
| `hb:` stops at N | process died at ~3N seconds |

## M8 — VIC CHANNEL_SUBMIT blit (built, gated behind the arm file)

`applet_mitm_nv.cpp::TryVicBlit()` + `vic40_config.hpp`. On queueBuffer #>300
(real content on screen), one-shot:

1. nvdrv:s up, import game nvmap (`FROM_ID`), `SET_NVMAP_FD`.
2. `CREATE`+`ALLOC` three of our own nvmap buffers: config (0x4000), host1x
   cmdbuf (0x1000), linear dst (0x40000).
3. Fill `vic::VicConfigStruct` (1552 B, `/16 = 97`) for a **scale-free 320×180
   crop** of the frame's top-left: src A8B8G8R8 block-linear kind 0xFE
   `SlotBlkHeight=4` `SlotCacheWidth=64Bx4`, dst A8B8G8R8 pitch.
4. Build host1x pushbuf (libdrm `vic40_execute`): SET_APPLICATION_ID=1,
   SET_CONTROL_PARAMS=97<<16, SET_CONFIG_STRUCT_OFFSET / OUTPUT_SURFACE_LUMA /
   SURFACE0_SLOT0_LUMA as reloc placeholders (shift 8), EXECUTE=1<<8. 18 words.
5. `NVHOST_IOCTL_CHANNEL_SUBMIT` (`0xC0700001`, 1 cmdbuf + 3 relocs + 1
   syncpt_incr + 1 fence). Wait fence via ctrl `SYNCPT_WAIT` (300 ms).
6. Checksum + hexdump `g_vic_dst_buf`; release everything.

Breadcrumbs `vb:1`..`vb:VIC_BLIT_DONE` in `applet-mitm.last`. Failure modes:
malformed submit → possible nvservices fatal (recover as below); wrong
config → VIC faults, `SYNCPT_WAIT` times out, clean release.

**Read after test:** `vb:11` line (SUBMIT rc/nverr/fence), `vb:13` dst
sum32/nonzero + `dst[0..64]` (all-zero pre, structured post = de-swizzle works).

## Current module behaviour

`applet_mitm_service.cpp` wraps the chain and logs binder transactions
(rate-limited). First 8 `setPreallocatedBuffer` → `CaptureGameSurface()` records
nvmap id + per-slot plane offsets + geometry into `g_game_surface`. First
queueBuffer past txn 300 → `TryVicBlit(slot)` (one-shot).

## Next: drive the VIC (implementation, no open unknowns)

The VIC reads block-linear and writes linear, converting format in hardware —
exactly how nvnflinger consumes these buffers. Steps:

1. `/dev/nvhost-ctrl`: allocate a syncpoint (`NVHOST_IOCTL_CTRL_SYNCPT_ALLOC` /
   read via `..._SYNCPT_READ`).
2. `/dev/nvhost-vic`: `NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD`, then bind the channel
   and map the source (imported nvmap 1268) and destination (our nvmap) into
   the channel's address space.
3. Build the VIC configuration struct — the big one. Describes:
   - src surface: 1920×1080, A8B8G8R8, BlockLinear, kind 0xFE,
     `block_height_log2 = 4`, pitch 7680, plane offset = the slot's
     `offset` (0 / 0x870000 / 0x10E0000 depending on which slot `queueBuffer`
     just handed over)
   - dst surface: linear, our buffer, our pitch
   - (optionally) scale/crop — VIC can downscale for free if 720p output wanted
   **References: Ryujinx `src/Ryujinx.Graphics.Vic/` and yuzu
   `src/video_core/host1x/vic.cpp` both implement this struct.** L4T kernel:
   `drivers/video/tegra/host/vic/`.
4. Submit a command buffer on the channel (`NVHOST_IOCTL_CHANNEL_SUBMIT`) that
   loads the config and kicks the VIC, with a syncpoint increment. Wait it.
5. Trigger from the `queueBuffer` (code 7) intercept. Its parcel carries the
   **slot index** and a **fence** — parse both; wait the fence so the frame is
   complete, pick the plane offset from the slot, blit.

## Then: NVENC + transport

- NVENC via `/dev/nvhost-msenc`: same channel-submit pattern. T210 == Jetson
  TX1, so L4T's multimedia sources (`nvmpi`, `nvv4l2`) are the reference. Feed
  it the linear buffer from the VIC.
- Transport: SysDVR's USB/TCP protocol, or the low-latency FFmpeg+SDL2 receiver
  already in `switch-stream/receiver/`.

## Files

```
applet-mitm/
  build.sh              rsync into ref/Atmosphere, apply patch, docker build, copy .nsp back
  patch_libstrat.py     the non-domain mitm sub-object forwarding patch (idempotent)
  applet-mitm.json      NPDM: service_host vi:u; service_access fatal:u lm fsp-srv
                        nvdrv{,:a,:s,:t} vi:m vi:s pm:dmnt; handle_table_size 512;
                        pool_partition 1 + application_type 2 (both Applet, M54/M55);
                        debug_flags force_debug; read-only debug SVCs; usb:ds
  source/
    applet_mitm_main.cpp     ServerManager, RegisterMitmServer("vi:u"), nv weak-global overrides
    applet_mitm_service.*    the wrapper chain + binder intercept
    applet_mitm_gbuf.*       NvGraphicBuffer / NvSurface layout + parser (offset static_asserts)
    applet_mitm_nv.*         hand-rolled nvdrv, the VIC pipeline (RunOneJob /
                             BuildCmdbuf / AppendIncrSyncpt), TryIndirectCapture,
                             TryDebugCapture
    vic40_config.hpp         VIC 4.0 methods + the 9 config structs, diffed
                             field-for-field against libdrm
    applet_mitm_log.*        SD logger + LogMark breadcrumb
```
