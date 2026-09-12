/*
 * nvenc_drv_h264.h - NVENC H.264 driver structures for Tegra X1.
 *
 * VERBATIM EXTRACT (lines 1-836) of classes/video/nvenc_drv.h from
 * https://github.com/NVIDIA/open-gpu-doc, which NVIDIA publishes under MIT.
 * Its licence notice is preserved below. Only the H.264 half is taken; the
 * VP8/H.265/VP9/AV1 sections are dropped.
 *
 * Vendored rather than transcribed on purpose: nvenc_h264_drv_pic_setup_s is
 * 512 bytes of bitfields with five sub-structures at offsets, and a hand-copy
 * error would show up as a firmware rejection with no indication of which field
 * was wrong.
 *
 * NV_NVENC_5_0 selects the generation. Tegra X1 is GM20B (Maxwell 2nd gen) and
 * its L4T firmware is nvhost_nvenc050.fw, so 5.0 is the expected match - magic
 * 0xd0b70006. If the firmware rejects it with a BadMagic error the version is
 * simply wrong, and 6.0 (0xc1b70006) is the next candidate; the firmware
 * validates the magic and errors rather than hanging, so probing is safe.
 */
#pragma once

#ifndef NV_NVENC_5_0
#define NV_NVENC_5_0 1
#endif

/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef NVENC_DRV_H
#define NVENC_DRV_H
//
// CLASS NV_C9B7_NVENC
// ===================
//
//
// 1  -  INTRODUCTION
//
// The NVENC is a video encoder engine. Currently only H.264 baseline, main
// and high profile are supported. For baseline, only 1 slicegroup is allowed.
// NVENC has built-in motion estimation that enables it to do encode on its own
// but also has the ability to work with external hints that could be produced
// by another motion search implementation such as CEA.<br>
// NVENC motion search is divided into a simple integer search module and a high
// perf subpel refinement module. The integer search capability of NVENC is
// limited (perf wise) so for higher perf the CEA approach should be used.
// Motion search/estimation (ME) results in a set of vectors with scores
// which it will evaluate for encoding cost and eligibility. It also searches
// intra encoding modes for the lowest cost encoding, picks a final winner
// from all inter and intra modes that have been evaluated and encodes the
// macroblock according to that mode. Skip and direct modes are also checked.
// Search and encoding involve forward dct and quantization, inverse dct and
// quantization, reconstruction, and optionally deblocking. Entropy encode can
// do either CAVLC or CABAC.
//
//
// 1.4. CLASS OPERATION
// ------------------------
//
// *** TBD ***
//
// If MVC is enabled, HW need to write the prefix NAL unit(14) when current view is a base view.
// As SVC is not supported in nvenc, so svc_extension_flag should be always false. Then, in the prefix NAL unit,
// only the syntax elements(H.7.3.1.1 NAL unit header MVC extension syntax) in the prefix NAL unit should be written:
// nal_unit_header_mvc_extension( ) {
//  non_idr_flag
//  priority_id
//  view_id
//  temporal_id
//  anchor_pic_flag
//  inter_view_flag
//  reserved_one_bit
// }
//
// 1.5. RESTRICTIONS
// ------------------------
//
// (1) Reference pictures to be accessed by NVENC must be in tiled 16x16 format
// (2) Input pictures are either all in BL format or all in tiled 16x16 format
// (3) Input pictures and reference pictures must be of the kind specified by
//     'pic_struct' (either fields or frames). NVENC cannot encode an interlaced
//     input picture (2 fields) as a frame or vice versa, and cannot use frame
//     reference pictures when encoding a field picture, or field reference
//     pictures when encoding a frame.
//
//
//
// 2  -  APPLICATION MEMORY STRUCTURES
//
// 2.1. DRV PICTURE SETUP BUFFER
// -----------------------------
//
// DrvPictureSetupBuffer contains sequence, picture, and slice level control
// data required for encoding.
//
// Input pictures (to be encoded) can be any size (HxV) but the encoded output
// is always an integer number of macroblocks high and wide, which means that
// H and V are rounded up to the next multiple of 16 (H'xV') and all pixels
// within this range are used to encode the macroblocks. NVENC does not clear,
// pad, duplicate or otherwise modify any pixels values within the H'xV' sized
// input picture contained in the input surface.
// The values for frame/pic_width_minus1 and frame/pic_height_minus1 contained
// in the config structures are the original width and height (display size)
// of the pictures, and from this the nvenc app will derive the cropping
// information. Cropping will only be done on the right and bottom side of the
// picture.

typedef struct
{
  unsigned int luma_log2_weight_denom              : 3;    // the base 2 logarithm of the denominator for all luma weighting factors. The value of luma_log2_weight_denom shall be in the range of 0 to 7, inclusive
  unsigned int chroma_log2_weight_denom            : 3;    // the base 2 logarithm of the denominator for all chroma weighting factors. The value of chroma_log2_weight_denom shall be in the range of 0 to 7, inclusive.
  unsigned int reversed                            : 26;

  unsigned char luma_weight_l0_flag[16];                   // equal to 1 specifies that weighting factors for the luma component of list 0 prediction are present, luma_weight_l0_flag[8~15] not used now
  unsigned char chroma_weight_l0_flag[16];                 // equal to 1 specifies that weighting factors for the chroma prediction values of list 0 prediction are present, chroma_weight_l0_flag[8~15] not used now

  unsigned char luma_weight_l1_flag[16];                   // have the same semantics as luma_weight_l0_flag, chroma_weight_l0_flag, not used now
  unsigned char chroma_weight_l1_flag[16];                 // not used now

  short        luma_weight_l0[16];                         // is the weighting factor applied to the luma prediction value for list 0 prediction using RefPicList0[ i ]. When luma_weight_l0_flag is equal to 1, the value of luma_weight_l0[ i ] shall be in the range of -128 to 127, inclusive. -128 to 255 for H265
  short        luma_offset_l0[16];                         // is the additive offset applied to the luma prediction value for list 0 prediction using RefPicList0[ i ]. The value of luma_offset_l0[ i ] shall be in the range of -128 to 127, inclusive. -2^9 to 2^9-1 for h265
  short        luma_weight_l1[16];                         // is the weighting factor applied to the luma prediction value for list 0 prediction using RefPicList0[ i ]. When luma_weight_l0_flag is equal to 1, the value of luma_weight_l0[ i ] shall be in the range of -128 to 127, inclusive. -128 to 255 for H265
  short        luma_offset_l1[16];                         // is the additive offset applied to the luma prediction value for list 0 prediction using RefPicList0[ i ]. The value of luma_offset_l0[ i ] shall be in the range of -128 to 127, inclusive. -2^9 to 2^9-1 for h265
  short        chroma_weight_l0[2][16];                    // has the similar meaning with luma_weight/offset
  short        chroma_offset_l0[2][16];                    // has the similar meaning with luma_weight/offset
  short        chroma_weight_l1[2][16];                    // has the similar meaning with luma_weight/offset
  short        chroma_offset_l1[2][16];                    // has the similar meaning with luma_weight/offset

} nvenc_pred_weight_table_s;   //  452 bytes

