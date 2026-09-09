> **HISTORICAL** — Phase 0 recon findings (what a plain sysmodule can/cannot reach). Still accurate; the mitm work in [mitm/WRITEUP.md](mitm/WRITEUP.md) builds on it.

# Phase 0 findings — live

Console: **Mariko**, firmware **22.5.0**, Atmosphère **1.11.2-master-5388824be**.
Method: `tier4-recon` boot2 sysmodule (`0100000000000C00`), read-only probes,
armed per-batch via `sdmc:/config/tier4-recon/RUN`, breadcrumb in
`sdmc:/tier4-recon.last`.

## Results

| Probe | Result | Notes |
|---|---|---|
| `sys` (setsys) | ✅ works | firmware / product model / tick rate all readable |
| `psm` `psmGetChargerType` | ✅ works | returned `Unconnected` on battery (86%), `rc=0`. **Charger-gated overclock detection from a sysmodule is viable.** |
| `apm` `apmInitialize` / `apmGetPerformanceMode` | ❌ **fatals `am`** | `2001-0132 / 0x10801` = Kernel `LimitReached`, program `0100000000000023` = `am`. Deterministic, hit twice. |
| `nv` `nvInitialize` (or first `nvOpen`) | ❌ **fatals a system process** | breadcrumb stops at `nv`; log ends right after `-> nv`. |
| `caps:sc` reachable from sysmodule | ✅ **no fatal** | all cmds return structured `capsrv` (module 206) errors - we can iterate here safely |
| `caps:sc` cmd 1204 (libnx `capsscCaptureJpegScreenShot`) | ❌ `2206-0820` | this is `CaptureCrashScreenShot` - only valid mid-crash-report. Wrong entry point. |
| `caps:sc` cmd 2 (`capsscCaptureRawImageWithTimeout`) | ❌ **stubbed** | `0x7FECE` = `2206-1023`, all stacks. Dead on 22.5.0. |
| `caps:sc` cmd 1201/1203 raw stream | ⚠️ **debug-gated** | `0x668CE` = `2206-0820`, all stacks. Needs `settings_debug!is_debug_mode_enabled`. |
| `caps:sc` cmd 3/5 shared-buffer | ⏳ | not wrapped by libnx, undocumented ABI (no input handle documented) |
| DC MMIO map (`svcQueryMemoryMapping 0x54200000`) | ⏳ | needs `-mmio` build |

### The debug-mode lever

`set:sys GetDebugModeFlag` (switchbrew: cmd 62) just reads the system setting
`<"settings_debug", "is_debug_mode_enabled">` via the normal `ReadSetting` path,
which **Atmosphere's `system_settings.ini` can override**. So:

```ini
[settings_debug]
is_debug_mode_enabled = u8!0x1
```

in `sdmc:/atmosphere/config/system_settings.ini`, then reboot (capsrv loads the
flag once at boot), should turn `2206-0820` into a working raw stream = full
composited screen, every layer stack incl. HOME menu, from our sysmodule.
Reversible: delete the two lines, reboot.

Result: `is_debug_mode_enabled = 0x01` **did apply**, and `caps:sc` raw stream
**still** `2206-0820`. So that error is a *staged-buffer* gate, not a debug gate —
capsrv's stream reads a buffer the screenshot-button / album flow fills, not a
live framebuffer. `caps:sc` cmd 3/5 kill the IPC session (`0xf601`).
**caps:sc is a dead end for streaming.**

## `vi` — works from a sysmodule ✅

| call | result |
|---|---|
| `viInitialize(ViServiceType_Manager)` | **`rc=0`** — a sysmodule can hold a `vi:m` session (unlike nvdrv/apm) |
| `viOpenDefaultDisplay` | `rc=0` |
| `viGetDisplayResolution` | **1280x720** (handheld panel) |
| `viGetDisplayLogicalResolution` | **1920x1080** (compositor works in 1080p logical space) |
| `viGetDisplayVsyncEvent` + wait | `rc=0`, intervals **~16.6 ms = 60 Hz** — real frame-pacing clock from a sysmodule |
| `viGetIndirectLayerImageRequiredMemoryInfo(1280x720)` | `rc=0`, size 3801088, align 4096 |
| `viGetIndirectLayerImageMap(handle=0)` | `0xe72` (bad handle) — call is *reachable*, needs a real consumer handle |

**But** `GetIndirectLayerImageMap` (switchbrew) needs a PID descriptor, an
`am GetIndirectLayerConsumerHandle` value, **and an AppletResourceUserId** — both
of which come from `am` / an applet context. A background sysmodule has neither.
The `IManagerDisplayService` `CreateIndirectLayer` / `CreateIndirect*EndPoint`
(cmd 2050-2055) route is undocumented and may still need an ARUID.

## Phase 0 verdict

A background **sysmodule** on 22.5.0 can reach:
- `grc:d` → 720p30 H.264, **game layer only** (= SysDVR)
- `vi:m` → display metadata, **60 Hz vsync event**, resolution — but **not**
  composited-framebuffer readback (needs an `am` ARUID + consumer handle)
- `psm` → charger type (overclock gating) ✅
- everything else (`nvdrv`, `apm`, `caps:sc` live capture) → blocked or fatal

**The sysmodule ceiling is SysDVR + better pacing + charger-gated overclock.**
Composited frames (menus) and native docked res require a non-sysmodule context.

### Refined Tier 4 plan

An Atmosphere **`am`-mitm module**: when a game is foreground, obtain/borrow a
valid `AppletResourceUserId` + an indirect-layer **consumer handle** for the
`Default` display's composited output, then call `vi`
`GetIndirectLayerImageMap` every vsync → composited 1080p-logical frames
(incl. system UI) → encoder → SysDVR transport.

This uses a **documented** readback API (`GetIndirectLayerImageMap`); the hard
part shrinks to "get a valid ARUID + consumer handle inside an am-mitm context",
which is far smaller than reversing NVENC + the nvnflinger binder.

Parallel pragmatic track: fork SysDVR now, add charger-gated overclock (psm
works) + vsync-event pacing. Ships in days, stays 720p30 game-only.

Charger note: second run read `charger=2` (LowPower USB-PD), not `1`
(EnoughPower). Overclock gate needs the official 39 W adapter straight into the
console.

## Interpretation

A plain **boot2 background sysmodule cannot touch the display / applet-session
services** on 22.5.0 — `apm` and `nvdrv` both fatal a system process when
initialised from this context (no AppletResourceUserId, wrong process
relationship with `am`/`nvservices`).

- SysDVR only survives because `grc:d` is explicitly built for a background
  recorder to call.
- **This confirms the DESIGN.md Phase 3 direction:** the native-resolution tap
  cannot be a sysmodule poking `nvdrv`. It has to be an Atmosphère **mitm
  module** interposing on the nvnflinger buffer-queue IPC, which runs in a
  context that already holds those relationships.
- Still open and decisive for Tier 1/2 (menu + docked capture): whether
  `caps:sc` works. Its heavy lifting runs in `capsrv`, not our process — we only
  send a request and receive a buffer — so it may well work where `nv` did not.

## Next

1. `caps_jpeg` alone (`RUN` = `sys psm caps_jpeg`), in-game and on HOME menu.
2. If JPEG works: `caps_raw` / `caps_stream` (hand-rolled), then the `-mmio`
   build for the DC map check.
3. If `caps:sc` also fatals from a sysmodule → the capture side moves to a
   mitm/applet context too, and Phase 1 is rescoped around that.
