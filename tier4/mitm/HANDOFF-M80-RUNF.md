# Run F of M80: install, run once on hardware, report back

Same job as `HANDOFF-M79-RUNE.md`, with a new build and a new probe. Install
the M80 build of `applet-mitm`, walk me through one run, collect and check the
results on the PC, and commit a verbatim report. I do the physical steps on
the console and you ask me when it's time for each one.

What this run tests: **`nvgrc`**. The module submits one H.264 IDR frame to
the hardware video encoder (NVENC) from its own channel. It uses exactly the
job grc (the system's game recorder) submits, as read out of grc in Run E,
with our own buffers and a striped test picture as input. It then saves the
encoder's status, the H.264 output and the reconstructed picture to the SD
card. This is the first time this project submits a job it expects NVENC to
complete. Background is in the M80 section of `tier4/mitm/STATUS.md`.

## Rules

- **One run, with exactly the arm file below.** Don't add flags. Never add
  `grc`, `grcscan`, `jpg`, `jpgdec`, `nvenc`, `exec`, `dbg`, `stream` or
  `usb`. Don't change any code. If anything is unclear or fails, stop and ask
  me.
- **Touch only these SD paths:** `atmosphere/contents/0100000000000C20/`,
  `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`,
  `nvenc-status.bin`, `nvenc-bits.bin`, `nvenc-recon-y.bin`, plus
  `grc-scan.bin` and `nvjpg-dec.rgba` (only to delete them if they are left
  over). Leave everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:** our encode shares the engine with grc's own
  recording. If our job stalls, grc's recording may break for the rest of
  that boot. A console freeze needing a forced power-off (hold Power ~12 s)
  is possible but has not happened with NVENC before. All are valid results.
  Record what happened and when.

## 1. Get the build (commit `799173e`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor 799173e HEAD && echo ok      # must print ok
git diff --stat 799173e HEAD -- tier4/applet-mitm/          # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M80: up"   # must match
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test   # OK
pip install av numpy pillow          # for the PC check, if not already installed
python3 tools/nvenc_replay.py selftest                     # must end with "selftest OK"
```

If `selftest` fails, stop and show me: the PC check below depends on it.

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`261ec875291a65840137844e2522450546513916263c418cb0ace520147dee67`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present: `$SD/applet-mitm.log`,
   `$SD/applet-mitm.last`, `$SD/nvenc-status.bin`, `$SD/nvenc-bits.bin`,
   `$SD/nvenc-recon-y.bin`, `$SD/grc-scan.bin`, `$SD/nvjpg-dec.rgba`.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic nvgrc wait=60\n' > "$SD/applet-mitm.armed"
   od -c "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/` (names only).
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part; walk me through it)

1. Boot normally into Atmosphère. Don't hold Volume Up.
2. Launch **Mario Kart 8 Deluxe** right away and **be racing by about 50 s
   after power-on**. The probe fires at ~60 s of uptime.
3. Keep racing until **at least 3 minutes after power-on**.
4. **Optional, and useful:** at about 2 minutes, hold the Capture button for
   about a second to save a video clip, and note whether it saved. That shows
   whether grc's own encoding survived ours.
5. Note anything visible: stutter, a freeze and roughly when, error screens.
6. Power off normally, or with "Reboot to Hekate". Hold Power ~12 s only if
   the console is frozen. Take the card out before it boots again.

## 4. Collect and check

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m80-runF-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m80-runF-applet-mitm.last` |
| `$SD/nvenc-status.bin`, `nvenc-bits.bin`, `nvenc-recon-y.bin` (whichever exist) | `logs/m80-runF/` (same names) |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m80-runF-<original name>` |

```
python3 tools/nvenc_replay.py check logs/m80-runF > logs/m80-runF/check.txt 2>&1; cat logs/m80-runF/check.txt
for f in logs/m80-runF/nvenc-*.bin; do python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print(sys.argv[1], '%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" "$f"; done
```

Compare each FNV-1a with its `sd(sdmc:/nvenc-...)` log line.

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M80-RUNF.md` with these copied verbatim from the
log, with timestamps:

- the boot banner (must read `applet-mitm M80: up`) and the `ARMED FLAGS:`
  line
- the whole NVENC section, from `---- NVENC: REPLAY OF GRC'S IDR JOB` to its
  `ng:` result. That includes every `nvmapOwn`, `MAP_CMD_BUFFER`,
  `clk-ensure`, `nvgrc:` and `sd(` line.
- the `hb:` lines from 5 s before to 30 s after that section, plus the last
  `hb:` line in the file
- the contents of `applet-mitm.last`

From the PC:
- all of `logs/m80-runF/check.txt`
- the FNV-1a comparisons (equal or not)
- if `check` wrote `logs/m80-runF/nvenc-decoded.png`, say so; it gets
  committed with the rest
- the names of any new crash-report files

Then add what I saw on screen, whether the optional video capture saved,
whether a forced power-off was needed, and a one-line summary. Don't analyze
beyond that line.

Commit the report and everything from step 4 on
`claude/laughing-albattani-26hnt8`, not `master`, as
`M80 Run F: <one-line result>`, and push. Don't edit `STATUS.md` or any code.
Show me the report and the commit hash.
