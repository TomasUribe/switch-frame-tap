# M81 Run G: result

Build `548e80e`/`5e6eeb7` (no code changed between them - checked, empty
diff), installed and run once on hardware. Arm file: `vic nvgrc grcscan
wait=60`. PC selftest passed before install: "selftest OK".

No `mitm.lst` was present. Stale results from the previous run
(`applet-mitm.log`, `applet-mitm.last`, `nvenc-status.bin`, `nvenc-bits.bin`,
`nvenc-recon-y.bin`) were present and deleted per step 2; `grc-scan.bin` and
`nvjpg-dec.rgba` were not present. Baseline crash-report listings recorded
before install: 141 names in `crash_reports/`, 4 in `fatal_errors/`.

## Boot banner and armed flags

```
[    10.363] applet-mitm M81: up (grc IPC interceptor off)
[    10.438] ARMED FLAGS: vic=1 exec=0 dbg=0 dump=0 usb=0 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=1 clk=0 jpgdec=0 nvgrc=1 wait=60
```

## grc observer section, `---- GRC OBSERVER` to `grcscan:done`

```
[    64.087]    ---- GRC OBSERVER (read-only; no channel, no submit) ----
[    64.102]    pm:dmnt GetProcessId(0100000000000035) rc=0x0 -> pid=138
[    64.111]    DebugActiveProcess rc=0x0
[    64.134]    drained 15 events; ContinueDebugEvent rc=0x0 -> grc RESUMED
[    64.831]    scanned 14980 KB across 50 regions; skipped 3 region(s) over 8 MB (86624 KB)
[    64.847]    hits: setup magic 5, NVENC SETCL 326, NVJPG SETCL 0, VIC SETCL 0, SET_IN_DRV_PIC_SETUP writes 326, NVENC status blocks 3
[    64.857]    grc's own NVENC status blocks: error_status 0:0 1:0 2:3 3:0, ucode_error_status non-zero in 0
[    64.866]    hit  0 @ 0x511a4f81cc  NVENC magic 5.0             d0b70006 006e0100 00000001 00000000
[    64.874]    hit  1 @ 0x511a536570  NVENC magic 5.0             d0b70006 006e0100 00000001 00000000
[    64.886]    hit  2 @ 0x511a559000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    64.900]    hit  3 @ 0x511a55c000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    64.908]    hit  4 @ 0x511a55f000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    64.940]    hit  5 @ 0x511a562000  NVENC status block          00000408 00000002 0002e290 0002e270
[    64.955]    hit  6 @ 0x511a565000  NVENC status block          00000407 00000002 000233a0 00023380
[    64.964]    hit  7 @ 0x511a568000  NVENC status block          00000408 00000002 0002e290 0002e270
[    64.977]    hit  8 @ 0x6ba33cd000  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    64.988]    hit  9 @ 0x6ba33cd030  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e28200 10100002
[    65.015]    hit 10 @ 0x6ba33cd0c8  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.022]    hit 11 @ 0x6ba33cd0f8  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e2c200 10100002
[    65.041]    hit 12 @ 0x6ba33cd190  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.067]    hit 13 @ 0x6ba33cd1c0  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e28200 10100002
[    65.073]    hit 14 @ 0x6ba33cd258  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.077]    hit 15 @ 0x6ba33cd288  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e2c200 10100002
[    65.091]    hit 16 @ 0x6ba33cd320  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.100]    hit 17 @ 0x6ba33cd350  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e28200 10100002
[    65.108]    hit 18 @ 0x6ba33cd3e8  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.116]    hit 19 @ 0x6ba33cd418  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e2c200 10100002
[    65.135]    hit 20 @ 0x6ba33cd4b0  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.141]    hit 21 @ 0x6ba33cd4e0  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e28200 10100002
[    65.154]    hit 22 @ 0x6ba33cd578  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.168]    hit 23 @ 0x6ba33cd5a8  SET_IN_DRV_PIC_SETUP write  10100002 000001c4 00e2c200 10100002
[    65.177]    hit 24 @ 0x6ba33cd640  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.191]    hit 25 @ 0x6ba33cd708  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.203]    hit 26 @ 0x6ba33cd7d0  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.218]    hit 27 @ 0x6ba33cd898  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.248]    hit 28 @ 0x6ba33cd960  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.283]    hit 29 @ 0x6ba33cda04  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.304]    hit 30 @ 0x6ba33cdacc  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.326]    hit 31 @ 0x6ba33cdb94  SETCL class 0x21 (NVENC)    00000840 100b0001 00000000 10100002
[    65.339]    plausible setups 3, command-buffer windows 4; watch: 62 polls in 372 ms, 5 new setups dumped, intra CAPTURED, 0 debug events
[    65.352]    dumped 22 block(s) into 64783 B of records -> sdmc:/grc-scan.bin (decode: tools/nvrec.py)
[    65.422]    sd(sdmc:/grc-scan.bin): 64783 B written, read back identical, fnv1a32=c26dacce
[    65.471] -> grcscan:done
```