typedef struct
{
  unsigned short frame_width_minus1;                        // frame width in pixels minus 1, range 0-4095
  unsigned short frame_height_minus1;                       // frame height in pixels minus 1, range 0-4095
  unsigned short sfc_pitch;                                 // pitch of luma plane
  unsigned short sfc_pitch_chroma;                          // pitch of chroma plane
  unsigned short sfc_trans_mode;                            // least 3 significient bits are used to stand for 8 modes, normal, xflip, yflip etc.
  unsigned short reserved2;                                 // pad to int

  // do not use offset from cmod trace as golden, cmod behavior is different.
  unsigned int   luma_top_frm_offset;                       // offset of luma top field or frame in units of 256
  unsigned int   luma_bot_offset;                           // offset of luma bottom field in units of 256. Not used if frame format.
  unsigned int   chroma_top_frm_offset;                     // offset of chroma top field or frame, or offset of chroma U(Cb) plane in planar mode, both in units of 256
  unsigned int   chroma_bot_offset;                         // offset of chroma bottom field, not used if frame format. Or offset of chroma V(Cr) plane in planar mode. Both in units of 256.
  unsigned int   block_height                       : 7;    // BL mapping block height setting
  unsigned int   tiled_16x16                        : 1;    // Surface is 16x16 tiled instead of BL mapped (must be 1 for refpics)
  unsigned int   memory_mode                        : 2;    // 0: semi-planar, 1: planar
  unsigned int   nv21_enable                        : 1;    // the surface format is yuv or yvu
  unsigned int   input_bl_mode                      : 2;    // the input block linear mode: 0~gpu bl; 1~tegra bl; 2~naive bl this is only for cmod and cmod/plugin
  unsigned int   reserved                           : 19;   // pad to NvU32
} nvenc_h264_surface_cfg_s;                                 // 32 bytes

typedef struct
{
  unsigned int   profile_idc                        : 8;    // seq_parameters(_ext) regs
  unsigned int   level_idc                          : 8;
  unsigned int   chroma_format_idc                  : 2;    // 0=monochrome, 1=yuv4:2:0, 2-3=not supported
  unsigned int   pic_order_cnt_type                 : 2;    // only support values 0 and 2
  unsigned int   log2_max_frame_num_minus4          : 4;
  unsigned int   log2_max_pic_order_cnt_lsb_minus4  : 4;
  unsigned int   frame_mbs_only                     : 1;
  //mvc
  unsigned int   stereo_mvc_enable                  : 1;    //if this bit is enabled, the profile_idc should be 128
  unsigned int   separate_colour_plane_flag         : 1;    //if this bit is enabled, the chroma_format_idc should be 3
  unsigned int   lossless_qpprime_flag              : 1;    //if this bit is enabled and QP=0, bypass trans/q and encoded as lossless MB (hori or vert)
} nvenc_h264_sps_data_s;                                    // 4 bytes

typedef struct
{
  unsigned int   pic_param_set_id                   : 8;    // picture parameter set identification
  unsigned int   entropy_coding_mode_flag           : 1;    // select entropy copding mode: cabac, cavlc
  unsigned int   num_ref_idx_l0_active_minus1       : 5;    // number of currently active reference pictures in list 0 minus 1, range 0..30
  unsigned int   num_ref_idx_l1_active_minus1       : 5;    // number of currently active reference pictures in list 1 minus 1, range 0..30
  unsigned int   weighted_bipred_idc                : 2;    // weighted prediction mode for B: only 0 (default) and 2 (implicit) are supported
           int   pic_init_qp_minus26                : 6;    // initial QP value, range -26..+25
           int   chroma_qp_index_offset             : 5;    // offset to add to chroma QPy for QPc table indexing for Cb, range -12..+12

           int   second_chroma_qp_index_offset      : 5;    // offset to add to chroma QPy for QPc table indexing for Cr, range -12..+12
  unsigned int   constrained_intra_pred_flag        : 1;    // if set, intra prediction can only use pixels from macroblocks that are also intra coded
  unsigned int   deblocking_filter_control_present_flag : 1;// if set, deblock filter controls syntax elements will be present/encoded in the stream
  unsigned int   transform_8x8_mode_flag            : 1;    // if set, enables the use of transform size 8x8 (flags will be encoded in stream per macroblock)
  unsigned int   pic_order_present_flag             : 1;    // if set, pic_order control info is encoded in the stream
  unsigned int   weighted_pred_flag                 : 1;
  unsigned int   reserved0                          : 22;   // pad to full int
} nvenc_h264_pps_data_s;                                    // 8 bytes

typedef struct
{
  unsigned int   self_temporal_stamp_l0             : 3;    // stamp to use for L0 integer search in stamp based mode
  unsigned int   self_temporal_stamp_l1             : 3;    // stamp to use for L1 integer search in stamp based mode
  unsigned int   self_temporal_explicit             : 1;    // explicitly evaluate these vectors
  unsigned int   self_temporal_search               : 1;    // integer search enable for this hint
  unsigned int   self_temporal_refine               : 1;    // subpel search enable for this hint
  unsigned int   self_temporal_enable               : 1;    // hint enable (enables processing + fetching of data) (only changeable at first slice in picture)

  unsigned int   coloc_stamp_l0                     : 3;    // stamp to use for L0 integer search in stamp based mode
  unsigned int   coloc_stamp_l1                     : 3;    // stamp to use for L1 integer search in stamp based mode
  unsigned int   coloc_explicit                     : 1;    // explicitly evaluate these vectors
  unsigned int   coloc_search                       : 1;    // integer search enable for this hint
  unsigned int   coloc_refine                       : 1;    // subpel search enable for this hint
  unsigned int   coloc_enable                       : 1;    // hint enable (enables processing + fetching of data) (only changeable at first slice in picture)

  unsigned int   self_spatial_stamp_l0              : 3;    // stamp to use for L0 integer search in stamp based mode
  unsigned int   self_spatial_stamp_l1              : 3;    // stamp to use for L1 integer search in stamp based mode
  unsigned int   self_spatial_explicit              : 1;    // explicitly evaluate these vectors
  unsigned int   self_spatial_search                : 1;    // integer search enable for this hint
  unsigned int   self_spatial_refine                : 1;    // subpel search enable for this hint
  unsigned int   self_spatial_enable                : 1;    // hint enable (enables processing + fetching of data) (only changeable at first slice in picture)
  unsigned int   reserved0                          : 2;    // pad to full int

  unsigned int   external_stamp_l0_refidx0_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 0
  unsigned int   external_stamp_l0_refidx1_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 1
  unsigned int   external_stamp_l0_refidx2_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 2
  unsigned int   external_stamp_l0_refidx3_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 3
  unsigned int   external_stamp_l0_refidx4_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 4
  unsigned int   external_stamp_l0_refidx5_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 5
  unsigned int   external_stamp_l0_refidx6_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 6
  unsigned int   external_stamp_l0_refidx7_stamp    : 3;    // Stamp to use for external L0 hint with refidx = 7
  unsigned int   external_stamp_l1_refidx0_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 0
  unsigned int   external_stamp_l1_refidx1_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 1
  unsigned int   reserved1                          : 2;    // pad to full int


  unsigned int   external_stamp_l1_refidx2_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 2
  unsigned int   external_stamp_l1_refidx3_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 3
  unsigned int   external_stamp_l1_refidx4_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 4
  unsigned int   external_stamp_l1_refidx5_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 5
  unsigned int   external_stamp_l1_refidx6_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 6
  unsigned int   external_stamp_l1_refidx7_stamp    : 3;    // Stamp to use for external L1 hint with refidx = 7


  unsigned int   external_explicit                  : 1;    // explicitly evaluate these vectors
  unsigned int   external_search                    : 1;    // integer search enable for this hint
  unsigned int   external_refine                    : 1;    // subpel search enable for this hint
  unsigned int   external_enable                    : 1;    // hint enable (enables processing + fetching of data) (only changeable at first slice in picture)

  unsigned int   const_mv_stamp_l0                  : 3;    // stamp to use for L0 integer search in stamp based mode
  unsigned int   const_mv_stamp_l1                  : 3;    // stamp to use for L1 integer search in stamp based mode
  unsigned int   const_mv_explicit                  : 1;    // explicitly evaluate these vectors
  unsigned int   const_mv_search                    : 1;    // integer search enable for this hint
  unsigned int   const_mv_refine                    : 1;    // subpel search enable for this hint
  unsigned int   const_mv_enable                    : 1;    // hint enable (enables processing + fetching of data)

  unsigned int   stamp_refidx1_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 1
  unsigned int   stamp_refidx2_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 2
  unsigned int   stamp_refidx3_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 3
  unsigned int   stamp_refidx4_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 4
  unsigned int   stamp_refidx5_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 5
  unsigned int   stamp_refidx6_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 6
  unsigned int   stamp_refidx7_stamp    : 3;    // Stamp to use for multi-ref hint with refidx = 7
  unsigned int   reserved2              : 11;   // pad to full int

} nvenc_h264_me_hint_cfg_s;                     //16 bytes

