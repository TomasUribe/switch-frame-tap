# ref/ — reference material, not part of this repository

Nothing in this directory is committed. It is where the build and the research
notes expect to find third-party sources, each of which has its own license and
its own upstream. Fetch what you need:

| path | what | upstream |
|---|---|---|
| `ref/Atmosphere/` | **Required to build `tier4/applet-mitm`.** `build.sh` rsyncs the module into `stratosphere/applet-mitm/`, applies `patch_libstrat.py`, and builds inside it. | <https://github.com/Atmosphere-NX/Atmosphere> (GPL-2.0) |
| `ref/SysDVR/` | The existing 720p30 streamer this project set out to beat. Read for its USB/TCP transport protocol. | <https://github.com/exelix11/SysDVR> (GPL-2.0) |
| `ref/libdrm-vic/` | `vic.h`, `vic40.h`, `vic40.c`, `vic-blit.c`, `host1x.h` from libdrm's Tegra backend — the reference for the VIC 4.0 config struct that `tier4/applet-mitm/source/vic40_config.hpp` was verified field-for-field against. | <https://gitlab.freedesktop.org/mesa/drm> → `tegra/` (MIT) |
| `ref/docs/` | Switchbrew wiki pages saved as wikitext: `NV_services`, `Display_services`, `Nvnflinger_services`, `Display_Controllers`, `Capture_services`, `SVC`, `NPDM`, `am`, `errcodes`. | <https://switchbrew.org/wiki/> |

## Getting the one you actually need

```bash
git clone --recursive https://github.com/Atmosphere-NX/Atmosphere ref/Atmosphere
```

That is the only one required to build. Check out the tag matching the
Atmosphère release you run on the console — this project was developed against
**1.11.2** on firmware **22.5.0**.

## open-gpu-doc (added M63)

```
git clone --depth 1 https://github.com/NVIDIA/open-gpu-doc.git ref/open-gpu-doc
```

NVIDIA's own published hardware interface documentation. `classes/video/`
carries the pieces this project had been missing:

| file | what it settles |
|---|---|
| `clceb6.h` | VIC methods. Its `SET_OUTPUT_SURFACE_LUMA_OFFSET` is **0x720**, byte-identical to the value we proved on hardware for the far older `NVB0B6` - which is what validates the two it adds, `SET_OUTPUT_SURFACE_CHROMA_U/V_OFFSET` at 0x724/0x728. Those unblock NV12 output, which M59 refused to guess at. |
| `clc5b7.h` | The complete NVENC method table: `SET_APPLICATION_ID` 0x200, `SET_CONTROL_PARAMS` 0x700, `SET_IN_DRV_PIC_SETUP` 0x710, `SET_IN_CUR_PIC` 0x734, `SET_OUT_BITSTREAM` 0x71C, `EXECUTE` 0x300, plus the error enum. |
| `nvenc_drv.h` | The driver structures, **version-gated back to `NV_NVENC_1_0`** and including `NV_NVENC_5_0` / `NV_NVENC_6_0` - the generation Tegra X1 (GM20B) belongs to. The magic encodes the class: 5.0 = `0xd0b70006`, 6.0 = `0xc1b70006`. |
| `nvjpg_drv.h` | NVJPG. Not useful here: switchbrew's `NV_services` lists `/dev/nvhost-nvjpg` on this firmware as **JPEG Decoder** only - hardware JPEG *encode* arrives on Xavier, not X1. |

Not redistributed; clone it yourself. Licensed by NVIDIA, see the repo.
