# M78 Run D: result

Build `feac866`/`a85f5f4` (no code changed between them - checked, empty
diff), installed and run once on hardware. Arm file:
`vic clk jpgdec grcscan wait=60`.

No `mitm.lst` was present. No old result files were present before install.
Baseline crash-report listings recorded before install: 139 names in
`crash_reports/`, 4 in `fatal_errors/`.

## Boot banner and armed flags

```
[    10.320] applet-mitm M78: up (grc IPC interceptor off)
[    10.371] ARMED FLAGS: vic=1 exec=0 dbg=0 dump=0 usb=0 bench=0 nvenc=0 jpg=0 sweep=0 mtx=0 stream=0 grc=0 grcscan=1 clk=1 jpgdec=1 wait=60
```

## Every `clk[...]`, `clk-watch`, `clk-ensure(...)` line

```
[    50.227]    clk[before      ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=0.0 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
[    50.734]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=0.0 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
[    51.249]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=0.0 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
[    51.760]    clk[before+     ]  VIC=422.4 MHz  NVENC=460.8 MHz  NVJPG=0.0 MHz  NVDEC=0.0 MHz  HOST1X=81.6 MHz
[    51.798]    clk: mm id 5 (libnx: NVENC, nvtegra: NVDEC) req=1  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    51.802]    clk: mm id 6 (libnx: NVDEC) req=2  get=979200000 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=979200000 Hz (rc=0x0)
[    51.810]    clk: mm id 7 (NVJPG) req=3  get=0 Hz (rc=0x0)  SetAndWait(max) rc=0x0  -> get=652800000 Hz (rc=0x0)
[    51.915]    clk[held        ]  VIC=652.8 MHz  NVENC=979.2 MHz  NVJPG=652.8 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    53.926]    clk[held+2s     ]  VIC=652.8 MHz  NVENC=979.2 MHz  NVJPG=652.8 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    54.040]    clk-watch: sampling NVJPG/VIC/NVDEC every 200 ms, logging changes only
[    54.063]    clk-watch  NVJPG=652.8  VIC=652.8  NVDEC=979.2 MHz
[    60.729]    clk-watch  NVJPG=0.0  VIC=422.4  NVDEC=979.2 MHz
[    61.281]    clk[jpgdec-pre  ]  VIC=422.4 MHz  NVENC=979.2 MHz  NVJPG=0.0 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    61.328]    clk-ensure(jpgdec) #1 re-set max     : 1 request(s), SetAndWait rc=0x0 -> clkrst NVJPG=422400000 Hz
[    61.352]    clk-watch  NVJPG=422.4  VIC=422.4  NVDEC=979.2 MHz
[    61.751]    clk-watch  NVJPG=0.0  VIC=422.4  NVDEC=979.2 MHz
[    62.546]    clk-watch: stopped (engine probe finished)
```

## NVJPG decode section, `---- NVJPG DECODE POSITIVE CONTROL` to its result

```
[    61.269] -> jd:1
[    61.275]    ---- NVJPG DECODE POSITIVE CONTROL (M77: clock ensured after open, read at submit) ----
[    61.281]    clk[jpgdec-pre  ]  VIC=422.4 MHz  NVENC=979.2 MHz  NVJPG=0.0 MHz  NVDEC=979.2 MHz  HOST1X=81.6 MHz
[    61.286]    MAP_CMD_BUFFER(jpgdec-buf handle=5720 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x2680000
[    61.292]    MAP_CMD_BUFFER(jpgdec-cmd handle=5716 compr=0 req=0xc0140009) rc=0x0 nverr=0 -> phys=0x29c0000
[    61.296]    jpgdec: syncpt=17 pic=0x2680000 stat=0x2681000 scan=0x2682000 (294 B) out=0x2690000, 21 words
[    61.319] -> jd:clock
[    61.328]    clk-ensure(jpgdec) #1 re-set max     : 1 request(s), SetAndWait rc=0x0 -> clkrst NVJPG=422400000 Hz
[    61.360] -> jd:submit
[    61.385] -> jd:submitted
[    61.395]    jpgdec: submit rc=0x0 nverr=0 fence 1/1 REACHED after 150 us | NVJPG 422400000 Hz read 162 us before submit returned, 422400000 Hz after | status used=292 mcu=0x0 result=0 | 16384/16384 output bytes written
[    61.401]    jpgdec: px(0,0)=dc 1e 1e ff  px(63,0)=1d c8 3c ff  px(0,63)=28 3c db ff
[    61.412]    jpgdec: worst 8x8 block-mean deviation 0 (block 0)  ->  *** NVJPG RAN AND DECODED CORRECTLY - this process can drive a non-VIC engine ***
[    61.431]    sd(sdmc:/nvjpg-dec.rgba): 16384 B written, read back identical, fnv1a32=3c391345
[    61.759] -> jd:MATCH
```

