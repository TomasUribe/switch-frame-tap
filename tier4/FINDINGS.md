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
| `caps:sc` JPEG | ⏳ pending | next batch |
| `caps:sc` raw / stream | ⏳ pending | |
| DC MMIO map (`svcQueryMemoryMapping 0x54200000`) | ⏳ pending | needs `-mmio` build |

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