typedef struct
{
  unsigned int   mvx_frac                           : 2;    // vector X component fraction
           int   mvx_int                            : 12;   // vector X component integer portion
  unsigned int   reserved_x                         : 2;    // padding X to 16 bit
  unsigned int   mvy_frac                           : 2;    // vector Y component fraction
           int   mvy_int                            : 10;   // vector Y component integer portion
  unsigned int   reserved_y                         : 4;    // padding Y to 16 bit
} nvenc_h264_me_const_vec_s;                                // 4 bytes

typedef struct
{
  unsigned int   bitmask[2]                         ;       // 8x8 search point pattern bitmask
  unsigned int   hor_adjust                         : 1;    // shift hor based on lsb of pred
  unsigned int   ver_adjust                         : 1;    // shift ver based on lsb of pred
  unsigned int   reserved                           : 30;
} nvenc_h264_me_stamp_s;                                    // 12 bytes

typedef struct
{
  unsigned int   mv_limit_enable                    : 1;    // 0=disable, 1=enable set the absolute MV range (Cisco flip camera request)
  unsigned int   left_mvx_frac                      : 2;    // vector X component fraction
           int   left_mvx_int                       : 12;   // vector X component integer portion
  unsigned int   reserved1                          : 1;    // padding X to 16 bit
  unsigned int   top_mvy_frac                       : 2;    // vector Y component fraction
           int   top_mvy_int                        : 10;   // vector Y component integer portion
  unsigned int   reserved2                          : 4;    // padding Y to 16 bit

  unsigned int   right_mvx_frac                     : 2;    // vector X component fraction
           int   right_mvx_int                      : 12;   // vector X component integer portion
  unsigned int   reserved3                          : 2;    // padding X to 16 bit
  unsigned int   bottom_mvy_frac                    : 2;    // vector Y component fraction
           int   bottom_mvy_int                     : 10;   // vector Y component integer portion
  unsigned int   reserved4                          : 4;    // padding Y to 16 bit
} nvenc_h264_me_limit_mv_s;                                 // 8 bytes

typedef struct
{
  unsigned char teb_K                               ;       // TEB K
  unsigned char teb_N                               ;       // TEB N
  unsigned char teb_P                               ;       // TEB P
  unsigned char teb_S                               : 4;    // TEB S
  unsigned char teb_mode                            : 4;    // TEB mode
} nvenc_me_tebif_s;

typedef struct
{
  unsigned int   me_predictor_mode                  : 1;    // 0=MDP, 1=const
  unsigned int   refinement_mode                    : 1;    // 0=hpel, 1=qpel
  unsigned int   lambda_mode                        : 1;    // 0=MDP, 1=const
  unsigned int   const_lambda                       : 11;   // U8.3 format lambda, used if lambda_mode=1
  unsigned int   refine_on_search_enable            : 1;    // 0=disable, 1=enable subpel refine for search candidates
  unsigned int   me_only_mode                       : 1;
  unsigned int   fps_mvcost                         : 1;
  unsigned int   sps_mvcost                         : 1;
  unsigned int   sps_cost_func                      : 1;
  unsigned int   me_icc_mode_mad                    : 1;

  unsigned int   sps_filter                         : 3;
  unsigned int   mc_filter                          : 3;
  unsigned int   vc1_fastuv_mc                      : 1;
  unsigned int   vc1_mc_rnd                         : 1;
  unsigned int   mbc_ctrl_arbitor                   : 2;
  unsigned int   mv_only_enable                     : 1;
  unsigned int   average_mvhint_enable              : 1;

  nvenc_h264_me_limit_mv_s      limit_mv;
  nvenc_h264_me_hint_cfg_s      predsrc;                    // predictor sources
  nvenc_h264_me_const_vec_s     l0_hint;                    // constant MV used as L0 hint
  nvenc_h264_me_const_vec_s     l1_hint;                    // constant MV used as L1 hint
  nvenc_h264_me_const_vec_s     l0_pred;                    // constant MV used as L0 predictor
  nvenc_h264_me_const_vec_s     l1_pred;                    // constant MV used as L1 predictor
  nvenc_h264_me_stamp_s         shape0;                     // priority 0 stamping pattern
  nvenc_h264_me_stamp_s         shape1;                     // priority 1 stamping pattern
  nvenc_h264_me_stamp_s         shape2;                     // priority 2 stamping pattern
  nvenc_h264_me_stamp_s         shape3;                     // priority 3 stamping pattern
  nvenc_h264_me_stamp_s         shape4;                     // priority 4 stamping pattern
  nvenc_h264_me_stamp_s         shape5;                     // priority 5 stamping pattern
  nvenc_h264_me_stamp_s         shape6;                     // priority 6 stamping pattern
  nvenc_h264_me_stamp_s         shape7;                     // priority 7 stamping pattern
  nvenc_me_tebif_s              teb_para;                   // TEB parameter
  unsigned int mbc_mb_size                        : 9;    // MBC size in MB
  unsigned int rmvp_source_half_scaled            : 1;    // only support picture size half scale down in horz&vert
  unsigned int partDecisionMadeByFPP              : 1;    // enable CU32 FPP;
  unsigned int penalty_factor_FPP                 : 6;    // MV with cost no larger than min_cost + ((min_cost * factor) >> 8) will go to SPS, not used from nvenc7
  unsigned int spatial_hint_pattern               : 4;
  unsigned int temporal_hint_pattern              : 6;
  unsigned int Cu16partDecisionMadeByFPP          : 1;    // enable CU16 making part decision
  unsigned int sps_mvp_from_fbm                   : 1;    //1:sps use fbm'MVP and lambda;0:based on me_predictor_mode and lambda_mode to set the mvp and lambda for sps
  unsigned int fbm_select_best_cu16_parttype_num    : 3;    // #winner partitions selected for cu16x16. valid range is 0~7;
  unsigned int fbm_op_winner_num_p_frame            : 3;    // FBM output number of integer winners for each PU. when fbm_op_winner_num_p_frame_cu8 has non-zero value, this field means the PU winners for CU64~CU16, i.e. w/o CU8
  unsigned int fbm_op_winner_num_b_frame_l0         : 3;    // FBM output number of integer winners for each PU
  unsigned int fbm_op_winner_num_b_frame_l1         : 3;    // FBM output number of integer winners for each PU
  unsigned int fbm_select_best_cu32_parttype_num    : 3;    // cu32 select best parttype num
  unsigned int sps_evaluate_merge_cand_num          : 3;    // number of merg candidate for sps to explicit evaluate
  unsigned int fps_quad_thresh_hold                 : 10;
  unsigned int external_hint_order                  : 1;    //0:Insert external hint at first ref; 1:insert external hint at last ref;
  unsigned int coloc_hint_order                     : 1;    //0:Insert coloc hint at first ref; 1: insert coloc hint at last ref.
  unsigned int reserved1                            : 5;
  short        ct_threshold                         ;    // for OFS only, threshold in Census Transform calculation
  //(1<<hint_type0)|(1<<hint_type1)|(1<<hint_type2)|(1<<hint_type3)|(1<<hint_type4) should be equal to 0x1f
  unsigned short hint_type0                        : 3; //0:const hint; 1:spatial hint; 2:temporal hint; 3:coloc hint; 4:external hint
  unsigned short hint_type1                        : 3; //the same as hint_type0
  unsigned short hint_type2                        : 3; //the same as hint_type0
  unsigned short hint_type3                        : 3; //the same as hint_type0
  unsigned short hint_type4                        : 3; //the same as hint_type0
  unsigned short pyramidal_hint_order              : 1;     // for OFS only, used when do pyramidal ME, 1: enable, the hint order will be: if one MB has external hint, use exhint, otherwise use spatial/temproal/const hints. 0: disable.
  unsigned char  left_hint_delay_N                 : 4;     // for OFS only, specify left hint delay MB number(1~8), used for high-perf mode (left_hint_delay_N need to set to 8 to achieve 4K@120FPS). set 0 for using default value, ie 3 for ofs/H265, 8 for H264
  unsigned char  reserved3                         : 4;

  unsigned char  cu8partDecisionMadeByFPP          : 1; //enable CU8x8 partition selection in FBM
  unsigned char  fbm_select_best_cu8_parttype_num  : 3; //#winner partitions selected for cu8x8. valid range is 0~7;
  unsigned char  fbm_op_winner_num_p_frame_cu8     : 3; // FBM output number of integer winners for each PU of CU size 8.
  unsigned char  reserved4                         : 1;

  unsigned char  cu64partDecisionMadeByFPP         : 1; //enable CU64x64 partition selection in FBM
  unsigned char  fbm_select_best_cu64_parttype_num : 3; //#winner partitions selected for cu64x64. valid range is 0~7;
  unsigned char  reserved5                         : 4;

  unsigned char  reserved[33];                              // padding to make me_control=192 to allow 3 reads of 64 in ucode
                                                            //     this is optimal size as 160=5 reads of 32 will exceed dma queue
} nvenc_h264_me_control_s;                                  // 192 bytes