## grc observer section, `---- GRC OBSERVER` to `grcscan:done`

```
[    61.789] -> grcscan:1
[    61.799]    ---- GRC OBSERVER (read-only; no channel, no submit) ----
[    61.810]    pm:dmnt GetProcessId(0100000000000035) rc=0x0 -> pid=138
[    61.822]    DebugActiveProcess rc=0x0
[    61.832]    drained 15 events; ContinueDebugEvent rc=0x0 -> grc RESUMED
[    62.533]    scanned 98356 KB across 52 regions; 5 hit(s)  (hit the scan cap)
[    62.557]    hit  0 @ 0x55c78f81cc  NVENC magic 5.0             d0b70006 006e0100 00000001 00000000
[    62.590]    hit  1 @ 0x55c7936570  NVENC magic 5.0             d0b70006 006e0100 00000001 00000000
[    62.609]    hit  2 @ 0x55c7959000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    62.617]    hit  3 @ 0x55c795c000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    62.624]    hit  4 @ 0x55c795f000  NVENC magic 5.0             d0b70006 02cf04ff 05000500 00000000
[    62.630]    dumped 5 hit(s) into 21182 B of records -> sdmc:/grc-scan.bin (decode: tools/nvrec.py)
[    62.678]    sd(sdmc:/grc-scan.bin): 21182 B written, read back identical, fnv1a32=60525240
[    62.708] -> grcscan:done
```

## Every line starting with `sd(`

```
[    61.431]    sd(sdmc:/nvjpg-dec.rgba): 16384 B written, read back identical, fnv1a32=3c391345
[    62.678]    sd(sdmc:/grc-scan.bin): 21182 B written, read back identical, fnv1a32=60525240
```

## `hb:` lines, 5 s before the decode section to 30 s after the observer section, plus the last line

Decode section starts at 61.269 (5 s before = 56.269); observer section ends
at 62.708 (30 s after = 92.708).

```
[    57.082] -> hb:15 sess=1 getdisp=1 relay=1 txn=1933 vic=waiting
[    60.106] -> hb:16 sess=1 getdisp=1 relay=1 txn=2295 vic=waiting
[    63.133] -> hb:17 sess=1 getdisp=1 relay=1 txn=2657 vic=vb:released
[    66.174] -> hb:18 sess=1 getdisp=1 relay=1 txn=2999 vic=vb:released
[    69.252] -> hb:19 sess=1 getdisp=1 relay=1 txn=3361 vic=vb:released
[    72.307] -> hb:20 sess=1 getdisp=1 relay=1 txn=3730 vic=vb:released
[    75.349] -> hb:21 sess=1 getdisp=1 relay=1 txn=4083 vic=vb:released
[    78.377] -> hb:22 sess=1 getdisp=1 relay=1 txn=4447 vic=vb:released
[    81.400] -> hb:23 sess=1 getdisp=1 relay=1 txn=4809 vic=vb:released
[    84.446] -> hb:24 sess=1 getdisp=1 relay=1 txn=5173 vic=vb:released
[    87.509] -> hb:25 sess=1 getdisp=1 relay=1 txn=5537 vic=vb:released
[    90.556] -> hb:26 sess=1 getdisp=1 relay=1 txn=5905 vic=vb:released
[    93.602] -> hb:27 sess=1 getdisp=1 relay=1 txn=6271 vic=vb:released

...

[   233.301] -> hb:73 sess=1 getdisp=1 relay=1 txn=21861 vic=vb:released
```

`txn` climbs steadily with no gap from `hb:1` through the last line - full log
checked, not just this excerpt.

## `applet-mitm.last`

