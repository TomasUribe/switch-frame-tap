# M83 Run I: result

Build `01d3f81`/`b322b34` (no code changed in `tier4/applet-mitm/` between
them - checked, empty diff), built locally (SHA-256 `c99aaf25...`, not the
prebuilt), installed and run once on hardware, handheld with the USB-C cable
to the PC. Arm file: `vic exec dbg usb csc nvframe nvstream nvp wait=60`.

PC side before install: `bash tools/run_pc_tests.sh` -> "all PC-side checks
passed" (11 PASS). The first attempt failed only the two raw-view checks
because `libusb-1.0-0-dev` and `libsdl2-dev` were missing; the user installed
them and the full suite then passed. `make -C tools/raw-recv` -> "raw-view
built WITH H.264 (libavcodec)". udev rule was already installed.

Install: no `mitm.lst`; no old result files were present, nothing deleted.
Baseline crash-report listings: 141 in `crash_reports/`, 4 in `fatal_errors/`.
The card was read back through Hekate's USB mass-storage mode (block-level,
not MTP), with a clean unmount/remount before copying.

## Boot banner and armed flags

```
[    10.351] applet-mitm M83: up (grc IPC interceptor off)
[    10.422] ARMED FLAGS: vic=1 exec=1 dbg=1 dump=0 usb=1 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=0 clk=0 jpgdec=0 nvgrc=0 csc=1 nvframe=1(120) nvstream=1(3600, qp 20) nvp=1(30) wait=60
```

## Colour-matrix section (header, every `csc[...]` line, `sd(...)`, completion line)

```
[    65.858]    ---- VIC COLOUR-MATRIX PROBES (M82/M83): 12 jobs on a 64x64 card we own ----
[    66.027]    csc[none      ] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    66.183]    csc[out_k8    ] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    66.324]    csc[out_k12   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    66.421]    csc[out_k16   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    66.529]    csc[out_k19   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    66.867]    csc[out_k16_s8] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    67.457]    csc[out_off   ] done  patch(200,200,200): Y=0 U=0 V=0  patch(200,40,40): Y=0 U=0 V=0
[    67.689]    csc[out_neg   ] done  patch(200,200,200): Y=255 U=255 V=0  patch(200,40,40): Y=255 U=255 V=0
[    67.865]    csc[out_dense ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    67.955]    csc[slot_k16  ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    68.053]    csc[m64_bt601 ] done  patch(200,200,200): Y=4 U=32 V=32  patch(200,40,40): Y=4 U=31 V=32
[    68.143]    csc[bt709     ] done  patch(200,200,200): Y=189 U=128 V=128  patch(200,40,40): Y=80 U=112 V=199
[    68.188]    sd(sdmc:/vic-csc.bin): 91056 B written, read back identical, fnv1a32=28e83567
[    68.195]    csc: 12 of 12 probes completed; 91056 B -> sdmc:/vic-csc.bin (decode: tools/vic_csc.py)
```

## Real-frame section, `---- NVENC ON REAL GAME FRAMES` to its last `nf:` line

