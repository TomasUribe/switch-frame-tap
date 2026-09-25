# Run H of M82: install, run once on hardware, report back

This is the same job as `HANDOFF-M81-RUNG.md`, with a new build and a new
arm file. Install the M82 build of `applet-mitm`, walk me through one run,
collect and check the results on the PC, and commit a verbatim report. I do
the physical steps on the console, and you ask me when it's time for each
one.

**What this run tests.** Two new things, in one boot:

- **`csc`**: 11 short jobs on the VIC (the console's image converter). Each
  converts a small colour card we made ourselves, with one test colour
  matrix per job, and all the results are saved to one file. The file tells
  us how the VIC's colour-conversion numbers really work; three guesses at
  that have failed before.
- **`nvframe`**: the first time real game frames go into the hardware video
  encoder.
  - The module reads the frame Mario Kart just displayed and has the VIC
    convert it to the encoder's input format.
  - NVENC encodes it as H.264.
  - One frame is saved at three quality levels. Then 120 frames run back to
    back to measure the whole path against the 60 fps budget, and the last
    frame is saved too.

The background is in the M82 section of `tier4/mitm/STATUS.md`.

## Rules

- **Do exactly one run, with exactly the arm file below.**
  - Don't add flags. Never add `grc`, `grcscan`, `nvgrc`, `jpg`, `jpgdec`,
    `nvenc`, `stream`, `usb`, `dump`, `bench`, `sweep` or `mtx`.
  - `exec` and `dbg` are required this time. `exec` lets the VIC run jobs;
    `dbg` lets the module read the game's frames.
  - Don't change any code.
  - If anything is unclear or fails, stop and ask me.
- **Play in handheld mode** (not docked) for the whole run. Handheld, the game
  draws 1280x720, which is exactly the encoder's size, so the frame goes in
  at native resolution with no scaling. Docked also works, but scaled, so
  handheld is the better test.
- **Touch only these SD paths:**
  - `atmosphere/contents/0100000000000C20/`
  - `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`
  - `vic-csc.bin`, and files matching `nvframe-*.bin`
  - files matching `nvenc-*.bin`, plus `grc-scan.bin` and `nvjpg-dec.rgba`,
    only to delete them if they are left over

  Leave everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:**
  - No run since M76 has had the VIC run jobs, and this is the first where
    it writes the encoder's layout. If the VIC hangs, the screen can
    freeze, and a forced power-off (hold Power ~12 s) would be needed.
  - The module pauses the game very briefly (a few ms) while it finds the
    frame buffer.
  - It then reads frames from the game for about 2-3 s. That may cause a
    short stutter around 60-65 s.
  - If the encoder stalls, the module stops engine work for that boot and
    says so in the log.

  All of these are valid results. Record what happened and when.

## 1. Get the build (commit `3cf0283`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor 3cf0283 HEAD && echo ok      # must print ok
git diff --stat 3cf0283 HEAD -- tier4/applet-mitm/ tools/   # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M82: up"   # must match
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test   # OK
pip install av numpy pillow          # for the PC checks, if not already installed
python3 tools/nvenc_replay.py selftest                     # must end with "selftest OK"
python3 tools/vic_csc.py selftest | tail -1                # must print "selftest OK"
python3 tools/nvframe_check.py selftest | tail -1          # must print "selftest OK"
```

If any selftest fails, stop and show me. The PC checks below depend on them.

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`0dd6c09c89fed994918e5bc06f66635df899d8d392d806706f5558cf39f1a6b1`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present:
   - `$SD/applet-mitm.log`, `$SD/applet-mitm.last`
   - `$SD/vic-csc.bin`, every `$SD/nvframe-*.bin`
   - every `$SD/nvenc-*.bin` (Run G's), `$SD/grc-scan.bin`,
     `$SD/nvjpg-dec.rgba`

   List what you deleted.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic exec dbg csc nvframe wait=60\n' > "$SD/applet-mitm.armed"
   od -c "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/` (names only).
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part; walk me through it)

