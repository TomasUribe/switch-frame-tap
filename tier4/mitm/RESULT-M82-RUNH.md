# M82 Run H: result

Build `3cf0283`/`597215d` (no code changed between them - checked, empty
diff), built locally (SHA-256 `285d5bc3...`, not the prebuilt), installed
and run once on hardware. Arm file: `vic exec dbg csc nvframe wait=60`.
All three PC selftests passed before install (`nvenc_replay`, `vic_csc`,
`nvframe_check`: "selftest OK").

No `mitm.lst` was present. Deleted per step 2: `applet-mitm.log`,
`applet-mitm.last`, `grc-scan.bin`, and Run G's 15 `nvenc-*.bin` files
(`nvenc-a-{bits,rc,recon-y,status}`, `nvenc-{b,d,e}-{bits,rc,status}`,
`nvenc-c-{bits,status}`). Not present: `vic-csc.bin`, `nvjpg-dec.rgba`, any
`nvframe-*.bin`. Baseline crash-report listings: 141 in `crash_reports/`, 4 in
`fatal_errors/`.

## Boot banner and armed flags

```
[    10.471] applet-mitm M82: up (grc IPC interceptor off)
[    10.548] ARMED FLAGS: vic=1 exec=1 dbg=1 dump=0 usb=0 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=0 clk=0 jpgdec=0 nvgrc=0 csc=1 nvframe=1(120) wait=60
```

## VIC setup and regression jobs, `VIC heap` to `vb:ALL_JOBS_DONE`