```
hb:73 sess=1 getdisp=1 relay=1 txn=21861 vic=vb:released
```

## `check` tool output and both FNV-1a comparisons

```
$ python3 tools/nvjpg_dec_control.py check logs/m78-runD-nvjpg-dec.rgba
logs/m78-runD-nvjpg-dec.rgba: 16384 B, fnv1a32=3c391345
max per-pixel channel diff 0, mean 0.00  -> logs/m78-runD-nvjpg-dec.png
MATCH
```

`nvjpg-dec.rgba`: check tool `fnv1a32=3c391345` vs log's `sd(...)`
`fnv1a32=3c391345` - **equal**.

```
$ python3 -c "...fnv1a32..." logs/m78-runD-grc-scan.bin
60525240
```

`grc-scan.bin`: computed `60525240` vs log's `sd(...)` `fnv1a32=60525240` -
**equal**.

## `nvrec.py` output on `grc-scan.bin` (all 17 lines - shorter than 150)

```
[    61.983] NOTE   hit 0 kind 1 @ 0x55c78f81cc in region 0x55c78ce000+0x85000 state 0xd perm 0x3
[    61.983] SETUP  iova=0x0 (grc va low 0xc78f81cc) 4096 B  magic=0xd0b70006
          -> logs/m78-runD-setups/setup_0.bin
[    61.984] NOTE   hit 1 kind 1 @ 0x55c7936570 in region 0x55c78ce000+0x85000 state 0xd perm 0x3
[    61.984] SETUP  iova=0x0 (grc va low 0xc7936570) 4096 B  magic=0xd0b70006
          -> logs/m78-runD-setups/setup_1.bin
[    61.985] NOTE   hit 2 kind 1 @ 0x55c7959000 in region 0x55c7959000+0x3000 state 0xd perm 0x3
[    61.985] SETUP  iova=0x0 (grc va low 0xc7959000) 4096 B  magic=0xd0b70006
          -> logs/m78-runD-setups/setup_2.bin
[    61.985] NOTE   hit 3 kind 1 @ 0x55c795c000 in region 0x55c795c000+0x3000 state 0xd perm 0x3
[    61.985] SETUP  iova=0x0 (grc va low 0xc795c000) 4096 B  magic=0xd0b70006
          -> logs/m78-runD-setups/setup_3.bin
[    61.985] NOTE   hit 4 kind 1 @ 0x55c795f000 in region 0x55c795f000+0x3000 state 0xd perm 0x3
[    61.985] SETUP  iova=0x0 (grc va low 0xc795f000) 4096 B  magic=0xd0b70006
          -> logs/m78-runD-setups/setup_4.bin

21182 of 21182 bytes decoded, 5 setup blob(s)
```

## First 60 lines of `setup_0.txt`, `setup_1.txt`, `setup_2.txt` (5 blobs total, first three shown)

