# Run I of M83: install, run once on hardware, report back

This is the same job as `HANDOFF-M82-RUNH.md`, with a new build, a new arm file
and one new thing on the PC: a live viewer. Install the M83 build of
`applet-mitm`, walk me through one run, collect and check the results on the
PC, and commit a verbatim report. I do the physical steps on the console, and
you ask me when it's time for each one.

**What this run tests.** Four things, in this order, in one boot:

1. **`csc`**: the VIC colour-matrix probes again, now with the production
   BT.709 matrix as a 12th probe.
2. **`nvframe`**: real game frames through the VIC and NVENC, as in Run H,
   but with the two fixes M83 made:
   - the block layout NVENC actually reads;
   - real BT.709 colour instead of pass-through.

   One frame is saved at three QPs, then 120 frames run back to back and the
   last is saved.
3. **`nvstream`**: **the first live stream.** Game frames go VIC -> NVENC
   H.264 -> USB -> the PC, where `raw-view` decodes and shows them, for about
   60 seconds.
4. **`nvp`**: one IDR frame then 29 P frames (the compressed kind), saved to
   the SD card. It runs last because it is the newest kind of engine job.

The background is the M83 section of `tier4/mitm/STATUS.md`. The overall
state is in `tier4/mitm/PROJECT-HANDOFF.md`.

## Rules

- **Do exactly one run, with exactly the arm file below.**
  - Don't add flags. Never add `grc`, `grcscan`, `nvgrc`, `jpg`, `jpgdec`,
    `nvenc`, `stream`, `dump`, `bench`, `sweep` or `mtx`.
  - `nvstream` is not `stream`: `stream` is the old raw stream.
  - Don't change any code.
  - If anything is unclear or fails, stop and ask me.
- **Handheld only, with the USB-C cable from the console to the PC** for the
  whole run. Never dock: the dock owns the USB port, and the stream goes over
  that cable.
- **Touch only these SD paths:**
  - `atmosphere/contents/0100000000000C20/`
  - `applet-mitm.armed`, `applet-mitm.log`, `applet-mitm.last`
  - `vic-csc.bin`, and files matching `nvframe-*.bin` and `nvp-*.bin`
  - `nvenc-*.bin`, `grc-scan.bin` and `nvjpg-dec.rgba`, only to delete them
    if they are left over

  Leave everything else alone.
- **Read the card in the card reader, never over MTP.**
- **The module truncates its log on every boot.** Copy the log off the card
  before the console boots again with the module installed.
- **Known, accepted risks:**
  - The VIC and NVENC jobs are Run H's, with a different block height and
    colour matrix. The stream reads frames from the game for about 60 s,
    which the M67 raw stream also did.
  - A short stutter now and then during the stream is possible.
  - `nvp` is the first non-IDR encoder job since M71. If it stalls, the
    module stops engine work for that boot and says so. Everything before it
    has already counted.
  - A console freeze needing a forced power-off (hold Power ~12 s) is
    possible but has not happened with these engines in Runs F-H.

  All of these are valid results. Record what happened and when.

## 1. Get the build (commit `01d3f81`) and the PC tools

```
git fetch origin
git checkout claude/laughing-albattani-26hnt8
git pull --ff-only
git merge-base --is-ancestor 01d3f81 HEAD && echo ok      # must print ok
git diff --stat 01d3f81 HEAD -- tier4/applet-mitm/          # must print nothing
MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh      # ends with "OK"
strings -n 6 tier4/applet-mitm/applet-mitm.nsp | grep "M83: up"   # must match
sudo apt install libusb-1.0-0-dev libsdl2-dev libavcodec-dev libavutil-dev   # if missing
pip install av numpy pillow                                # if missing
bash tools/run_pc_tests.sh                                 # must end "all PC-side checks passed"
make -C tools/raw-recv                                     # must say "raw-view built WITH H.264"
```

If `run_pc_tests.sh` reports any FAIL, stop and show me its output.

Install the udev rule once, if it is not installed yet, so `raw-view` can
open the device without sudo:

```
sudo cp tools/raw-recv/99-switch-frame-tap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

If I hand you the prebuilt `applet-mitm.nsp` from the remote session
instead, use it only if its SHA-256 is
`082303daf3df0d5cdefdb0dc5f0d0321d21680653938be83e8ec16128bbd983f`. A local
build won't match that hash, and that is fine.

## 2. Install (card in the reader; mount point `$SD`)

1. If `$SD/atmosphere/contents/0100000000000C20/mitm.lst` exists, delete it.
2. Delete old results if present:
   - `$SD/applet-mitm.log`, `$SD/applet-mitm.last`
   - `$SD/vic-csc.bin`, every `$SD/nvframe-*.bin`, every `$SD/nvp-*.bin`
   - every `$SD/nvenc-*.bin`, `$SD/grc-scan.bin`, `$SD/nvjpg-dec.rgba`

   List what you deleted.
3. Install:
   ```
   mkdir -p "$SD/atmosphere/contents/0100000000000C20/flags"
   cp tier4/applet-mitm/applet-mitm.nsp "$SD/atmosphere/contents/0100000000000C20/exefs.nsp"
   : > "$SD/atmosphere/contents/0100000000000C20/flags/boot2.flag"
   printf 'vic exec dbg usb csc nvframe nvstream nvp wait=60\n' > "$SD/applet-mitm.armed"
   od -c "$SD/applet-mitm.armed"     # exactly those ASCII bytes, no BOM
   ```
4. Record the listings of `$SD/atmosphere/crash_reports/` and
   `$SD/atmosphere/fatal_errors/` (names only).
5. Show me the `0100000000000C20` listing and the arm file, then sync and
   unmount safely.

## 3. The run (my part and yours; walk me through it)

1. **You:** have a terminal ready in the repo with:
   ```
   mkdir -p logs/m83-runI
   until lsusb -d 1209:5f1e >/dev/null 2>&1; do sleep 1; done
   tools/raw-recv/raw-view --record /tmp/m83-runI.sft > logs/m83-runI/raw-view.txt 2>&1
   ```
   Start it before I power on. The device appears a few seconds after boot,
   because `usb` is in the arm file. **Keep the recording in `/tmp`, not in
   the repo**: a minute of it is over a gigabyte. Everything the viewer
   prints goes into `raw-view.txt`; the live fps is in the window's title
   bar.
2. **Me:** the console is handheld, with the USB-C cable to the PC. Boot
   normally into Atmosphère; don't hold Volume Up.
3. **Me:** launch **Mario Kart 8 Deluxe** right away and **be racing by about
   50 s after power-on**. The probes fire at ~60 s.
4. **You:** at about 65-70 s a window should open on the PC showing the game.
   It runs for about a minute. Tell me when it opens, and keep an eye on it.
   Note:
   - whether the picture is right: the game, correct colours, not scrambled;
   - whether it is smooth;
   - the fps in the title bar.
5. **Me:** keep racing until **at least 3 minutes after power-on**, well after
   the window stops updating.
6. **You:** when the window has stopped updating for ~30 s, close it (Esc,
   or Ctrl-C in the terminal if no window ever opened). Either way the viewer
   writes its summary to `logs/m83-runI/raw-view.txt` as it exits.
7. **Me:** power off normally, or with "Reboot to Hekate". Hold Power ~12 s
   only if the console is frozen. Take the card out before it boots again.

If the window never opens, don't retry the run. Note it, finish the run
normally, and collect everything; the log will say why.

## 4. Collect and check

| from the card / PC | to |
|---|---|
| `$SD/applet-mitm.log` | `logs/m83-runI-applet-mitm.log` |
| `$SD/applet-mitm.last` | `logs/m83-runI-applet-mitm.last` |
| `$SD/vic-csc.bin` | `logs/m83-runI-vic-csc.bin` |
| every `$SD/nvframe-*.bin` and `$SD/nvp-*.bin` | `logs/m83-runI/` (same names) |
| any file in `crash_reports/` or `fatal_errors/` not in the step-2 listing | `logs/m83-runI-<original name>` |

The card should have:

- up to 13 `nvframe-*.bin`: `-0-y`, `-0-uv`, `-0-src`, `-q16/q20/q24-status/bits`
  and `-last-y/uv/status/bits`;
- up to 5 `nvp-*.bin`: `nvp-index`, `nvp-stream-0` (and `-1`, `-2`... if the
  GOP is big), `nvp-last-y`, `nvp-last-uv`.

List which exist. Fewer is a result too.

```
python3 tools/vic_csc.py logs/m83-runI-vic-csc.bin > logs/m83-runI-csc.txt 2>&1; tail -30 logs/m83-runI-csc.txt
python3 tools/nvframe_check.py logs/m83-runI > logs/m83-runI/check.txt 2>&1; cat logs/m83-runI/check.txt
python3 tools/nvp_check.py logs/m83-runI > logs/m83-runI/nvp.txt 2>&1; cat logs/m83-runI/nvp.txt
python3 tools/sft_tool.py stats /tmp/m83-runI.sft --decode 600 > logs/m83-runI/stream-stats.txt 2>&1; cat logs/m83-runI/stream-stats.txt
python3 tools/sft_tool.py head /tmp/m83-runI.sft logs/m83-runI/stream-first10.sft 10
ls -l /tmp/m83-runI.sft; sha256sum /tmp/m83-runI.sft
for f in logs/m83-runI-vic-csc.bin logs/m83-runI/nvframe-*.bin logs/m83-runI/nvp-*.bin; do python3 -c "import sys; sys.path.insert(0,'tools'); import nvjpg_dec_control as n; print(sys.argv[1], '%08x' % n.fnv1a32(open(sys.argv[1],'rb').read()))" "$f"; done
```

Compare each FNV-1a with its `sd(sdmc:/...)` log line. Keep
`/tmp/m83-runI.sft` somewhere safe outside the repo. Don't commit it.

## 5. Uninstall (ask me first)

By default, remove `$SD/atmosphere/contents/0100000000000C20/` and
`$SD/applet-mitm.armed`. Ask before doing it.

## 6. Report, commit, push

Write `tier4/mitm/RESULT-M83-RUNI.md`. From the log, copy these verbatim, with
timestamps:

- The boot banner (must read `applet-mitm M83: up`) and the `ARMED FLAGS:`
  line.
- From the colour-matrix section: the `---- VIC COLOUR-MATRIX PROBES` line,
  every `csc[...]` line, the `sd(sdmc:/vic-csc.bin)` line and the
  `csc: N of 12 probes completed` line. The `[csc:...]` job lines can be
  left out this time.
- The whole real-frame section, from `---- NVENC ON REAL GAME FRAMES` to its
  last `nf:` line.
- The whole stream section, from `---- H.264 STREAM OVER USB` to its last
  `ns:` line. That includes every `nvstream` line, the progress lines among
  them.
- The whole P-frame section, from `---- P FRAMES` to its last `np:` line.
- The debug-capture summary, every line from `DebugActiveProcess(pid=` to the
  `---- VIC on real game pixels ----` line.
- The `hb:` lines from 5 s before the colour-matrix section to 30 s after the
  P-frame section, plus the last `hb:` line in the file.
- The contents of `applet-mitm.last`.

From the PC:

- The last 30 lines of `logs/m83-runI-csc.txt`: they include the `bt709`
  probe's fit and its "against the float bt709 conversion" line.
- All of `logs/m83-runI/check.txt`, `logs/m83-runI/nvp.txt` and
  `logs/m83-runI/stream-stats.txt`.
- All of `logs/m83-runI/raw-view.txt`: the viewer's summary, including its
  last `packets=... frames=... undecoded=... lost=...` line.
- The size and SHA-256 of `/tmp/m83-runI.sft`.
- The FNV-1a comparisons, equal or not, one per file.
- Which PNGs the tools wrote (`nvframe-*-vic.png`,
  `nvframe-*-decoded.png`, `nvp-last-decoded.png`). They get committed with
  the rest.
- The names of any new crash-report files.

Then add:

- what the viewer window showed: picture, colours, smoothness, the fps in the
  title bar, and when it opened and stopped;
- what I saw on the console, with rough times;
- whether a forced power-off was needed;
- a one-line summary.

Don't analyze beyond that line. In particular, don't interpret the colour
fits, the PSNR numbers, the P-frame result or the stream timings; that
analysis is the next session's.

Commit the report and everything in step 4 on
`claude/laughing-albattani-26hnt8`, not `master`, as
`M83 Run I: <one-line result>`, and push. That includes the `.txt` files, the
PNGs and `stream-first10.sft`, but **not** `/tmp/m83-runI.sft`. Don't edit
`STATUS.md` or any code. Show me the report and the commit hash.