```
[    61.037]    VIC heap 24 MB at 0x7295000000 (capture buffer 21120 KB on heap): cfg=0x7295000000 cmd=0x7295004000 dst=0x7295010000 src=0x7295350000
[    61.054]    capture region 21120 KB vs one 1080p frame 8100 KB -> FITS
[    61.078]    stage buf at +8640 KB; full-frame capture clear of staging
[    61.133] -> vb:1_smGetService
[    61.147]    using nvdrv:t rc=0x0
[    61.197] -> vb:2_tmem
[    61.244] -> vb:3_Initialize
[    61.261]    Initialize rc=0x0
[    61.312] -> vb:4a_discover_aruid
[    61.334]    discovered game aruid = 142 (on a throwaway fd, now closed)
[    61.347]    SetAruidWithoutCheck(142) BEFORE any real Open: rc=0x0 err=0
[    61.439] -> vb:4_open_nvmap
[    61.456]    /dev/nvmap rc=0x0 fd=23592960 nverr=0
[    61.470]    perm probe: /dev/nvhost-gpu rc=0x0 nverr=0 -> OPEN (bit0 set: full mask)
[    61.578] -> vb:4b_node_survey
[    61.596]    node /dev/nvhost-display    OPEN (rc=0x0 nverr=0)
[    61.611]    node /dev/nvdisp-ctrl       OPEN (rc=0x0 nverr=0)
[    61.630]    node /dev/nvdisp-disp0      OPEN (rc=0x0 nverr=0)
[    61.645]    node /dev/nvdisp-disp1      OPEN (rc=0x0 nverr=0)
[    61.662]    node /dev/nvdcutil-disp0    OPEN (rc=0x0 nverr=0)
[    61.679]    node /dev/nvcec-ctrl        OPEN (rc=0x0 nverr=0)
[    61.696]    node /dev/nvhost-as-gpu     OPEN (rc=0x0 nverr=0)
[    61.712]    node /dev/nvhost-ctrl-gpu   OPEN (rc=0x0 nverr=0)
[    61.729]    node /dev/nvhost-msenc      OPEN (rc=0x0 nverr=0)
[    61.750]    node /dev/nvhost-nvdec      OPEN (rc=0x0 nverr=0)
[    61.764]    node /dev/nvhost-tsec       denied (rc=0x0 nverr=196611)
[    61.780]    node /dev/nvhost-nvjpg      OPEN (rc=0x0 nverr=0)
[    61.830] -> vb:5_FROM_ID
[    61.844]    FROM_ID(1268) under aruid 142 rc=0x0 nverr=0 -> handle=1268
[    61.895] -> vb:6_alloc_bufs
[    61.911]    SetMemoryAttribute(0x7295000000, 0x4000, uncached) rc=0x0
[    61.923]    nvmapOwn ok handle=5676 id=5676 kind=0 size=0x4000
[    61.937]    SetMemoryAttribute(0x7295004000, 0x1000, uncached) rc=0x0
[    61.950]    nvmapOwn ok handle=5680 id=5680 kind=0 size=0x1000
[    61.964]    SetMemoryAttribute(0x7295010000, 0x340000, uncached) rc=0x0
[    61.979]    nvmapOwn ok handle=5684 id=5684 kind=0 size=0x340000
[    61.996]    SetMemoryAttribute(0x7295350000, 0x10000, uncached) rc=0x0
[    62.010]    nvmapOwn ok handle=5688 id=5688 kind=0 size=0x10000
[    62.062] -> vb:7_open_vic
[    62.078]    /dev/nvhost-vic rc=0x0 fd=23592961 nverr=0
[    62.096]    GET_SYNCPOINT rc=0x0 nverr=0 -> syncpt=12
[    62.148] -> vb:8_set_nvmap_fd
[    62.162]    SET_NVMAP_FD(23592960) rc=0x0 nverr=0
[    62.177]    SET_SUBMIT_TIMEOUT rc=0x0 nverr=0
[    62.223] -> vb:8a_map_cfg
[    62.236]    MAP_CMD_BUFFER(cfg handle=5676 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2680000
[    62.285] -> vb:8b_map_dst
[    62.299]    MAP_CMD_BUFFER(dst handle=5684 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x26a0000
[    62.344] -> vb:8c_map_src
[    62.357]    MAP_CMD_BUFFER(src(game) handle=1268 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x0
[    62.378]    MAP_CMD_BUFFER(src(game) handle=1268 compr=1 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x0
[    62.396]    MAP_CMD_BUFFER(src(game) handle=1268 compr=0 req=0xc0140025) rc=0x0 nverr=0 -> phys=0x0
[    62.444] -> vb:8d_map_self
[    62.461]    MAP_CMD_BUFFER(src(ours) handle=5688 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x29e0000
[    62.477]    pinned: cfg=0x2680000 dst=0x26a0000 self=0x29e0000 game_src=0x0 (+slot off 0x0)
[    62.490]    self-src pattern: row0[0..8]=ff 00 00 11 ff 04 00 11
[    62.540] -> vb:job_fill
[    62.561]    [vb:job_fill] setcl=1 words=18 cmd[0..3]=00001740 10100002 00000080 00000001
[    62.574]    [vb:job_fill] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2403
[    62.589]    [vb:job_fill] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00000000 (<<8: 0x2680000 0x26a0000 0x0)
[    62.603]    [vb:job_fill] WAIT nverr=0  syncpt=2405 (want >= 2403)  OP_DONE fired
[    62.618]    [vb:job_fill] dst sum32=11022336 changed=16384/65536  *** ENGINE WROTE OUR MEMORY ***
[    62.631]    [vb:job_fill] dst[0..32]: ff c0 80 40 ff c0 80 40 ff c0 80 40 ff c0 80 40 ff c0 80 40 ff c0 80 40 ff c0 80 40 ff c0 80 40 
[    62.644]    [vb:job_fill] row 32: ff c0 80 40 ff c0 80 40
[    62.696] -> vb:job_blit_self
[    62.711]    [vb:job_blit_self] setcl=1 words=21 cmd[0..3]=00001740 10100002 00000080 00000001
[    62.723]    [vb:job_blit_self] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2414
[    62.736]    [vb:job_blit_self] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00029e00 (<<8: 0x2680000 0x26a0000 0x29e0000)
[    62.750]    [vb:job_blit_self] WAIT nverr=0  syncpt=2416 (want >= 2414)  OP_DONE fired
[    62.765]    [vb:job_blit_self] dst sum32=10551296 changed=16384/65536  *** ENGINE WROTE OUR MEMORY ***
[    62.778]    [vb:job_blit_self] dst[0..32]: ff 00 00 11 ff 04 00 11 ff 08 00 11 ff 0c 00 11 ff 10 00 11 ff 14 00 11 ff 18 00 11 ff 1c 00 11 
[    62.790]    [vb:job_blit_self] row 32: ff 00 80 11 ff 04 80 11
[    62.810]    game blit still not possible: handle pins to phys=0
[    62.856] -> vb:game_blit_SKIPPED
[    62.899] -> vb:ALL_JOBS_DONE
```