```
[    69.161]    ---- NVENC ON REAL GAME FRAMES (M83): game slot -> VIC BT.709 NV12, 16-row block-linear -> IDR ----
[    69.225]    nvmapOwn(0x3b88560000, 0x870000) kept CACHEABLE - caller must flush before submit
[    69.254]    nvmapOwn ok handle=7176 id=7176 kind=0 size=0x870000
[    69.271]    MAP_CMD_BUFFER(nvf-slot handle=7176 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x1980000
[    69.321]    nvmapOwn(0x3b89430000, 0x4e4000) kept CACHEABLE - caller must flush before submit
[    69.333]    nvmapOwn ok handle=7216 id=7216 kind=0 size=0x4e4000
[    69.360]    MAP_CMD_BUFFER(nvf-arena(vic) handle=7216 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2a00000
[    69.386]    MAP_CMD_BUFFER(nvf-arena(enc) handle=7216 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2a00000
[    69.394]    nvframe: arena 5008 KB at +15168 KB; VIC sees it at 0x2a00000, NVENC at 0x2a00000 (syncpt 14); picture 0x2c30000/0x2d11000, bits 0x2a10000
[    69.409]    clk(nvframe): smGetService(mm:u) rc=0x0
[    69.427]    clk: mm id 5 (libnx: NVENC, nvtegra: NVDEC) req=1  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    69.439]    clk: mm id 6 (libnx: NVDEC) req=2  get=979200000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    69.455]    clk: mm id 7 (NVJPG) req=3  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=652800000 Hz (rc=0x0)
[    69.470]    clk-ensure(nvframe) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    69.502]    nvframe: content 1280x720 (handheld) (140 samples outside the top-left 1280x720 all zero) -> the VIC converts the corner 1:1; later frames read only its 3.9 MB
[    69.658]    sd(sdmc:/nvframe-0-src.bin): 655360 B written, read back identical, fnv1a32=59a71634
[    69.771] -> nf:vic0
[    69.801]    [nf:vic0] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    69.821]    [nf:vic0] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2517
[    69.843]    [nf:vic0] resolved addrs: cfg=0x00026800 dst=0x0002c300 src=0x0002d110 (<<8: 0x2680000 0x2c30000 0x2d11000)
[    69.867]    [nf:vic0] WAIT nverr=0  syncpt=2521 (want >= 2517)  OP_DONE fired
[    69.896]    [nf:vic0] output went to a caller-owned buffer at 0x2c30000 - the caller checks and saves it
[    70.003]    nvframe[0]: slot read 16799 us, VIC -> BT.709 NV12 (16-row blocks) done in 308038 us incl. logging (NVENC 979200000 Hz)
[    70.024]    nvframe[0] QP 16: 88790 B in 2449 us (status: error 2 ucode 0 pic_type 3 avgQP 16 intra 3600)
[    70.104]    sd(sdmc:/nvframe-q16-status.bin): 4096 B written, read back identical, fnv1a32=4784590a
[    70.167]    sd(sdmc:/nvframe-q16-bits.bin): 88806 B written, read back identical, fnv1a32=395a4897
[    70.175]    nvframe[0] QP 20: 64906 B in 2070 us (status: error 2 ucode 0 pic_type 3 avgQP 20 intra 3600)
[    70.238]    sd(sdmc:/nvframe-q20-status.bin): 4096 B written, read back identical, fnv1a32=9468008f
[    70.306]    sd(sdmc:/nvframe-q20-bits.bin): 64922 B written, read back identical, fnv1a32=8058032c
[    70.342]    nvframe[0] QP 24: 45161 B in 2059 us (status: error 2 ucode 0 pic_type 3 avgQP 24 intra 3600)
[    70.409]    sd(sdmc:/nvframe-q24-status.bin): 4096 B written, read back identical, fnv1a32=7d057174
[    70.506] -> hb:19 sess=1 getdisp=1 relay=1 txn=3335 vic=nf:vic0
[    70.529]    sd(sdmc:/nvframe-q24-bits.bin): 45177 B written, read back identical, fnv1a32=f0e6749b
[    70.690]    sd(sdmc:/nvframe-0-y.bin): 921600 B written, read back identical, fnv1a32=b2938d1d
[    70.769]    sd(sdmc:/nvframe-0-uv.bin): 471040 B written, read back identical, fnv1a32=66625dc5
[    72.651] *** binder txn #3600  program=0100152000022000  session=20  code=7(queueBuffer) x1796  flags=0x0  in=192 out=4096
[    72.955]    nvframe: 120 of 120 frames at QP 20 in 2168 ms -> 55.3 fps (game presented 59.0 fps), 1 distinct, 0 with errors
[    72.987]    nvframe: per frame avg/max us: read+flush 7181/29441  VIC 1363/15538  NVENC 3202/23837  work 11747/44250  (60 fps budget 16667); waiting for presents avg 6319
[    72.999]    nvframe: IDR size avg 66320 B (min 64615, max 68132) -> 31 Mbps at 60 fps; NVENC 979200000 Hz after
[    73.042]    sd(sdmc:/nvframe-last-status.bin): 4096 B written, read back identical, fnv1a32=c70599eb
[    73.085]    sd(sdmc:/nvframe-last-bits.bin): 66153 B written, read back identical, fnv1a32=b6dcbcc6
[    74.307] -> hb:20 sess=1 getdisp=1 relay=1 txn=3701 vic=nf:vic
[    74.337]    sd(sdmc:/nvframe-last-y.bin): 921600 B written, read back identical, fnv1a32=df52da09
[    74.392]    sd(sdmc:/nvframe-last-uv.bin): 471040 B written, read back identical, fnv1a32=66625dc5
[    74.427] -> nf:DONE
```

