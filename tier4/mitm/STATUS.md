# applet-mitm — status & resume point

Console: Mariko, FW **22.5.0**, Atmosphère **1.11.2**. Module TID
`0100000000000C20`. 47 hardware test cycles. Current build: **M34**.

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

- **Never take large static memory.** A sysmodule's `.bss` comes from the shared
  system pool (`pool_partition 2`). A 4 MB array fataled *another* sysmodule with
  `0x10801 LimitReached` at boot. `.bss` lives at ~1.45 MB; the heap ceiling is
  **2 MB**.
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

## The route itself: kernel debug SVCs (M32 -> M46)

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
                        pool_partition 2; debug_flags force_debug; read-only debug SVCs
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