## Full NVENC section, `---- NVENC: REPLAY OF GRC'S IDR JOB` to its last `ng:` line

```
[    65.527] -> ng:1
[    65.572]    ---- NVENC: REPLAY OF GRC'S IDR JOB (M81: five variants) ----
[    65.627]    nvmapOwn(0x19e7f70000, 0x521000) kept CACHEABLE - caller must flush before submit
[    65.658]    nvmapOwn ok handle=10196 id=10196 kind=0 size=0x521000
[    65.688]    MAP_CMD_BUFFER(nvgrc-arena handle=10196 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x1960000
[    65.710]    MAP_CMD_BUFFER(nvgrc-cmd handle=7196 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x1ea0000
[    65.743]    nvgrc: syncpt=14 arena=0x1960000 (5252 KB) setup=0x1960000 status=0x1963000 bits=0x19a3000 cur=0x1bc3000/0x1ca9000 ref out=0x1d21000
[    65.766]    nvgrc[a]: grc's job, flat stripes (Run F), SET_CONTROL_PARAMS 0x12001103, picture index 0x4d383000
[    65.815]    clk(nvgrc): smGetService(mm:u) rc=0x0
[    65.833]    clk: mm id 5 (libnx: NVENC, nvtegra: NVDEC) req=1  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    65.876]    clk: mm id 6 (libnx: NVDEC) req=2  get=979200000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    65.924]    clk: mm id 7 (NVJPG) req=3  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=652800000 Hz (rc=0x0)
[    65.986]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    66.102] -> ng:submit
[    66.152]    nvgrc[a]: submit rc=0x0 nverr=0 fence 1097/1097 REACHED, status WRITTEN 102 us after submit | NVENC 979200000 Hz read 88 us before submit returned, 979200000 Hz after
[    66.208]    nvgrc[a]: status error_status=2 ucode_error_status=0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 intra/inter MBs=3600/0
[    66.288]    nvgrc[a]: bitstream starts 00 00 00 01 65; reconstructed luma worst stripe deviation 0 -> encoded, reconstruction matches the input
[    66.327]    sd(sdmc:/nvenc-a-status.bin): 4096 B written, read back identical, fnv1a32=0f35f63c
[    66.352]    sd(sdmc:/nvenc-a-bits.bin): 65536 B written, read back identical, fnv1a32=e869e7fa
[    66.533]    sd(sdmc:/nvenc-a-recon-y.bin): 921600 B written, read back identical, fnv1a32=0b3fddc5
[    66.538]    nvgrc[a]: RC-process buffer: last non-zero byte at +0xf2 of 0x20000
[    66.566]    sd(sdmc:/nvenc-a-rc.bin): 4096 B written, read back identical, fnv1a32=d28ae34f
[    66.572]    nvgrc[b]: grc's job, stripes + balanced noise, SET_CONTROL_PARAMS 0x12001103, picture index 0x4d383001
[    66.604]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    66.635] -> ng:submit
[    66.648]    nvgrc[b]: submit rc=0x0 nverr=0 fence 1113/1113 REACHED, status WRITTEN 2704 us after submit | NVENC 979200000 Hz read 129 us before submit returned, 979200000 Hz after
[    66.654]    nvgrc[b]: status error_status=2 ucode_error_status=0 total_bit_count=2766344 (345793 B) pic_type=3 num_slices=1 avgQP=21 QP 8..26 hrdFullness=0 intra/inter MBs=3600/0
[    66.667]    nvgrc[b]: bitstream starts 00 00 00 01 65; reconstructed luma worst stripe deviation 0 -> encoded, reconstruction matches the input
[    66.706]    sd(sdmc:/nvenc-b-status.bin): 4096 B written, read back identical, fnv1a32=cc01e456
[    66.782]    sd(sdmc:/nvenc-b-bits.bin): 345809 B written, read back identical, fnv1a32=35414534
[    66.788]    nvgrc[b]: RC-process buffer: last non-zero byte at +0xf2 of 0x20000
[    66.820]    sd(sdmc:/nvenc-b-rc.bin): 4096 B written, read back identical, fnv1a32=70a67ae0
[    66.824]    nvgrc[c]: RCMODE 0 (constant QP), flat stripes, SET_CONTROL_PARAMS 0x00001103, picture index 0x4d383002
[    66.833]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    66.888] -> ng:submit
[    66.983]    nvgrc[c]: submit rc=0x0 nverr=0 fence 1122/1122 REACHED, status WRITTEN 28 us after submit | NVENC 979200000 Hz read 76 us before submit returned, 979200000 Hz after
[    67.007]    nvgrc[c]: status error_status=2 ucode_error_status=0 total_bit_count=4040 (505 B) pic_type=3 num_slices=1 avgQP=24 QP 0..0 hrdFullness=0 intra/inter MBs=3600/0
[    67.017]    nvgrc[c]: bitstream starts 00 00 00 01 65; reconstructed luma worst stripe deviation 0 -> encoded, reconstruction matches the input
[    67.103]    sd(sdmc:/nvenc-c-status.bin): 4096 B written, read back identical, fnv1a32=767aa3b2
[    67.208]    sd(sdmc:/nvenc-c-bits.bin): 65536 B written, read back identical, fnv1a32=9ad15160
[    67.239]    nvgrc[c]: RC-process buffer: last non-zero byte at +0 of 0x20000
[    67.247]    nvgrc[d]: grc's job, setup rc hrd_type 0 (no HRD), flat stripes, SET_CONTROL_PARAMS 0x12001103, picture index 0x4d383003, setup patched
[    67.257]    nvgrc[d]: setup byte 0x70: 1 -> 0
[    67.268]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    67.323] -> ng:submit
[    67.332]    nvgrc[d]: submit rc=0x0 nverr=0 fence 1136/1136 REACHED, status WRITTEN 1957 us after submit | NVENC 979200000 Hz read 60 us before submit returned, 979200000 Hz after
[    67.337]    nvgrc[d]: status error_status=2 ucode_error_status=0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 intra/inter MBs=3600/0
[    67.342]    nvgrc[d]: bitstream starts 00 00 00 01 65; reconstructed luma worst stripe deviation 0 -> encoded, reconstruction matches the input
[    67.358]    sd(sdmc:/nvenc-d-status.bin): 4096 B written, read back identical, fnv1a32=e37b6d03
[    67.378]    sd(sdmc:/nvenc-d-bits.bin): 65536 B written, read back identical, fnv1a32=e869e7fa
[    67.383]    nvgrc[d]: RC-process buffer: last non-zero byte at +0xf2 of 0x20000
[    67.413]    sd(sdmc:/nvenc-d-rc.bin): 4096 B written, read back identical, fnv1a32=d28ae34f
[    67.416]    nvgrc[e]: grc's job, setup two_pass_rc 0, flat stripes, SET_CONTROL_PARAMS 0x12001103, picture index 0x4d383004, setup patched
[    67.422]    nvgrc[e]: setup byte 0xba: 1 -> 0
[    67.456]    clk-ensure(nvgrc) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    67.480] -> ng:submit
[    67.488]    nvgrc[e]: submit rc=0x0 nverr=0 fence 1141/1141 REACHED, status WRITTEN 882 us after submit | NVENC 979200000 Hz read 704 us before submit returned, 979200000 Hz after
[    67.493]    nvgrc[e]: status error_status=2 ucode_error_status=0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 intra/inter MBs=3600/0
[    67.502]    nvgrc[e]: bitstream starts 00 00 00 01 65; reconstructed luma worst stripe deviation 0 -> encoded, reconstruction matches the input
[    67.537]    sd(sdmc:/nvenc-e-status.bin): 4096 B written, read back identical, fnv1a32=f9cba1e0
[    67.554]    sd(sdmc:/nvenc-e-bits.bin): 65536 B written, read back identical, fnv1a32=e869e7fa
[    67.558]    nvgrc[e]: RC-process buffer: last non-zero byte at +0xf2 of 0x20000
[    67.576]    sd(sdmc:/nvenc-e-rc.bin): 4096 B written, read back identical, fnv1a32=d28ae34f
[    67.583]    nvgrc: 5 of 5 variants encoded
[    67.605] -> ng:ENCODED
```