## Colour-matrix section, `---- VIC COLOUR-MATRIX PROBES` to `csc: N of 11 probes completed`

```
[    62.961]    ---- VIC COLOUR-MATRIX PROBES (M82): 11 jobs on a 64x64 card we own ----
[    63.015] -> csc:none
[    63.029]    [csc:none] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    63.044]    [csc:none] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2433
[    63.061]    [csc:none] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    63.074]    [csc:none] WAIT nverr=0  syncpt=2435 (want >= 2433)  OP_DONE fired
[    63.106]    [csc:none] dst sum32=10908672 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    63.118]    [csc:none] dst[0..32]: 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 
[    63.131]    [csc:none] row 32: ab ab ab ab ab ab ab ab
[    63.145]    csc[none      ] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    63.186] -> csc:out_k8
[    63.199]    [csc:out_k8] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    63.212]    [csc:out_k8] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2444
[    63.247]    [csc:out_k8] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    63.285]    [csc:out_k8] WAIT nverr=0  syncpt=2450 (want >= 2444)  OP_DONE fired
[    63.323]    [csc:out_k8] dst sum32=10908672 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    63.362]    [csc:out_k8] dst[0..32]: 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 
[    63.399]    [csc:out_k8] row 32: ab ab ab ab ab ab ab ab
[    63.433]    csc[out_k8    ] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    63.517] -> csc:out_k12
[    63.553]    [csc:out_k12] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    63.596]    [csc:out_k12] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2467
[    63.631]    [csc:out_k12] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    63.676]    [csc:out_k12] WAIT nverr=0  syncpt=2475 (want >= 2467)  OP_DONE fired
[    63.702]    [csc:out_k12] dst sum32=11722752 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    63.717]    [csc:out_k12] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    63.730]    [csc:out_k12] row 32: ab ab ab ab ab ab ab ab
[    63.745]    csc[out_k12   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    63.781] -> csc:out_k16
[    63.796]    [csc:out_k16] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    63.807]    [csc:out_k16] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2482
[    63.818]    [csc:out_k16] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    63.831]    [csc:out_k16] WAIT nverr=0  syncpt=2484 (want >= 2482)  OP_DONE fired
[    63.872]    [csc:out_k16] dst sum32=11722752 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    63.889] -> hb:17 sess=1 getdisp=1 relay=1 txn=2993 vic=csc:out_k16
[    63.897] *** binder txn #3000  program=0100152000022000  session=20  code=7(queueBuffer) x1496  flags=0x0  in=192 out=4096
[    63.905]    [csc:out_k16] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    63.911]    [csc:out_k16] row 32: ab ab ab ab ab ab ab ab
[    63.918]    csc[out_k16   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    63.951] -> csc:out_k19
[    63.964]    [csc:out_k19] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    63.969]    [csc:out_k19] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2493
[    63.977]    [csc:out_k19] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    63.981]    [csc:out_k19] WAIT nverr=0  syncpt=2495 (want >= 2493)  OP_DONE fired
[    63.989]    [csc:out_k19] dst sum32=11722752 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    63.996]    [csc:out_k19] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    64.004]    [csc:out_k19] row 32: ab ab ab ab ab ab ab ab
[    64.010]    csc[out_k19   ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    64.045] -> csc:out_k16_s8
[    64.054]    [csc:out_k16_s8] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.061]    [csc:out_k16_s8] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2500
[    64.070]    [csc:out_k16_s8] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.079]    [csc:out_k16_s8] WAIT nverr=0  syncpt=2502 (want >= 2500)  OP_DONE fired
[    64.084]    [csc:out_k16_s8] dst sum32=10908672 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.096]    [csc:out_k16_s8] dst[0..32]: 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 28 
[    64.100]    [csc:out_k16_s8] row 32: ab ab ab ab ab ab ab ab
[    64.110]    csc[out_k16_s8] done  patch(200,200,200): Y=200 U=200 V=200  patch(200,40,40): Y=40 U=200 V=40
[    64.150] -> csc:out_off
[    64.163]    [csc:out_off] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.167]    [csc:out_off] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2507
[    64.177]    [csc:out_off] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.181]    [csc:out_off] WAIT nverr=0  syncpt=2509 (want >= 2507)  OP_DONE fired
[    64.189]    [csc:out_off] dst sum32=10156032 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.196]    [csc:out_off] dst[0..32]: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
[    64.203]    [csc:out_off] row 32: ab ab ab ab ab ab ab ab
[    64.210]    csc[out_off   ] done  patch(200,200,200): Y=0 U=0 V=0  patch(200,40,40): Y=0 U=0 V=0
[    64.244] -> csc:out_neg
[    64.255]    [csc:out_neg] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.266]    [csc:out_neg] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2514
[    64.273]    [csc:out_neg] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.280]    [csc:out_neg] WAIT nverr=0  syncpt=2516 (want >= 2514)  OP_DONE fired
[    64.285]    [csc:out_neg] dst sum32=11461632 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.296]    [csc:out_neg] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    64.300]    [csc:out_neg] row 32: ab ab ab ab ab ab ab ab
[    64.307]    csc[out_neg   ] done  patch(200,200,200): Y=255 U=255 V=0  patch(200,40,40): Y=255 U=255 V=0
[    64.345] -> csc:out_dense
[    64.354]    [csc:out_dense] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.361]    [csc:out_dense] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2521
[    64.370]    [csc:out_dense] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.374]    [csc:out_dense] WAIT nverr=0  syncpt=2521 (want >= 2521)  OP_DONE fired
[    64.384]    [csc:out_dense] dst sum32=11722752 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.389]    [csc:out_dense] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    64.396]    [csc:out_dense] row 32: ab ab ab ab ab ab ab ab
[    64.401]    csc[out_dense ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    64.436] -> csc:slot_k16
[    64.446]    [csc:slot_k16] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.451]    [csc:slot_k16] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2528
[    64.462]    [csc:slot_k16] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.469]    [csc:slot_k16] WAIT nverr=0  syncpt=2528 (want >= 2528)  OP_DONE fired
[    64.477]    [csc:slot_k16] dst sum32=11722752 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.485]    [csc:slot_k16] dst[0..32]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
[    64.489]    [csc:slot_k16] row 32: ab ab ab ab ab ab ab ab
[    64.496]    csc[slot_k16  ] done  patch(200,200,200): Y=255 U=255 V=255  patch(200,40,40): Y=255 U=255 V=255
[    64.531] -> csc:m64_bt601
[    64.540]    [csc:m64_bt601] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    64.545]    [csc:m64_bt601] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2533
[    64.552]    [csc:m64_bt601] resolved addrs: cfg=0x00026800 dst=0x00026a00 src=0x00026a40 (<<8: 0x2680000 0x26a0000 0x26a4000)
[    64.556]    [csc:m64_bt601] WAIT nverr=0  syncpt=2535 (want >= 2533)  OP_DONE fired
[    64.567]    [csc:m64_bt601] dst sum32=10237120 changed=6144/65536  *** ENGINE WROTE OUR MEMORY ***
[    64.571]    [csc:m64_bt601] dst[0..32]: 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 04 
[    64.584]    [csc:m64_bt601] row 32: ab ab ab ab ab ab ab ab
[    64.588]    csc[m64_bt601 ] done  patch(200,200,200): Y=4 U=32 V=32  patch(200,40,40): Y=4 U=31 V=32
[    64.619]    sd(sdmc:/vic-csc.bin): 84836 B written, read back identical, fnv1a32=fbc6487f
[    64.630]    csc: 11 of 11 probes completed; 84836 B -> sdmc:/vic-csc.bin (decode: tools/vic_csc.py)
```

