# stream-oc

Charger-gated overclock companion sysmodule. Runs **alongside stock SysDVR** —
no fork. While the console is on a strong charger (and, by default, a game is
running) it raises CPU/GPU/EMC to docked/boost clocks so the game doesn't drop
frames while the hardware encoder is also running. On unplug it restores
handheld defaults. A reboot resets everything regardless.

Track A of the Tier 4 plan — the pragmatic deliverable. Does **not** add menus or
native resolution (that's the am-mitm track).

## Why

Phase 0 confirmed `psmGetChargerType` works from a sysmodule and returns
`EnoughPower` only for the official 39 W adapter. `clkrst` (same API `sys-clk`
uses) also works. So we can safely gate a boost on "plugged into the real
charger".

## Build

```bash
cd tier4/stream-oc
docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkita64:latest make
```

## Install

```
sdmc:/atmosphere/contents/0100000000000C10/exefs.nsp        <- stream-oc.nsp (renamed)
sdmc:/atmosphere/contents/0100000000000C10/flags/boot2.flag <- empty file
sdmc:/config/stream-oc/config.ini                           <- copy config.ini.example, set enable=1
```

Reboot. **Nothing happens until `enable = 1`** in the config.

## Config (`config.ini.example`)

| key | default | meaning |
|---|---|---|
| `enable` | `0` | master switch |
| `require_charger` | `enough` | `enough` = official 39 W only; `any` = any charger |
| `require_game` | `1` | only boost while an application is running |
| `poll_ms` | `2000` | how often to check the charger |
| `cpu_hz` / `gpu_hz` / `emc_hz` | `1785 / 768 / 1600 MHz` | boost targets (snapped to allowed rates) |
| `nvenc_hz` | `0` | encoder clock; `0` = leave alone |
| `handheld_*_hz` | `1020 / 461 / 1331 MHz` | restored on unplug |

## Log

`sdmc:/stream-oc.log` — one line per state change, e.g.
`BOOST (charger=EnoughPower) rc=0 -> cpu=1785000000 gpu=768000000 emc=1600000000`.

## Notes / safety

- `clkrst` clock control is the same mechanism `sys-clk` uses; requested rates
  are snapped to the console's real DVFS table.
- If you also run `sys-clk`, don't have both fighting over the same clocks —
  pick one. `stream-oc` reasserts every ~30 s.
- Erista (original) units: keep `cpu_hz <= 1785000000`, `gpu_hz <= 921600000`.
  Mariko tolerates more but runs hotter.
- Undock/redock while boosted: the OS may reset clocks; `stream-oc` reasserts on
  its next poll.
