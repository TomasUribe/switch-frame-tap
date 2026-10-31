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

Headroom (still relevant if lean boot fails):
```
donor: SwitchU still holds … - waiting, not launching
```

## Package

`…/build/switch-frame-tap-0.7.19-plus-busyfix/` — update 0C20 only; keep 0C21 donor.