## Real-frame section, `---- NVENC ON REAL GAME FRAMES` to its last `nf:` line

```
[    65.138] -> nf:1
[    65.147]    ---- NVENC ON REAL GAME FRAMES (M82): 1920x1080 slot -> VIC NV12 block-linear 1280x720 -> IDR ----
[    65.153]    nvmapOwn(0x7295360000, 0x870000) kept CACHEABLE - caller must flush before submit
[    65.162]    nvmapOwn ok handle=5696 id=5696 kind=0 size=0x870000
[    65.168]    MAP_CMD_BUFFER(nvframe-slot handle=5696 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2a20000
[    65.180]    nvmapOwn(0x7296230000, 0x4ee000) kept CACHEABLE - caller must flush before submit
[    65.185]    nvmapOwn ok handle=5700 id=5700 kind=0 size=0x4ee000
[    65.198]    MAP_CMD_BUFFER(nvframe-arena(vic) handle=5700 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x32a0000
[    65.206]    MAP_CMD_BUFFER(nvframe-arena(enc) handle=5700 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x32a0000
[    65.214]    nvframe: arena 5048 KB at +15168 KB; VIC sees it at 0x32a0000, NVENC at 0x32a0000 (syncpt 14); picture 0x34d0000/0x35b6000 (luma/chroma), bits 0x32b0000
[    65.219]    clk(nvframe): smGetService(mm:u) rc=0x0
[    65.232]    clk: mm id 5 (libnx: NVENC, nvtegra: NVDEC) req=2  get=460800000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    65.236]    clk: mm id 6 (libnx: NVDEC) req=3  get=979200000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    65.246]    clk: mm id 7 (NVJPG) req=4  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=652800000 Hz (rc=0x0)
[    65.250]    clk-ensure(nvframe) #1 re-set max     : 2 request(s), SetAndWait rc=0x0 -> clkrst NVENC=979200000 Hz
[    65.268]    nvframe: content 1280x720 (handheld) (140 samples outside the top-left 1280x720 all zero) -> VIC copies it 1:1
[    65.305] -> nf:vic0
[    65.315]    [nf:vic0] setcl=1 words=24 cmd[0..3]=00001740 10100002 00000080 00000001
[    65.319]    [nf:vic0] req=0xc0340001 sz=52 nr=0 rc=0x0 nverr=0 -> fence=2582
[    65.330]    [nf:vic0] resolved addrs: cfg=0x00026800 dst=0x00034d00 src=0x00035b60 (<<8: 0x2680000 0x34d0000 0x35b6000)
[    65.334]    [nf:vic0] WAIT nverr=0  syncpt=2582 (want >= 2582)  OP_DONE fired
[    65.347]    [nf:vic0] dst sum32=11206656 changed=0/65536  (untouched - poison 0xAB intact)
[    65.352]    [nf:vic0] dst[0..32]: ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab 
[    65.362]    [nf:vic0] row 32: ab ab ab ab ab ab ab ab
[    65.367]    nvframe[0]: slot read 6125 us, VIC -> NV12 block-linear done in 93836 us (NVENC 979200000 Hz)
[    65.381]    nvframe[0] QP 16: 385052 B in 4348 us (status: error 2 ucode 0 pic_type 3 avgQP 16 intra 3600)
[    65.432]    sd(sdmc:/nvframe-q16-status.bin): 4096 B written, read back identical, fnv1a32=9bba3301
[    65.570]    sd(sdmc:/nvframe-q16-bits.bin): 385068 B written, read back identical, fnv1a32=1137a21f
[    65.584]    nvframe[0] QP 20: 308889 B in 3623 us (status: error 2 ucode 0 pic_type 3 avgQP 20 intra 3600)
[    65.650]    sd(sdmc:/nvframe-q20-status.bin): 4096 B written, read back identical, fnv1a32=ba7e2a7e
[    65.740]    sd(sdmc:/nvframe-q20-bits.bin): 308905 B written, read back identical, fnv1a32=662d9a5a
[    65.751]    nvframe[0] QP 24: 241032 B in 3192 us (status: error 2 ucode 0 pic_type 3 avgQP 24 intra 3600)
[    65.785]    sd(sdmc:/nvframe-q24-status.bin): 4096 B written, read back identical, fnv1a32=6c358e1a
[    65.835]    sd(sdmc:/nvframe-q24-bits.bin): 241048 B written, read back identical, fnv1a32=51e68654
[    65.967]    sd(sdmc:/nvframe-0-y.bin): 942080 B written, read back identical, fnv1a32=30e411f3
[    66.024]    sd(sdmc:/nvframe-0-uv.bin): 491520 B written, read back identical, fnv1a32=c9b4e08c
[    66.962] -> hb:18 sess=1 getdisp=1 relay=1 txn=3361 vic=nf:vic
[    68.022]    nvframe: 120 of 120 frames at QP 20 in 1981 ms -> 60.5 fps (game presented 60.0 fps), 120 distinct, 0 with errors
[    68.038]    nvframe: per frame avg/max us: read+flush 7734/20799  VIC 681/5102  NVENC 4317/10443  work 12732/26150  (60 fps budget 16667); waiting for presents avg 3779
[    68.054]    nvframe: IDR size avg 344110 B (min 338035, max 347376) -> 165 Mbps at 60 fps; NVENC 979200000 Hz after
[    68.087]    sd(sdmc:/nvframe-last-status.bin): 4096 B written, read back identical, fnv1a32=106b35cd
[    68.140]    sd(sdmc:/nvframe-last-bits.bin): 345063 B written, read back identical, fnv1a32=93b40243
[    68.892] *** binder txn #3600  program=0100152000022000  session=20  code=7(queueBuffer) x1796  flags=0x0  in=192 out=4096
[    69.336]    sd(sdmc:/nvframe-last-y.bin): 942080 B written, read back identical, fnv1a32=675d0daf
[    69.414]    sd(sdmc:/nvframe-last-uv.bin): 491520 B written, read back identical, fnv1a32=f1492a49
[    69.462] -> nf:DONE
```

