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
- `NVHOST_IOCTL_CHANNEL_MAP_CMD_BUFFER` from this context **crashed nvservices**
  (white flash + `fatal`-service abort). It pins memory into the channel and is
  fragile here. The real VIC path is `NVHOST_IOCTL_CHANNEL_SUBMIT` with a reloc
  list; the kernel pins per-submit. Don't use MAP_CMD_BUFFER.
- Channel devices are **one fd per session** — a leaked survey fd made the real
  open fail `nverr=4096`. The survey now closes each fd.

## M8 — VIC CHANNEL_SUBMIT blit (awaiting hardware test)

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
