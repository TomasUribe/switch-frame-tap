# tier4-recon — Phase 0 diagnostics

A **read-only** Atmosphère sysmodule that probes the paths Tier 4 depends on and
writes findings to `sdmc:/tier4-recon.log`. It maps no writable MMIO and forces
no system state. Worst case is a reboot.

## What it checks

| Probe | Question it answers |
|---|---|
| `probe_sys` | exact firmware, console model, tick rate |
| `probe_psm` | `psmGetChargerType` — can we detect the official 39 W charger? |
| `probe_caps` | `caps:sc` cmd2 (expect stub `0x7FECE`), **JPEG capture** alive? fps? does `ViLayerStack_Default` include the HOME menu? raw-stream (cmds 1201/1203) usable? at 720p and 1080p |
| `probe_mmio` | does `svcQueryIoMapping(0x54200000)` work on this firmware? if so, dump of the live Display Controller window registers (base addr / stride / format) |
| `probe_nv` | which `/dev/nv*` nodes our `nvdrv` session can open — critically `/dev/nvdisp-disp0`, `/dev/nvhost-vic`, `/dev/nvhost-msenc` |
| `probe_apm` | current performance mode |

It runs the static probes once, then repeats the `caps:sc` probes every 15 s for
~7 minutes. **While it loops, move between a game, the HOME menu, and System
Settings** — the log is timestamped so we can see what each capture path returns
in each foreground state.

## Build

```bash
export DEVKITPRO=/opt/devkitpro          # wherever yours lives
sudo dkp-pacman -S switch-dev            # if not already installed
cd tier4/recon && make
```
Produces `tier4-recon.nsp`.

## Install

```
sdmc:/atmosphere/contents/0100000000000C00/exefs.nsp        <- tier4-recon.nsp
sdmc:/atmosphere/contents/0100000000000C00/flags/boot2.flag <- empty file
```
Reboot. Wait ~8 minutes (or play/navigate around for that long).

> If the console fails to boot: delete the `0100000000000C00` folder (hold Vol+
> for the Atmosphère no-sysmodule boot, or pull the SD). The most likely culprit
> is the `map` capability for `0x54200000` being rejected — in that case remove
> the two `"type": "map"` blocks from `recon.json`, rebuild, and we lose only the
> DC-register dump.

## Send back

`sdmc:/tier4-recon.log`. Paste it or attach it. That plus your firmware +
Atmosphère versions tells us which Phase 1 path is real.