## Debug-capture summary, `DebugActiveProcess(pid=` to `---- VIC on real game pixels ----`

```
[    69.673]    DebugActiveProcess(pid=142) rc=0x0 ATTACHED
[    69.684]    walked 2021 regions (complete); 6 big enough (1 exactly 26542080 B); 3 probe reads
[    69.696]      region 0: base=0x1987de6000 size=26542080   <== EXACT SWAPCHAIN SIZE
[    69.705]      region 1: base=0x197606d000 size=41541632
[    69.715]      region 2: base=0x19a0422000 size=92274688
[    69.724]      region 3: base=0x19b9fe4000 size=41021440
[    69.738]      region 4: base=0x19c98f9000 size=53616640
[    69.750]      region 5: base=0x19db0dd000 size=39002112
[    69.762]    *** SWAPCHAIN AT 0x1987de6000 (offset 0 into its region) - matched by EXACT SIZE ***
[    69.776]        slot0 +0x0000000: 80 d8 eb ff  4d c3 eb ff  5d c8 eb ff  52 c4 eb ff
[    69.793]        slot1 +0x0870000: 86 db eb ff  4d c3 eb ff  4d c3 eb ff  52 c4 eb ff
[    69.803]        slot2 +0x10e0000: 78 d4 eb ff  4d c3 eb ff  64 cc eb ff  52 c4 eb ff
[    69.815]    GAME FROZEN FOR 0 ms (M45 was 4780 ms - only the strip read needs a halt)
[    69.836]    drained 51 debug events; ContinueDebugEvent rc=0x0 -> GAME RUNNING WHILE WE STAY ATTACHED
[    69.848]    live sample over 120 ms: identical (game may be paused, or the corner is static)
[    69.863]        t0: 4d 5a 61 ff 4d 5a 61 ff
[    69.876]        t1: 4d 5a 61 ff 4d 5a 61 ff
[    69.906]    read 983040 B of block-row in 547 us -> 1795 MB/s
[    69.955]    => one full 8,847,360 B slot ~4927 us; 60 fps needs <= 16667 us  [FEASIBLE]
[    69.973]    block-row: nonzero=655360/983040  distinct=204  pixels=245760
[    70.044]    0xFF per lane: [0]=0 [1]=0 [2]=0 [3]=163777  (the alpha lane is the big one)
[    70.060] -> hb:19 sess=1 getdisp=1 relay=1 txn=3665 vic=dbg:2_attached
[    70.086]    first 16 B: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[    70.098]    FULL SLOT into RAM: 9/9 strips, 5960 us total, 1484 MB/s -> *** FITS IN A 60 fps FRAME BUDGET ***
[    70.110]    ---- VIC on real game pixels ----
```

