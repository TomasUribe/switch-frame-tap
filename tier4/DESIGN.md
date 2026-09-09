> **HISTORICAL** — research-phase working notes. Superseded by [mitm/WRITEUP.md](mitm/WRITEUP.md) + [mitm/STATUS.md](mitm/STATUS.md). Kept for the trail.

# Tier 4 — native-resolution, all-layers, high-framerate Switch capture

Working doc. Fork base: **SysDVR** (cloned at `ref/SysDVR/`). Reference wikitext at
`ref/docs/*.wiki` (switchbrew), grepped 2026-09-08.

---

## 1. What the research changed

The picture shifted meaningfully from the Tier-1..4 sketch. Summary:

| Goal | Old assumption | After research |
|---|---|---|
| Stream home menu / system menus | "research project" | **Real API exists**: `caps:sc` capture with `ViLayerStack_Default`. Friction on modern firmware (see below), but not RE. |
| Docked resolution | patch GRC params | GRC stays 720p regardless. Real path = force **OperationMode = Console** + capture at 1920×1080 through a non-GRC path. |
| 60 fps native, pure software | "map the DC registers" | Userland MMIO mapping is **blocked on FW 12+** (`svcQueryIoMapping` gone; Nintendo modules moved to the `svcReadWriteRegister` whitelist, which does not cover the display controller). The viable path is an **Atmosphère MITM module on the nvnflinger buffer-queue IPC**, not register poking. |

### Key facts dug up

**Display / layer stacks** (`libnx/vi.h`):
```
ViLayerStack_Default             = 0   // ALL layers — game + home menu + system UI + overlays
ViLayerStack_Lcd                 = 1
ViLayerStack_Screenshot          = 2   // user screenshot stack (album button) — game only
ViLayerStack_Recording           = 3   // video recording stack — ~ what GRC/SysDVR sees
ViLayerStack_LastFrame           = 4
ViLayerStack_Arbitrary           = 5   // am-only normally
ViLayerStack_ApplicationForDebug = 6   // current application only (creport/debug)
ViLayerStack_Null                = 10
```
`Default` is the whole composited screen. This is the "menu capture" enabler.

**`caps:sc` (`nn::capsrv::sf::IScreenShotControlService`)** — switchbrew `Capture_services` / `Display_services` pages:
- cmd 2 `CaptureRawImageRgba32IntoArrayWithTimeout(outbuf, layer_stack, w, h, buffer_count, buffer_index, timeout)` — **STUBBED since [5.0.0], returns `0x7FECE`.** Dead on our firmware.
- cmd 3 `AttachSharedBufferToCaptureModule` + cmd 5 `CaptureRawImageToAttachedSharedBuffer` — **[5.0.0+] replacement path.** Attach a shared buffer once, capture into it repeatedly. Not wrapped by libnx — raw IPC. **Prime candidate for a sustained RGBA capture loop.**
- cmd 1201/1202/1203 `Open/Close/ReadRawScreenShotReadStream(layer_stack, timeout)` — streaming reader, **but only works when `set:sys GetDebugModeFlag` is set** (console debug mode).
- `capsscCaptureJpegScreenShot(layer_stack, timeout)` — JPEG path (album/share-applet uses it). Need to verify not stubbed on FW 12+. If alive → trivial **MJPEG of `ViLayerStack_Default`**.

**SVC / MMIO** (`SVC` page):
- `svcQueryIoMapping` (0x55): `[1.0.0-9.2.0]` — **removed**.
- `svcReadWriteRegister` (0x4E): whitelist only (PMC etc.), **no DC**.
- `CreateIoPool`/`CreateIoRegion`/`MapIoRegion` (0x39/0x3A/0x46/0x47): **[13.0.0+]** — needs a matching kernel capability; unverified whether a userland sysmodule can get a DC-covering IoRegion.
- NPDM `MemoryMap` capability descriptor still exists (map phys range as IO RO/RW). `0x54200000` (DC0) is below the `0x80000000` normal-map ban and not on the legacy IO blacklist — but `[5.0.0+]` "two range checks" were added for IO and their bounds are not documented. **Must test on-device.**
- DRAM (framebuffers) is at phys `0x80000000+` → **cannot** be static-mapped as Normal via NPDM caps. Framebuffer bytes must be reached as an **nvmap handle**, not a raw physical map.

**`/dev/nvdisp-disp0/1`** (`NV_services`): has `NVDISP_FLIP` (the present ioctl carrying buffer handles), `NVDISP_GET_CRC` (checksum only — no pixel readback), vblank events. These nodes are **privileged**; a normal sysmodule's `nvdrv` session almost certainly cannot open them. Confirm in Phase 0.

---

## 2. Revised target architecture (the 60 fps native tap)

```
        ┌─────────────────────────────────────── Switch (Atmosphère) ───────────────────────────────────────┐
        │                                                                                                   │
  game/menu layers ──▶ nvnflinger ──(IHOSBinderDriver: TransactParcel / queueBuffer)──▶ display controller  │
        │                                   │                                                               │
        │                          ┌────────┴─────────┐  MITM (ams)                                          │
        │                          │  binder-mitm     │  observe queueBuffer: {nvmap handle id, fence,       │
        │                          │  module          │   crop, transform, format, stride}                  │
        │                          └────────┬─────────┘                                                      │
        │                                   ▼                                                                │
        │                          import handle via /dev/nvmap (read-only)                                  │
        │                                   ▼                                                                │
        │                          wait fence ─▶ frame is finished in DRAM (block-linear)                    │
        │                                   ▼                                                                │
        │                          detile block-linear ─▶ linear NV12/RGBA  (GPU blit, or VIC, or CPU)       │
        │                                   ▼                                                                │
        │                          Tegra NVENC  (ported from L4T / TX1 multimedia)  ─▶ H.264 Annex-B         │
        │                                   ▼                                                                │
        │                          SysDVR transport (USB 3 in dock / TCP)                                    │
        └───────────────────────────────────────────────────────────────────────────────────────────────────┘
                                            ▼
                                     PC receiver (our earlier low-latency decoder, or SysDVR-Client)
```