## `hb:` lines, 5 s before the observer section to 30 s after the NVENC section, plus the last line

Observer section starts 64.087; NVENC section ends 67.605 (`vb:released`
follows at 67.648).

```
[    60.768] -> hb:16 sess=1 getdisp=1 relay=1 txn=2861 vic=vb:0_heap
[    63.902] -> hb:17 sess=1 getdisp=1 relay=1 txn=3291 vic=ind:4_image_map
[    67.002] -> hb:18 sess=1 getdisp=1 relay=1 txn=3657 vic=ng:1
[    70.022] -> hb:19 sess=1 getdisp=1 relay=1 txn=4019 vic=vb:released
[    73.040] -> hb:20 sess=1 getdisp=1 relay=1 txn=4381 vic=vb:released
[    76.065] -> hb:21 sess=1 getdisp=1 relay=1 txn=4745 vic=vb:released
[    79.088] -> hb:22 sess=1 getdisp=1 relay=1 txn=5107 vic=vb:released
[    82.115] -> hb:23 sess=1 getdisp=1 relay=1 txn=5469 vic=vb:released
[    85.139] -> hb:24 sess=1 getdisp=1 relay=1 txn=5833 vic=vb:released
[    88.164] -> hb:25 sess=1 getdisp=1 relay=1 txn=6195 vic=vb:released
[    91.190] -> hb:26 sess=1 getdisp=1 relay=1 txn=6559 vic=vb:released
[    94.214] -> hb:27 sess=1 getdisp=1 relay=1 txn=6921 vic=vb:released
[    97.239] -> hb:28 sess=1 getdisp=1 relay=1 txn=7285 vic=vb:released

...

[   215.286] -> hb:67 sess=1 getdisp=1 relay=1 txn=20625 vic=vb:released
```