## `hb:` lines, 5 s before the colour-matrix section to 30 s after the real-frame section, plus the last line

Colour-matrix section starts 62.961; real-frame section ends 69.462.

```
[    60.821] -> hb:16 sess=1 getdisp=1 relay=1 txn=2577 vic=vb:0_heap
[    63.889] -> hb:17 sess=1 getdisp=1 relay=1 txn=2993 vic=csc:out_k16
[    66.962] -> hb:18 sess=1 getdisp=1 relay=1 txn=3361 vic=nf:vic
[    70.060] -> hb:19 sess=1 getdisp=1 relay=1 txn=3665 vic=dbg:2_attached
[    73.139] -> hb:20 sess=1 getdisp=1 relay=1 txn=4027 vic=vb:released
[    76.268] -> hb:21 sess=1 getdisp=1 relay=1 txn=4398 vic=vb:released
[    79.345] -> hb:22 sess=1 getdisp=1 relay=1 txn=4761 vic=vb:released
[    82.396] -> hb:23 sess=1 getdisp=1 relay=1 txn=5129 vic=vb:released
[    85.445] -> hb:24 sess=1 getdisp=1 relay=1 txn=5495 vic=vb:released
[    88.504] -> hb:25 sess=1 getdisp=1 relay=1 txn=5861 vic=vb:released
[    91.546] -> hb:26 sess=1 getdisp=1 relay=1 txn=6227 vic=vb:released
[    94.595] -> hb:27 sess=1 getdisp=1 relay=1 txn=6593 vic=vb:released
[    97.644] -> hb:28 sess=1 getdisp=1 relay=1 txn=6957 vic=vb:released

...

[   250.299] -> hb:78 sess=1 getdisp=1 relay=1 txn=24035 vic=vb:released
```