typedef struct
{
  unsigned short intra_refresh_cnt;                         // distance between forced-intra MBs in slice; 0 means OFF
  unsigned short intra_refresh_offs;                        // first forced-intra MB in slice

  unsigned int   intra_luma4x4_mode_enable          : 9;    // bitmask indicating which intra luma 4x4 modes to enable
  unsigned int   intra_luma8x8_mode_enable          : 9;    // bitmask indicating which intra luma 8x8 modes to enable
  unsigned int   intra_luma16x16_mode_enable        : 4;    // bitmask indicating which intra luma 16x16 modes to enable
  unsigned int   intra_chroma_mode_enable           : 4;    // bitmask indicating which intra chroma modes to enable
  unsigned int   inter_penalty_factor_for_ip1       : 6;    // early teminate ip1 intra search if intra cost is larger than inter_cost + ((inter_cost * factor) >> 8)

  unsigned int   l0_part_16x16_enable               : 1;    // enable L0 prediction for 16x16
  unsigned int   l0_part_16x8_enable                : 1;    // enable L0 prediction for 16x8
  unsigned int   l0_part_8x16_enable                : 1;    // enable L0 prediction for 8x16
  unsigned int   l0_part_8x8_enable                 : 1;    // enable L0 prediction for 8x8
  unsigned int   l0_part_8x4_enable                 : 1;    // enable L0 prediction for 8x4
  unsigned int   l0_part_4x8_enable                 : 1;    // enable L0 prediction for 4x8
  unsigned int   l0_part_4x4_enable                 : 1;    // enable L0 prediction for 4x4
  unsigned int   l0_part_reserved                   : 1;    // reserved for future L0 prediction extensions
  unsigned int   l1_part_16x16_enable               : 1;    // enable L1 prediction for 16x16
  unsigned int   l1_part_16x8_enable                : 1;    // enable L1 prediction for 16x8
  unsigned int   l1_part_8x16_enable                : 1;    // enable L1 prediction for 8x16
  unsigned int   l1_part_8x8_enable                 : 1;    // enable L1 prediction for 8x8
  unsigned int   l1_part_reserved                   : 4;    // reserved for future L1 prediction extensions
  unsigned int   bi_part_16x16_enable               : 1;    // enable Bi prediction for 16x16
  unsigned int   bi_part_16x8_enable                : 1;    // enable Bi prediction for 16x8
  unsigned int   bi_part_8x16_enable                : 1;    // enable Bi prediction for 8x16
  unsigned int   bi_part_8x8_enable                 : 1;    // enable Bi prediction for 8x8
  unsigned int   bi_part_reserved                   : 4;    // reserved for future Bi prediction extensions
  unsigned int   bdirect_mode                       : 2;    // 0: disable, 1: spatial, 2: temporal
  unsigned int   bskip_enable                       : 1;    // enable b_skip encoding (requires b_direct encoding)
  unsigned int   pskip_enable                       : 1;    // enable p_skip encoding
  unsigned int   special_reserved                   : 4;    // reserved for future special prediction extensions

           short bias_inter_16x16;                          // bias for inter 16x16 (subtracts from inter 16x16 cost)
           short bias_inter_16x8;                           // bias for inter 16x8  (subtracts from inter 16x8  cost)

           short bias_inter_8x16;                           // bias for inter 8x16  (subtracts from inter 8x16  cost)
           short bias_inter_8x8;                            // bias for inter 8x8   (subtracts from inter 8x8   cost)

           short bias_pskip;                                // bias for skip       (subtracts from skip cost)
           short bias_bdir;                                 // bias for bdirect     (subtracts from bdirect cost)

           short bias_intra_over_inter;                     // bias for any intra   (subtracts from intra cost)
           short bias_intra_16x16;                          // bias for intra 16x16 (subtracts from intra 16x16 cost)

           short bias_intra_8x8;                            // bias for intra 8x8   (subtracts from intra 8x8 cost)
           short bias_intra_4x4;                            // bias for intra 4x4   (subtracts from intra 4x4 cost)

           short bias_intra_most_prob;                      // bias for intra most probable mode
  unsigned short mv_cost_bias;                              // bias applied to internal motion vector cost calculation

  unsigned short intra_nxn_bias_multiplier;                 // 0..255 range; used for internal bias calculation
  unsigned short intra_most_prob_bias_multiplier;           // 0..31 range; used for internal bias calculation

           short pskip_bias_multiplier;                     // S16 range; used for internal bias calculation
           short bdirect_bias_multiplier;                   // S16 range; used for internal bias calculation

  unsigned short pskip_esc_threshold;                       // if pskip cost is below this, disable full search & attempt pskip encode
  unsigned short bdirect_esc_threshold;                     // if bdirect cost is below this, disable full search & attempt bdirect encode

  unsigned short early_intra_disable_mpeb_threshold;        // if early intra cost is below this, disable full search & select intra
  unsigned short tempmv_wt_spread_threshold;                // The threshold for the sum of differences between 8x8s and 16x16 mvs. Used for temporal mv weight
  unsigned int   tempmv_wt_distort_threshold        : 16;   // The threshold for the cost of the 16x16 mv. Used for temporal mv weight
  unsigned int   mv_cost_predictor_control          : 1;    // 0=use 16x16 predictor only; 1=use accurate predictor (slower but better)
  unsigned int   mv_cost_enable                     : 1;    // enable mv cost calculations in mode decision
  unsigned int   intra_most_prob_force_on           : 1;    // always evaluate most probably 4x4 or 8x8 intra mode during full search
  unsigned int   early_intra_mode_control           : 2;    // 0=inter; 1=eval_intra; 2=ext_hint; 3=none (low perf,best qual)
  unsigned int   early_intra_mode_type_16x16dc      : 1;    // evaluate 16x16dc if early_intra_mode_control==1
  unsigned int   early_intra_mode_type_16x16h       : 1;    // evaluate 16x16h  if early_intra_mode_control==1
  unsigned int   early_intra_mode_type_16x16v       : 1;    // evaluate 16x16v  if early_intra_mode_control==1
  unsigned int   early_ip_is_final                  : 1;    // if set, result of early intra is final (disable full intra search)
  unsigned int   chroma_eval_mode                   : 1;    // for intra chroma search 0 = U and V; 1 = U only
  unsigned int   ip_search_mode                     : 3;    // which intra sizes to search before making IP decision b0=4x4,b1=8x8,b2=16x16
  unsigned int   multiply_bias_with_lambda          : 1;    // setting this to 1 will result in bias being multiplied by lambda
  unsigned int   force_ipcm                         : 1;    // 0=normal encode; 1=force mpeb/mpec encode as IPCM
  unsigned int   early_termination_ip1              : 1;    // enable IP1 early termination
           short bias_favor_intra_16x16_early;              // S16 range; used for internal bias calculation
  unsigned short priority_ipred_type_ip1            : 3;    // which intra type search first for early termination, must be a subset of ip_search_mode
  unsigned short intra_ssd_cnt_4x4                  : 4;    // SSD cost evaluation between N top intra modes selected by satd
  unsigned short intra_ssd_cnt_8x8                  : 4;    // SSD cost evaluation between N top intra modes selected by satd
  unsigned short intra_ssd_cnt_16x16                : 4;    // SSD cost evaluation between N top intra modes selected by satd
  unsigned short skip_evaluate_enable               : 1;    // 0: not check skip ssd cost; 1, evaluate skip ssd cost
  unsigned int   rdo_level                          : 3;    // 0:mode decision by satd  1:mode decision by ssd
  unsigned int   tu_search_num                      : 3;    // tu search part num per MB
  unsigned int   luma_residual_zero_eval            : 2;    // luma residual zero cost evaluate, bit0:intra; bit1:inter
  unsigned int   num_1div8_lambda_ssd               : 7;    // number of 1/8 lambda ssd for lambda_coef delta. lambda_coef = (1+num_1div8_lambda_ssd*1/8)*lambda_ssd
  unsigned int   ofs_mvx_range                      : 2;    // control mvx precision/range, 0: ouptput format: S10.5, range[-1024, 1023]; 1: output format: S11.4, range[-2048, 2047]; 2: output format: S12.3, range[-4096, 4095]; 3: output format: S12.2, range[-4096, 4095]
  unsigned int   reserved4                          : 15;   // pad to 32 bytes
  short          bias_tu_4x4;                               // bias for TU 4x4
  short          bias_tu_8x8;                               // bias for TU 8x8
  unsigned int   ssim_rdo                           : 2;    // SSIM-RDO: 0 disable, 1: enable SW SSIM-RDO, 2: enable HW SSIM-RDO,
  unsigned int   calc_ssim                          : 1;    // Calculate SSIM distortion: 0 disable, 1: enable. ssim_rdo = 1 => calc_ssim = 1
  unsigned int   reserved5                          : 29;   // reserved
  unsigned int   ssd_over_ssim_factor;                      // distortion ssd/ssim ratio, in U24.8 format
  unsigned int   reserved6[13];                             // reserved
} nvenc_h264_md_control_s;                                  // 128 bytes