## Stream section, `---- H.264 STREAM OVER USB` to its last `ns:` line

```
[    74.475]    ---- H.264 STREAM OVER USB (M83): 3600 frames, IDR-only, QP 20, BT.709 ----
[    74.487]    nvmapOwn(0x3b88560000, 0x870000) kept CACHEABLE - caller must flush before submit
[    74.496]    nvmapOwn ok handle=11088 id=11088 kind=0 size=0x870000
[    74.505]    MAP_CMD_BUFFER(nvf-slot handle=11088 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2f00000
[    74.516]    nvmapOwn(0x3b89430000, 0x4e4000) kept CACHEABLE - caller must flush before submit
[    74.525]    nvmapOwn ok handle=11092 id=11092 kind=0 size=0x4e4000
[    74.534]    MAP_CMD_BUFFER(nvf-arena(vic) handle=11092 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x3780000
[    74.545]    MAP_CMD_BUFFER(nvf-arena(enc) handle=11092 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x3780000
[    74.554]    nvstream: arena 5008 KB at +15168 KB; VIC sees it at 0x3780000, NVENC at 0x3780000 (syncpt 14); picture 0x39b0000/0x3a91000, bits 0x3790000
[    74.567]    clk-ensure(nvstream) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    74.604]    nvstream: content 1280x720 (handheld) (140 samples outside the top-left 1280x720 all zero) -> the VIC converts the corner 1:1; later frames read only its 3.9 MB
[    77.346] -> hb:21 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    80.392] -> hb:22 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    83.434] -> hb:23 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    86.478] -> hb:24 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    89.517] -> hb:25 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    91.072]    nvstream: 600 frames in 16457 ms, 599 sent, avg 65988 B/frame, NVENC 979200000 Hz
[    92.561] -> hb:26 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    95.599] -> hb:27 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    98.642] -> hb:28 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   101.726] -> hb:29 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   104.842] -> hb:30 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   107.298]    nvstream: 1200 frames in 32683 ms, 1199 sent, avg 65988 B/frame, NVENC 979200000 Hz
[   107.910] -> hb:31 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   111.020] -> hb:32 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   114.093] -> hb:33 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   117.167] -> hb:34 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   120.206] -> hb:35 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   123.253] -> hb:36 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   124.214]    nvstream: 1800 frames in 49599 ms, 1799 sent, avg 65988 B/frame, NVENC 979200000 Hz
[   126.303] -> hb:37 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   127.046]    nvstream: stopped: slot read failed
[   127.110]    nvstream: 1883 frames sent in 52422 ms -> 35.9 fps (game presented 0.0 fps, 0 presents skipped), 0 encodes with errors
[   127.192]    nvstream: per frame avg us: read+flush 2429  VIC 536  NVENC 2183  copy 38  usb 702; worst work 217673 us
[   127.272]    nvstream: avg 65988 B/frame (max 65989) -> 18 Mbps at the achieved rate; clock re-ensured 0 time(s)
[   127.654] -> ns:stopped
```

## P-frame section, `---- P FRAMES` to its end

The section logs no `np:` line after `-> np:1`; it ends at `nvp: slot read
failed`, after which the debug-capture summary begins.

```
[   127.727] -> np:1
[   127.744]    ---- P FRAMES (M83 nvp): 1 IDR + 29 P from real frames, QP 20, grc's P setup and P job ----
[   127.761]    nvmapOwn(0x3b88560000, 0x870000) kept CACHEABLE - caller must flush before submit
[   127.777]    nvmapOwn ok handle=11100 id=11100 kind=0 size=0x870000
[   127.797]    MAP_CMD_BUFFER(nvf-slot handle=11100 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x3c80000
[   127.821]    nvmapOwn(0x3b88ff0000, 0x6c5000) kept CACHEABLE - caller must flush before submit
[   127.840]    nvmapOwn ok handle=11104 id=11104 kind=0 size=0x6c5000
[   127.857]    MAP_CMD_BUFFER(nvf-arena(vic) handle=11104 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x4500000
[   127.871]    MAP_CMD_BUFFER(nvf-arena(enc) handle=11104 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x4500000
[   127.893]    nvp: arena 6932 KB at +10816 KB; VIC sees it at 0x4500000, NVENC at 0x4500000 (syncpt 14); picture 0x4730000/0x4811000, bits 0x4510000
[   127.909]    clk-ensure(nvp) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[   127.958]    nvp: slot read failed
[   128.035] -> dbg:2_attached
```

