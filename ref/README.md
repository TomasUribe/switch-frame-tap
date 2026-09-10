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
