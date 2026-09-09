# tier4-recon — Phase 0 diagnostics (v2)

A **read-only, opt-in** Atmosphère sysmodule that probes the paths Tier 4 needs
and writes findings to `sdmc:/tier4-recon.log`.

## v1 → v2

v1 fatalled `am` with **Kernel `LimitReached` (0x10801)** — it reserved a 9.3 MB
static buffer (whole 1080p frame) and ran every probe at boot. v2:

- one **256 KB** buffer; captures are requested tiny (we only want the return
  code + first bytes) — total process footprint ~0.8 MB.
- **armed / self-disarming**: does nothing unless `sdmc:/config/tier4-recon/RUN`
  exists, and **deletes that file before running** — a crash can never loop.
- **per-probe opt-in** via keywords in that file.
- 20 s settle so `am` / HOME are fully up first.
- DC-register mapping moved to a separate opt-in build (`tier4-recon-mmio.nsp`).

## Build

```bash
cd tier4/recon
docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkita64:latest make
# optional MMIO variant:
docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkita64:latest \
    make TARGET=tier4-recon-mmio CONFIG_JSON=recon-mmio.json
```

## Install

```
sdmc:/atmosphere/contents/0100000000000C00/exefs.nsp        <- tier4-recon.nsp (renamed)
sdmc:/atmosphere/contents/0100000000000C00/flags/boot2.flag <- empty file
```

## Run a batch

1. On the SD card create `config/tier4-recon/RUN` containing keywords, e.g.:
   ```
   sys psm apm nv caps_jpeg
   ```
2. Reboot. The module runs those probes once (~30 s after boot) and idles.
   The RUN file is **gone** afterwards.
3. Pull `sdmc:/tier4-recon.log`.
4. To run again (or in a different foreground state — game / HOME / Settings),
   recreate `RUN` and reboot.

### Keywords

| keyword | probe | risk |
|---|---|---|
| `sys` | firmware, model, ticks | safe |
| `psm` | charger type, battery | safe |
| `nv` | open `/dev/nvhost-*`, `/dev/nvmap` | safe-ish |
| `caps_jpeg` | `capsscCaptureJpegScreenShot` (Recording / Default / Screenshot stacks) | medium |
| `all_safe` | = `sys psm nv caps_jpeg` | medium |
| `apm` | performance mode — **fatalled `am` in v1**, kept only for a controlled retry | **risky** |
| `loop` | repeat the caps probes ~20x (15 s apart) — good for game vs HOME comparison | — |
| `nv_disp` | also try `/dev/nvdisp-*` | **risky** |
| `caps_raw` | hand-rolled `caps:sc` cmd 2 | **risky** |
| `caps_stream` | hand-rolled `caps:sc` cmd 1201/1203 | **risky** |
| `mmio_map` | `svcQueryMemoryMapping(DC)` — needs the `-mmio` build | **risky** |
| `mmio_read` | also dereference DC registers — needs the `-mmio` build | **risky** |

**Suggested first batch:** `all_safe`. If that logs cleanly and the console is
stable, try the risky ones one at a time, each its own reboot.

## If it crashes again

Boot holding **Volume Up** (skips `contents` sysmodules), or delete
`atmosphere/contents/0100000000000C00/`. The log's last line names the probe that
was running. Send it over.