## Debug-capture summary, `DebugActiveProcess(pid=` to `---- VIC on real game pixels ----`

```
[   128.053]    DebugActiveProcess(pid=142) rc=0x0 ATTACHED
[   128.065]    walked 1664 regions (complete); 6 big enough (1 exactly 26542080 B); 3 probe reads
[   128.077]      region 0: base=0x324bfe6000 size=26542080   <== EXACT SWAPCHAIN SIZE
[   128.089]      region 1: base=0x323a26d000 size=41541632
[   128.103]      region 2: base=0x323d2ca000 size=87912448
[   128.114]      region 3: base=0x3264622000 size=92274688
[   128.138]      region 4: base=0x327e1e4000 size=41021440
[   128.147]      region 5: base=0x3293431000 size=33554432
[   128.160]    *** SWAPCHAIN AT 0x324bfe6000 (offset 0 into its region) - matched by EXACT SIZE ***
[   128.173]        slot0 +0x0000000: ff ff ff ff  ff ff ff ff  ff ff ff ff  ff ff ff ff
[   128.181]        slot1 +0x0870000: ff ff ff ff  ff ff ff ff  ff ff ff ff  ff ff ff ff
[   128.193]        slot2 +0x10e0000: ff ff ff ff  ff ff ff ff  ff ff ff ff  ff ff ff ff
[   128.204]    GAME FROZEN FOR 0 ms (M45 was 4780 ms - only the strip read needs a halt)
[   128.214]    drained 35 debug events; ContinueDebugEvent rc=0x0 -> GAME RUNNING WHILE WE STAY ATTACHED
[   128.224]    ---- VIC on real game pixels ----
```

## `hb:` lines, 5 s before the colour-matrix section to 30 s after the P-frame section, plus the last line

Colour-matrix section starts 65.858; P-frame section ends 127.958.

```
[    64.285] -> hb:17 sess=1 getdisp=1 relay=1 txn=2545 vic=vb:8a_map_cfg
[    67.420] -> hb:18 sess=1 getdisp=1 relay=1 txn=2971 vic=csc:out_off
[    70.506] -> hb:19 sess=1 getdisp=1 relay=1 txn=3335 vic=nf:vic0
[    74.307] -> hb:20 sess=1 getdisp=1 relay=1 txn=3701 vic=nf:vic
[    77.346] -> hb:21 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    80.392] -> hb:22 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    83.434] -> hb:23 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    86.478] -> hb:24 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    89.517] -> hb:25 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    92.561] -> hb:26 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    95.599] -> hb:27 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[    98.642] -> hb:28 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   101.726] -> hb:29 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   104.842] -> hb:30 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   107.910] -> hb:31 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   111.020] -> hb:32 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   114.093] -> hb:33 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   117.167] -> hb:34 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   120.206] -> hb:35 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   123.253] -> hb:36 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   126.303] -> hb:37 sess=1 getdisp=1 relay=1 txn=3719 vic=ns:vic
[   129.352] -> hb:38 sess=1 getdisp=1 relay=1 txn=3719 vic=vb:released
[   132.400] -> hb:39 sess=1 getdisp=1 relay=1 txn=3719 vic=vb:released
[   135.467] -> hb:40 sess=2 getdisp=2 relay=2 txn=3722 vic=vb:released
[   138.558] -> hb:41 sess=2 getdisp=2 relay=2 txn=3726 vic=vb:released
[   141.670] -> hb:42 sess=2 getdisp=2 relay=2 txn=3850 vic=vb:released
[   144.739] -> hb:43 sess=2 getdisp=2 relay=2 txn=4218 vic=vb:released
[   147.825] -> hb:44 sess=2 getdisp=2 relay=2 txn=4584 vic=vb:released
[   150.879] -> hb:45 sess=2 getdisp=2 relay=2 txn=4952 vic=vb:released
[   153.956] -> hb:46 sess=2 getdisp=2 relay=2 txn=5318 vic=vb:released
[   157.019] -> hb:47 sess=2 getdisp=2 relay=2 txn=5690 vic=vb:released

...

[   230.589] -> hb:71 sess=2 getdisp=2 relay=2 txn=13704 vic=vb:released
```

## `applet-mitm.last`

```
hb:71 sess=2 getdisp=2 relay=2 txn=13704 vic=vb:released
```

## Last 30 lines of `logs/m83-runI-csc.txt`

