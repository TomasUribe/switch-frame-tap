# Run G of M81: install, run once on hardware, report back

This is the same job as `HANDOFF-M80-RUNF.md`, with a new build and one more
flag. Install the M81 build of `applet-mitm`, walk me through one run,
collect and check the results on the PC, and commit a verbatim report. I do
the physical steps on the console, and you ask me when it's time for each
one.

**What this run tests.** Run F's encode was correct, but the engine flagged it
with `error_status` 2, a value NVIDIA's header does not define. This run finds
out what that flag reacts to, in two ways:

- **`grcscan`** runs first. It reads grc's memory, touches no engine, and
  looks for grc's own encoder status blocks (does grc get the same flag?) and
  for any VIC jobs grc runs.
- **`nvgrc`** then submits Run F's job five times from our own channel. Each
  time one thing changes: the input picture, the rate-control mode, or one
  byte of the setup. Each variant saves its status, its H.264 output and its
  rate-control state to the SD card.

The background is in the M81 section of `tier4/mitm/STATUS.md`.

## Rules

- **Do exactly one run, with exactly the arm file below.**
  - Don't add flags. Never add `grc`, `jpg`, `jpgdec`, `nvenc`, `exec`,
    `dbg`, `stream` or `usb`.
  - Don't change any code.
  - If anything is unclear or fails, stop and ask me.
- **Touch only these SD paths:**
  - `atmosphere/contents/0100000000000C20/`
  - `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`
  - `grc-scan.bin`
  - files matching `nvenc-*.bin`
  - `nvjpg-dec.rgba`, only to delete it if it is left over

  Leave everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:**
  - The last two variants (d, e) each change one byte of grc's setup. That is
    the first time since M71 that a setup differs from grc's. If one stalls,
    the module stops engine work for the rest of that boot and says so in the
    log. That is a valid result.
  - grc's recording may break for that boot.
  - A console freeze needing a forced power-off (hold Power ~12 s) is
    possible, but has not happened with NVENC before.

  Record what happened and when.

## 1. Get the build (commit `548e80e`)

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor 548e80e HEAD && echo ok      # must print ok
git diff --stat 548e80e HEAD -- tier4/applet-mitm/ tools/   # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M81: up"   # must match
g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o /tmp/armfile_test && /tmp/armfile_test   # OK
pip install av numpy pillow          # for the PC check, if not already installed
python3 tools/nvenc_replay.py selftest                     # must end with "selftest OK"
```

If `selftest` fails, stop and show me. The PC check below depends on it.

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`e4d65a6b4d3b50768275b5777c1bee3249beb2d1bd1efdc81418662f210cb839`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present:
   - `$SD/applet-mitm.log`, `$SD/applet-mitm.last`
   - `$SD/grc-scan.bin`, `$SD/nvjpg-dec.rgba`
   - every `$SD/nvenc-*.bin` (Run F's `nvenc-status.bin`, `nvenc-bits.bin`
     and `nvenc-recon-y.bin` among them, if they are still there)

   List what you deleted.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic nvgrc grcscan wait=60\n' > "$SD/applet-mitm.armed"
   od -c "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/` (names only).
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part; walk me through it)

1. Boot normally into Atmosphère. Don't hold Volume Up.
2. Launch **Mario Kart 8 Deluxe** right away and **be racing by about 50 s
   after power-on**. The observer fires at ~60 s of uptime, and needs grc to
   be encoding by then.
3. Keep racing until **at least 3 minutes after power-on**.
4. **Optional, and useful:** at about 2 minutes, hold the Capture button for
   about a second to save a video clip, and note whether it saved.
5. Note anything visible: stutter, a freeze and roughly when, error screens.
6. Power off normally, or with "Reboot to Hekate". Hold Power ~12 s only if
   the console is frozen. Take the card out before it boots again.

## 4. Collect and check

| from the card | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m81-runG-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m81-runG-applet-mitm.last` |
| `$SD/grc-scan.bin` | `logs/m81-runG-grc-scan.bin` |
| every `$SD/nvenc-*.bin` | `logs/m81-runG/` (same names) |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m81-runG-<original name>` |

There should be up to 16 `nvenc-*.bin` files:

- `nvenc-X-status.bin`, `nvenc-X-bits.bin` and `nvenc-X-rc.bin` for X = a to e;
- `nvenc-a-recon-y.bin`.

Fewer is a result too. List which ones exist.

```
mkdir -p logs/m81-runG
python3 tools/nvenc_replay.py check logs/m81-runG > logs/m81-runG/check.txt 2>&1; cat logs/m81-runG/check.txt
python3 tools/nvrec.py logs/m81-runG-grc-scan.bin --out logs/m81-runG-setups > logs/m81-runG-grc-scan.txt
for f in logs/m81-runG/nvenc-*.bin logs/m81-runG-grc-scan.bin; do python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print(sys.argv[1], '%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" "$f"; done
```

Compare each FNV-1a with its `sd(sdmc:/...)` log line.

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M81-RUNG.md`. From the log, copy these verbatim, with
timestamps:

- The boot banner (must read `applet-mitm M81: up`) and the `ARMED FLAGS:`
  line.
- The whole grc observer section, from `---- GRC OBSERVER` to
  `grcscan:done`. That includes the `scanned ...` line, the `hits: ...` line,
  the `grc's own NVENC status blocks: ...` line, every `hit` line, and every
  `sd(...)` line.
- The whole NVENC section, from `---- NVENC: REPLAY OF GRC'S IDR JOB` to its
  last `ng:` line. That includes every `nvgrc`, `nvgrc[X]`, `clk`,
  `clk-ensure`, `nvmapOwn`, `MAP_CMD_BUFFER` and `sd(` line.
- The `hb:` lines from 5 s before the observer section to 30 s after the NVENC
  section, plus the last `hb:` line in the file.
- The contents of `applet-mitm.last`.

From the PC:

- All of `logs/m81-runG/check.txt`.
- The FNV-1a comparisons, equal or not, one per file.
- From `logs/m81-runG-grc-scan.txt`:
  - every `NOTE` line containing `grc status @`;
  - every `NOTE` line containing `VIC SETCL`, each with the whole `CMDBUF`
    block that follows it;
  - the first `NOTE ... NVENC SETCL` line, with its `CMDBUF` block.

  If the file is under 600 lines, include all of it instead.
- Whether `check` wrote any `nvenc-X-decoded.png`. They get committed with the
  rest.
- The names of any new crash-report files.

Then add:

- what I saw on screen;
- whether the optional video capture saved;
- whether a forced power-off was needed;
- a one-line summary.

Don't analyze beyond that line. In particular, don't interpret
`error_status` values; that analysis is mine.

Commit the report and everything from step 4 (the setups directory and the
`.txt` files included) on `claude/laughing-albattani-26hnt8`, not `master`,
as `M81 Run G: <one-line result>`, and push. Don't edit `STATUS.md` or any
code. Show me the report and the commit hash.