`txn` climbs with no gap from `hb:1` through the last line - full log checked.

## `applet-mitm.last`

```
hb:78 sess=1 getdisp=1 relay=1 txn=24035 vic=vb:released
```

## `logs/m82-runH-csc.txt` (all)

```
vic-csc v1: 64x64 card, source fmt 32, output fmt 67, 11 probes

== none (none, shift 0) completed
   Y = +0.0000*R +0.0000*G +1.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   U = +1.0000*R +0.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   V = +0.0000*R +1.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)

== out_k8 (out, shift 0) completed
   programmed rows: [256 0 0 0]  [0 256 0 0]  [0 0 256 0]
   Y = +0.0000*R +0.0000*G +1.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   U = +1.0000*R +0.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   V = +0.0000*R +1.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)

== out_k12 (out, shift 0) completed
   programmed rows: [4096 0 0 0]  [0 4096 0 0]  [0 0 4096 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)

== out_k16 (out, shift 0) completed
   programmed rows: [65536 0 0 0]  [0 65536 0 0]  [0 0 65536 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)

== out_k19 (out, shift 0) completed
   programmed rows: [524287 0 0 0]  [0 524287 0 0]  [0 0 524287 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)

== out_k16_s8 (out, shift 8) completed
   programmed rows: [65536 0 0 0]  [0 65536 0 0]  [0 0 65536 0]
   Y = +0.0000*R +0.0000*G +1.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   U = +1.0000*R +0.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)
   V = +0.0000*R +1.0000*G +0.0000*B +0.00   (rms 0.00, 16 used, 0 clipped)

== out_off (out, shift 0) completed
   programmed rows: [0 0 0 256]  [0 0 0 512]  [0 0 0 768]
   Y: 0 usable of 16 (16 clipped), range 0..0 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 0..0 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 0..0 - no fit (constant)

== out_neg (out, shift 0) completed
   programmed rows: [65536 0 0 0]  [0 -65536 0 768]  [0 0 65536 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   V: 0 usable of 16 (16 clipped), range 0..0 - no fit (constant)

== out_dense (out, shift 0) completed
   programmed rows: [2048 4096 6144 0]  [8192 10240 12288 0]  [14336 16384 18432 0]
   Y: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
   U: 0 usable of 16 (16 clipped), range 255..255 - no fit (constant)
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

## `logs/m82-runH/check.txt` (all)

```
== frame 0, as the VIC wrote it ==
nvframe-0-y.bin: 942080 B; roughness by layout: block-linear h=2 10.2, block-linear h=4 12.5, block-linear h=3 12.7, block-linear h=1 14.2, block-linear h=0 18.3, pitch 64.0  -> smoothest: block-linear h=2  (as configured)
   luma mean 177.4 range 0..255; U(=R) mean 135.0, V(=G) mean 165.1
   -> logs/m82-runH/nvframe-0-vic.png

== frame 0 at QP 16 ==
status: picture_index=0x4d383200 error_status=2 ucode_error_status=0x0 385052 B pic_type=3 slices=1 avgQP=16 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 385064 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 9.5 dB, U 9.9 dB, V 11.7 dB -> *** decoded picture does not match the VIC's ***
   -> logs/m82-runH/nvframe-q16-decoded.png

