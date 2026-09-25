# M77 Run C: result

Build `36ee103`/`d975670` (no code changed between them - checked, empty diff),
installed and run once on hardware. Arm file: `vic clk jpgdec wait=60`.

## `atmosphere/contents` listing before install (step 2)

```
00FF000053434153
00FF0000636C6BFF
00FF0000A11CE0FF
00FF0000A53BB665
00FF0000B378D640
00FF007468656D65
0100000000000803
0100000000001000
0100000000001007
0100000000001013
010000000000bd00
0100277011F1A000
01003BC0000A0000
01006A800016E000
0100B00B51230000
0100F8F0000A2000
420000000000000B
420000000007E51A
420000000007E51B
```

No `mitm.lst` was present. No `applet-mitm.log`/`.last`/`nvjpg-dec.rgba` were
present before install.

## Boot banner and armed flags

```
[    10.400] applet-mitm M77: up (grc IPC interceptor off)
[    10.456] ARMED FLAGS: vic=1 exec=0 dbg=0 dump=0 usb=0 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=0 clk=1 jpgdec=1 wait=60
```

## Every `clk[...]` survey line

```
[    50.197]    clk[before      ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=422.4 MHz  NVDEC=460.8 MHz  HOST1X=81.6 MHz
[    50.733]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=422.4 MHz  NVDEC=460.8 MHz  HOST1X=81.6 MHz
[    51.264]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=422.4 MHz  NVDEC=460.8 MHz  HOST1X=81.6 MHz
[    51.775]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=422.4 MHz  NVDEC=460.8 MHz  HOST1X=81.6 MHz
[    51.964]    clk[held        ]  VIC=652.8 MHz  NVENC=979.2 MHz  NVJPG=652.8 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    53.976]    clk[held+2s     ]  VIC=652.8 MHz  NVENC=979.2 MHz  NVJPG=652.8 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    62.434]    clk[jpgdec-pre  ]  VIC=422.4 MHz  NVENC=979.2 MHz  NVJPG=422.4 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
```

## Every `clk-watch` line

```
[    54.094]    clk-watch: sampling NVJPG/VIC/NVDEC every 200 ms, logging changes only
[    54.126]    clk-watch  NVJPG=652.8  VIC=652.8  NVDEC=979.2 MHz
[    59.382]    clk-watch  NVJPG=652.8  VIC=652.8  NVDEC=0.0 MHz
[    60.807]    clk-watch  NVJPG=422.4  VIC=422.4  NVDEC=0.0 MHz
[    63.720]    clk-watch: stopped (engine probe finished)
```

## `node /dev/...` survey and `/dev/nvhost-vic` open

```
[    60.597] -> vb:4b_node_survey
[    60.607]    node /dev/nvhost-display    OPEN (rc=0x0 nverr=0)
[    60.616]    node /dev/nvdisp-ctrl       OPEN (rc=0x0 nverr=0)
[    60.625]    node /dev/nvdisp-disp0      OPEN (rc=0x0 nverr=0)
[    60.633]    node /dev/nvdisp-disp1      OPEN (rc=0x0 nverr=0)
[    60.643]    node /dev/nvdcutil-disp0    OPEN (rc=0x0 nverr=0)
[    60.659]    node /dev/nvcec-ctrl        OPEN (rc=0x0 nverr=0)
[    60.666]    node /dev/nvhost-as-gpu     OPEN (rc=0x0 nverr=0)
[    60.674]    node /dev/nvhost-ctrl-gpu   OPEN (rc=0x0 nverr=0)
[    60.682]    node /dev/nvhost-msenc      OPEN (rc=0x0 nverr=0)
[    60.693]    node /dev/nvhost-nvdec      OPEN (rc=0x0 nverr=0)
[    60.704]    node /dev/nvhost-tsec       denied (rc=0x0 nverr=196611)
[    60.718]    node /dev/nvhost-nvjpg      OPEN (rc=0x0 nverr=0)
...
[    61.486] -> vb:7_open_vic
[    61.535]    /dev/nvhost-vic rc=0x0 fd=23265281 nverr=0
[    61.551]    GET_SYNCPOINT rc=0x0 nverr=0 -> syncpt=12
```

## Every `clk-ensure(...)` line

```
[    62.986]    clk-ensure(jpgdec) #1 re-set max     : 1 request(s), SetAndWait rc=0x0 -> clkrst NVJPG=422400000 Hz
```

## `---- NVJPG DECODE POSITIVE CONTROL` to end of section

```
[    62.415] -> jd:1
[    62.426]    ---- NVJPG DECODE POSITIVE CONTROL (M77: clock ensured after open, read at submit) ----
[    62.434]    clk[jpgdec-pre  ]  VIC=422.4 MHz  NVENC=979.2 MHz  NVJPG=422.4 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
[    62.445]    MAP_CMD_BUFFER(jpgdec-buf handle=6996 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x1960000
[    62.476]    MAP_CMD_BUFFER(jpgdec-cmd handle=6644 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x1ca0000
[    62.507]    jpgdec: syncpt=17 pic=0x1960000 stat=0x1961000 scan=0x1962000 (294 B) out=0x1970000, 21 words
[    62.935] -> jd:clock
[    62.986]    clk-ensure(jpgdec) #1 re-set max     : 1 request(s), SetAndWait rc=0x0 -> clkrst NVJPG=422400000 Hz
[    63.144] -> jd:submit
[    63.307] -> jd:submitted
[    63.377]    jpgdec: submit rc=0x0 nverr=0 fence 1/1 REACHED after 314 us | NVJPG 422400000 Hz read 163 us before submit returned, 422400000 Hz after | status used=292 mcu=0x0 result=0 | 16384/16384 output bytes written
[    63.431]    jpgdec: px(0,0)=dc 1e 1e ff  px(63,0)=1d c8 3c ff  px(0,63)=28 3c db ff
[    63.496]    jpgdec: worst 8x8 block-mean deviation 0 (block 0)  ->  *** NVJPG RAN AND DECODED CORRECTLY - this process can drive a non-VIC engine ***
[    63.636] -> jd:MATCH
[    63.676] -> vb:released
[    63.720]    clk-watch: stopped (engine probe finished)
[    63.858] -> clk:done
```

