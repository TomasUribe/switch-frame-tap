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
| SwitchU hard reclaim + 30 s hold | On device (menu) |
| Commit / PR onto `feat/sftap-switchu-headroom` | Open if not pushed yet |

## DonorObtain policy (100D up)

1. `HeadroomAskAndWait` rounds of 2.5 s, up to ~15 s total.
2. After each round: if `!AppletBusy` → brief re-assert → launch.
3. If still busy → `HeadroomClear`, log “waiting, not launching”, retry.
4. After 15 s still busy → return false (engines retry in 5 s). **Never** “launching donor anyway” beside SwitchU.
5. Without 100D: legacy single 2 s ask then launch.

Busy-wait (`vb:applet_busy`) still asks ~every 30 s and clears the bit if the
pool stays tight (icon-flash fix).

## Verify

```
donor: SwitchU still holds the Applet pool (... MB used ...) - waiting, not launching
donor: no Applet-pool headroom after 15000 ms beside SwitchU - deferring donor
```
Success: `donor: … MB from the Applet pool mapped` → `live[1]: the Album / hbmenu`.

## Package

`…/build/switch-frame-tap-0.7.19-plus-busyfix/` — update 0C20 only; keep 0C21 donor.