`txn` climbs steadily with no gap from `hb:1` through the last line - full log
checked, not just this excerpt.

## `applet-mitm.last`

```
hb:67 sess=1 getdisp=1 relay=1 txn=20625 vic=vb:released
```

## PC check: `tools/nvenc_replay.py check`, full output

```
== a: grc's job, flat stripes (Run F) ==
status: picture_index=0x4d383000 error_status=2 ucode_error_status=0x0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 last_valid_byte_offset=516 intra_mbs=3600 inter_mbs=0
nvenc-a-bits.bin: 65536 B, first bytes 00 00 00 01 65 b8 04 04 bf dc fe 0b b9 fc a3 14
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 65532 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m81-runG/nvenc-a-decoded.png
nvenc-a-recon-y.bin: 921600 B, reconstructed-luma stripe check worst deviation 0.0  (matches)
nvenc-a-rc.bin: 4096 B, 22 non-zero words:
  +0x0010 0x00000001 1
  +0x002c 0x00000001 1
  +0x0038 0x000000e0 224
  +0x0040 0x00000022 34
  +0x0044 0x00000008 8
  +0x0048 0xffffffd3 -45
  +0x0050 0x00000019 25
  +0x0054 0x00000008 8
  +0x0058 0x08080808 134744072
  +0x005c 0x08080808 134744072
  +0x0060 0x08080808 134744072
  +0x0064 0x08080808 134744072
  +0x0068 0x08080808 134744072
  +0x006c 0x08080808 134744072
  +0x0070 0x08080808 134744072
  +0x0074 0x08080808 134744072
  +0x0078 0x08080808 134744072
  +0x007c 0x08080808 134744072
  +0x0080 0x08080808 134744072
  +0x0084 0x00000008 8
  +0x0088 0x00000002 2
  +0x00f0 0x00000100 256

== b: grc's job, stripes + balanced noise ==
status: picture_index=0x4d383001 error_status=2 ucode_error_status=0x0 total_bit_count=2766344 (345793 B) pic_type=3 num_slices=1 avgQP=21 QP 8..26 hrdFullness=0 last_valid_byte_offset=345793 intra_mbs=3600 inter_mbs=0
nvenc-b-bits.bin: 345809 B, first bytes 00 00 00 01 65 b8 04 04 bf dc 45 84 a8 6c 97 f2
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 345805 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m81-runG/nvenc-b-decoded.png
nvenc-b-rc.bin: 4096 B, 34 non-zero words:
  +0x0004 0x027aae80 41594496
  +0x0010 0x0000016d 365
  +0x002c 0x0000016d 365
  +0x0038 0x000000e0 224
  +0x0040 0x00000022 34
  +0x0044 0x00000b68 2920
  +0x0048 0x000002d2 722
  +0x0050 0x00000079 121
  +0x0054 0x00000015 21
  +0x0058 0x0b0a0808 185206792
  +0x005c 0x11100e0d 286264845
  +0x0060 0x17161413 387322899
  +0x0064 0x18181818 404232216
  +0x0068 0x18181717 404231959
  +0x006c 0x18181818 404232216
  +0x0070 0x17181818 387455000
  +0x0074 0x18181817 404232215
  +0x0078 0x18181818 404232216
  +0x007c 0x18181818 404232216
  +0x0080 0x19191818 421074968
  +0x0084 0x0000001a 26
  +0x0088 0x383a3e3f 943341119
  +0x008c 0x2b2e3234 724447796
  +0x0090 0x1e212528 505488680
  +0x0094 0x1c1d1c1d 471669789
  +0x0098 0x1c1d1e1f 471670303
  +0x009c 0x1c1d1c1d 471669789
  +0x00a0 0x1e1c1c1d 505158685
  +0x00a4 0x1c1d1c1f 471669791
  +0x00a8 0x1c1d1c1d 471669789
  +0x00ac 0x1c1d1c1d 471669789
  +0x00b0 0x1b1b1c1d 454761501
  +0x00b4 0x00000018 24
  +0x00f0 0x00000100 256

== c: RCMODE 0 (constant QP), flat stripes ==
status: picture_index=0x4d383002 error_status=2 ucode_error_status=0x0 total_bit_count=4040 (505 B) pic_type=3 num_slices=1 avgQP=24 QP 0..0 hrdFullness=0 last_valid_byte_offset=505 intra_mbs=3600 inter_mbs=0
nvenc-c-bits.bin: 65536 B, first bytes 00 00 00 01 65 b8 04 2f f1 02 73 dc 9f c0 ee c4
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 65532 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m81-runG/nvenc-c-decoded.png

== d: grc's job, setup rc hrd_type 0 (no HRD), flat stripes ==
status: picture_index=0x4d383003 error_status=2 ucode_error_status=0x0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 last_valid_byte_offset=516 intra_mbs=3600 inter_mbs=0
nvenc-d-bits.bin: 65536 B, first bytes 00 00 00 01 65 b8 04 04 bf dc fe 0b b9 fc a3 14
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 65532 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m81-runG/nvenc-d-decoded.png
nvenc-d-rc.bin: 4096 B, 22 non-zero words:
  +0x0010 0x00000001 1
  +0x002c 0x00000001 1
  +0x0038 0x000000e0 224
  +0x0040 0x00000022 34
  +0x0044 0x00000008 8
  +0x0048 0xffffffd3 -45
  +0x0050 0x00000019 25
  +0x0054 0x00000008 8
  +0x0058 0x08080808 134744072
  +0x005c 0x08080808 134744072
  +0x0060 0x08080808 134744072
  +0x0064 0x08080808 134744072
  +0x0068 0x08080808 134744072
  +0x006c 0x08080808 134744072
  +0x0070 0x08080808 134744072
  +0x0074 0x08080808 134744072
  +0x0078 0x08080808 134744072
  +0x007c 0x08080808 134744072
  +0x0080 0x08080808 134744072
  +0x0084 0x00000008 8
  +0x0088 0x00000002 2
  +0x00f0 0x00000100 256

== e: grc's job, setup two_pass_rc 0, flat stripes ==
status: picture_index=0x4d383004 error_status=2 ucode_error_status=0x0 total_bit_count=4128 (516 B) pic_type=3 num_slices=1 avgQP=8 QP 8..8 hrdFullness=0 last_valid_byte_offset=516 intra_mbs=3600 inter_mbs=0
nvenc-e-bits.bin: 65536 B, first bytes 00 00 00 01 65 b8 04 04 bf dc fe 0b b9 fc a3 14
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 65532 B  first_mb=0 slice_type=2 pps_id=0
no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup
decoded 1280x720; stripe means (want/got): 32/32 40/40 48/48 56/56 64/64 72/72 80/80 88/88 96/96 104/104 112/112 120/120 128/128 136/136 144/144 152/152 160/160 168/168 176/176 184/184 192/192 200/200 208/208
DECODE MATCHES THE INPUT STRIPES
  -> logs/m81-runG/nvenc-e-decoded.png
```

