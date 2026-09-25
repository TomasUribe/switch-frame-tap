# M80 Run F: result

Build `799173e`/`809bd91` (no code changed between them - checked, empty
diff), installed and run once on hardware. Arm file: `vic nvgrc wait=60`.
PC selftest (`python3 tools/nvenc_replay.py selftest`) passed before install:
"selftest OK".

No `mitm.lst` was present. Stale results from the previous run
(`applet-mitm.log`, `grc-scan.bin`) were present and deleted per step 2;
`nvenc-status.bin`/`nvenc-bits.bin`/`nvenc-recon-y.bin`/`nvjpg-dec.rgba` were
not present. Baseline crash-report listings recorded before install: 141
names in `crash_reports/`, 4 in `fatal_errors/`.

## Boot banner and armed flags

```
[    10.339] applet-mitm M80: up (grc IPC interceptor off)
[    10.411] ARMED FLAGS: vic=1 exec=0 dbg=0 dump=0 usb=0 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=0 clk=0 jpgdec=0 nvgrc=1 wait=60
```

## Full NVENC section, `---- NVENC: REPLAY OF GRC'S IDR JOB` to its result

```
[    62.464]    ---- NVENC: REPLAY OF GRC'S IDR JOB (M80) ----
[    62.477]    nvmapOwn(0x3e4d570000, 0x521000) kept CACHEABLE - caller must flush before submit
[    62.485]    nvmapOwn ok handle=5932 id=5932 kind=0 size=0x521000
[    62.497]    MAP_CMD_BUFFER(nvgrc-arena handle=5932 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2680000
[    62.519]    MAP_CMD_BUFFER(nvgrc-cmd handle=5820 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2bc0000
[    62.527]    nvgrc: syncpt=14 arena=0x2680000 (5252 KB) setup=0x2680000 status=0x2683000 bits=0x26c3000 cur=0x28e3000/0x29c9000 ref out=0x2a41000, 41 words
[    62.578] -> ng:clock
[    62.594]    clk(nvgrc): smGetService(mm:u) rc=0x0
[    62.609]    clk: mm id 5 (libnx: NVENC, nvtegra: NVDEC) req=2  get=460800000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    62.624]    clk: mm id 6 (libnx: NVDEC) req=3  get=979200000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    62.648]    clk: mm id 7 (NVJPG) req=4  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=652800000 Hz (rc=0x0)
[    62.664]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    62.714] -> ng:submit
[    62.734]    nvgrc: submit rc=0x0 nverr=0 fence 979/979 REACHED, status WRITTEN (our picture index), after 2111 us | NVENC 979200000 Hz read 192 us before submit returned, 979200000 Hz after
[    62.757]    nvgrc: status error_status=2 ucode_error_status=0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 cycles=0 start_pos=0 last_valid=516 intra/inter MBs=3600/0
[    62.767]    nvgrc: bitstream 412/65536 B nonzero, first bytes 00 00 00 01 65 b8 04 04 bf dc fe 0b
[    62.779]    nvgrc: reconstructed luma, worst stripe deviation 0  ->  the engine reported a problem - see status
[    62.810]    sd(sdmc:/nvenc-status.bin): 4096 B written, read back identical, fnv1a32=0f35f63c
[    62.844] -> hb:17 sess=1 getdisp=1 relay=1 txn=3123 vic=ng:1
[    62.857]    sd(sdmc:/nvenc-bits.bin): 65536 B written, read back identical, fnv1a32=e869e7fa
[    62.977]    sd(sdmc:/nvenc-recon-y.bin): 921600 B written, read back identical, fnv1a32=0b3fddc5
[    63.001] -> ng:engine_error
```

## `hb:` lines, 5 s before to 30 s after the NVENC section, plus the last line

Section spans 62.464 to 63.001.

```
[    59.812] -> hb:16 sess=1 getdisp=1 relay=1 txn=2769 vic=waiting
[    62.844] -> hb:17 sess=1 getdisp=1 relay=1 txn=3123 vic=ng:1
[    65.940] -> hb:18 sess=1 getdisp=1 relay=1 txn=3451 vic=vb:released
[    69.059] -> hb:19 sess=1 getdisp=1 relay=1 txn=3825 vic=vb:released
[    72.140] -> hb:20 sess=1 getdisp=1 relay=1 txn=4187 vic=vb:released
[    75.213] -> hb:21 sess=1 getdisp=1 relay=1 txn=4553 vic=vb:released
[    78.285] -> hb:22 sess=1 getdisp=1 relay=1 txn=4919 vic=vb:released
[    81.309] -> hb:23 sess=1 getdisp=1 relay=1 txn=5285 vic=vb:released
[    84.333] -> hb:24 sess=1 getdisp=1 relay=1 txn=5647 vic=vb:released
[    87.358] -> hb:25 sess=1 getdisp=1 relay=1 txn=6011 vic=vb:released
[    90.383] -> hb:26 sess=1 getdisp=1 relay=1 txn=6373 vic=vb:released
[    93.409] -> hb:27 sess=1 getdisp=1 relay=1 txn=6737 vic=vb:released

...

[   257.321] -> hb:81 sess=1 getdisp=1 relay=1 txn=25663 vic=vb:released
```

`txn` climbs steadily with no gap from `hb:1` through the last line - full log
checked, not just this excerpt.

## `applet-mitm.last`

```
hb:81 sess=1 getdisp=1 relay=1 txn=25663 vic=vb:released
```

## PC check: `tools/nvenc_replay.py check`, full output

```
status: picture_index=1295527936 error_status=2 ucode_error_status=0x0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 cycle_count=0 bitstream_start_pos=0 last_valid_byte_offset=516 intra_mbs=3600 inter_mbs=0
nvenc-bits.bin: 65536 B, first bytes 00 00 00 01 65 b8 04 04 bf dc fe 0b b9 fc a3 14
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 65532 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m80-runF/nvenc-decoded.png
nvenc-recon-y.bin: 921600 B, reconstructed-luma stripe check worst deviation 0.0  (matches)
```

`logs/m80-runF/nvenc-decoded.png` was written by the check tool and is
committed alongside the rest.

## FNV-1a comparisons

| file | PC-computed | log's `sd(...)` line | |
|---|---|---|---|
| `nvenc-status.bin` | `0f35f63c` | `0f35f63c` | equal |
| `nvenc-bits.bin` | `e869e7fa` | `e869e7fa` | equal |
| `nvenc-recon-y.bin` | `0b3fddc5` | `0b3fddc5` | equal |

## Crash reports

No names in `crash_reports/` or `fatal_errors/` beyond the step-2 baseline
(141 and 4 respectively, both unchanged). No files copied.

## What was seen on screen / capture / shutdown

Everything ran fine, no artifacts or stutter reported. The optional video
capture was taken and confirmed saved. Shutdown was normal, via the
Ultrahand/Tesla overlay's "Reboot to Hekate" shortcut. No forced power-off
was needed.

## Summary

The submit reached completion (fence matched, status written) with
`error_status=2` reported by the engine, but the H.264 bitstream it produced
decodes as a valid IDR slice matching the input stripes exactly and the
reconstructed luma matches at zero deviation; all three output files verify
byte-for-byte against the console's own hashes.