## `hb:` lines, 5 s before to 30 s after the jpgdec section, plus the last line in the file

```
[    60.440] -> hb:16 sess=1 getdisp=1 relay=1 txn=2771 vic=vb:3_Initialize
[    63.532] -> hb:17 sess=1 getdisp=1 relay=1 txn=3131 vic=jd:1
[    66.643] -> hb:18 sess=1 getdisp=1 relay=1 txn=3507 vic=vb:released
[    69.726] -> hb:19 sess=1 getdisp=1 relay=1 txn=3867 vic=vb:released
[    72.753] -> hb:20 sess=1 getdisp=1 relay=1 txn=4231 vic=vb:released
[    75.781] -> hb:21 sess=1 getdisp=1 relay=1 txn=4595 vic=vb:released
[    78.814] -> hb:22 sess=1 getdisp=1 relay=1 txn=4957 vic=vb:released
[    81.836] -> hb:23 sess=1 getdisp=1 relay=1 txn=5321 vic=vb:released
[    84.862] -> hb:24 sess=1 getdisp=1 relay=1 txn=5685 vic=vb:released
[    87.886] -> hb:25 sess=1 getdisp=1 relay=1 txn=6047 vic=vb:released
[    90.911] -> hb:26 sess=1 getdisp=1 relay=1 txn=6411 vic=vb:released
[    93.935] -> hb:27 sess=1 getdisp=1 relay=1 txn=6773 vic=vb:released

...

[   363.749] -> hb:116 sess=1 getdisp=1 relay=1 txn=38357 vic=vb:released
```

`txn` climbs steadily and without a gap from `hb:1` (t=13.569, txn=0) through
`hb:116` (t=363.749, txn=38357) - full log checked, not just this excerpt.

## `applet-mitm.last`

```
hb:116 sess=1 getdisp=1 relay=1 txn=38357 vic=vb:released
```

## `check` tool output, and the `.rgba` file's actual content

```
$ python3 tools/nvjpg_dec_control.py check logs/m77-runC-nvjpg-dec.rgba
max per-pixel channel diff 255, mean 165.54  -> logs/m77-runC-nvjpg-dec.png
MISMATCH (see the png)
```

This conflicts with the console's own `px(...)` samples logged above (which
match the known test image's red/green/blue quadrants to within 1 count -
checked by hand against `tools/nvjpg_dec_control.py`'s `make_image()`). Ruled
out host-side caching: re-checked after an explicit `udisksctl unmount` +
`mount` cycle, same bytes. Directly inspected `logs/m77-runC-nvjpg-dec.rgba`
(16384 B, matches the expected size):

- 7124 of 16384 bytes are non-zero; the last non-zero byte is at offset 13564
  (row 52 of 64) - rows 56-63 are entirely zero.
- Row 0's first 16 bytes decode as ASCII: `hb:23 sess=1 get` - a fragment of
  this run's own `applet-mitm.log` text. Row 8 contains `a.png` and row 48
  contains `Cruiser.msbt` (a Mario Kart 8 Deluxe asset filename).
- `tier4/applet-mitm/source/applet_mitm_nv.cpp:3534` calls
  `static_cast<void>(fs::WriteFile(f, 0, g_vic_dst_buf + OutOff, OutSize, fs::WriteOption::Flush));`
  - the `Result` is discarded, never checked for success.

So `nvjpg-dec.rgba` is not the engine's output; it is whatever was previously
on those FAT clusters, consistent with `WriteFile` failing or writing nothing
and nobody noticing.

## Crash reports

- `atmosphere/fatal_errors/`: unchanged from the known pre-existing baseline
  (same 4 files, same names, directory not modified). No new entry from this
  run.
- `atmosphere/crash_reports/`: contains many entries, but the directory's own
  mtime is `2026-08-31 23:32` - three weeks before this run - so nothing in it
  is from tonight. (File mtimes on this card have been unreliable all session;
  the directory mtime is the one signal trusted here, since it only advances
  when an entry is added or removed.)

No files copied to `logs/` from either directory: nothing in `fatal_errors/`
is new, and nothing in `crash_reports/` could be attributed to this run.

## What was seen on screen / shutdown

Game ran with no visual artifacts, no stutter, no slowdown, for the entire
session. Console was powered off normally, via the Ultrahand/Tesla overlay
menu's "Reboot to Hekate" - a controlled software reboot, not a forced
power-off, and not the literal "Power -> Power Options -> Turn Off" path this
document specified, but equally not a crash or freeze. No forced power-off
was needed at any point.

## Summary

NVJPG decoded correctly in 314 us with the clock confirmed non-zero at
submit time (M77's fix works); the separate PC-verifiable proof file was not
actually written due to an unrelated, unchecked `fs::WriteFile` result.
