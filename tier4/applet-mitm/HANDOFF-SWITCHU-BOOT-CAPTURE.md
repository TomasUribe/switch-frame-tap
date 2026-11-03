# Handoff: SwitchU coexist — boot / first remount

SwitchU lite path + NeedHeadroom ACK: see SwitchU `docs/FT_VIEWER_LIGHT_PATH.md`
(and `docs/MEMORY_OPTIMIZATION_PLAN.md`). Optional poster LOD later is SwitchU-only.

## Status

| Piece | State |
|-------|--------|
| `sftap` NeedHeadroom bit 6 + cmd 15 `AckHeadroom` | Shipped (`feat/sftap-switchu-headroom`) |
| HBL 100D = Album for Beside margins | In tree (donor.cpp) |
| Busy-wait ask + clear bit if pool tight (anti-thrash) | In tree (nv.cpp) |
| **No donor launch on timeout** while SwitchU holds pool | In tree (DonorObtain) |
| **Wait up to ~15 s** / re-ask until Applet-pool room | In tree (DonorObtain) |
| SwitchU hard reclaim + lean boot (~190 MB free ACK) | On device (menu) |
| **Boot 100D attach** when vi shared buffer missing | In tree (`SeedRunningAlbumSurface`) |
| **Boot seed tear fix** (no binder → content-sync slots) | In tree (`g_album_seed_no_binder` + `PickSeededPresent`) |
| Commit / PR onto `feat/sftap-switchu-headroom` | Soak / push |

## DonorObtain policy (100D up)

1. `HeadroomAskAndWait` rounds of 2.5 s, up to ~15 s total.
2. After each round: if `!AppletBusy` → brief re-assert → launch.
3. If still busy → `HeadroomClear`, log “waiting, not launching”, retry.
4. After 15 s still busy → return false (engines retry in 5 s). **Never** “launching donor anyway” beside SwitchU.
5. Without 100D: legacy single 2 s ask then launch.

Busy-wait (`vb:applet_busy`) still asks ~every 30 s and clears the bit if the
pool stays tight (icon-flash fix).

## Verify

Boot with SwitchU already HOME (viewer connected):
```
live-ui: seeded Album/SwitchU from already-running 100D …
live[1]: the Album / hbmenu is in front (pid …)
```
Not: `vi's shared buffer not found` → `live:no_game` while SwitchU is on screen.

Boot glitch (seed without binder history): log showed `game presented 0.0 fps`
and all frames on the “old latest-present rule” (always slot 0, no fences → tears).
Opening Sphaira then returning recreates layers through `vi:m`, so real
`queueBuffer` + fences land and the picture cleans up. After **seedfix**, expect:
```
… boot capture without binder history (content-sync slots)
nvstream: presents taken (M96): … N by seed content-sync
```
(not hundreds “by the old latest-present rule” with 0 fence waits).

Headroom (still relevant if lean boot fails):
```
donor: SwitchU still holds … - waiting, not launching
```

## Package

`…/build/switch-frame-tap-0.7.19-plus-seedfix3/` — quiet-window seed + never encode
torn frames (SeedTorn). Keep 0C21.

**SwitchU remount (2026-10-31):** on first FT viewer connect,
`GpuDevice::remountDisplayForCaptureMitm()` (`__nx_win_exit`/`init` + swapchain).
Built into `sd_out/switch/SwitchU/bin/menu/` — flash menu ExeFS when FTP up, reboot.
Expect FT log: `[ft-light] remounting display…` then `captured applet slot` + fence waits.