```
   V: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)

== slot_k16 (slot, shift 0) completed
   programmed rows: [65536 0 0 0]  [0 65536 0 0]  [0 0 65536 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)

== m64_bt601 (out, shift 8) completed
   programmed rows: [66 129 25 4096]  [-38 -74 112 32768]  [112 -94 -18 32768]
   Y = +0.0000*R +0.0000*G +0.0000*B +4.00   (rms 0.00, 16 used, 0 clipped)
   U = -0.0014*R -0.0026*G +0.0048*B +31.45   (rms 0.26, 16 used, 0 clipped)
   V = +0.0055*R -0.0021*G -0.0005*B +31.28   (rms 0.26, 16 used, 0 clipped)

== bt709 (out, shift 8) completed
   programmed rows: [4064 40254 11966 16896]  [-2639 -26145 28784 131584]  [28785 -22189 -6596 131584]
   Y = +0.1863*R +0.6189*G +0.0628*B +15.32   (rms 0.23, 16 used, 0 clipped)
   U = -0.1021*R -0.3410*G +0.4412*B +128.34   (rms 0.27, 16 used, 0 clipped)
   V = +0.4401*R -0.4024*G -0.0421*B +128.59   (rms 0.27, 16 used, 0 clipped)
   against the float bt709 conversion: worst 1.24 steps over the 16 patches  -> NOT within one step; the law predicts 35 of 48 values exactly

== what the probes say ==
   pass-through (no matrix): Y=Bx1.00, U=Rx1.00, V=Gx1.00
   out_k8      K=256: Y<-B gain 1.0000 = K x 2^-8.00; U<-R gain 1.0000 = K x 2^-8.00; V<-G gain 1.0000 = K x 2^-8.00
   out_k12     K=4096: Y: const 255; U: const 255; V: const 255
   out_k16     K=65536: Y: const 255; U: const 255; V: const 255
   out_k19     K=524287: Y: const 255; U: const 255; V: const 255
   out_k16_s8  K=65536: Y<-B gain 1.0000 = K x 2^-16.00; U<-R gain 1.0000 = K x 2^-16.00; V<-G gain 1.0000 = K x 2^-16.00
   slot_k16    K=65536: Y: const 255; U: const 255; V: const 255
   offsets alone: Y=0.0 for offset 256 (0.0000/unit); U=0.0 for offset 512 (0.0000/unit); V=0.0 for offset 768 (0.0000/unit)
```

## `logs/m83-runI/check.txt` (all)

```
== settings: VIC block height h=1, colour bt709 ==

== frame 0, as the VIC wrote it ==
nvframe-0-y.bin: 921600 B; roughness by layout: block-linear h=1 4.3, block-linear h=4 4.7, block-linear h=2 5.2, block-linear h=3 5.3, block-linear h=0 5.5, pitch 12.8  -> smoothest: block-linear h=1  (as configured)
   Y mean 227.3 range 16..236; U mean 128.0 range 128..128; V mean 128.0 range 128..128
   -> logs/m83-runI/nvframe-0-vic.png
nvframe-0-src.bin: the game's pixels for rows 0-127, converted to bt709 in float, against the VIC's planes:
   Y: mean error 0.98, 99th percentile 1.31 steps
   U/V against a 2x2 average: mean 0.00/0.00; against the top-left sample: mean 0.00/0.00 steps
   -> THE VIC WRITES REAL BT709 YUV

== frame 0 at QP 16 ==
status: picture_index=0x4d383300 error_status=2 ucode_error_status=0x0 88790 B pic_type=3 slices=1 avgQP=16 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 88802 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 49.1 dB, U 99.0 dB, V 99.0 dB -> NVENC ENCODED THE PICTURE THE VIC WROTE
   -> logs/m83-runI/nvframe-q16-decoded.png

== frame 0 at QP 20 ==
status: picture_index=0x4d383301 error_status=2 ucode_error_status=0x0 64906 B pic_type=3 slices=1 avgQP=20 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 64918 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 46.4 dB, U 99.0 dB, V 99.0 dB -> NVENC ENCODED THE PICTURE THE VIC WROTE
   -> logs/m83-runI/nvframe-q20-decoded.png

== frame 0 at QP 24 ==
status: picture_index=0x4d383302 error_status=2 ucode_error_status=0x0 45161 B pic_type=3 slices=1 avgQP=24 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 45173 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 43.7 dB, U 99.0 dB, V 99.0 dB -> NVENC ENCODED THE PICTURE THE VIC WROTE
   -> logs/m83-runI/nvframe-q24-decoded.png

== the last frame of the timed loop (QP 20) ==
nvframe-last-y.bin: 921600 B; roughness by layout: block-linear h=1 4.2, block-linear h=4 4.7, block-linear h=2 5.2, block-linear h=3 5.3, block-linear h=0 5.6, pitch 12.8  -> smoothest: block-linear h=1  (as configured)
   Y mean 227.1 range 16..236; U mean 128.0 range 128..128; V mean 128.0 range 128..128
   -> logs/m83-runI/nvframe-last-vic.png
status: picture_index=0x4d38337a error_status=2 ucode_error_status=0x0 66137 B pic_type=3 slices=1 avgQP=20 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 66149 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 46.4 dB, U 99.0 dB, V 99.0 dB -> NVENC ENCODED THE PICTURE THE VIC WROTE
   -> logs/m83-runI/nvframe-last-decoded.png
```

