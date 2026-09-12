# applet-mitm — status & resume point

Console: Mariko, FW **22.5.0**, Atmosphère **1.11.2**. Module TID
`0100000000000C20`. 47 hardware test cycles. Current build: **M69**.

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

## What is blocked, and why — BOTH ROUTES CLOSED

**We can process frames. We cannot legally obtain one.**

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