typedef struct
{
  unsigned short qpp_run_vector_4x4;                        // cost values for 4x4 transform (16 bit total vector)
  unsigned short qpp_run_vector_8x8[3];                     // cost values for 8x8 transform (48 bit total vector)
                                                            // 2 bits each for first 12 coefs, then 1 bit each for next 24
  unsigned char  qpp_luma8x8_cost;                          // luma 8x8 cost threshold, 0 = throw out all coefs, range 0-15
  unsigned char  qpp_luma16x16_cost;                        // luma 16x16 cost threshold, 0 = throw out all coefs, range 0-15
  unsigned char  qpp_chroma_cost;                           // chroma cost threshold, 0 = throw out all coefs, range 0-15
  unsigned char  qpp_mode                           : 2;    // 0 = OFF, 1 = 8x8, 2 = 16x16_8x8
  unsigned char  reserved1                          : 4;    // padding
    //add by cl
  unsigned char  quant_intra_sat_flag               : 1;    // 0: no saturation is applied; 1: saturation is applied
  unsigned char  quant_inter_sat_flag               : 1;    // 0: no saturation is applied; 1: saturation is applied
  unsigned int   quant_intra_sat_limit              : 16;   // quantization saturation limit for intra MB
  unsigned int   quant_inter_sat_limit              : 16;   // quantization saturation limit for inter MB
    //~
  unsigned short dz_4x4_YI[16];                             // deadzone for 4x4 transform of Luma Intra
  unsigned short dz_4x4_YP[16];                             // deadzone for 4x4 transform of Luma Inter
  unsigned short dz_4x4_CI;                                 // deadzone for 4x4 transform of Chroma Intra
  unsigned short dz_4x4_CP;                                 // deadzone for 4x4 transform of Chroma Inter
  unsigned short dz_8x8_YI[16];                             // deadzone for 8x8 transform of Luma Intra, 16 values are mapped to 64 coefs
  unsigned short dz_8x8_YP[16];                             // deadzone for 8x8 transform of Luma Inter, 16 values are mapped to 64 coefs
  unsigned   int reserved2[11];                             // pad to 192 bytes to allow 3 reads of 64 bytes in ucode
                                                            //     this is optimal size as 160=5 reads of 32 will exceed dma queue
} nvenc_h264_quant_control_s;                               // 192 bytes

typedef struct
{
    unsigned int reorder_l0_cmd_count                 : 4;
    unsigned int reorder_l1_cmd_count                 : 4;
    unsigned int mmco_cmd_count                       : 4;
    unsigned int no_output_of_prior_pic_flag          : 1;
    unsigned int long_term_ref_flag                   : 1;
    unsigned int reserved                             :18;
} nvenc_h264_refpic_cmd_s;                                     // 4 bytes

typedef struct
{
  // ref_pic_list_reorder_cmd includes both opcode and op arguments
    unsigned int   reordering_of_pic_nums_idc         : 3;     //  Bit 31-29 (0-5  (Table 7-7)
    unsigned int   abs_diff_pic_num_minus1            :17;     //  (opcode=0/1)
    unsigned int   long_term_pic_num                  : 4;     //  (opcode=2):
    unsigned int   abs_diff_view_idx_minus1           : 4;     //  (opcode=4/5):
    unsigned int   reserved                           : 4;
} nvenc_h264_ref_pic_reorder_s;                                // 4 bytes

typedef struct
{
    unsigned int   mmco_cmd_id                        : 3;     // totally 6 commands

    unsigned int   difference_of_pic_nums_minus1      :17;     // 1) mark a short_term ref as long (mmco=3); 2) mark a short_term as un-used (mmco=1)
    unsigned int   long_term_pic_num                  : 3;     // [0 to MaxLongTermFrameIdx ]; mark a long as "unused for ref" (mmco=2);
    unsigned int   long_term_frame_idx                : 3;     // [0 to MaxLongTermFrameIdx] 1) mark a picture to long (mmco=3, mmco=6)
    unsigned int   max_long_term_frame_idx_plus1      : 4;     // [0, num_ref_frames] max long term frm indexes allowed for long term pictures.
                                                               // max_long_term_frame_idx_plus1=0 -> MaxLongTermFrameIdx="no-long-term"
                                                               // max_long_term_frame_idx_plus1>0 -> MaxLongTermFrameIdx= max_long_term_frame_idx_plus1-1
    unsigned int   reserved                           : 2;
} nvenc_h264_mmco_s;                                           // 4 bytes

