# Handoff: M76 results, for continuation

From: Sonnet 5 (interactive session)
To: the Opus 5.5 session that authored M76 (PR #1, merged as c7d6945)
Repo state: `master` at `ee6ef8c`. Full detail in `tier4/mitm/STATUS.md`; this
is the condensed version for picking the next move.

## What I did with your PR

Reviewed it before merging rather than taking it on trust, given this
project's history of expensive wrong guesses. Independently verified against
primary sources I could reach locally:

- **NVJPG register table fix** — pulled the method offsets out of
  `ref/open-gpu-doc/classes/video/cle7d0.h` myself; they match your fix
  exactly (0x704=PICTURE_INDEX not TOTAL_CORE_NUM, 0x710=BITSTREAM not
  CORE_INDEX). Confirmed the prior code (mine, M73) wrote a literal `0` into
  the real bitstream-address register.
- **PCV/mm:u module IDs** — cross-checked against this machine's actual
  installed libnx headers (`pcv.h`, `mm.h`). Exact match, including the
  Nvenc=5/Nvdec=6/Nvjpg=7 ambiguity you flagged.
- **armfile tokenizer** — pulled it and its host test into a standalone build
  and ran it myself. 18/18 pass.
- **JPEG decode control data** — regenerated `nvjpg_dec_control.h` and the
  reference PNG from your committed script; byte-for-byte identical to what's
  committed. Confirmed the expected-means check is non-circular (computed by
  Pillow from the same JPEG bytes, independent of whatever the engine
  produces).
- Could **not** independently confirm the exact `mm:u`/`clkrst` raw IPC
  command numbers — traced them into libnx's compiled wrappers but the low
  bits turned out to be a shared HIPC header field, not the command id, and
  unwinding a firmware-version-gated dispatch further wasn't worth it. Low
  risk regardless: wrong IDs on a non-engine service return an error, not a
  hang, and the code already handles that.

Merged as PR #1. Then ran both steps of your test plan on real hardware.

## Run A (`clk wait=60`) — clean, and answers two things at once

No engine channel touched. 230+ s of gameplay, steady 60 fps throughout, no
crash, no freeze.

```
              VIC     NVENC    NVJPG    NVDEC   HOST1X
before      422.4    460.8      0.0      0.0     81.6  MHz
held        652.8    979.2    652.8    979.2     81.6  MHz
released    422.4    460.8      0.0      0.0     81.6  MHz
```

- **NVJPG: 0 before, 652.8 held.** Confirms your theory exactly — NVJPG was
  genuinely unclocked in every prior probe (M73/M74), and `mm:u` fixes it, at
  least transiently (see Run B below).
- **NVENC: 460.8 MHz BEFORE we ever call `mm:u`.** Non-zero baseline, present
  through the whole survey window before any request. Per your own decision
  table this means **the clock was never NVENC's problem** — something
  (almost certainly grc's background 30 s capture buffer, which MK8D
  supports) already keeps it running. So **M68-M71's NVENC "accepted, never
  executed" results are NOT explained by the clock fix at all.** NVENC needs
  its own investigation, separate from this one. Worth deciding whether that
  is next, or whether NVJPG (now that decode is closer) is the better use of
  a cycle.
- Side finding: requesting NVENC/NVDEC/NVJPG clocks also raised VIC's own
  reported clock (422.4 → 652.8), suggesting a shared DVFS voltage/frequency
  domain across the video complex, not independent per-engine clocks.

## Run B (`vic clk jpgdec wait=60`) — refused to submit, and the refusal is the finding

No crash, no freeze — the guard worked exactly as designed and declined an
uncertain submit rather than gambling. `nvjpg-dec.rgba` was never written; the
decode itself is **still untested**.

```
50.4   clk:1_survey
52.3   ClocksHoldForEngines("survey") - mm:u ids 5,6,7 all SetAndWait(max) rc=0x0
52.6   clk[held]      NVENC=979.2  NVJPG=652.8  NVDEC=979.2
54.6   clk[held+2s]   NVENC=979.2  NVJPG=652.8  NVDEC=979.2   <- stable for 2s
54.7   "holding for the engine probes armed in this run" - NEVER released
...
62.9   clk[jpgdec-pre] NVENC=979.2  NVJPG=0.0   NVDEC=979.2
63.0   clk[jpgdec]     NVENC=979.2  NVJPG=0.0   NVDEC=979.2
63.05  jpgdec: NOT submitting - hold=1 clkrst readable=1 NVJPG=0 Hz
```

**NVENC and NVDEC held their requested clock rock-steady for the whole ~8-10 s
gap with zero engine work on either. NVJPG decayed from 652.8 back to 0 in
that same window**, with zero engine work, despite the mm:u request never
being released (`FinalizeWithId` was never called — `g_engine_wedged` is
false, `keep_holding` is true the entire time). So NVJPG appears to have some
auto-idle/timeout behavior on this firmware that NVENC/NVDEC do not exhibit,
at least not within this window.

**There's also a real bug in the M76 code that compounded this.**
`ClocksHoldForEngines` is idempotent:

```cpp
bool ClocksHoldForEngines(const char *who) {
    std::scoped_lock lk(g_clk_lock);
    if (g_holding) { return true; }   // <-- short-circuits, no re-request
    ...
```

Once the initial survey's hold succeeds, `g_holding` is true, and every later
call — including jpgdec's own, ~10 s later — returns `true` immediately
without touching `mm:u` again. So `held=1` in the refusal log means "we
successfully requested a hold at some point in the past," not "the clock is
elevated right now," and for NVJPG on this firmware those are demonstrably
not the same thing.

## The one concrete next step

Fix `ClocksHoldForEngines` (or add a separate path) to actually re-issue
`SetAndWaitWithId` for NVJPG immediately before `jpgdec` submits — not rely on
a hold requested up to ten seconds earlier via the `wait-10` survey timing.
The simplest version: drop the idempotency guard, or add a
`ClockRateOf(NVJPG) == 0` check right before the submit that re-requests if
so, in a tight loop with a short sleep, until either it comes up or a small
retry budget is spent. Then rerun `vic clk jpgdec wait=60` — if NVJPG's clock
is confirmed non-zero in the same log line as the submit itself, the decode
either completes (and the config is proven right, meaning M68-M74's NVENC/
NVJPG failures were clock timing, full stop) or it stalls with a clock
confirmed present (meaning something else, in our submit path specifically,
is still wrong) — either result is a real answer this project hasn't had at
any point since M68.

Also worth deciding, independent of the above: whether NVENC's now-confirmed
"never was the clock" status makes it the better next target instead — it
already has NVENC's known-good config work half-done (`nvenc_drv_h264.h`, the
M71 ladder), and a similarly-well-verified positive control (the way `jpgdec`
was built for NVJPG) might be more valuable there.

## Everything else is unchanged

No new crash reports. SD card was cleaned after this session (module and all
its files removed, SysDVR and the user's overlays untouched) so the console
is safe to use in the meantime.
