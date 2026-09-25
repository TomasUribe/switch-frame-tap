# Run E of M79: install, run once on hardware, report back

Same job as `HANDOFF-M78-RUND.md`, with a new build and a simpler arm file.
Install the M79 build of `applet-mitm`, walk me through one run, collect and
decode the results, and commit a verbatim report. I do the physical steps on
the console and you ask me when it's time for each one.

What this run tests: **only the grc observer, version 2.** It attaches to
grc (the system's game recorder), resumes it immediately, and scans its
memory for NVENC encoder setups and for the command buffers that point the
engine at them. It then watches grc's setup ring for up to 3 seconds to catch
an intra frame, writes everything to `sdmc:/grc-scan.bin`, and detaches. No
engine is touched: no channel is opened for it, no job is submitted, no clock
is requested. Background is in the M79 section of `tier4/mitm/STATUS.md`.

## Rules

- **One run, with exactly the arm file below.** Don't add flags. In
  particular never add `grc`: it arms a different, known-dangerous
  interceptor. Also never add `jpg`, `jpgdec`, `nvenc`, `exec`, `dbg`,
  `stream` or `usb`. Don't change any code. If anything is unclear or fails,
  stop and ask me.
- **Touch only these SD paths:** `atmosphere/contents/0100000000000C20/`,
  `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`, `grc-scan.bin`,
  and `nvjpg-dec.rgba` (only to delete it if it is left over). Leave
  everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:** the observer stays attached to grc for up to
  ~3 s (Run D: under 1 s). Worst cases: video recording breaks for the rest of
  that boot, or grc gets a crash report. A console freeze is unlikely. All
  are valid results. Record what happened and when.

## 1. Get the build (commit `ff59939`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor ff59939 HEAD && echo ok      # must print ok
git diff --stat ff59939 HEAD -- tier4/applet-mitm/          # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M79: up"   # must match
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test   # OK
cc -O2 -o tools/nvsetup-dump tools/nvsetup-dump.c -I tier4/applet-mitm/source
```

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`d3b798a0d4f7d122675bafb024bb7574d369ab12ef86ea8898c07824817ee7a7`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present: `$SD/applet-mitm.log`,
   `$SD/applet-mitm.last`, `$SD/grc-scan.bin`, `$SD/nvjpg-dec.rgba`.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic grcscan wait=60\n' > "$SD/applet-mitm.armed"
   od -c "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/` (names only).
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part; walk me through it)

1. Boot normally into Atmosphère. Don't hold Volume Up.
2. Launch **Mario Kart 8 Deluxe** right away and **be racing by about 50 s
   after power-on**. grc only encodes while a capture-capable game is running,
   and the observer fires at ~60 s of uptime.
3. Keep racing until **at least 3 minutes after power-on**.
4. **Optional, and useful:** at about 2 minutes, hold the Capture button for
   about a second to save a video clip, and note whether it saved.
5. Note anything visible: stutter, a freeze and roughly when, error screens.
6. Power off normally, or with "Reboot to Hekate". Hold Power ~12 s only if
   the console is frozen. Take the card out before it boots again.

## 4. Collect and decode

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m79-runE-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m79-runE-applet-mitm.last` |
| `$SD/grc-scan.bin` | `logs/m79-runE-grc-scan.bin` |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m79-runE-<original name>` |

```
python3 tools/nvrec.py logs/m79-runE-grc-scan.bin --out logs/m79-runE-setups > logs/m79-runE-grc-scan.txt
for f in logs/m79-runE-setups/setup_*.bin; do tools/nvsetup-dump "$f" > "${f%.bin}.txt"; done
python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print('%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" logs/m79-runE-grc-scan.bin
```

Compare the printed FNV-1a with the log line
`sd(sdmc:/grc-scan.bin): ... fnv1a32=...`.

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M79-RUNE.md` with these copied verbatim from the
log, with timestamps:

- the boot banner (must read `applet-mitm M79: up`) and the `ARMED FLAGS:`
  line
- the whole grc observer section, from `---- GRC OBSERVER` to
  `grcscan:done`. That includes the `scanned ...`, `hits: ...`, every `hit`,
  `plausible setups ... watch: ...`, `dumped ...` and `sd(...)` lines.
- the `hb:` lines from 5 s before to 30 s after that section, plus the last
  `hb:` line in the file
- the contents of `applet-mitm.last`

From the PC:
- the FNV-1a comparison (equal or not)
- **all** of `logs/m79-runE-grc-scan.txt`. If it is over 600 lines, include
  every `NOTE` and `CMDBUF` block in full and just the header line of each
  `SETUP`.
- for every `setup_N.txt`: its `input_cfg` line, `sps.profile_idc`, and the
  whole `pic_control:` block. Then give the full file for the first setup
  whose `pc->pic_type` is 2 or 3 (an intra frame), if there is one.
- the names of any new crash-report files

Then add what I saw on screen, whether the optional video capture saved,
whether a forced power-off was needed, and a one-line summary. Don't analyze
beyond that line.

Commit the report and everything from step 4 (the setups directory and the
`.txt` files included) on `claude/laughing-albattani-26hnt8`, not `master`,
as `M79 Run E: <one-line result>`, and push. Don't edit `STATUS.md` or any
code. Show me the report and the commit hash.