`setup_0.txt`:
```
== logs/m78-runD-setups/setup_0.bin (4096 B) ==
  magic                                        0xd0b70006  (5.0=0xd0b70006 6.0=0xc1b70006)
 input_cfg  : 17123x9444 pitch=0 pitch_c=0 trans=23502  luma@0/0x24ed7028 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 refpic_cfg : 257x111 pitch=1 pitch_c=0 trans=0  luma@0/0x24d91934 chroma@0/0x24de2df2  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 output_cfg : 1x1 pitch=0 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 half_scaled: 5x1 pitch=0 pitch_c=0 trans=60338  luma@0/0x5 chroma@0/0x24d400fc  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 sps:
  s->sps_data.profile_idc                      0
  s->sps_data.level_idc                        0
  s->sps_data.chroma_format_idc                0
  s->sps_data.pic_order_cnt_type               0
  s->sps_data.log2_max_frame_num_minus4        0
  s->sps_data.log2_max_pic_order_cnt_lsb_minus4 0
  s->sps_data.frame_mbs_only                   0
 pps:
  s->pps_data.pic_param_set_id                 0
  s->pps_data.entropy_coding_mode_flag         0
  s->pps_data.num_ref_idx_l0_active_minus1     0
  s->pps_data.pic_init_qp_minus26              0
  s->pps_data.chroma_qp_index_offset           0
  s->pps_data.deblocking_filter_control_present_flag 0
  s->pps_data.transform_8x8_mode_flag          0
  s->pps_data.constrained_intra_pred_flag      0
 rate_control:
  rc->hrd_type                                 0
  QP[P,B,I]=0,0,0  minQP=0,0,0  maxQP=0,0,0  maxQPD=0 baseQPD=0
  rhopbi=0,0,0
  rc->framerate                                0
  rc->buffersize                               0
  rc->nal_cpb_size                             0
  rc->nal_bitrate                              5
  rc->vcl_cpb_size                             5
  rc->vcl_bitrate                              5
  rc->gop_length                               0
  rc->Np                                       0
  rc->Bmin                                     0
  rc->Ravg                                     0
  rc->R                                        16777216
  rc->ab_alpha                                 0
  rc->ab_beta                                  0
  rc->aqMode                                   4
  rc->single_frame_VBV                         0
  rc->two_pass_rc                              0
  rc->rc_class                                 0
 pic_control:
  pc->pic_struct                               0
  pc->pic_type                                 0
  pc->ref_pic_flag                             0
  pc->slice_mode                               0
  pc->codec                                    0
  pc->frame_num                                0
  pc->pic_order_cnt_lsb                        0
  pc->idr_pic_id                               0
  pc->max_slice_size                           0
  pc->max_byte_count_before_resid_zero         0
  pc->num_forced_slices_minus1                 0
  pc->num_me_controls_minus1                   0
  pc->num_md_controls_minus1                   0
  pc->num_q_controls_minus1                    0
  pc->slice_control_offset                     0
```

`setup_1.txt`:
```
== logs/m78-runD-setups/setup_1.bin (4096 B) ==
  magic                                        0xd0b70006  (5.0=0xd0b70006 6.0=0xc1b70006)
 input_cfg  : 1x1 pitch=0 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 refpic_cfg : 257x111 pitch=1 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 output_cfg : 1x1 pitch=0 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 half_scaled: 1x1 pitch=0 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 sps:
  s->sps_data.profile_idc                      0
  s->sps_data.level_idc                        0
  s->sps_data.chroma_format_idc                0
  s->sps_data.pic_order_cnt_type               0
  s->sps_data.log2_max_frame_num_minus4        0
  s->sps_data.log2_max_pic_order_cnt_lsb_minus4 0
  s->sps_data.frame_mbs_only                   0
 pps:
  s->pps_data.pic_param_set_id                 0
  s->pps_data.entropy_coding_mode_flag         0
  s->pps_data.num_ref_idx_l0_active_minus1     0
  s->pps_data.pic_init_qp_minus26              0
  s->pps_data.chroma_qp_index_offset           0
  s->pps_data.deblocking_filter_control_present_flag 0
  s->pps_data.transform_8x8_mode_flag          0
  s->pps_data.constrained_intra_pred_flag      0
 rate_control:
  rc->hrd_type                                 0
  QP[P,B,I]=0,0,0  minQP=0,0,0  maxQP=0,0,0  maxQPD=0 baseQPD=0
  rhopbi=0,0,0
  rc->framerate                                0
  rc->buffersize                               0
  rc->nal_cpb_size                             0
  rc->nal_bitrate                              0
  rc->vcl_cpb_size                             0
  rc->vcl_bitrate                              0
  rc->gop_length                               0
  rc->Np                                       0
  rc->Bmin                                     0
  rc->Ravg                                     0
  rc->R                                        0
  rc->ab_alpha                                 0
  rc->ab_beta                                  0
  rc->aqMode                                   0
  rc->single_frame_VBV                         0
  rc->two_pass_rc                              0
  rc->rc_class                                 0
 pic_control:
  pc->pic_struct                               2
  pc->pic_type                                 3
  pc->ref_pic_flag                             1
  pc->slice_mode                               1
  pc->codec                                    7
  pc->frame_num                                0
  pc->pic_order_cnt_lsb                        0
  pc->idr_pic_id                               0
  pc->max_slice_size                           0
  pc->max_byte_count_before_resid_zero         0
  pc->num_forced_slices_minus1                 0
  pc->num_me_controls_minus1                   0
  pc->num_md_controls_minus1                   0
  pc->num_q_controls_minus1                    0
  pc->slice_control_offset                     0
```

