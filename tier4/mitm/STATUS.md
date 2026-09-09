# Track B status — read this first

Console: Mariko, **FW 22.5.0**, Atmosphère 1.11.2. Module TID `0100000000000C20`.
Build: `bash tier4/applet-mitm/build.sh` (patches libstratosphere, builds in the
`devkitpro/devkita64` container, copies the `.nsp` back).
Test loop: replace `atmosphere/contents/0100000000000C20/exefs.nsp`, keep
`flags/boot2.flag`, reboot, launch a game, read `sdmc:/applet-mitm.log` and
`sdmc:/applet-mitm.last`.

## Proven working (do not re-litigate)

1. **M1** — a standalone libstratosphere mitm sysmodule loads and registers.
2. **`vi:u` is mitm-able.** `appletOE` is NOT (one-session-only service;
   even a fully transparent mitm breaks game launch). Don't go back to it.
3. **Non-domain mitm sub-object wrapping works** — but only because of our
   patch. See below. This is the core enabler and it is verified on hardware
   (games run with a wrapped `IApplicationDisplayService`).
4. **Manual forwarding works** on a wrapped sub-object
   (`OpenDisplay` → `display_id=17`).
5. **Auto-forwarding works** for undeclared commands on a wrapped sub-object
   (step 2: empty interface, games ran fine).

## The libstratosphere patch (`tier4/applet-mitm/patch_libstrat.py`)

Upstream only wires a forward service to sub-objects returned from a mitm
command on **domain** sessions. A game's `vi:u` session is **non-domain**, so
`SetOutObjectImpl` used plain `RegisterSession` → the sub-session is not a mitm
session → the first undeclared command hits
`ForwardRequest()`'s `AMS_ABORT_UNLESS(this->IsMitmSession())` → `std::abort()`
= the `0xffe` fatal.

The patch adds a thread-local `ams::sf::impl::g_tier4_pending_mitm_forward`.
A handler sets it right before `out.SetValue(...)`; the patched
`SetOutObjectImpl` consumes it and calls `RegisterMitmSession` (with forward
service) instead. Same thread, same dispatch call stack. Idempotent; applied by
`build.sh` on every build. **Editing that header forces a full ~15 min
libstratosphere rebuild** — expect it, and run the build in the background.

## Hard-won ABI rules (these cost us several console cycles)

- `AMS_SF_DEFINE_MITM_INTERFACE` must be at **global scope**, not inside a
  namespace block — it expands its own `namespace`. Nesting it produces a wall
  of template errors.
- libstratosphere sorts raw args by **alignment ascending**
  (`RawDataOffsetCalculator`). For `OpenLayer` that happens to match libnx's
  wire struct, so ordering was never the bug.
- **`sf::ClientProcessId` is `ArgumentType::InData`** — it consumes a `u64` of
  raw data (the SDK's pid placeholder, cf. `fsp-srv SetCurrentProcess` =
  "PID-descriptor **and an input u64**"). A command that has `send_pid` in the
  HIPC header but **no** raw placeholder (like `vi OpenLayer`) therefore
  **cannot be expressed** in libstratosphere. Declaring `ClientProcessId` makes
  `InDataSize` 8 too big; omitting it leaves the dispatch rejecting the message.
  **This is why `OpenLayer` is not declared and should stay undeclared.**
- Declaring a command whose signature doesn't match causes a **dispatch-level
  error return** (game fails, no crash), not an abort. An abort means something
  else — read `.last`.
- Only declare commands you actually intercept; everything else auto-forwards.
- `TransactParcel` is **cmd 0 (MapAlias)** pre-3.0.0 and **cmd 3 (AutoSelect)**
  on ≥ 3.0.0. We're on 22.5.0 → cmd 3.

## M3 RESULT: we are on the frame pipeline ✅

Verified on hardware with Mario Kart 8 Deluxe (`0100152000022000`), game
running normally:

```
-> GetRelayService:wrapped
*** binder txn #1  session=20  code=10(connect)   in=108
*** binder txn #3  session=20  code=14(?)         in=476   x3   <- setPreallocatedBuffer
*** binder txn #6  session=20  code=3(dequeueBuffer)
*** binder txn #7  session=20  code=1(requestBuffer)
*** binder txn #600  at 43.237s  queueBuffer x296
*** binder txn #1200 at 48.237s  queueBuffer x596
```

600 transactions in exactly 5.000 s; queueBuffer 296 -> 596 = **300 frames in
5 s = 60.0 fps**. We observe every frame the game presents.

