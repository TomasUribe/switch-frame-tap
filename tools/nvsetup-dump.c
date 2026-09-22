/* nvsetup-dump - print an NVENC H.264 drv_pic_setup captured by the M72 grc
 * recorder, field by field, using NVIDIA's own nvenc_drv.h (MIT, vendored in
 * the sysmodule source) so the bitfield layout is exact rather than hand-copied.
 *
 *   cc -O2 -o tools/nvsetup-dump tools/nvsetup-dump.c \
 *      -I tier4/applet-mitm/source
 *   tools/nvsetup-dump setup_0.bin
 *
 * The blob is 0x1000 bytes read from the IOVA grc wrote to SET_IN_DRV_PIC_SETUP.
 * The control arrays (slice/ME/MD/quant) live after the 512-byte header at the
 * offsets pic_control names, and are decoded from there.
 *
 * Built for the host. x86-64 and AArch64 GCC lay out these bitfields the same
 * way (little-endian, allocation within the declared unit), and the size
 * asserts below catch it if that ever stops being true.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvenc_drv_h264.h"

_Static_assert(sizeof(nvenc_h264_drv_pic_setup_s) == 512, "drv_pic_setup layout");
_Static_assert(sizeof(nvenc_h264_slice_control_s) == 128, "slice_control layout");
_Static_assert(sizeof(nvenc_h264_me_control_s) == 192, "me_control layout");
_Static_assert(sizeof(nvenc_h264_md_control_s) == 128, "md_control layout");
_Static_assert(sizeof(nvenc_h264_quant_control_s) == 192, "quant_control layout");

#define P(fmt, x) printf("  %-44s " fmt "\n", #x, (x))

static void surf(const char *name, const nvenc_h264_surface_cfg_s *c)
{
    printf(" %s: %ux%u pitch=%u pitch_c=%u trans=%u  luma@%#x/%#x chroma@%#x/%#x"
           "  block_height=%u tiled16=%u mem_mode=%u nv21=%u bl_mode=%u\n",
           name, c->frame_width_minus1 + 1u, c->frame_height_minus1 + 1u,
           c->sfc_pitch, c->sfc_pitch_chroma, c->sfc_trans_mode,
           c->luma_top_frm_offset, c->luma_bot_offset,
           c->chroma_top_frm_offset, c->chroma_bot_offset,
           c->block_height, c->tiled_16x16, c->memory_mode, c->nv21_enable, c->input_bl_mode);
}

static void hex(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("    %04zx:", i);
        for (size_t j = i; j < i + 16 && j < n; j++) printf(" %02x", p[j]);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s setup_N.bin\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static unsigned char buf[0x1000];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < sizeof(nvenc_h264_drv_pic_setup_s)) { fprintf(stderr, "short file (%zu B)\n", n); return 1; }

    const nvenc_h264_drv_pic_setup_s *s = (const void *)buf;
    printf("== %s (%zu B) ==\n", argv[1], n);
    printf("  magic                                        %#010x  (5.0=0xd0b70006 6.0=0xc1b70006)\n", s->magic);
    surf("input_cfg  ", &s->input_cfg);
    surf("refpic_cfg ", &s->refpic_cfg);
    surf("output_cfg ", &s->outputpic_cfg);
    surf("half_scaled", &s->half_scaled_outputpic_cfg);

    printf(" sps:\n");
    P("%u", s->sps_data.profile_idc);             P("%u", s->sps_data.level_idc);
    P("%u", s->sps_data.chroma_format_idc);       P("%u", s->sps_data.pic_order_cnt_type);
    P("%u", s->sps_data.log2_max_frame_num_minus4);
    P("%u", s->sps_data.log2_max_pic_order_cnt_lsb_minus4);
    P("%u", s->sps_data.frame_mbs_only);

    printf(" pps:\n");
    P("%u", s->pps_data.pic_param_set_id);        P("%u", s->pps_data.entropy_coding_mode_flag);
    P("%u", s->pps_data.num_ref_idx_l0_active_minus1);
    P("%d", s->pps_data.pic_init_qp_minus26);     P("%d", s->pps_data.chroma_qp_index_offset);
    P("%u", s->pps_data.deblocking_filter_control_present_flag);
    P("%u", s->pps_data.transform_8x8_mode_flag); P("%u", s->pps_data.constrained_intra_pred_flag);

    const nvenc_h264_rc_s *rc = &s->rate_control;
    printf(" rate_control:\n");
    P("%u", rc->hrd_type);
    printf("  QP[P,B,I]=%u,%u,%u  minQP=%u,%u,%u  maxQP=%u,%u,%u  maxQPD=%u baseQPD=%u\n",
           rc->QP[0], rc->QP[1], rc->QP[2], rc->minQP[0], rc->minQP[1], rc->minQP[2],
           rc->maxQP[0], rc->maxQP[1], rc->maxQP[2], rc->maxQPD, rc->baseQPD);
    printf("  rhopbi=%d,%d,%d\n", rc->rhopbi[0], rc->rhopbi[1], rc->rhopbi[2]);
    P("%d", rc->framerate);  P("%u", rc->buffersize);
    P("%d", rc->nal_cpb_size); P("%d", rc->nal_bitrate); P("%d", rc->vcl_cpb_size); P("%d", rc->vcl_bitrate);
    P("%#x", rc->gop_length); P("%d", rc->Np); P("%d", rc->Bmin); P("%d", rc->Ravg); P("%d", rc->R);
    P("%u", rc->ab_alpha); P("%u", rc->ab_beta); P("%u", rc->aqMode);
    P("%u", rc->single_frame_VBV); P("%u", rc->two_pass_rc); P("%u", rc->rc_class);

    const nvenc_h264_pic_control_s *pc = &s->pic_control;
    printf(" pic_control:\n");
    P("%u", pc->pic_struct); P("%u", pc->pic_type); P("%u", pc->ref_pic_flag);
    P("%u", pc->slice_mode); P("%u", pc->codec); P("%u", pc->frame_num);
    P("%u", pc->pic_order_cnt_lsb); P("%u", pc->idr_pic_id);
    P("%u", pc->max_slice_size); P("%u", pc->max_byte_count_before_resid_zero);
    P("%u", pc->num_forced_slices_minus1); P("%u", pc->num_me_controls_minus1);
    P("%u", pc->num_md_controls_minus1); P("%u", pc->num_q_controls_minus1);
    P("%#x", pc->slice_control_offset); P("%#x", pc->me_control_offset);
    P("%#x", pc->md_control_offset); P("%#x", pc->q_control_offset);
    P("%u", pc->hist_buf_size); P("%u", pc->bitstream_buf_size); P("%u", pc->bitstream_start_pos);
    P("%u", pc->e4byteStartCode); P("%u", pc->qpfifo); P("%u", pc->intraRefreshCount);
    P("%u", pc->mpec_threshold); P("%#x", pc->slice_stat_offset); P("%#x", pc->mpec_stat_offset);
    P("%#x", pc->wp_control_offset); P("%u", pc->bit_depth_minus_8);
    P("%u", pc->enable_source_image_padding); P("%#x", pc->aq_stat_offset);
    P("%#x", pc->act_stat_offset); P("%#x", pc->stats_fifo_offset);
    P("%u", s->gpTimer_timeout_val);

    struct { const char *name; unsigned off; unsigned cnt; size_t sz; } arr[] = {
        { "slice_control", pc->slice_control_offset, pc->num_forced_slices_minus1 + 1u, sizeof(nvenc_h264_slice_control_s) },
        { "me_control",    pc->me_control_offset,    pc->num_me_controls_minus1 + 1u,   sizeof(nvenc_h264_me_control_s) },
        { "md_control",    pc->md_control_offset,    pc->num_md_controls_minus1 + 1u,   sizeof(nvenc_h264_md_control_s) },
        { "quant_control", pc->q_control_offset,     pc->num_q_controls_minus1 + 1u,    sizeof(nvenc_h264_quant_control_s) },
    };
    for (unsigned i = 0; i < 4; i++) {
        size_t end = arr[i].off + arr[i].cnt * arr[i].sz;
        printf(" %s[%u] @ %#x:\n", arr[i].name, arr[i].cnt, arr[i].off);
        if (arr[i].off == 0 || end > n) { printf("    (outside the captured 0x%zx bytes)\n", n); continue; }
        if (i == 0) {
            const nvenc_h264_slice_control_s *sl = (const void *)(buf + arr[i].off);
            printf("    num_mb=%u qp_avr=%u qp_min=%u qp_max=%u force_intra=%u deblock_idc=%u cabac_init=%u me/md/q idx=%u/%u/%u\n",
                   sl->num_mb, sl->qp_avr, sl->qp_slice_min, sl->qp_slice_max, sl->force_intra,
                   sl->disable_deblocking_filter_idc, sl->cabac_init_idc,
                   sl->me_control_idx, sl->md_control_idx, sl->q_control_idx);
        } else if (i == 2) {
            const nvenc_h264_md_control_s *md = (const void *)(buf + arr[i].off);
            printf("    intra4x4=%#x intra8x8=%#x intra16x16=%#x chroma=%#x l0_16x16=%u\n",
                   md->intra_luma4x4_mode_enable, md->intra_luma8x8_mode_enable,
                   md->intra_luma16x16_mode_enable, md->intra_chroma_mode_enable, md->l0_part_16x16_enable);
        }
        hex(buf + arr[i].off, arr[i].cnt * arr[i].sz);
    }
    printf(" raw header:\n");
    hex(buf, 512);
    return 0;
}
