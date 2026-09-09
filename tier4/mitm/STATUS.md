# Track B status

## M1 — DONE ✅

`tier4/applet-mitm` (TID `0100000000000C20`). Standalone libstratosphere mitm on
`appletOE`. Declines every session (`ShouldMitm` → false) so games are untouched;
logs the program id of each connecting application.

Verified on 22.5.0: module loads, mitm registers, both test games observed
(`0100152000022000`, `01007ef00011e000`), games launch normally.

Foundations proven: Atmosphere/libstratosphere build in `devkitpro/devkita64`,
`build.sh` loop (rsync into AMS tree → docker build → copy nsp back), SD logging
from a libstratosphere module.

## M2 — the cost, honestly

Goal: capture a game's **ARUID** (`IWindowController::GetAppletResourceUserId`,
cmd 1) and its **separable recording LayerId**
(`ISelfController::CreateManagedDisplaySeparableLayer`, cmd 44).

The chain: `appletOE`.OpenApplicationProxy(0) → `IApplicationProxy` →
.GetSelfController(1) / .GetWindowController(2) → those interfaces.

**Blocker:** libstratosphere has **no partial-passthrough for returned
sub-objects**. The root mitm session auto-forwards undeclared commands; a
sub-interface you return from a command does not. So to wrap `IApplicationProxy`
we must hand-declare and manually forward **every** command:

| interface | ~cmds | we intercept | we hand-forward |
|---|---|---|---|
| IApplicationProxyService (appletOE) | 3 | OpenApplicationProxy | 2 |
| IApplicationProxy | ~9 | GetSelfController, GetWindowController | ~7 |
| IWindowController | ~8 | GetAppletResourceUserId | ~7 |
| ISelfController | ~45 | CreateManagedDisplaySeparableLayer | ~44 |

Each hand-forward must be signature-exact or that command breaks for every game
(hang / "software closed", like M1 v1's handle-consumption bug). Each fix cycle
is build → flash → test → (likely) recover on a daily-driver console.

### Scope reduction

Do **ARUID-only** M2: wrap `appletOE` + `IApplicationProxy` + `IWindowController`
(~17 hand-forwards, skip `ISelfController`'s 45). Then **M3** tries to create our
*own* separable/recording layer from a `vi:m` session using that ARUID
(`viCreateManagedLayer(display, flags, aruid, &layer_id)` — Phase 0 showed
`vi:m` works from our context). If M3 works we never wrap `ISelfController`.

## Decision needed

1. **Grind M2 (ARUID-only) now** — I write it carefully, you iterate/recover.
2. **Pause B1** at M1, revisit later.
3. **Pivot to B2** (binder mitm) — also gets menus, similar sub-object nesting
   pain via `vi`, plus detile + NVENC.
