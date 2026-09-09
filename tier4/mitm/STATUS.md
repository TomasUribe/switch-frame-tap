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

**M3b (built, awaiting result)** parses it — see `applet_mitm_gbuf.{hpp,cpp}`.
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
