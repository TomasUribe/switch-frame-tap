# Run C of M77: install, run once on hardware, report back

You're helping me test `applet-mitm`, a homebrew sysmodule from this repo
(github.com/TomasUribe/switch-frame-tap), on my own Switch. You have the SD
card (through a card reader) and the repo. I do the physical steps on the
console: booting, launching the game, powering off. Ask me when it's time for
each one. The job is one hardware run, "Run C" of the M77 build: install it,
walk me through the run, collect the results, commit them, and write a report
for the remote session that wrote the build.

Background, briefly. M76 found that the Tegra JPEG engine (NVJPG) had never
been clocked. Its Run B refused to submit a test job because NVJPG's clock
had dropped back to 0 by the time of the submit. M77 re-establishes the clock
right before the submit and logs every clock change at 200 ms intervals. The
full write-up is the M77 section of `tier4/mitm/STATUS.md`; you don't need it
to do this.

## Rules

- **One run, with exactly the arm file below.** Don't add flags. In
  particular never add `jpg`, `nvenc`, `grc`, `grcscan`, `exec`, `dbg`,
  `stream` or `usb`. Don't change any code. If anything is unclear or fails,
  stop and ask me.
- **Touch only these SD paths:** `atmosphere/contents/0100000000000C20/`,
  `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`,
  `nvjpg-dec.rgba`. Leave everything else alone: other `atmosphere/contents/`
  entries, SysDVR, overlays, `Nintendo/`, and any config files.
- **Read the card in the card reader, never over MTP.** MTP returns I/O
  errors on a log cut short by a forced power-off.
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risk:** if the engine job stalls, the console can freeze
  and need a forced power-off (hold Power ~12 s). That is a valid result, not
  a failure of yours. Record roughly when it happened.

## 1. Get the build (commit `36ee103`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git merge-base --is-ancestor 36ee103 HEAD && echo ok      # must print ok
git diff --stat 36ee103 HEAD -- tier4/applet-mitm/          # must print nothing: no code changed since M77
```

Build it. This needs Docker and an Atmosphère 1.11.2 checkout in
`ref/Atmosphere`; see `ref/README.md`.

```
# only if ref/Atmosphere is missing:
git clone --recursive --depth 1 --branch 1.11.2 https://github.com/Atmosphere-NX/Atmosphere ref/Atmosphere
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh     # ends with "OK"; a first build takes ~15 min
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M77: up"   # must match
```

The build script needs `rsync`. If I hand you the prebuilt
`applet-mitm.nsp` from the remote session instead, use it only if its SHA-256
is `1dbb016c31acb3c2554460fa28d8522387370415672e0b5424f5e3703313e1a6`. A local
build won't match that hash (different toolchain image and paths), and that is
fine.

Also run the arm-file parser's host test. It must print `OK`:

```
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test
```

## 2. Install (card in the reader)

Ask me for the mount point if you don't know it. Below it is `$SD`.

1. Record a listing of `$SD/atmosphere/contents/` for the report.
2. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
3. Delete old results if present: `$SD/applet-mitm.log`,
   `$SD/applet-mitm.last`, `$SD/nvjpg-dec.rgba`. The old runs' logs are
   already in the repo.
4. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"      # empty file
   printf 'vic clk jpgdec wait=60\n' > "$SD/applet-mitm.armed"
   xxd "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount the card safely.

## 3. The run (my part; walk me through it)

1. Put the card in the console and boot normally into Atmosphère. Don't
   hold Volume Up: that skips the module.
2. As soon as the home menu appears, launch **Mario Kart 8 Deluxe** and get
   into gameplay. A race is best; the title screen also works.
3. Keep the game running until **at least 3 minutes after power-on**. The
   probes fire at about 50 s and 60 s of uptime and the key part is over by
   ~75 s, but the clock watch runs longer.
4. Note anything visible: stutter, a freeze (roughly when), whether the game
   kept running.
5. Power off with Power → Power Options → Turn Off. If the console is frozen,
   hold Power ~12 s.
6. Take the card out before the console boots again.

## 4. Collect

Copy into `logs/` in the repo:

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m77-runC-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m77-runC-applet-mitm.last` |
| `$SD/nvjpg-dec.rgba` (only if it exists) | `logs/m77-runC-nvjpg-dec.rgba` |
| any file in `$SD/atmosphere/crash_reports/` or `$SD/atmosphere/fatal_errors/` newer than the install | `logs/m77-runC-<original name>` |

If `nvjpg-dec.rgba` exists, check it (needs Pillow). This writes a `.png`
next to it; keep that file too:

```
python3 tools/nvjpg_dec_control.py check logs/m77-runC-nvjpg-dec.rgba
```

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed` so the console is back to normal. Ask me before
doing it, in case I want to keep the module installed.

## 6. Report, commit, push

Write the report to `tier4/mitm/RESULT-M77-RUNC.md`. Copy these verbatim
from the log, with their timestamps:

- the boot banner (must read `applet-mitm M77: up`) and the `ARMED FLAGS:`
  line
- every `clk[...]` survey line
- every `clk-watch` line, including `clk-watch: stopped (...)`
- the `node /dev/...` survey lines and the `/dev/nvhost-vic` open line
- every `clk-ensure(...)` line
- everything from `---- NVJPG DECODE POSITIVE CONTROL` to the end of that
  section, including every `jpgdec:` and `MAP_CMD_BUFFER` line
- the `hb:` heartbeat lines from 5 s before to 30 s after the jpgdec section,
  plus the last `hb:` line in the file. The `txn` counter shows whether the
  game kept presenting.
- the contents of `applet-mitm.last`
- the `check` tool's output, if it ran
- the names of any crash report files

Then add what I saw on screen, whether a forced power-off was needed, the
`atmosphere/contents` listing from step 2, and a one-line summary. **Don't
analyze beyond that line**; the remote session will do the analysis.

Commit the report with the files from step 4 on
`claude/laughing-albattani-26hnt8` (not `master`), as
`M77 Run C: <one-line result>`, and push. Don't edit `STATUS.md` or any code.
Finally, show me the report and the commit hash so I can pass them on.