## `logs/m83-runI/nvp.txt` (all)

```
nvp-index.bin: missing
```

## `logs/m83-runI/stream-stats.txt` (all)

```
/tmp/m83-runI.sft: 1883 packets (1883 H.264, 0 other)
   frame numbers 0..1882: 0 missing in 0 gap(s)
   payload avg 64 KB, min 64 KB, max 64 KB -> 32 Mbps / 4.0 MB/s at 60 fps
   decoded 600 frame(s) from the first 600 packets, 0 error(s); 1280x720 colorspace 1 range 1
```

## `logs/m83-runI/raw-view.txt` (all)

```
waiting for frames on 1209:5f1e ep 0x81...

stream: 1280x720 H.264

30 frames  39.3 fps  31 Mbps  worst gap 31.6 ms  0 lost   
60 frames  38.0 fps  25 Mbps  worst gap 31.6 ms  0 lost   
90 frames  37.5 fps  23 Mbps  worst gap 31.6 ms  0 lost   
120 frames  37.4 fps  22 Mbps  worst gap 31.6 ms  0 lost   
150 frames  37.3 fps  22 Mbps  worst gap 31.6 ms  0 lost   
180 frames  37.2 fps  21 Mbps  worst gap 31.6 ms  0 lost   
210 frames  37.1 fps  21 Mbps  worst gap 31.6 ms  0 lost   
240 frames  37.1 fps  21 Mbps  worst gap 31.6 ms  0 lost   
270 frames  37.1 fps  21 Mbps  worst gap 31.6 ms  0 lost   
300 frames  37.1 fps  21 Mbps  worst gap 31.6 ms  0 lost   
330 frames  37.1 fps  20 Mbps  worst gap 31.6 ms  0 lost   
360 frames  37.1 fps  20 Mbps  worst gap 31.6 ms  0 lost   
390 frames  37.0 fps  20 Mbps  worst gap 31.6 ms  0 lost   
420 frames  37.0 fps  20 Mbps  worst gap 31.6 ms  0 lost   
450 frames  37.0 fps  20 Mbps  worst gap 31.6 ms  0 lost   
480 frames  37.0 fps  20 Mbps  worst gap 31.6 ms  0 lost   
510 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
540 frames  37.1 fps  20 Mbps  worst gap 38.9 ms  0 lost   
570 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
600 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
630 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
660 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
690 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
720 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
750 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
780 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
810 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
840 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
870 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
900 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
930 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
960 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
990 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1020 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1050 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1080 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1110 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1140 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1170 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1200 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1230 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1260 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1290 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1320 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1350 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1380 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1410 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1440 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1470 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1500 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1530 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1560 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1590 frames  37.0 fps  20 Mbps  worst gap 38.9 ms  0 lost   
1620 frames  36.9 fps  20 Mbps  worst gap 47.4 ms  0 lost   
1650 frames  36.7 fps  20 Mbps  worst gap 50.1 ms  0 lost   
1680 frames  36.5 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1710 frames  36.5 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1740 frames  36.5 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1770 frames  36.5 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1800 frames  36.5 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1830 frames  36.4 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1860 frames  36.3 fps  19 Mbps  worst gap 60.2 ms  0 lost   
1883 packets, 1883 frames shown/decoded, 0 not decoded, 0 lost (frame-number gaps), 124.3 MB, 36.5 fps, worst gap 60.2 ms, decode avg 0.06 ms
packets=1883 frames=1883 undecoded=0 lost=0
```