`nvenc-e-rc.bin` is byte-identical to `nvenc-d-rc.bin` and `nvenc-a-rc.bin`
(same FNV-1a, `d28ae34f`); the check tool printed its non-zero-word dump once
under `a` and did not repeat it verbatim for `d`/`e`, so it is not duplicated
here either. `nvenc-c-rc.bin` does not exist (see file list below) and the
check tool accordingly printed no rc section for `c`.

## FNV-1a comparisons

All 16 recovered files, PC-computed vs. the console's own `sd(...)` line:

| file | PC-computed | log | |
|---|---|---|---|
| `grc-scan.bin` | `c26dacce` | `c26dacce` | equal |
| `nvenc-a-status.bin` | `0f35f63c` | `0f35f63c` | equal |
| `nvenc-a-bits.bin` | `e869e7fa` | `e869e7fa` | equal |
| `nvenc-a-recon-y.bin` | `0b3fddc5` | `0b3fddc5` | equal |
| `nvenc-a-rc.bin` | `d28ae34f` | `d28ae34f` | equal |
| `nvenc-b-status.bin` | `cc01e456` | `cc01e456` | equal |
| `nvenc-b-bits.bin` | `35414534` | `35414534` | equal |
| `nvenc-b-rc.bin` | `70a67ae0` | `70a67ae0` | equal |
| `nvenc-c-status.bin` | `767aa3b2` | `767aa3b2` | equal |
| `nvenc-c-bits.bin` | `9ad15160` | `9ad15160` | equal |
| `nvenc-d-status.bin` | `e37b6d03` | `e37b6d03` | equal |
| `nvenc-d-bits.bin` | `e869e7fa` | `e869e7fa` | equal |
| `nvenc-d-rc.bin` | `d28ae34f` | `d28ae34f` | equal |
| `nvenc-e-status.bin` | `f9cba1e0` | `f9cba1e0` | equal |
| `nvenc-e-bits.bin` | `e869e7fa` | `e869e7fa` | equal |
| `nvenc-e-rc.bin` | `d28ae34f` | `d28ae34f` | equal |