Why MITM the binder instead of the DC registers:
- It is the **last software chokepoint** every on-screen pixel passes through — games, home menu, settings, error applets, boot-ish. Native res, native rate.
- No MMIO capability fight, no SMMU/IOVA translation, no kernel patch.
- Atmosphère's `mitm` framework is built for exactly this kind of service interposition.
- We get per-layer buffers + composition metadata, so we can either (a) grab the single already-composited surface when nvnflinger GPU-composites, or (b) composite the layers ourselves.

Hard parts, in order of risk:
1. **Tegra NVENC port.** Biggest. The TX1/Nano share the T210 encoder; L4T's multimedia API + `nvv4l2`/`nvmpi` sources document the hardware submission path. Bring-up fallback: software x264 (`ultrafast`) at reduced fps, or MJPEG.
2. **Block-linear detile** at native res every frame without blowing the frame budget. Options: GPU (our own NVN/deko3d context), the **VIC** (Video Image Compositor — purpose-built for exactly this, also in L4T), or CPU with the 16Bx2 sector swizzle (documented; expensive at 1080p).
3. **Fence sync** — reading before the GPU finishes the frame = tearing/garbage. Need to wait the sync-point fence that rides along in the parcel.
4. **nvnflinger version drift** — parcel layouts and binder code numbers shift across firmware. Pin to the user's version; keep an offsets table.
5. **Forcing Console operation mode** with no real display attached — may destabilise the compositor. Test in isolation.

---

## 3. Phased plan

### Phase 0 — recon (build first, run on hardware, together)
A throwaway diagnostic sysmodule + notes. Resolves every "must test on-device" above. Deliverable: a findings file for *your* console + firmware. Details in `recon/README.md`.

### Phase 1 — RGBA capture via `caps:sc` shared-buffer path
Implement cmd3+cmd5 (`AttachSharedBufferToCaptureModule` / `CaptureRawImageToAttachedSharedBuffer`) as a new SysDVR **capture backend**. `ViLayerStack_Default`. Push raw/LZ4 frames over USB; view on PC. Measure sustained fps at 720p and (Console-mode) 1080p. **This alone delivers goals #1 and #2** (menus + docked res), at whatever framerate caps:sc sustains (expect 15–40).

### Phase 2 — operation-mode forcing
Charger check via `psm` (`PsmChargerType_EnoughPower` only) → force `OperationMode = Console` via `omm`/`am` while the backend is active; restore on exit. Now Phase 1 captures at true 1080p docked res with no dock/TV attached.

### Phase 3 — the binder MITM
Atmosphère MITM module on `IHOSBinderDriver`. Log every `queueBuffer`: handle id, fence, crop, transform, format, stride, which layer/display. Import read-only via `/dev/nvmap`. Prove we can pull one finished native frame this way.

### Phase 4 — detile + encode
Block-linear → linear (VIC or GPU). Then Tegra NVENC (L4T port); MJPEG/x264 fallback for bring-up. Wire into the SysDVR transport as the high-rate backend; keep the caps:sc backend as the low-risk fallback.

### Phase 5 — receiver / latency
Mostly done — reuse the low-latency decoder from `switch-stream/receiver/` (or SysDVR-Client). Tune jitter buffer, add stats overlay, A/V sync against GRC audio.

---

## 4. Open questions Phase 0 answers

1. `caps:sc` cmd3+cmd5 — callable from a sysmodule with `caps:sc` service access? Sustained fps? `ViLayerStack_Default` really include the home menu? Max width/height accepted?
2. `capsscCaptureJpegScreenShot` on FW 12+ — alive or stubbed?
3. `OpenRawScreenShotReadStream` — is debug-mode flag flippable on this setup, and what breaks when it is?
4. NPDM `MemoryMap` for `0x54200000` as IO RO — does the kernel honour it on this firmware? If yes, DC register dump (active head, window bases, formats, strides).
5. Can our `nvdrv` session open `/dev/nvdisp-disp0` / `-ctrl`? `/dev/nvhost-vic`? `/dev/nvhost-msenc`?
6. Force `OperationMode = Console` with no display — stable? Does the composited surface become 1080p?
7. Firmware + Atmosphère versions; nvnflinger binder command numbers on this version.

---

## 5. What I need from you

- **Firmware version** and **Atmosphère version** (exact).
- Console model (Erista / Mariko / OLED) — affects Mariko-only quirks and OLED panel path.
- Confirm you have: official dock, official 39 W charger, USB-C↔USB-A/C cable to the PC.
- **emuMMC** in use? (Strongly recommended for this — we will crash it repeatedly.)
- Logging preference: sysmodule logs to `sdmc:/tier4.log`, or a TCP log sink on the PC (faster iteration). I'll build both; you pick the default.
- Risk tolerance: Phase 3+ can hard-crash the compositor. Fine to reboot a lot?

---

## 6. Recommendation on ordering

Do **Phase 0 → 1 → 2** before touching Phase 3. Reasons:
- Phases 1–2 deliver two of your three goals with real APIs and low crash risk.
- They produce the SysDVR-backend scaffolding, transport reuse, and PC-side plumbing that Phase 3/4 also need.
- Phase 0's DC-register dump (if the MemoryMap cap works) tells us the exact scanout format/stride the MITM path must handle — cheap intel for the hard phase.
