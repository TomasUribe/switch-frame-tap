# applet-mitm — status & resume point

Console: Mariko, FW **22.5.0**, Atmosphère **1.11.2**. Module TID
`0100000000000C20`.

Read **[WRITEUP.md](WRITEUP.md)** first for the why. This file is the what-now.

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
  applet-mitm.json      NPDM: service_host vi:u, service_access nvdrv:s + fsp-srv/lm/fatal:u
  source/
    applet_mitm_main.cpp     ServerManager, RegisterMitmServer("vi:u"), nv weak-global overrides
    applet_mitm_service.*    the wrapper chain + binder intercept
    applet_mitm_gbuf.*       NvGraphicBuffer / NvSurface layout + parser (offset static_asserts)
    applet_mitm_nv.*         hand-rolled nvdrv: import, engine survey, own-buffer alloc
    applet_mitm_log.*        SD logger + LogMark breadcrumb
```