typedef struct
{
  // int0
  unsigned int   num_mb                            : 19;     // number of macroblocks in this slice, support upto 8kx8k
  unsigned int   qp_avr                            :  8;     // 6bit value used by hw. Other bits can be used for rounding.
  unsigned int   reserved                          :  5;
  // int1
  unsigned int   slice_tgt_rate;                            // target bit rate (size) (if RC mode == slice)

  // int2
           char  slice_alpha_c0_offset_div2         : 4;
           char  slice_beta_offset_div2             : 4;
  unsigned char  qp_slice_min;                              // min slice qp value
  unsigned short qp_slice_max                       : 8;    // max slice qp value
  unsigned short force_intra                        : 1;    // force entire slice to be intra
  unsigned short disable_deblocking_filter_idc      : 2;
  unsigned short cabac_init_idc                     : 2;
  unsigned short disable_slice_header               : 1;    // if set to 1, disables HW slice header output
  unsigned short reserved0                          : 2;

  // int3
  unsigned int   num_ref_idx_active_override_flag   : 1;
  unsigned int   num_ref_idx_l0_active_minus1       : 5;    // [0.31]
  unsigned int   num_ref_idx_l1_active_minus1       : 5;    // [0,31]
  unsigned int   reserved1                          : 9;    // padding
  unsigned int   me_control_idx                     : 4;    // index in array of nvenc_h264_me_control_s to use for this slice
  unsigned int   md_control_idx                     : 4;    // index in array of nvenc_h264_md_control_s to use for this slice
  unsigned int   q_control_idx                      : 4;    // index in array of nvenc_h264_quant_control_s to use for this slice

  // int 4
  unsigned int   limit_slice_top_boundary           : 1;    // limit ME to slice boundaries
  unsigned int   limit_slice_bot_boundary           : 1;    // limit ME to slice boundaries
  unsigned int   limit_slice_left_boundary          : 1;    // limit ME to slice boundaries
  unsigned int   limit_slice_right_boundary         : 1;    // limit ME to slice boundaries
  unsigned int   ROI_enable                         : 1;    // Enable region  of interest processing
  unsigned int   ROI_me_control_idx                 : 4;
  unsigned int   ROI_md_control_idx                 : 4;
  unsigned int   ROI_q_control_idx                  : 4;
  unsigned int   ROI_qp_delta                       : 8;    // QP delta in ROI relative to picture outside ROI
  unsigned int   wp_control_idx                     : 4;    // index in array of nvenc_pred_weight_table_s to use for this slice
  unsigned int   reserved2                          : 3;    // reserved for alignment

  // int 5
  unsigned int   ROI_top_mbx                        : 8;    // top left mbx of the ROI
  unsigned int   ROI_top_mby                        : 8;
  unsigned int   ROI_bot_mbx                        : 8;    // bottom right mbx of the ROI
  unsigned int   ROI_bot_mby                        : 8;

  // int 6: ref_pic_cmd_cfg
  nvenc_h264_refpic_cmd_s        ref_pic_cmd_cfg;

  // int 7-22: RefPicListReorder/Modification: Defined in Section 7.4.3.1
  nvenc_h264_ref_pic_reorder_s   ref_pic_list_reorder_cmd[2][8];     // size shall not exceed num_ref_idx_lx_active_minus1 + 1 (8+8=16)

  //int 23-30: MMCO: Defined in Section: 7.4.3.3
  nvenc_h264_mmco_s              mmco_cmd[8] ;

  // int 31
  unsigned int   reserved3;

} nvenc_h264_slice_control_s;                               // 128 bytes


// Quarter-resolution modifier flags for first_pass_source_half_scaled
#define NVENC_QRES_FLAG_ENABLE          0x1     // Set if quarter-resolution is enabled
#define NVENC_QRES_FLAG_DECIMATED       0x10    // Set to indicate that subsampling was performed with decimation rather than low-pass
#define NVENC_QRES_FLAG_PASS2REFS       0x20    // Set to indicate that 1st-pass uses subsampled 2nd pass references instead of self-generated references

// Lookahead macros for ab_beta(INFO1) and prev_act(INFO2)
#define NVENC_LOOKAHEAD_INFO1(Wp2Wi)        ((unsigned char)(Wp2Wi))    // clamp((int)(Wp*256.0/Wi), 0, 255) -> ratio of inter over intra complexity, 0=no lookahead
// Lookahead INFO2: (depth<<8)|(nextI)
// nextI: 1..255=distance to next scene change, 0=unknown or greater than lookahead depth;
// depth: 1..255=max lookahead depth (0=no lookahead)
#define NVENC_LOOKAHEAD_INFO2(nextI,depth)  (((unsigned short)(nextI)) | (((unsigned short)(depth))<<8))

// HRD conformance and misc RC flags
#define NVENC_RC_HRD_VCL                0x1 // vcl_cpb_size and vcl_bitrate
#define NVENC_RC_HRD_NAL                0x2 // nal_cpb_size and nal_bitrate
#define NVENC_RC_HRD_STRICTGOPTARGET    0x4 // Strict GOP target (minimizes GOP-to-GOP rate fluctuations)
#define NVENC_RC_FIXED_PBRATIO          0x8U // Disable dynamic B-frame rhopbi adjustments

 // needed only if picture level RC is enabled; as part of picture level RC, it will also perform HRD verification
 // The contents of this file is produced by the driver during sequence level operations and should not be modified.
typedef struct
{
  unsigned char  hrd_type;                                  // nal (=2) and vcl (=1) type
  unsigned char  QP[3];                                     // QP for 0:P picture, ,1:B picture 2; I picture
  unsigned char  minQP[3];                                  // min QP for 0:P picture, ,1:B picture 2; I picture
  unsigned char  maxQP[3];                                  // max QP for 0:P picture, ,1:B picture 2; I picture
  unsigned char  maxQPD;                                    // max QP delta between two consecutive QP updates
  unsigned char  baseQPD;                                   // initial QP delta between two consecutive QP updates
           int   rhopbi[3];                                 // 23.8 signed fixed point quant ratios P/I, B/I, I/I(=1)

           int   framerate;                                 // fps
  unsigned int   buffersize;                                // total buffer size

           int   nal_cpb_size;                              // size in bits
           int   nal_bitrate;                               // rate in bps
           int   vcl_cpb_size;                              // size in bits
           int   vcl_bitrate;                               // rate in bps

  unsigned int   gop_length;                                // I period, gop_length == 0xffffffff is used for infinite gop length
           int   Np;                                        // 27.4 signed fixed (gop_length + num_b_frames -1)/(num_b_frames + 1)
           int   Bmin;                                      // 23.8 signed fixed point min buffer level, updated by driver at sequence level only.
           int   Ravg;                                      // 23.8 signed fixed point average rate, updated by driver at sequence level only.
           int   R;                                         // 23.8 signed fixed point rate

  unsigned char  ab_alpha;                                  // VBR/CBR: min/target quality level (1..51); rcmode4: weight of prev frame activity compared to part of the current picture
  unsigned char  ab_beta;                                   // lookahead info1: ratio of inter over intra complexity (fix8); rcmode4: weight of current activity compared to neighbors
  unsigned short prev_act;                                  // lookahead info2: nextI, depth and valid flag; rcmode4: average activity level of the first reference (L0)

  unsigned char  aqMode                             : 3;    // 0: disable adaptive quantization, 1: AQ mode fast, 2: AQ mode full
  unsigned char  dump_aq_stats                      : 1;    // 1: reads aq stats
  unsigned char  is_emphasis_level                  : 1;    // 1: enables emphasis level map, treats QP map as level map
  unsigned char  reserve0                           : 3;    // Reserved for alignment
  unsigned char  single_frame_VBV;                          // 1: VBV buffer size is set to average frame size; 0: otherwise
  unsigned char  two_pass_rc;                               // 0: single pass rc , 1: first pass of 2 pass rc 2: second pass of 2 pass rc
  unsigned char  rc_class;                                  // reserved

  unsigned char  first_pass_source_half_scaled;             // 0 : first pass on half resolution 1: first pass on full resolution (along with NVENC_QRES_FLAG_XXX)
  unsigned char  iSizeRatioX;                               // ratio between I picture target size over average picture size numerator
  unsigned char  iSizeRatioY;                               // ratio between I picture target size over average picture size denominator
  unsigned char  ext_scene_change_flag;                     // Scene change flag set as hint by external preprocessing unit. 0: No scene change, 1: Current picture is first in scene
  unsigned int   ext_intra_satd;                            // If non zero this represents the intra SATD for current picture computed by external preprocessing unit
  unsigned char  ext_picture_rc_hints;                      // picture rc hints are set by external source. 0: no hints, 1: hints are avaialble for current picture
  unsigned char  session_max_qp;                            // Current frame qp will never exceed this value when ext_picture_rc_hints is set
  unsigned char  reserve[2];
} nvenc_h264_rc_s;                                          // 88 bytes

typedef struct
{
  unsigned int   var_min;
  unsigned int   var_max;
  unsigned int   var_avg;
  unsigned char  reserved[4];
} nvenc_aq_stat_s;                                         // 16 bytes

