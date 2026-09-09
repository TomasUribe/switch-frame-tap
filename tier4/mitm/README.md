# am-mitm indirect-layer tap — Track B (research)

Goal: composited frames (game **and** system UI), up to 60 fps, native docked
resolution, via the **documented** `vi` `GetIndirectLayerImageMap` — which Phase 0
proved is reachable from an in-process `vi:m` session. The only missing pieces
are a valid **AppletResourceUserId** and an **indirect-layer consumer handle**,
both of which live in `am`/applet context.

This is a multi-session effort. Milestones below.

## Why a mitm module, not a sysmodule

`GetIndirectLayerImageMap` (switchbrew) wants: PID descriptor, out buffer, w/h,
an `am GetIndirectLayerConsumerHandle` value, and an ARUID. A background
sysmodule has no ARUID and can't get a consumer handle. An Atmosphère **mitm**
on the `am` service interfaces sees every applet's `Open*Proxy` /
`GetIndirectLayerConsumerHandle` traffic and can borrow/clone what it needs while
the real applet keeps working.

## Build environment (different from the sysmodules here)

Needs the Atmosphère source tree + **libstratosphere**, not plain libnx:

```
git clone --recursive https://github.com/Atmosphere-NX/Atmosphere
# module goes in a libstratosphere "mitm" template; build with its Makefiles
```

The `devkitpro/devkita64` image has the toolchain; libstratosphere is vendored in
the Atmosphère checkout.

## Milestones

### M1 — observe ARUIDs
Minimal mitm on `appletOE` / `appletAE`. Log every `OpenApplicationProxy` /
`OpenSystemAppletProxy` call and the ARUID it produces. Confirms we can see a
live application's ARUID. (No behaviour change — pass everything through.)

### M2 — obtain a consumer handle
From the mitm, when the foreground app is up, call
`am GetIndirectLayerConsumerHandle` (or intercept one) for the `Default`
display's composited output. Open our own `vi:m` session (Phase 0: works) and
call `GetIndirectLayerImageRequiredMemoryInfo` — already returns rc=0.

### M3 — pull one frame
`GetIndirectLayerImageMap(buf, w, h, consumerHandle, aruid)` → one composited
RGBA frame to SD. Verify it contains the HOME menu when that's foreground.
This is the make-or-break test.

### M4 — sustained capture
Loop on the `vi` vsync event (Phase 0: `viGetDisplayVsyncEvent` works, ~16.6 ms).
Double-buffer. Measure real fps at 720p and (docked / forced Console mode) 1080p.

### M5 — encode + transport
Frames are linear RGBA (no block-linear detile needed — that's the win over the
binder route). Encode:
- bring-up: software x264 `ultrafast`, or MJPEG
- target: Tegra NVENC. `PcvModuleId_NVENC` clock is controllable (Phase 0), a
  good sign the engine is reachable; port submission from L4T / TX1 multimedia.
Transport: reuse SysDVR's USB/TCP, or `switch-stream/receiver` from this repo.

### M6 — operation-mode forcing
Force `OperationMode = Console` (charger-gated, `stream-oc` already proves
`psm` gating) so the composited surface is 1080p with no dock attached.

## Open risks

- ARUID borrowing may not survive the real applet's lifecycle (handle refcounts).
- `GetIndirectLayerImageMap` may only expose *indirect* layers (applet-in-applet),
  not the root composited display output — M3 settles this.
- NVENC port is the long pole if software encode isn't fast enough.
- Forcing Console mode with no display attached: stability unknown (test isolated).
