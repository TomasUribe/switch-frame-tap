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

Testing next: does the stream open with debug mode on? reported resolution
(720p vs 1080p)? sustained fps of open/drain/close? pixel format (first bytes)?

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