`setup_2.txt`:
```
== logs/m78-runD-setups/setup_2.bin (4096 B) ==
  magic                                        0xd0b70006  (5.0=0xd0b70006 6.0=0xc1b70006)
 input_cfg  : 1280x720 pitch=1280 pitch_c=1280 trans=0  luma@0/0 chroma@0/0  block_height=2 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 refpic_cfg : 1280x720 pitch=1280 pitch_c=1280 trans=0  luma@0/0 chroma@0xe10/0  block_height=0 tiled16=1 mem_mode=0 nv21=0 bl_mode=0
 output_cfg : 1280x720 pitch=1280 pitch_c=1280 trans=0  luma@0/0 chroma@0xe10/0  block_height=0 tiled16=1 mem_mode=0 nv21=0 bl_mode=0
 half_scaled: 1x1 pitch=0 pitch_c=0 trans=0  luma@0/0 chroma@0/0  block_height=0 tiled16=0 mem_mode=0 nv21=0 bl_mode=0
 sps:
  s->sps_data.profile_idc                      100
  s->sps_data.level_idc                        32
  s->sps_data.chroma_format_idc                1
  s->sps_data.pic_order_cnt_type               2
  s->sps_data.log2_max_frame_num_minus4        4
  s->sps_data.log2_max_pic_order_cnt_lsb_minus4 4
  s->sps_data.frame_mbs_only                   1
 pps:
  s->pps_data.pic_param_set_id                 0
  s->pps_data.entropy_coding_mode_flag         1
  s->pps_data.num_ref_idx_l0_active_minus1     0
  s->pps_data.pic_init_qp_minus26              0
  s->pps_data.chroma_qp_index_offset           0
  s->pps_data.deblocking_filter_control_present_flag 1
  s->pps_data.transform_8x8_mode_flag          1
  s->pps_data.constrained_intra_pred_flag      0
 rate_control:
  rc->hrd_type                                 1
  QP[P,B,I]=24,28,24  minQP=0,0,0  maxQP=51,51,51  maxQPD=6 baseQPD=3
  rhopbi=0,0,0
  rc->framerate                                7680
  rc->buffersize                               5000000
  rc->nal_cpb_size                             0
  rc->nal_bitrate                              0
  rc->vcl_cpb_size                             5000000
  rc->vcl_bitrate                              5000000
  rc->gop_length                               0xf
  rc->Np                                       14
  rc->Bmin                                     277
  rc->Ravg                                     0
  rc->R                                        46
  rc->ab_alpha                                 0
  rc->ab_beta                                  0
  rc->aqMode                                   0
  rc->single_frame_VBV                         0
  rc->two_pass_rc                              1
  rc->rc_class                                 0
 pic_control:
  pc->pic_struct                               0
  pc->pic_type                                 0
  pc->ref_pic_flag                             1
  pc->slice_mode                               0
  pc->codec                                    0
  pc->frame_num                                1
  pc->pic_order_cnt_lsb                        2
  pc->idr_pic_id                               0
  pc->max_slice_size                           3600
  pc->max_byte_count_before_resid_zero         0
  pc->num_forced_slices_minus1                 0
  pc->num_me_controls_minus1                   0
  pc->num_md_controls_minus1                   0
  pc->num_q_controls_minus1                    0
  pc->slice_control_offset                     0x300
```

## Crash reports

No names in `crash_reports/` or `fatal_errors/` beyond the step-2 baseline
(139 and 4 respectively, both unchanged). No files copied.

## What was seen on screen / capture / shutdown

Game ran with no visual artifacts, no stutter, no slowdown. The optional
video capture was taken at about the 2-minute mark and the console confirmed
it saved; the user reports the clip "looks fine" in every video player tried
except the native Ubuntu player, where "it doesn't look quite right." No
forced power-off was needed. Shutdown method for this run not separately
specified by the user beyond the above.

## Summary

Both probes verified clean this run: NVJPG decode matches byte-for-byte
(fnv1a32 equal, PC check MATCH, diff 0) and grc-scan.bin's FNV-1a matches the
console's own - fixing Run C's silent-write bug. The grc observer found 5
hits carrying NVENC magic 5.0 in grc's live memory, including one
(`setup_2.bin`) with populated, plausible-looking H.264 SPS/PPS/rate-control
fields at 1280x720.