typedef struct
{
           char  l0[8];                                     // reference picture list 0
           char  l1[8];                                     // reference picture list 1
           char  temp_dist_l0[8];                           // temporal distance of each ref pic in list 0 DiffPicOrderCnt( currPicOrField, pic0) )
           char  temp_dist_l1[8];                           // temporal distance of each ref pic in list 1 DiffPicOrderCnt( currPicOrField, pic1) )
           short dist_scale_factor[8][8];                   // (h264 spec eq 8-203) array [refidx1][refidx0] signed 11 bit values
  unsigned int   diff_pic_order_cnt_zero[2];                // This is a 2-dimensional array of booleans (1 bit flags) indicating that DiffPicOrderCnt(refidx1, refidx0) == 0
  unsigned int   max_slice_size;                            // for use in slice_mode 1 only:  dynamic insertion at/before this boundary (#bytes)
  unsigned int   max_byte_count_before_resid_zero;          // Maximum byte count before micrcode forces the residuals to zero.
           int   delta_pic_order_cnt_bottom;                // direct value of syntax element to be put in every slice of the picture
  unsigned short frame_num;                                 // value for slice header syntax element
  unsigned short pic_order_cnt_lsb;                         // value of the corresponding syntax element
  unsigned short idr_pic_id;
  unsigned short colour_plane_id;
  unsigned int   longTermFlag;                              // Bits 7:0 for pictures in RefPicList0 and bits 23:16 for pictures in RefPicList1
  unsigned char  num_forced_slices_minus1;                  // number of forced slice boundary locations minus1(number of entries in h264_slice_control_s)
  unsigned char  num_me_controls_minus1;                    // number of nvenc_h264_me_control_s entries in array minus1
  unsigned char  num_md_controls_minus1;                    // number of nvenc_h264_md_control_s entries in array minus1
  unsigned char  num_q_controls_minus1;                     // number of nvenc_h264_quant_control_s entries in array minus1
  unsigned int   slice_control_offset;                      // offset from start of top level "nvenc_h264_drv_pic_setup_s" structure
                                                            // to start of array of "nvenc_h264_slice_control_s" structures
  unsigned int   me_control_offset;                         // offset from start of top level "nvenc_h264_drv_pic_setup_s" structure
                                                            // to start of array of "nvenc_h264_me_control_s" structures
  unsigned int   md_control_offset;                         // offset from start of top level "nvenc_h264_drv_pic_setup_s" structure
                                                            // to start of array of "nvenc_h264_md_control_s" structures
  unsigned int   q_control_offset;                          // offset from start of top level "nvenc_h264_drv_pic_setup_s" structure
                                                            // to start of array of "nvenc_h264_quant_control_s" structures
  unsigned int   hist_buf_size;                             // size in bytes of the buffer allocated for history
  unsigned int   bitstream_buf_size;                        // size in bytes of the buffer allocated for bitstream slice/mb data
  unsigned int   bitstream_start_pos;                       // start position in bitstream buffer where data should be written (byte offset)
  unsigned int   pic_struct                         : 2;    // 0 = frame, 1 = top/first field, 2 = bot/second field
  unsigned int   pic_type                           : 2;    // 0 = P, 1 = B, 2 = I, 3 = IDR
  unsigned int   ref_pic_flag                       : 1;    // reference picture (0 = no, 1 = yes)
  unsigned int   slice_mode                         : 1;    // 0 = dynamic slice insertion based on slice size
                                                            // 1 = static slice insertion based on slice control array
                                                            //     (insert slices as defined in array, if all array elements used
                                                            //      before end of picture is reached, start over with entry 0)
  unsigned int   ipcm_rewind_enable                 : 1;
  unsigned int   cabac_zero_word_enable             : 1;

  //mvc
  unsigned int   base_view                          : 1;    // 1: current view component is a base view; 0: non-base view
  unsigned int   priority_id                        : 6;    // priority identifier for the NAL unit
  unsigned int   view_id                            : 10;   // view identifier for the NAL unit
  unsigned int   temporal_id                        : 3;    // temporal identifier for the NAL unit
  unsigned int   anchor_pic_flag                    : 1;    // equal to 1 specifies that the current access unit is an anchor access unit
  unsigned int   inter_view_flag                    : 1;    // equal to 1 specifies that the current view component may be used for inter-view prediction by other view components in the current access unit
  unsigned int   opm_enable                         : 1;    // check with OPM if encryption needed
  unsigned int   temporal_hint8x8_enable            : 1;    // 8x8 hint enable
  char           cur_interview_ref_pic;                     //current interview ref pic
  char           prev_interview_ref_pic;                    //previous interview ref pic to replace current interview ref pic when write the regs ME_MBC_REFLIST0_DPBLUT and ME_MBC_REFLIST1_DPBLUT
  unsigned char  codec                              : 3;
  unsigned char  e4byteStartCode                    : 1;    // if enable to 1, all the slices in a picture will use 4 byte start code 00000001(used in rtp mode), original is 000001
  unsigned char  qpfifo                             : 1;    // 0 = Polling mode, 1 = Interrupt mode.
  unsigned int   me_candidates_output_struct        : 1;    // 0 = me-only mode, 1 = MV & cost for all blocks (hybrid format)
  unsigned int   me_candidates_output_enable        : 1;    // hybrid mode results configuration (see me_candidates_output_struct). 0 = results disabled. 1 = results enabled (for fullscale image).
  unsigned char  medma_output_cost_mode             : 1;    // 0 = residual cost only. 1 = residual cost + mv cost
  unsigned char  intraRefreshCount;                         // Set to number of pictures over which intra refresh will happen. Set to 0 for non intra refresh picture
  unsigned int   mpec_threshold                     : 30;   // collect mpec stats after threshold mbs
                                                            // when mpec_stat_on is enabled and mpec_threshold is not equal to picWidthInMbs,
                                                            // rc mode 0 will be used to collect stats
  unsigned int   me_lambda_fifo_enable              : 1;    // lambda map fifo enable. 1: enable. 0: disable
  unsigned int   me_candidates_rdo_mode             : 1;    // me candidates rdo mode enable. 1: enable. 0: disable.
  unsigned int   slice_stat_offset;                         // offset from start of top level "nvenc_stat_data_s" structure
                                                            // to start of array of "nvenc_slice_stat_s" structure
  unsigned int   mpec_stat_offset;                          // offset from start of top level "nvenc_stat_data_s" structure
                                                            // to start of array of "nvenc_mpec_stat_s" structure
  unsigned int   wp_control_offset;                         // offset from start of top level "nvenc_h264_drv_pic_setup_s" structure
                                                            // to start of array of "nvenc_pred_weight_table_s" structure
                                                            // to start of array of "nvenc_mpec_stat_s" structure

  unsigned char  num_wp_controls_minus1;                    // number of nvenc_pred_weight_table_s entries in array minus 1
  unsigned char  b_as_ref                           : 1;    // use b frame as reference
  unsigned char  ofs_mode                           : 2;    // ofs mode. 0: OFF; 1: Optical Flow; 2: Stereo (output mvx only), 3: Stereo (output mvx/mvy)
  unsigned char  new_subframe                       : 1;
  unsigned char  bit_depth_minus_8                  : 4;
  unsigned char  stripID;
  unsigned char  strips_in_frame;
  unsigned short slice_encoding_row_start;                  // start mb row for slice encoding
  unsigned short slice_encoding_row_num;                    // num of mb row for slice encoding

  unsigned char  segment_enable                     : 1;    // used by ofs mode only for segment map enable flag, 0: (default) disable/ 1: enable
  unsigned char  background_seg                     : 5;    // used by ofs mode only when segment map is enabled, to specify the segment id of background
  unsigned char  input_sub_sampling                 : 1;    // used by PDMA 2x2 sub-sampling
  unsigned char  output_sub_sampling                : 1;    // used by MPEB 2x2 sub-sampling output
  unsigned char  segment_P2                            ;
  unsigned char  segment_thr                           ;
  unsigned char  accurate_skip_mv                   : 1;
  unsigned char  intra_sel_4x4_enable               : 1;    // seletive intra4x4 , 0:disable(always do intra4x4 serach), 1:enable(enable or disable intra4x4 search based on pixel variance)
  unsigned int   enable_source_image_padding        : 1;    // enable/disable HW source image padding
  unsigned int   luma_padding_mode                  : 2;    // HW souce padding mode for luma: 0: copy frame boundary, 1: zero padding, 2: half value, i.e 1<<(bitdepth-1)
  unsigned int   chroma_padding_mode                : 2;    // HW souce padding mode for chroma: 0: copy frame boundary, 1: zero padding, 2: half value, i.e 1<<(bitdepth-1)
  unsigned int   stats_fifo_enable                  : 1;    // enable per-mb short stats fifo
  unsigned int   chroma_skip_threshold_4x4         : 16;    // low 3 bits is the factional, to check if chroma residual is big in skip mb, can't simply set chroma residual to 0 even though luma has been decided to skip
  unsigned int   intra_sel_4x4_threshold           : 16;    // disable intra4x4 search when intra_sel_4x4_enable=true and max variance of 8x8 blocks in current MB is less than intra_sel_4x4_threshold
  unsigned int   aq_stat_offset;                            // start of array of "nvenc_aq_stat_s" structure
  unsigned int   act_stat_offset;                           // offset from start of "nvenc_rc_pic_stat_s" structure,
                                                            // in the RC stats buffer. This field points to the start
                                                            // of data for external (delta-)QP map.
  unsigned int   stats_fifo_offset;                         // offset from start of top level "nvenc_stat_data_s" structure
} nvenc_h264_pic_control_s;                                 // 276 bytes