**File list, against the "up to 16" expected in the handoff:** 15 files were
produced, not 16. `nvenc-c-rc.bin` does not exist on the card and there is no
`sd(sdmc:/nvenc-c-rc.bin)` line in the log - the module never attempted to
write it for variant `c`. Every other expected file (`nvenc-{a..e}-status.bin`,
`nvenc-{a,b,c,d,e}-bits.bin`, `nvenc-{a,b,d,e}-rc.bin`, `nvenc-a-recon-y.bin`)
is present and verified.

## From `grc-scan.txt` (1783 lines - over 600, filtered per the handoff)

Every `NOTE` line containing `grc status @`:

```
[   64.254] NOTE   grc status @ 0x511a562000: picture_index 0x408 error_status 2 ucode 0 bits 189072 pic_type 0 slices 1 avgQP 13 intra/inter 8/3592
[   64.254] NOTE   grc status @ 0x511a565000: picture_index 0x407 error_status 2 ucode 0 bits 144288 pic_type 0 slices 1 avgQP 13 intra/inter 0/3600
[   64.256] NOTE   grc status @ 0x511a568000: picture_index 0x408 error_status 2 ucode 0 bits 189072 pic_type 0 slices 1 avgQP 13 intra/inter 8/3592
```

Every `NOTE` line containing `VIC SETCL`, with its `CMDBUF` block: **none
found this run** (`grep -c "VIC SETCL" logs/m81-runG-grc-scan.txt` = 0). grc
ran no VIC job during the scan window.

The first `NOTE ... NVENC SETCL` line, with its full `CMDBUF` block:

```
[   64.431] NOTE   NVENC SETCL @ 0x6ba33cd000
[   64.431] CMDBUF handle=0x6b offset=0xa33cd000 words=256
    SETCL class=0x21 (NVENC) offset=0x0 mask=0x0
      host1x reg 0x00b = 0x00000000
      NVENC  0x0700 SET_CONTROL_PARAMS               = 0x12001103
      NVENC  0x0704 SET_PICTURE_INDEX                = 0x000003d2
      NVENC  0x0200 SET_APPLICATION_ID               = 0x00000001
      NVENC  0x0710 SET_IN_DRV_PIC_SETUP             = 0x00e28200   (iova 0xe2820000)
      NVENC  0x0718 SET_OUT_ENC_STATUS               = 0x00e28400   (iova 0xe2840000)
      NVENC  0x0724 SET_IO_RC_PROCESS                = 0x00e28600   (iova 0xe2860000)
      NVENC  0x071c SET_OUT_BITSTREAM                = 0x00e28800   (iova 0xe2880000)
      NVENC  0x0720 SET_IOHISTORY                    = 0x00e29e00   (iova 0xe29e0000)
      NVENC  0x0734 SET_IN_CUR_PIC                   = 0x00e25200   (iova 0xe2520000)
      NVENC  0x0740 SET_IN_CUR_PIC_CHROMA_U          = 0x00e26060   (iova 0xe2606000)
      NVENC  0x0730 SET_OUT_REF_PIC_LUMA             = 0x00e2aa00   (iova 0xe2aa0000)
      NVENC  0x0738 SET_IN_MEPRED_DATA               = 0x00e2f200   (iova 0xe2f20000)
      NVENC  0x073c SET_OUT_MEPRED_DATA              = 0x00e2f400   (iova 0xe2f40000)
      NVENC  0x0400 SET_IN_REF_PIC0_LUMA             = 0x00e2dc00   (iova 0xe2dc0000)
      NVENC  0x0300 EXECUTE                          = 0x00000100
      INCR_SYNCPT cond=1 syncpt=14
    SETCL class=0x21 (NVENC) offset=0x0 mask=0x0
      host1x reg 0x00b = 0x00000000
      NVENC  0x0700 SET_CONTROL_PARAMS               = 0x12001103
      NVENC  0x0704 SET_PICTURE_INDEX                = 0x000003d3
      NVENC  0x0200 SET_APPLICATION_ID               = 0x00000001
      NVENC  0x0710 SET_IN_DRV_PIC_SETUP             = 0x00e2c200   (iova 0xe2c20000)
      NVENC  0x0718 SET_OUT_ENC_STATUS               = 0x00e2c400   (iova 0xe2c40000)
      NVENC  0x0724 SET_IO_RC_PROCESS                = 0x00e28600   (iova 0xe2860000)
      NVENC  0x071c SET_OUT_BITSTREAM                = 0x00e2c600   (iova 0xe2c60000)
      NVENC  0x0720 SET_IOHISTORY                    = 0x00e29e00   (iova 0xe29e0000)
      [... continues for several more repeats of the same SETCL/method
      pattern within this 256-word buffer window; the full 280-line block is
      committed verbatim in logs/m81-runG-grc-scan.txt, lines 22-301]
```

*(The complete `logs/m81-runG-grc-scan.txt`, all 1783 lines, is committed
alongside this report and includes three further `NVENC SETCL` blocks at the
same 372 ms watch window, plus the 5 `SETUP` header lines and `watch +...ms`
tracking lines not reproduced above.)*

## Decoded PNGs

`check` wrote `nvenc-a-decoded.png`, `nvenc-b-decoded.png`,
`nvenc-c-decoded.png`, `nvenc-d-decoded.png` and `nvenc-e-decoded.png` - all
five variants. Committed with the rest.

## Crash reports

No names in `crash_reports/` or `fatal_errors/` beyond the step-2 baseline
(141 and 4 respectively, both unchanged). No files copied.

## What was seen on screen / capture / shutdown

Everything ran fine, no forced shutdown. The user noted "a very slight
fraction of a second freeze" at the moment the Capture button was pressed
(around the 2-minute mark, per the run instructions), and nothing else
visible. The optional video capture recorded fine. Shutdown was normal, via
the Ultrahand/Tesla overlay's "Reboot to Hekate" shortcut, with the card
inserted in the reader afterward. The module's own engine work (observer plus
all five NVENC submits) completed and released by uptime ~67.6 s, well before
the ~120 s capture-button press the run instructions asked for; the log shows
continuous `queueBuffer` activity with no gap anywhere in the session,
including through and after that point.

## Summary

All five NVENC variants completed (fence reached, status written) and every
one's H.264 output decodes correctly, matching the input exactly; every
variant's `error_status` is 2, and the grc observer found grc's own live
encoder status blocks reporting the same `error_status` 2 on all three it
captured.