== frame 0 at QP 20 ==
status: picture_index=0x4d383201 error_status=2 ucode_error_status=0x0 308889 B pic_type=3 slices=1 avgQP=20 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 308901 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 9.5 dB, U 9.9 dB, V 11.7 dB -> *** decoded picture does not match the VIC's ***
   -> logs/m82-runH/nvframe-q20-decoded.png

== frame 0 at QP 24 ==
status: picture_index=0x4d383202 error_status=2 ucode_error_status=0x0 241032 B pic_type=3 slices=1 avgQP=24 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 241044 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 9.5 dB, U 9.9 dB, V 11.7 dB -> *** decoded picture does not match the VIC's ***
   -> logs/m82-runH/nvframe-q24-decoded.png

== the last frame of the timed loop (QP 20) ==
nvframe-last-y.bin: 942080 B; roughness by layout: block-linear h=2 13.9, block-linear h=4 16.9, block-linear h=3 17.3, block-linear h=1 19.1, block-linear h=0 23.5, pitch 79.4  -> smoothest: block-linear h=2  (as configured)
   luma mean 163.1 range 0..255; U(=R) mean 137.7, V(=G) mean 162.7
   -> logs/m82-runH/nvframe-last-vic.png
status: picture_index=0x4d38327a error_status=2 ucode_error_status=0x0 345047 B pic_type=3 slices=1 avgQP=20 intra/inter=3600/0
  NAL @ 0x00000: type  5 (IDR slice) ref_idc 3 345059 B  first_mb=0 slice_type=2 pps_id=0
decoded 1280x720; PSNR against the VIC's planes: Y 7.9 dB, U 9.3 dB, V 10.1 dB -> *** decoded picture does not match the VIC's ***
   -> logs/m82-runH/nvframe-last-decoded.png
```

## FNV-1a comparisons

| file | PC-computed | log `sd(...)` | |
|---|---|---|---|
| `vic-csc.bin` | `fbc6487f` | `fbc6487f` | equal |
| `nvframe-q16-status.bin` | `9bba3301` | `9bba3301` | equal |
| `nvframe-q16-bits.bin` | `1137a21f` | `1137a21f` | equal |
| `nvframe-q20-status.bin` | `ba7e2a7e` | `ba7e2a7e` | equal |
| `nvframe-q20-bits.bin` | `662d9a5a` | `662d9a5a` | equal |
| `nvframe-q24-status.bin` | `6c358e1a` | `6c358e1a` | equal |
| `nvframe-q24-bits.bin` | `51e68654` | `51e68654` | equal |
| `nvframe-0-y.bin` | `30e411f3` | `30e411f3` | equal |
| `nvframe-0-uv.bin` | `c9b4e08c` | `c9b4e08c` | equal |
| `nvframe-last-status.bin` | `106b35cd` | `106b35cd` | equal |
| `nvframe-last-bits.bin` | `93b40243` | `93b40243` | equal |
| `nvframe-last-y.bin` | `675d0daf` | `675d0daf` | equal |
| `nvframe-last-uv.bin` | `f1492a49` | `f1492a49` | equal |

All 13 equal.

**File list:** all 12 `nvframe-*.bin` files the handoff enumerates exist
(`nvframe-0-{y,uv}`, `nvframe-{q16,q20,q24}-{status,bits}`,
`nvframe-last-{y,uv,status,bits}`), plus `vic-csc.bin`.

## PNGs written by the tools

`nvframe-0-vic.png`, `nvframe-last-vic.png`, `nvframe-q16-decoded.png`,
`nvframe-q20-decoded.png`, `nvframe-q24-decoded.png`,
`nvframe-last-decoded.png` - all in `logs/m82-runH/`, committed.

## Crash reports

No names in `crash_reports/` or `fatal_errors/` beyond the step-2 baseline
(141 and 4, both unchanged). No files copied.

## Handheld or docked / what was seen / shutdown

- **Handheld** the whole run (user-confirmed; the module's own log line
  `nvframe: content 1280x720 (handheld)` agrees).
- On screen: the user noticed no stutters and nothing else unusual at any
  point, including the 60-65 s window.
- Shutdown: normal, via the Ultrahand/Tesla "Reboot to Hekate" shortcut. No
  forced power-off was needed.

## Summary

All 11 colour-matrix probes and all 120 real-frame encodes completed (0 with
errors), every output file verifies byte-for-byte, and `nvframe_check`
reports the decoded pictures do not match the VIC planes it compares them to.