## Stream recording (not committed)

`/tmp/m83-runI.sft`: 124,393,804 B, SHA-256
`cf82a607761ef745ccecb4dd1f995fe086396125c6f556f40fa40c8fa12388c6`.
`logs/m83-runI/stream-first10.sft` (its first 10 packets, via
`sft_tool.py head`) is committed.

## FNV-1a comparisons

| file | PC-computed | log `sd(...)` | |
|---|---|---|---|
| `vic-csc.bin` | `28e83567` | `28e83567` | equal |
| `nvframe-0-src.bin` | `59a71634` | `59a71634` | equal |
| `nvframe-0-uv.bin` | `66625dc5` | `66625dc5` | equal |
| `nvframe-0-y.bin` | `b2938d1d` | `b2938d1d` | equal |
| `nvframe-last-bits.bin` | `b6dcbcc6` | `b6dcbcc6` | equal |
| `nvframe-last-status.bin` | `c70599eb` | `c70599eb` | equal |
| `nvframe-last-uv.bin` | `66625dc5` | `66625dc5` | equal |
| `nvframe-last-y.bin` | `df52da09` | `df52da09` | equal |
| `nvframe-q16-bits.bin` | `395a4897` | `395a4897` | equal |
| `nvframe-q16-status.bin` | `4784590a` | `4784590a` | equal |
| `nvframe-q20-bits.bin` | `8058032c` | `8058032c` | equal |
| `nvframe-q20-status.bin` | `9468008f` | `9468008f` | equal |
| `nvframe-q24-bits.bin` | `f0e6749b` | `f0e6749b` | equal |
| `nvframe-q24-status.bin` | `7d057174` | `7d057174` | equal |

All 14 equal.

**File list:** all 13 `nvframe-*.bin` the handoff enumerates exist
(`-0-y`, `-0-uv`, `-0-src`, `-q16/q20/q24-status/bits`,
`-last-y/uv/status/bits`), plus `vic-csc.bin`. **No `nvp-*.bin` files exist**
(none of `nvp-index`, `nvp-stream-*`, `nvp-last-y`, `nvp-last-uv`).

## PNGs written by the tools

`nvframe-0-vic.png`, `nvframe-last-vic.png`, `nvframe-q16-decoded.png`,
`nvframe-q20-decoded.png`, `nvframe-q24-decoded.png`,
`nvframe-last-decoded.png`, all in `logs/m83-runI/`, committed.
`nvp-last-decoded.png` was not written (no `nvp-*` inputs).

## Crash reports

No names in `crash_reports/` or `fatal_errors/` beyond the step-2 baseline
(141 and 4, both unchanged). No files copied.

## What the viewer window showed

The PC window opened during a Mario Kart 8 Deluxe load screen and showed that
load screen (the user's screenshot of it has the title bar
`switch-frame-tap 1280x720 37.0 fps 20 Mbps (worst gap 38.9 ms, 0 lost)`).
As soon as the window appeared, the picture froze on that load screen and
stayed frozen for the rest of the stream; it never showed racing. The window
closed on its own when the console rebooted. `raw-view` itself kept
receiving and decoding throughout: 1883 frames, 0 undecoded, 0 lost, ~37 fps
in its periodic lines.

## What the user saw on the console

- The game booted fine.
- As soon as the PC window showed (during a load screen), **the game froze on
  that load screen** at the same moment the stream picture froze.
- The console itself did not freeze: the user returned to the home menu,
  closed the game, relaunched it and raced for a while (the log's
  `sess=2 getdisp=2 relay=2` from `hb:40` at 135.467 onward, with `txn`
  climbing again, lines up with this).
- The PC picture stayed on the frozen load screen after the relaunch.

## Shutdown

Normal, via the Ultrahand/Tesla "Reboot to Hekate" shortcut. No forced
power-off was needed. The PC window closed when the console rebooted.

## Summary

The colour probes (12/12) and the real-frame encodes completed with every file
verified and `nvframe_check` reporting "NVENC ENCODED THE PICTURE THE VIC
WROTE"; the stream delivered 1883 decodable frames at ~36 fps with 0 lost, but
the game froze on its load screen when the stream began (log: game presented
0.0 fps), the PC picture stayed on that frame, and `nvp` wrote nothing
(`nvp: slot read failed`).
