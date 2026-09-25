# Run D of M78: install, run once on hardware, report back

Same job as `HANDOFF-M77-RUNC.md`, with a new build and one more probe.
Install the M78 build of `applet-mitm`, walk me through one run, collect the
results, decode them on the PC, and commit a verbatim report. I do the
physical steps on the console and you ask me when it's time for each one.

What this run tests:
- **`jpgdec`:** the NVJPG decode that worked in Run C, repeated as a control.
  This time its output file should verify on the PC.
- **`grcscan`:** a read-only observer. It attaches to grc (the system's game
  recorder), resumes it immediately, scans its memory for the NVENC encoder
  job grc uses, dumps what it finds to `sdmc:/grc-scan.bin`, and detaches.
  It opens no engine channel and submits nothing.

The full write-up is the M78 section of `tier4/mitm/STATUS.md`; you don't
need it for this.

## Rules

- **One run, with exactly the arm file below.** Don't add flags. In
  particular never add `grc`: the observer here uses a different mechanism,
  and `grc` arms an interceptor known to hang the console. Also never add
  `jpg`, `nvenc`, `exec`, `dbg`, `stream` or `usb`. Don't change any code. If
  anything is unclear or fails, stop and ask me.
- **Touch only these SD paths:** `atmosphere/contents/0100000000000C20/`,
  `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`,
  `nvjpg-dec.rgba`, `grc-scan.bin`. Leave everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:** attaching to grc could break video recording for
  the rest of that boot, or in the worst case end in a crash report naming
  grc. A console freeze needing a forced power-off (hold Power ~12 s) is
  unlikely but possible. Both are valid results. Record what happened and
  when.

## 1. Get the build (commit `feac866`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor feac866 HEAD && echo ok      # must print ok
git diff --stat feac866 HEAD -- tier4/applet-mitm/          # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M78: up"   # must match
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test   # OK
cc -O2 -o tools/nvsetup-dump tools/nvsetup-dump.c -I tier4/applet-mitm/source
```

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`66f7931c7832d75cf2eb851adc7c77f73d96bbf6e3db5819447d0bfa00fac339`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present: `$SD/applet-mitm.log`,
   `$SD/applet-mitm.last`, `$SD/nvjpg-dec.rgba`, `$SD/grc-scan.bin`.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic clk jpgdec grcscan wait=60\n' > "$SD/applet-mitm.armed"
   xxd "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/`, names only, so new entries can be spotted
   afterwards.
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part; walk me through it)

1. Boot normally into Atmosphère. Don't hold Volume Up.
2. Launch **Mario Kart 8 Deluxe** right away and get into a race. grc only
   has an encoder job while a game that supports video capture is running,
   and MK8D does.
3. Keep playing until **at least 3 minutes after power-on**. Both probes are
   done by ~70 s.
4. **Optional, and useful:** after about 2 minutes, hold the Capture button
   for about a second to save a video clip. Note whether the console says it
   saved. That shows whether grc is still healthy after being observed.
5. Note anything visible: stutter, a freeze and roughly when, error screens.
6. Power off. A normal shutdown or "Reboot to Hekate" are both fine; hold
   Power ~12 s only if frozen. Take the card out before it boots again.

## 4. Collect and decode

Copy into `logs/`:

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m78-runD-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m78-runD-applet-mitm.last` |
| `$SD/nvjpg-dec.rgba` | `logs/m78-runD-nvjpg-dec.rgba` |
| `$SD/grc-scan.bin` | `logs/m78-runD-grc-scan.bin` |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m78-runD-<original name>` |

Then run these and keep all their output:

```
python3 tools/nvjpg_dec_control.py check logs/m78-runD-nvjpg-dec.rgba
python3 tools/nvrec.py logs/m78-runD-grc-scan.bin --out logs/m78-runD-setups > logs/m78-runD-grc-scan.txt
for f in logs/m78-runD-setups/setup_*.bin; do tools/nvsetup-dump "$f" > "${f%.bin}.txt"; done
```

The `check` tool's first line prints `fnv1a32=...`. Compare it with the log
line `sd(sdmc:/nvjpg-dec.rgba): 16384 B written, read back identical,
fnv1a32=...`. Equal values mean the file is what the console wrote. Do the
same for `grc-scan.bin`: compute its FNV-1a with the snippet below and
compare it with its `sd(sdmc:/grc-scan.bin)` log line.

```
python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print('%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" logs/m78-runD-grc-scan.bin
```

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M78-RUND.md` with these copied verbatim from the
log, with timestamps:

- the boot banner (must read `applet-mitm M78: up`) and the `ARMED FLAGS:`
  line
- every `clk[...]`, `clk-watch` and `clk-ensure(...)` line
- the whole NVJPG decode section, from `---- NVJPG DECODE POSITIVE CONTROL`
  to its `jd:` result
- the whole grc observer section, from `---- GRC OBSERVER` to
  `grcscan:done`: the process id, attach and resume lines, scanned KB, every
  `hit` line, and the `dumped ...` line
- every line starting with `sd(`
- the `hb:` lines from 5 s before the decode section to 30 s after the
  observer section, plus the last `hb:` line in the file
- the contents of `applet-mitm.last`

From the PC:
- the `check` tool's full output, and both FNV-1a comparisons (equal or not)
- the first 150 lines of `logs/m78-runD-grc-scan.txt`, and its final
  `... bytes decoded, N setup blob(s)` line
- the first 60 lines of each `setup_N.txt` (all of them if there are three
  or fewer, otherwise the first three)
- the names of any new crash-report files

Then add what I saw on screen, whether the optional video capture saved,
whether a forced power-off was needed, and a one-line summary. Don't analyze
beyond that line.

Commit the report and everything from step 4 (the setups directory and the
`.txt` files included) on `claude/laughing-albattani-26hnt8`, not `master`,
as `M78 Run D: <one-line result>`, and push. Don't edit `STATUS.md` or any
code. Show me the report and the commit hash.