// This is the top level config structure passed by driver to nvenc. It must be aligned
// to a 256B boundary. The first integer in this struct is a 'magic' number which driver
// must set to a specific value, and which the main Falcon app must check to see if it
// matches the expected value. This prevents running wrong combinations of traces/drvr
// code and falcon code. Encoding is as follows:
//  Bit 31..16 = 0xc9b7
//  Bit 15..8  = Version number
//  Bit 7..0   = Revision number
// This number changes whenever there is a change to the class. If the change is small and
// backward compatible, only the revision number is incremented. If the change is major or
// not backward compatible, the revision number is reset to zero and the version number
// incremented. Falcon app should check if the upper 24 bits match the expected value, and
// terminate with error code "NvC9B7EncErrorH264BadMagic" if there is a mismatch. The value
// for the current class is defined as:

#ifdef NV_NVENC_8_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC9B70006
#elif NV_NVENC_8_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xC8B70006
#elif NV_NVENC_7_3
#define NV_NVENC_DRV_MAGIC_VALUE  0xC7B70006
#elif NV_NVENC_7_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC4B70006
#elif NV_NVENC_7_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xC5B70006
#elif NV_NVENC_6_8
#define NV_NVENC_DRV_MAGIC_VALUE  0xB6B70006
#elif NV_NVENC_6_6
#define NV_NVENC_DRV_MAGIC_VALUE  0xB4B70006
#elif NV_NVENC_6_4
#define NV_NVENC_DRV_MAGIC_VALUE  0xC3B70006
#elif NV_NVENC_6_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC2B70006
#elif NV_NVENC_6_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xc1b70006
#elif NV_NVENC_5_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xd0b70006
#elif NV_NVENC_1_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xc0b70006
#elif NV_MSENC_2_0
#define NV_MSENC_DRV_MAGIC_VALUE  0xa0b70006
#else
#define NV_MSENC_DRV_MAGIC_VALUE  0x90b70006
#endif

#define HIST_BLOCK_SIZE  192
// Move all local define of these values to this file
//align to current AV1HIST MAS
const static int AV1_HIST_PIXEL_FRAME_SIZE  =  (8 * 8192);
const static int AV1_HIST_MV_SB_SIZE        =  (256 * 4);

typedef struct
{
  // 256 aligned
  unsigned int                      magic;                  // magic number, see text above              4 bytes
  nvenc_h264_surface_cfg_s          refpic_cfg;             // defines layout of reference pictures     32 bytes
  nvenc_h264_surface_cfg_s          input_cfg;              // defines layout of input pictures         32 bytes
  nvenc_h264_surface_cfg_s          outputpic_cfg;          // defines layout of reconstructed pictures 32 bytes
  nvenc_h264_sps_data_s             sps_data;               //                                           4 bytes
  nvenc_h264_pps_data_s             pps_data;               //                                           8 bytes
  nvenc_h264_rc_s                   rate_control;           // Rate Control information                 88 bytes
  nvenc_h264_pic_control_s          pic_control;            //                                         276 bytes
  nvenc_h264_surface_cfg_s          half_scaled_outputpic_cfg; // defines layout of 2x2 subsampled reconstructed picture 32 bytes
  unsigned int                      gpTimer_timeout_val;    // GPTimer cycle count set from driver       4 bytes

} nvenc_h264_drv_pic_setup_s;                               // 512 bytes



/* nvenc_pic_stat_s - the buffer SET_OUT_ENC_STATUS points at, where the
 * firmware reports ucode_error_status (BAD_MAGIC / INVALID_INPUT / NONE).
 * Same MIT source, taken from further down nvenc_drv.h than the H.264 cut. */
typedef struct
{
  unsigned int   picture_index;                             // value received from SetPictureIndex method
  unsigned int   error_status                       : 2;    // report error if any
  unsigned int   ucode_error_status                 : 30;   // report error status from ucode to driver
  unsigned int   total_bit_count;                           // picture size in bits
  unsigned int   type1_bit_count;                           // type1 bit count for the entire picture
  unsigned short pic_type;                                  // copied from pic_type in nvenc_h264_pic_control_s
  unsigned short num_slices;                                // number of slices produced
  unsigned short ave_activity;                              // report average activity if activity based RC enabled
  unsigned short avgQP;                                     // average QP
  unsigned int   cycle_count;                               // total cycles taked for execute. Written out only when DumpCycleCount in SetControlParams is set
           int   hrdFullness;
  unsigned int   bitstream_start_pos;                       // byte_offset where mpec will start writing slice header
  unsigned int   last_valid_byte_offset;                    // greatest offset in output buffer which is assured to have valid compressed data
  unsigned short intra_mb_count;                            // number of intra MBs.
  unsigned short inter_mb_count;                            // number of inter MBs.
  unsigned int   cumulative_intra_cost;                     // sum of all intra SA(T)Ds
  unsigned int   cumulative_inter_cost;                     // sum of all inter SA(T)Ds
  unsigned int   total_intra_cost;                          // sum of all intra best mode's SATDs, each mb has one intra SATD.
  unsigned int   total_inter_cost;                          // sum of all inter best mode's SATDs, each mb has one inter SATD.
  unsigned int   complexity;                                // sum of mbRowBitcount * current QS
           short average_mvx;                               // average mvx info, can be used for ME hint
           short average_mvy;                               // average mvy info, can be used for ME hint
  unsigned short actual_min_qp_used;                        // actual minimum qp used in the frame
  unsigned short actual_max_qp_used;                        // actual maximum qp used in the frame
  unsigned int  hw_perf_me_sps;                             // me slice per second
  unsigned int  hw_perf_me_fps;                             // me frame per second
  unsigned int  hw_perf_mpec_active_cnt;                    // mpec active count
  unsigned int  hw_perf_mpeb_active_cnt;                    // mpeb active count
  unsigned long long int total_dist_ssd;                    // sum of all ssd distortion of the MB best mode
  unsigned int  total_dist_ssim;                            // sum of all ssim distortion of the MB best mode
  unsigned int  best_satd_l0;                               // sum of all best satd cost of L0
  unsigned int  best_satd_l1;                               // sum of all best satd cost of L1
  unsigned int reserved[5];                                 // align to 128 bytes

} nvenc_pic_stat_s;                                         // 128 bytes

#endif /* NVENC_DRV_H */
