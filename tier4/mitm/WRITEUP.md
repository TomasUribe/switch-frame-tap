# Tapping the Nintendo Switch frame pipeline with an Atmosphère mitm module

Target console: Mariko, firmware **22.5.0**, Atmosphère **1.11.2**.
All work is homebrew on the author's own console. Nothing here touches NAND or
the bootloader.

---

## 1. The goal, and why the obvious tool is capped

[SysDVR](https://github.com/exelix11/SysDVR) streams the Switch screen to a PC.
It reads `grc:d`, the game-recording service that feeds the 30-second capture
buffer. That encoder is hardwired to **1280×720, 30 fps, H.264, game layer
only** — the config lives in firmware, and no amount of overclocking changes it.

The question: can homebrew do native resolution (1080p when docked) and 60 fps?

## 2. A plain sysmodule tops out at SysDVR

`tier4/recon` is a read-only, opt-in diagnostic sysmodule. It established, on
this firmware, that from a normal background sysmodule context:

- `grc:d` works → SysDVR's 720p30.
- `vi:m` works → display resolution, a 60 Hz vsync event, but **not**
  composited-framebuffer readback.
- `psm` works → charger type (usable for gating an overclock).
- `nvdrv` and `apm` **fatal the process** when initialised.
- `caps:sc` is reachable but its live-capture paths are all dead ends:
  cmd 2 is stubbed since 5.0.0; the raw-stream path (1201/1203) is gated on a
  debug flag *and* only returns a staged screenshot buffer, not live frames;
  cmd 1204 is crash-report only.

The `nvdrv` fatal turned out to be misleading — see §6 — but at the time the
conclusion held: **you cannot beat SysDVR from a sysmodule.** Composited /
native-resolution capture needs a different execution context.

## 3. Mitm: `appletOE` is dead, `vi:u` is not

Atmosphère's mitm framework lets a module interpose on a system service. The
plan: interpose between a game and the graphics stack.

**`appletOE`** (the applet proxy service games open first) is **not
mitm-able.** switchbrew notes it allows only one session at a time; even a
fully transparent mitm — zero intercepted commands, everything forwarded —
breaks game launch. No public AMS mitm targets a session-limited service.
Confirmed empirically, then abandoned.

**`vi:u`** (the display root service) *is* mitm-able. A transparent mitm of it
was invisible to games. So the chain became:

```
mitm vi:u
  └─ wrap IApplicationRootService.GetDisplayService(0)
       └─ wrap IApplicationDisplayService
            ├─ .OpenDisplay(1010)      – liveness check
            └─ .GetRelayService(100)
                 └─ wrap IHOSBinderDriver
                      └─ .TransactParcelAuto(3)   – every IGraphicBufferProducer txn
```

## 4. The libstratosphere gap, and the patch

### The abort

Wrapping `GetDisplayService` and returning a wrapped `IApplicationDisplayService`
compiled and loaded fine — then the game fatalled with
`std::abort() (0xffe)` the moment it issued any command the wrapper didn't
explicitly declare.

The cause, in libstratosphere source:

```cpp
// sf_hipc_server_session_manager.cpp
Result ServerSession::ForwardRequest(...) const {
    AMS_ABORT_UNLESS(ServerManagerBase::CanAnyManageMitmServers());
    AMS_ABORT_UNLESS(this->IsMitmSession());   // <-- here
    ...
}
```

An undeclared command on a mitm object is meant to auto-forward via
`ForwardRequest`, which requires the *session* to be a mitm session (one that
carries a forward `::Service`). But:

```cpp
// sf_impl_command_serialization.hpp — SetOutObjectImpl, for a returned sub-object
R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));
```

`SetOutObjectImpl` registers sub-objects with plain `RegisterSession` — **no
forward service.** So the wrapper's session is not a mitm session, and its first
undeclared command aborts.

There *is* a code path that does this correctly — but only for **domain**
sessions (`sf_hipc_server_domain_session_manager.cpp` clones the parent's
forward service and calls `RegisterMitmSession`). No AMS mitm ever needed the
non-domain case: they either only see domain sessions (`fsp-srv`), or they
override commands that return values, not objects (`set:sys`). A game's `vi:u`
session is **non-domain**, and it returns sub-objects, so it hits the gap.

### The fix (`patch_libstrat.py`)

A thread-local side channel. A mitm command handler runs on the same thread, in
the same dispatch call stack, as `SetOutObjectImpl`. So:

```cpp
// added at namespace scope in sf_impl_command_serialization.hpp
#if AMS_SF_MITM_SUPPORTED
namespace ams::sf::impl {
    inline thread_local ::std::shared_ptr<::Service> g_tier4_pending_mitm_forward{};
}
#endif
```

```cpp
// SetOutObjectImpl, replacing the single RegisterSession call
#if AMS_SF_MITM_SUPPORTED
    if (auto _t4fwd = std::move(g_tier4_pending_mitm_forward); _t4fwd != nullptr) {
        R_ABORT_UNLESS(manager->RegisterMitmSession(server_handle, std::move(object), std::move(_t4fwd)));
    } else
#endif
    {
        R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));
    }
```

The handler forwards the parent command itself, wraps the result, sets the
thread-local to the forwarded `::Service`, then calls `out.SetValue(...)`.
`SetOutObjectImpl` consumes the thread-local and registers a proper mitm
session. Undeclared commands on the wrapper now forward correctly.

`build.sh` applies the patch idempotently before every build. Editing that
header forces a full libstratosphere rebuild (~15 min the first time).

**This is the piece worth reusing.** Anyone wanting to mitm a non-domain
service that hands out sub-objects (much of `vi`, `nvdrv`, older `am`
interfaces) hits the same wall.

## 5. The frame pipeline, end to end

With the wrapper working, the module sits on the game's `IHOSBinderDriver` and
sees every `IGraphicBufferProducer` transaction. On Mario Kart 8 Deluxe:

```
connect · query · setPreallocatedBuffer ×3 · dequeueBuffer · requestBuffer
then steady-state: dequeueBuffer / requestBuffer / queueBuffer at 60.0 fps
```

(Measured precisely: 300 `queueBuffer` calls in exactly 5.000 s.)

### The descriptor

Each of the three `setPreallocatedBuffer` (code 14, a Nintendo extension)
input parcels carries a flattened `NvGraphicBuffer` — the full description of
one swapchain slot's memory. Parsed:

```
nvmap_id  = 1268                    (one nvmap object for the whole swapchain)
3 buffers at offsets 0x000000, 0x870000, 0x10E0000   (0, 1×, 2× the slot size)
each slot: 1920 × 1080, A8B8G8R8 (0x0100532120), RGBA_8888
layout    = BlockLinear
kind      = 0xFE (Generic_16BX2)
block_height_log2 = 4              (block height 16 GOBs)
pitch     = 7680 bytes             (= 1920 × 4)
size      = 8,847,360 bytes each   (1920 × 1152 × 4; height 1080 aligned to 1152)
```

**The game renders at native 1920×1080.**

### The memory

`nvmap` ids are cross-process — nvnflinger runs in a separate process and must
map this same buffer to composite it. So `NVMAP_IOC_FROM_ID(1268)` on our own
`nvdrv` session reaches the identical memory. Verified: `PARAM(Size)` returns
**26,542,080** = exactly 3 × 8,847,360, the whole triple-buffered swapchain.

### The engines

Surveying `/dev/nvhost-*` from the working `nvdrv` session:

| node | result | role |
|---|---|---|
| `/dev/nvhost-vic` | **open** | block-linear → linear + format convert, in HW |
| `/dev/nvhost-msenc` | **open** | NVENC — hardware H.264 |
| `/dev/nvhost-ctrl` | **open** | host1x syncpoints (fencing) |
| `/dev/nvhost-gpu`, `-as-gpu`, `-ctrl-gpu` | denied (0x30003) | not needed |
| `/dev/nvhost-nvdec`, `-nvjpg` | denied (0x1000) | not needed |

The GPU is denied, but the video path doesn't need it: VIC converts, NVENC
encodes, host1x fences.

### The destination

An *imported* nvmap handle has no CPU mapping (Switch nvmap has no MMAP ioctl).
But an object the module **creates** is backed by its own pages. Verified:
`CREATE` + `ALLOC(kind=Pitch, our cpu_addr)` + `GET_ID`, then a CPU
write/read-back. That is the linear buffer a VIC blit writes into, and which
the module can then read and hand to NVENC.

## 6. ABI traps (a practical reference)

Things that cost console cycles to find:

- **`AMS_SF_DEFINE_MITM_INTERFACE` must be at global scope.** It expands its
  own `namespace` block; nesting it produces a wall of template errors.
- **libstratosphere packs raw command args by alignment, ascending**
  (`RawDataOffsetCalculator`). Declaration order doesn't matter; a `char[0x40]`
  (align 1) lands before a `u64` regardless.
- **`sf::ClientProcessId` is `ArgumentType::InData`** — it consumes a `u64` of
  raw data (the SDK's pid placeholder). A command whose `send_pid` lives only
  in the HIPC header with *no* raw placeholder (like `vi OpenLayer`) therefore
  **cannot be expressed in libstratosphere.** Declaring it over-counts the raw
  size by 8; omitting it leaves the dispatch rejecting the message. `OpenLayer`
  was abandoned for this reason — and it was unnecessary anyway; the binder is
  the real frame path.
- A **wrong command signature causes a dispatch-level error return**, not a
  crash. An `AMS_ABORT` means something structural — read the breadcrumb.
- **libnx `nvInitialize()` fatals from an odd context** because
  `_nvInitialize()` calls `appletGetAppletType()` to choose a service. Override
  the weak globals: `__nx_nv_service_type = NvServiceType_System` (forces
  `nvdrv:s`, never touches applet) and `__nx_nv_transfermem_size` to something
  that fits your allocator (libnx's 3 MB default won't).
- **`NvColorFormat` is a 64-bit enum** (`A8B8G8R8 = 0x0100532120`). Declaring it
  `u32` in the `NvSurface` layout shifts every following field by 4 — and
  `width`/`height`/`size` still read correctly, so it looks half-right.
- **`TransactParcel` is cmd 0 (MapAlias) pre-3.0.0, cmd 3 (AutoSelect) on
  ≥ 3.0.0.**
- A probe that opens an `nvdrv` session or an imported handle **must release
  them** — holding them wedged homebrew apps.

Diagnosis discipline that made this tractable: a `LogMark()` breadcrumb written
to a separate one-line file (`applet-mitm.last`) before every risky step, so a
hard fatal names the exact call instead of leaving an ambiguous truncated log.

## 7. Where it stands

**Proven, on hardware, games and homebrew unaffected:** every access primitive
a native-resolution capture pipeline needs — frame visibility at 60 fps,
per-frame layout, the game's framebuffer memory, VIC, NVENC, host1x syncpoints,
and a CPU-readable destination buffer.

**Not built:** the VIC and NVENC channel programming (host1x command submission
— substantial, but Ryujinx and yuzu both implement the VIC config struct as a
reference, and T210 = Jetson TX1 so L4T is the NVENC reference), the per-frame
trigger off `queueBuffer`, and the transport (reuse SysDVR's, or the receiver
in `switch-stream/receiver/`).

See `STATUS.md` for the concrete next steps.
