# switch-frame-tap

Building a Nintendo Switch → PC screen streamer that runs at **native
resolution and 60 fps**, and the research needed to get there. Homebrew,
developed on and for the author's own console.

> **There is a working end-to-end stream.** Live video from the console to a PC
> over USB at **480x270 / 59.4 fps**, with a one-process libusb + SDL2 viewer.
> Capture, downscale and transport are solved and measured on hardware.
>
> **It is still not a finished tool.** The goal is native resolution at 60 fps,
> and raw pixels cannot get there: USB 2.0 bulk delivers ~31 MB/s, while 720p60
> needs 83 MB/s even in NV12. The remaining work is **compression**, or
> SuperSpeed. What is finished is finished properly and verified on hardware;
> what is not is marked as such throughout. See [Roadmap](#roadmap).

**Console under test:** Mariko, firmware **22.5.0**, Atmosphère **1.11.2**.
47 hardware test cycles.

![Mario Kart 8 Deluxe captured at native 1920x1080 from the game's own swapchain](docs/frame-1080p.png)

*A real capture: read out of the game's swapchain by the sysmodule, de-swizzled
from Tegra block-linear on the PC. Native 1920x1080, docked.*


## How this was built — with an AI, openly

I built this together with **Claude** (Anthropic's AI model, Opus 5, through
Claude Code). I want to be straightforward about that, because it should change
how you read and trust what's here.

- **Claude** wrote nearly all of the code and the documentation, did the
  source-reading (Atmosphère and its kernel mesosphere, libdrm, switchbrew),
  designed each hardware probe, and interpreted the logs that came back.
- **I** set the goal and the direction, ran every one of the 47 hardware tests
  on my own console, read the logs back off the SD card, decided which routes to
  keep pushing and when to drop one, and decided what to publish.

What that means for you:

- **"Verified on hardware" means exactly that** — a real run on a real console,
  with the log. Conclusions drawn from reading source rather than running it are
  labelled as such.
- **The AI got things wrong, and some mistakes were not cheap.** Since Claude
  wrote nearly all the code, the bugs are its bugs: including the ones that
  froze my console, one that fataled another sysmodule at boot, and a build
  (M33) that shipped without the NPDM flag it needed to work at all. Every one
  is recorded in [`tier4/mitm/STATUS.md`](tier4/mitm/STATUS.md). That log is
  written in the first person because Claude wrote it as the work happened.
- **The history shows it too.** Most commits carry a
  `Co-Authored-By: Claude` trailer.

Review it as you would a contribution from someone you haven't worked with
before — which is good advice for homebrew that drives hardware engines anyway.

---

## Why

[SysDVR](https://github.com/exelix11/SysDVR) streams the Switch screen to a PC,
capped at **720p30, game layer only**. That cap is not SysDVR's: it reads
`grc:d`, the game-recording encoder, whose configuration is fixed in firmware.

The obvious question is whether a sysmodule can do better by taking the frame
*before* the encoder — reading the game's own swapchain and running it through
the Tegra X1's own fixed-function blocks. This repository is the answer, worked
all the way down.

## Where the difficulty is

**A sysmodule can process frames at full speed. Getting hold of one is the
problem.**

Three independent routes to another process's pixels have each been taken to
the point of a definite verdict:

| route | verdict |
|---|---|
| Import the game's swapchain `nvmap` handle (`FROM_ID` + `MAP_CMD_BUFFER`) | Pins to `phys=0`, silently. Survives `is_compr`, `MAP_CMD_BUFFER_EX`, relocs, the full `0xFFFFFFFF` `nvdrv:t` permission mask, and the game's exact aruid adopted before any `Open`. **Structural:** at `Initialize` nvservices is handed `CUR_PROCESS_HANDLE` and maps client memory through *that*. The game's pages live in the game's process. |
| `vi` indirect layers (`GetIndirectLayerImageMap`) | `0x60A PreconditionViolation`. The whole object graph builds — `CreateIndirectLayer` → producer endpoint → consumer endpoint — and the layer is simply **empty**. Wiring an application's layer to an indirect layer is **AM's** job, and a sysmodule cannot drive AM. |
| Read back the display controller | **No such ioctl exists.** `nvdisp-disp0` is `FLIP` / `SET_MODE` / `GET_WINDOW`; `nvdcutil` is DSI/EDID test plumbing. Grepping all of nvdrv for `READBACK\|CAPTURE\|GET_FRAME\|SCANOUT` returns nothing. |

`caps` `CaptureRawImage`, the other obvious candidate, is `[1.0.0]` — removed
long before 22.5.0.

**This is very likely why SysDVR is stuck at 720p30.** It is not that nobody
tried; the platform does not let a sysmodule reach another process's
framebuffer through the graphics stack.

The one route that is *not* closed goes around the graphics stack entirely —
the kernel's debug SVCs. See [Where it stands](#where-it-stands).

## What is here that you might want

Several pieces are finished, verified on hardware, and — as far as I can tell —
not published anywhere else. They are useful on their own, whether or not the
capture problem is solved, so take any of them.

### 1. Non-domain mitm sub-object forwarding for libstratosphere

[`tier4/applet-mitm/patch_libstrat.py`](tier4/applet-mitm/patch_libstrat.py) —
55 lines, idempotent.

Atmosphère's mitm framework can forward commands it does not implement, but
only for objects on a **domain** session. `vi:u` hands out
`IApplicationDisplayService` and `IHOSBinderDriver` as sub-objects on a
**non-domain** session, and upstream libstratosphere has nowhere to put the
forward service for those — so every undeclared command on a wrapped sub-object
fails instead of passing through. The patch adds that path.

**If you have ever tried to mitm `vi` and given up, this is the missing piece.**

### 2. A transparent `vi:u` mitm that sees every frame

Wraps `GetDisplayService` → `IApplicationDisplayService` → `GetRelayService` →
`IHOSBinderDriver`, and intercepts `TransactParcelAuto`. Measured **60.0 fps
sustained**, invisible to the game.

From the binder traffic it recovers the exact layout of every frame:
`setPreallocatedBuffer` (code 14) carries a flattened `NvGraphicBuffer`, and
`queueBuffer` (code 7) names the swapchain slot — the latter behind Android's
`writeInterfaceToken`, which is parsed rather than guessed. For Mario Kart 8:
one nvmap object, 3 × 1920×1080 A8B8G8R8, block-linear kind `0xFE`,
`block_height_log2 = 4`, pitch 7680, slots at `0` / `0x870000` / `0x10E0000`.

### 3. A complete VIC pipeline driven from a sysmodule, byte-exact

The Video Image Compositor is the Tegra block that converts block-linear to
linear, scales, and changes pixel format — no GPU involved.
[`applet_mitm_nv.cpp`](tier4/applet-mitm/source/applet_mitm_nv.cpp) +
[`vic40_config.hpp`](tier4/applet-mitm/source/vic40_config.hpp) drive it end to
end over raw `nvdrv` ioctls:

```
heap alloc -> svcSetMemoryAttribute(Uncached) -> nvmap CREATE/ALLOC
  -> MAP_CMD_BUFFER pin -> host1x cmdbuf (SETCL + methods + INCR_SYNCPT)
  -> CHANNEL_SUBMIT -> syncpoint wait -> cache invalidate -> read back
```

A fill and a real blit both reproduce their expected output **byte for byte**.
Output byte order is A,R,G,B (`AV_PIX_FMT_ARGB`). NVENC
(`/dev/nvhost-msenc`) opens too, so the rest of a GPU-free encode pipeline is
reachable.

Four things cost days each and are worth knowing before you start:

- **`SETCL` is mandatory.** `METHOD_OFFSET` (0x10) and `METHOD_DATA` (0x11) are
  registers *of the current host1x class*. libdrm never emits `SETCL` because
  the DRM kernel driver sets the class itself; nvservices' `CHANNEL_SUBMIT`
  does **not**. Without `SETCL(0, 0x5D, 0)` every method write lands on
  meaningless registers — while `INCR_SYNCPT` (register 0x00, present in every
  class) still fires, so the job looks like it completed. This cost six runs.
- **Relocs are inert on Horizon.** The command buffer is never patched. Pin
  with `MAP_CMD_BUFFER` and inline the returned address; submit with
  `num_relocs = 0`.
- **The syncpoint increment must be in the command stream.** `syncpt_incrs` in
  the submit only raises the syncpoint's *max*. Omit the
  `NONINCR(UCLASS_INCR_SYNCPT, 1)` and nvnflinger — which composites on the
  same VIC syncpoint — waits forever, and the console freezes.
- **A zero address does not fail politely.** The VIC hangs, and a hung VIC takes
  the compositor and the whole console with it. Froze this console twice.

### 4. Undocumented `vi` ABIs

`CreateIndirectLayer` (2050), `CreateIndirectProducerEndPoint` (2052),
`CreateIndirectConsumerEndPoint` (2054) — all `{u64, u64} -> u64`, guessed by
analogy with `viCreateManagedLayer` and confirmed on hardware. Also:
`GetDisplayService`'s **command id is the service type**, not 0 — `vi:u` = 0,
`vi:s` = 1, `vi:m` = 2.

### 5. A low-latency PC receiver

[`switch-stream/receiver/`](switch-stream/receiver) — FFmpeg + SDL2 over
USB or TCP, hardware decode, near-zero buffering. Written before settling on
this research direction; builds and runs, and is reusable as the client for
anything here.

## Where it stands

The kernel debug SVCs are the remaining avenue, and unlike the graphics stack
they are not obviously closed: Atmosphère's own cheat engine reads a running
game's memory at 60 Hz through them.

```
pm:dmnt GetApplicationProcessId -> svcDebugActiveProcess
  -> svcQueryDebugProcessMemory (find the framebuffer)
  -> svcReadDebugProcessMemory  (read it)
```

Reading mesosphere settles most of it in advance:

- **The framebuffer's attributes do not block the read.**
  `kern_k_page_table_base.cpp:2743` checks state and permission with an
  attribute mask of `None`, so `MemoryAttribute_DeviceShared` — which every
  nvmap-pinned page carries — does not disqualify the range. This is exactly
  what defeated the nvmap route, and it does not apply here.
- **The NPDM must declare a debug flag.** `kern_svc_debug.cpp:38` requires
  `target->IsPermittedDebug() || CanForceDebug() || CanForceDebugProd()`. This
  module now declares `"force_debug": true`, the same flag `creport` and
  `dmnt.gen2` use.
- **`svcMapProcessMemory` is not an alternative**, tempting as it looks.
  `kern_svc_process_memory.cpp:92` requires the source range to have *no*
  attributes set at all, which permanently excludes nvmap-pinned memory.
- **Staying attached is possible.** `ContinueDebugEvent(ExceptionHandled |
  ContinueAll)` resumes the target while the debug handle is held — that is how
  dmnt reads at 60 Hz, and it is what a streaming implementation would need.

**Run on hardware: it works, and it now streams.** The swapchain is located at
runtime by exact-size match, `ContinueDebugEvent` keeps the game running while
we stay attached, and a whole 8,847,360-byte slot reads in **5.6 ms** against a
16.67 ms frame budget.

The full pipeline runs end to end:

```
queueBuffer intercept -> svcReadDebugProcessMemory (5.6 ms)
  -> CPU point-sample downscale (4.3 ms) -> USB bulk IN -> SDL2 viewer
```

Two findings shaped it, both the hard way:

- **The VIC cannot be used per-frame.** A full-frame blit cost **119 ms**, and
  worse, nvnflinger composites on the same engine and syncpoint: a tight blit
  loop starved the compositor and the game fell to 3.8 fps. The VIC is fine for
  a one-shot blit. The stream uses a CPU point-sampler instead — at an exact 4x
  reduction every output pixel lands on a 16-byte group boundary, so it is one
  aligned read per pixel with no engine involved.
- **Every thread here is pinned to core 3**, including the mitm's own IPC
  thread. A stream loop at equal priority starves it and the game blocks on a
  binder call nobody answers. The worker runs below IPC priority and yields
  each iteration.

The full research log, in reverse chronological order with every dead end and
its evidence, is [`tier4/mitm/STATUS.md`](tier4/mitm/STATUS.md). The narrative
version is [`tier4/mitm/WRITEUP.md`](tier4/mitm/WRITEUP.md).

## Roadmap

| stage | state |
|---|---|
| `vi:u` mitm frame tap at 60 fps | **done, on hardware** |
| Swapchain geometry from binder traffic | **done, on hardware** |
| VIC: block-linear → linear, scale, format convert | **done, byte-exact on hardware** |
| **Get the game's pixels into our address space** | **done, on hardware** — three graphics routes closed; the kernel debug-SVC route works. 120 consecutive native 1080p frames, 0 missed, 59 fps, ~9 ms of a 16.67 ms budget |
| USB transport, device side | **done, on hardware** — enumerates as `1209:5f1e`, bulk IN, byte-exact |
| Live PC viewer | **done** — [`tools/raw-recv/raw-view.c`](tools/raw-recv/raw-view.c), libusb + SDL2 in one process |
| **End-to-end stream** | **done, on hardware** — 480x270 at **59.4 fps**; 640x360 at ~40 fps |
| Native resolution at 60 fps | **blocked on bandwidth.** USB 2.0 gives ~31 MB/s; 720p60 needs 83 MB/s in NV12, 221 in RGBA |
| NVENC H.264 encode | channel opens and takes a submit; the method table is unknown and undocumented |
| USB 3.0 SuperSpeed | descriptors accepted, link still negotiates High — cable or console, not the PC |
| Capture the home menu and system overlays | wanted, and not possible through any route found so far |

Capture is no longer the gate — **bandwidth is**. Everything upstream of the
cable is done and measured; the next move is compression, or proving
SuperSpeed.

A wrinkle worth knowing before starting the encoder: NVENC wants NV12 input and
the VIC is the natural way to produce it, but the VIC cannot be used per-frame
without starving the compositor. Producing NV12 cheaply is an unsolved
sub-problem of the encode path.

## Layout

| path | what | state |
|---|---|---|
| `tier4/applet-mitm/` | The mitm module. `vi:u` interposition, binder intercept, hand-rolled nvdrv, the VIC pipeline, and the debug-SVC probe. | mitm + VIC **verified on hardware**; debug probe untested |
| `tier4/recon/` | `tier4-recon` — a read-only, opt-in diagnostic sysmodule that mapped what a *plain* (non-mitm) sysmodule can reach. `nvdrv`, `apm` and `caps:sc` live capture are all off-limits from there. | done; its job is finished |
| `tier4/stream-oc/` | `stream-oc` — a charger-gated overclock companion (docked clocks while on the official 39 W adapter). Runs alongside stock SysDVR. | builds; untested |
| `switch-stream/` | A from-scratch low-latency USB/TCP streamer. The **PC receiver** is the reusable half. | receiver builds and runs; its sysmodule half is untested |
| `tier4/DESIGN.md`, `tier4/FINDINGS.md` | Working notes from the early research phase. | historical |
| `ref/` | Not committed. Third-party reference trees — see [`ref/README.md`](ref/README.md). | — |

## Building

Everything Switch-side builds in the `devkitpro/devkita64` Docker image; no
local toolchain needed.

```bash
# plain libnx sysmodules
cd tier4/recon && docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkita64:latest make
```

```bash
# applet-mitm: needs an Atmosphere checkout in ref/Atmosphere (see ref/README.md).
# build.sh rsyncs the module in, applies patch_libstrat.py, builds, copies the
# .nsp back. The first build recompiles libstratosphere - about 15 minutes.
git clone --recursive https://github.com/Atmosphere-NX/Atmosphere ref/Atmosphere
bash tier4/applet-mitm/build.sh
```

```bash
# PC receiver: FFmpeg + SDL2 + libusb
cd switch-stream/receiver && cmake -B build && cmake --build build -j
```

## Installing

```
sdmc:/atmosphere/contents/<TITLE_ID>/exefs.nsp         <- the built .nsp, renamed
sdmc:/atmosphere/contents/<TITLE_ID>/flags/boot2.flag  <- an EMPTY file
```

| module | title id |
|---|---|
| `tier4-recon` | `0100000000000C00` |
| `stream-oc` | `0100000000000C10` |
| `applet-mitm` | `0100000000000C20` |

`applet-mitm` is a **pure observer** unless `sdmc:/applet-mitm.armed` exists.
Keywords in that file opt in to each stage: `vic` runs the nvdrv/VIC probe,
`exec` runs the real blit rather than a no-op command buffer, `dbg` runs the
debug-SVC probe. Logs land at `sdmc:/applet-mitm.log`, with a last-step
breadcrumb in `sdmc:/applet-mitm.last` that survives a hard power-off.

**Read the SD card in a card reader, not over MTP** — MTP returns I/O errors on
a log whose tail was cut by a forced power-off.

## If something goes wrong

Delete `atmosphere/contents/<TITLE_ID>/` from the SD card on a PC, or boot
holding **Volume Up**, which makes Atmosphère skip `contents` sysmodules.
Nothing here touches NAND or the bootloader. Have a NAND backup anyway.

These modules interpose on the graphics stack and drive hardware engines
directly. Bugs in them freeze the console — that happened twice here, both
times from an address of zero reaching the VIC. Run this on a console you are
willing to have crash.

## Contact

Questions, corrections, or if you want to take a piece of this further — I'd
genuinely like to hear about it, especially if you can tell me I'm wrong about
why nvservices refuses foreign handles.

- **Email:** Some_Potato_1@protonmail.com
- **Reddit:** [u/Papux200](https://www.reddit.com/user/Papux200)
- **Issues:** [GitHub issues](https://github.com/TomasUribe/switch-frame-tap/issues)

Forks are welcome and no permission is needed. If you get further than this
repo does, please say so publicly — the whole point of writing the dead ends
down was to save somebody else the 47 test cycles.

## License

GPL-2.0. `tier4/applet-mitm` links libstratosphere and ships a patch against
it, so it is a derivative of Atmosphère and could not be anything else. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).