1. **Handheld mode.** Boot normally into Atmosphère. Don't hold Volume Up.
2. Launch **Mario Kart 8 Deluxe** right away and **be racing by about 50 s
   after power-on**. The probes fire at ~60 s of uptime and need the game
   running and drawing.
3. Keep racing until **at least 3 minutes after power-on**.
4. Note anything visible, with rough times: a stutter or freeze (especially
   around 60-65 s), error screens.
5. Power off normally, or with "Reboot to Hekate". Hold Power ~12 s only if
   the console is frozen. Take the card out before it boots again.

(No Capture-button step this time.)

## 4. Collect and check

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m82-runH-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m82-runH-applet-mitm.last` |
| `$SD/vic-csc.bin` | `logs/m82-runH-vic-csc.bin` |
| every `$SD/nvframe-*.bin` | `logs/m82-runH/` (same names) |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m82-runH-<original name>` |

There should be up to 16 `nvframe-*.bin` files:

- `nvframe-0-y.bin`, `nvframe-0-uv.bin`;
- `nvframe-q16-status.bin` and `nvframe-q16-bits.bin`, and the same for
  `q20` and `q24`;
- `nvframe-last-y.bin`, `nvframe-last-uv.bin`, `nvframe-last-status.bin`,
  `nvframe-last-bits.bin`.

Fewer is a result too. List which ones exist.

```
mkdir -p logs/m82-runH
python3 tools/vic_csc.py logs/m82-runH-vic-csc.bin > logs/m82-runH-csc.txt 2>&1; cat logs/m82-runH-csc.txt
python3 tools/nvframe_check.py logs/m82-runH > logs/m82-runH/check.txt 2>&1; cat logs/m82-runH/check.txt
for f in logs/m82-runH-vic-csc.bin logs/m82-runH/nvframe-*.bin; do python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print(sys.argv[1], '%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" "$f"; done
```

Compare each FNV-1a with its `sd(sdmc:/...)` log line.

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M82-RUNH.md`. From the log, copy these verbatim, with
timestamps:

- The boot banner (must read `applet-mitm M82: up`) and the `ARMED FLAGS:`
  line.
- The VIC setup and regression jobs: every line from `VIC heap` to
  `vb:ALL_JOBS_DONE`. That includes the `pinned:` line, and the `[vb:job_fill]`
  and `[vb:job_blit_self]` lines.
- The whole colour-matrix section, from `---- VIC COLOUR-MATRIX PROBES` to
  `csc: N of 11 probes completed`. That includes every `csc[...]` line, every
  `[csc:...]` line, and the `sd(sdmc:/vic-csc.bin)` line.
- The whole real-frame section, from `---- NVENC ON REAL GAME FRAMES` to its
  last `nf:` line. That includes every `nvframe`, `[nf:vic0]`, `clk`,
  `clk-ensure`, `nvmapOwn`, `MAP_CMD_BUFFER` and `sd(` line.
- The debug-capture summary that is logged after it: every line from
  `DebugActiveProcess(pid=` to the `---- VIC on real game pixels ----` line
  (the `SWAPCHAIN AT`, `GAME FROZEN FOR` and `drained` lines are in there).
- The `hb:` lines from 5 s before the colour-matrix section to 30 s after the
  real-frame section, plus the last `hb:` line in the file.
- The contents of `applet-mitm.last`.

From the PC:

- All of `logs/m82-runH-csc.txt`.
- All of `logs/m82-runH/check.txt`.
- The FNV-1a comparisons, equal or not, one per file.
- Which PNGs the tools wrote (`nvframe-*-vic.png`,
  `nvframe-*-decoded.png`). They get committed with the rest.
- The names of any new crash-report files.

Then add:

- whether I played handheld or docked;
- what I saw on screen, with rough times;
- whether a forced power-off was needed;
- a one-line summary.

Don't analyze beyond that line. In particular, don't interpret the colour
fits, the PSNR numbers or the timings; that analysis is mine.

Commit the report and everything from step 4 (the `.txt` files and PNGs
included) on `claude/laughing-albattani-26hnt8`, not `master`, as
`M82 Run H: <one-line result>`, and push. Don't edit `STATUS.md` or any
code. Show me the report and the commit hash.