The three `code=14` (`SET_PREALLOCATED_BUFFER`, Nintendo's extension) calls at
startup are MK8 registering its triple-buffered swapchain. Each carries a
flattened `NvGraphicBuffer` in its **input** parcel.

## M3b RESULT: full frame descriptor decoded ✅

The three `setPreallocatedBuffer` parcels decode (after the u64 `color_format`
fix in M3c) to:

```
nvmap_id  = 1268                 <- ONE nvmap object holds the whole swapchain
buffers   = 3, at offsets 0x000000, 0x870000, 0x10E0000  (0, 1x, 2x total_size)
each      = 1920 x 1080, A8B8G8R8 (0x0100532120), format=1 (RGBA_8888)
layout    = 3 (BlockLinear)
kind      = 0xFE (Generic_16BX2)
block_height_log2 = 4            (block height 16)
size      = 8,847,360 B each     = 1920 x 1152 x 4 (height 1080 aligned to 1152)
total_size= 8,847,360   stride = 1920 px   usage = 0xB00
```

**The game's swapchain is native 1920x1080** — vs SysDVR's 720p30 via `grc:d`.

Gotcha that cost one run: libnx's `NvColorFormat` is a **64-bit** enum
(`A8B8G8R8 = 0x0100532120`). Declaring it `u32` shifts every following
`NvSurface` field by 4, which made `pitch`/`offset`/`kind`/`block_height_log2`
read as garbage while `width`/`height`/`size` still looked right. Fixed, with
offset static_asserts (`layout` @0x10, `offset` @0x1C, `kind` @0x20,
`size` @0x38).

**M3c CONFIRMED on hardware** — every field decodes cleanly
(`pitch=7680 B` = 1920x4, offsets exactly 0 / 1x / 2x size,
`layout=3(BlockLinear)`, `kind=0xfe(Generic_16BX2)`, `block_h_log2=4`,
`scan=0(Progressive)`). See `applet_mitm_gbuf.{hpp,cpp}`. **M3 is done.**

## M4b RESULT: we can open the game's framebuffer memory ✅

```
nv:1_smGetService      rc=0x0
nv:2_tmemCreateFromMemory rc=0x0  handle=0xa0002
nv:3_Initialize        rc=0x0
nv:4_open_nvmap        rc=0x0  fd=23855104  nverr=0
nv:5_FROM_ID(id=1268)  rc=0x0  nverr=0  -> handle=1268
nv:6_PARAM(Size)       rc=0x0  nverr=0  -> 26542080 B (25 MB)
```

**26,542,080 = exactly 3 x 8,847,360** — the whole triple-buffered swapchain.
nvmap ids ARE cross-process, and Phase 0's `nvInitialize()` fatal was entirely
libnx's `appletGetAppletType()` service selection. `PARAM(Kind)` returns
nverr=11; kind is not queryable on an imported handle and we already have it
from the GraphicBuffer descriptor, so it does not matter.

### Regression found and fixed
Holding the nvdrv session + the imported nvmap handle open forever wedged
homebrew apps. A probe must release them: M5 adds `serviceClose` + `tmemClose`
after the survey. When we build the real capture loop we will hold them
deliberately, scoped to a running game.

## M5 (built, awaiting result): which engines can we reach?

With a working nvdrv session, `Open` each `/dev/nvhost-*`. The one that matters
is **`/dev/nvhost-vic`** — the VIC reads a block-linear surface and writes a
linear one, doing the de-swizzle *and* format conversion in hardware, which is
exactly how nvnflinger consumes these buffers. `/dev/nvhost-msenc` is NVENC for
the encode stage.

## Superseded: M4 first attempt

`nvmap_id` is a **cross-process** id — it has to be, because nvnflinger runs in
a different process and receives this same parcel in order to composite the
buffer. So `NVMAP_IOC_FROM_ID` on our own nvdrv session should reach the same
memory. That is exactly how nvnflinger consumes it.

Phase 0 saw a plain sysmodule fatal inside libnx's `nvInitialize()`. Cause:
`_nvInitialize()` calls `appletGetAppletType()` to choose a service, which is
meaningless in our context. Fix: override the weak global
`__nx_nv_service_type = NvServiceType_System` (forces `"nvdrv:s"`, never touches
applet) and `__nx_nv_transfermem_size = 0x40000` (libnx's 3 MB default would not
fit our allocator; our ioctls are tiny). **Also had to add `nvdrv:s` to
`service_access` in applet-mitm.json** — it was not there.

One-shot probe, breadcrumbed at each step: `nvInitialize` -> `nvMapInit` ->
`nvMapLoadRemote(id)` -> log handle/size. Expected size if it works:
3 x 8,847,360 = 26,542,080 B (~25 MB) for the whole swapchain.

### After M4 — reading the pixels (the real remaining work)

Getting an nvmap *handle* is not the same as getting a CPU pointer. On the
Switch, nvmap has no MMAP ioctl: the creating process allocates the backing
memory itself and nvmap just tracks it. A foreign object's pages are not
CPU-mapped into us. So expect to need one of:

- **VIC (Video Image Compositor)** via `/dev/nvhost-vic` — purpose-built to read
  a block-linear surface and write a linear one. This is what nvnflinger uses,
  and it does the de-swizzle **and** format conversion for free. Best option.
- **GPU blit** via `/dev/nvhost-gpu` + a channel (deko3d/NVN-style) — more setup.
- CPU de-swizzle, which still requires the pages mapped somehow.

Phase 0 recon's `probe_nv` can tell us which `/dev/nvhost-*` nodes we can open
now that we have a working nvdrv session — worth re-running that list from
inside this module.

Block-linear parameters we already have: GOB is 64x8 bytes,
`block_height_log2 = 4` (16 GOBs tall), `kind = 0xFE` (Generic_16BX2),
`pitch = 7680`, surface 1920x1080 stored as 1920x1152.
Layout pinned with static_asserts: `sizeof(NvGraphicBuffer) == 0x150`,
`nvmap_id` @0x10, `magic`(0xDAFFCAFF) @0x18, `planes` @0x40,
`sizeof(NvSurface) == 0x58`. We scan the parcel for the magic and dump
nvmap_id, stride, format, and per-plane width/height/pitch/offset/size/kind/
block_height_log2.

## Previous section: M3 design

Dropped `OpenLayer` (its only purpose was `aruid` + `layer_id`, and it's the one
thing that broke games). Went straight for the frame pipeline instead:

```
vi:u.GetDisplayService(0)      -> wrap IApplicationDisplayService
  .OpenDisplay(1010)           -> manual forward, logs display_id (liveness)
  .GetRelayService(100)        -> wrap IHOSBinderDriver
    .TransactParcelAuto(3)     -> LOG every IGraphicBufferProducer transaction
```

Every frame the game presents crosses the binder as `dequeueBuffer`(3) /
`queueBuffer`(7); `requestBuffer`(1) replies carry the GraphicBuffer descriptor
(nvmap handle, stride, format) — the actual pixel data path. `TransactParcelAuto`
has **no pid descriptor**, which sidesteps the `OpenLayer` blocker entirely.

Logging is rate-limited (first 3 of each code + a heartbeat every 600) because
`queueBuffer` runs at up to 60 Hz and one open/append/close per line would
hammer the SD card.

### Reading the M3 result

| Log | Meaning | Next |
|---|---|---|
| `*** binder txn … code=7(queueBuffer)` repeating | **We are on the frame pipeline.** Milestone. | Manual-forward `TransactParcelAuto` so we can read the *reply* parcel, then parse `requestBuffer` replies for the GraphicBuffer (nvmap handle/stride/format) |
| `GetRelayService:wrapped` but no txns | binder obtained but cmd 3 signature wrong | try declaring cmd 0 as well (MapAlias buffers) |
| `GetRelayService:forward_FAILED rc=…` | the `serviceDispatch(…,100,…)` forward was rejected | check cmd number / out-object handling |
| game fails, `.last` = `GetRelayService:enter` | wrapping the binder breaks the game | fall back to log-only via auto-forward |

## After M3 — the remaining road

1. Parse `requestBuffer` reply parcels → GraphicBuffer → nvmap handle, stride,
   format, and the buffer's dimensions.
2. Import that nvmap handle read-only in our process, wait the fence from
   `queueBuffer`, and copy/detile the frame (Tegra block-linear, 16Bx2 sectors).
3. Encode. Bring-up: software or MJPEG. Target: Tegra NVENC (T210 = Jetson TX1,
   so L4T's multimedia sources are the reference).
4. Transport: reuse SysDVR's USB/TCP or `switch-stream/receiver/` in this repo.

## Recovery, always

Boot holding **Volume Up** (skips `contents` sysmodules), or pull the SD and
delete `atmosphere/contents/0100000000000C20/`. A fatal writes a fresh file to
`atmosphere/fatal_errors/`. The console is never at real risk — see the earlier
risk analysis; user has a NAND backup.
