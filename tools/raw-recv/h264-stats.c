/*
 * h264-stats - how well the console's encoder uses its P frames: decodes a
 * recording (MP4 from the viewer's R) with motion-vector export and reports,
 * per GOP and overall: I and P frame sizes, how much of each P frame is
 * predicted from the previous picture (blocks with a motion vector) and how
 * far the vectors reach. Bitrate-tuning evidence (v0.6).
 *
 *   h264-stats FILE.mp4 [--frames N] [--per-frame]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>

/* v0.6: the slice QP from the first slice header of an MP4 sample (4-byte
 * NAL lengths). Written for the console's stream: CABAC, frame MBs only, POC
 * type 2, 8-bit frame_num, one reference, no list modification or MMCO, and
 * pic_init_qp 26 (its PPS). -1 if the header is not what that expects. */
typedef struct { const uint8_t *d; size_t n, pos; } br_t;
static int br_u(br_t *b, int k) { int v = 0; while (k--) { if (b->pos >= b->n * 8) return -1; v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1); b->pos++; } return v; }
static int br_ue(br_t *b) { int z = 0; while (br_u(b, 1) == 0) { if (++z > 20) return -1; } return (1 << z) - 1 + br_u(b, z); }
static int br_se(br_t *b) { const int k = br_ue(b); return (k & 1) ? (k + 1) / 2 : -(k / 2); }

static int slice_qp(const uint8_t *s, int n)
{
    int off = 0;
    while (off + 4 < n) {
        const int len = (s[off] << 24) | (s[off + 1] << 16) | (s[off + 2] << 8) | s[off + 3];
        const uint8_t *nal = s + off + 4;
        const int type = nal[0] & 0x1F, ref_idc = (nal[0] >> 5) & 3;
        if (type == 1 || type == 5) {
            uint8_t rb[64]; size_t rn = 0;
            for (int i = 1; i < len && rn < sizeof(rb); i++) {   /* drop emulation-prevention bytes */
                if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
                rb[rn++] = nal[i];
            }
            br_t b = { rb, rn, 0 };
            br_ue(&b);                              /* first_mb_in_slice */
            const int st = br_ue(&b) % 5;           /* slice_type: 0 P, 2 I */
            br_ue(&b);                              /* pps id */
            br_u(&b, 8);                            /* frame_num */
            if (type == 5) br_ue(&b);               /* idr_pic_id */
            if (st == 0) {
                if (br_u(&b, 1)) br_ue(&b);         /* num_ref_idx_active_override */
                if (br_u(&b, 1)) return -1;         /* ref_pic_list_modification: not expected */
            }
            if (ref_idc) {
                if (type == 5) { br_u(&b, 1); br_u(&b, 1); }
                else if (br_u(&b, 1)) return -1;    /* MMCO: not expected */
            }
            if (st != 2) br_ue(&b);                 /* cabac_init_idc */
            return 26 + br_se(&b);
        }
        off += 4 + len;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s FILE.mp4 [--frames N] [--per-frame]\n", argv[0]); return 2; }
    long max_frames = 0; int per = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--per-frame")) per = 1;
    }
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot open\n"); return 1; }
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    const AVCodec *c = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
    AVCodecContext *d = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(d, fc->streams[vs]->codecpar);
    d->thread_count = 1;
    AVDictionary *o = NULL;
    av_dict_set(&o, "flags2", "+export_mvs", 0);
    av_dict_set(&o, "export_side_data", "+venc_params", 0);   /* per-block QP */
    if (avcodec_open2(d, c, &o) < 0) return 1;
    const int mbs = ((d->width + 15) / 16) * ((d->height + 15) / 16);
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    /* packet sizes in decode order; with no B frames, output order is the same */
    int *sizes = calloc(1 << 20, sizeof(int));
    int *qps = calloc(1 << 20, sizeof(int));
    long np = 0, nf = 0;
    double i_bytes = 0, p_bytes = 0, p_cover = 0, p_zero = 0, p_mag = 0, i_qp = 0, p_qp = 0; long ni = 0, npf = 0, i_qn = 0, p_qn = 0;
    while (av_read_frame(fc, p) >= 0 && (!max_frames || nf < max_frames)) {
        if (p->stream_index != vs) { av_packet_unref(p); continue; }
        if (np < (1 << 20)) { qps[np] = slice_qp(p->data, p->size); sizes[np++] = p->size; }
        avcodec_send_packet(d, p);
        av_packet_unref(p);
        while (avcodec_receive_frame(d, f) == 0) {
            const int sz = nf < np ? sizes[nf] : 0;
            const int key = f->pict_type == AV_PICTURE_TYPE_I;   /* FFmpeg 4.4 has no AV_FRAME_FLAG_KEY */
            double cover = 0, zero = 0, mag = 0; long nmv = 0;
            const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_MOTION_VECTORS);
            if (sd) {
                const AVMotionVector *mv = (const AVMotionVector *)sd->data;
                const int n = sd->size / sizeof(*mv);
                for (int k = 0; k < n; k++) {
                    const double area = (double)mv[k].w * mv[k].h;
                    cover += area;
                    const double dx = (double)mv[k].motion_x / mv[k].motion_scale, dy = (double)mv[k].motion_y / mv[k].motion_scale;
                    const double m = sqrt(dx * dx + dy * dy);
                    if (m < 0.01) zero += area;
                    mag += m * area;
                    nmv++;
                }
            }
            const double px = (double)mbs * 256;
            /* the QP the blocks were actually coded at (the slice header only
             * carries the setup's starting QP, 24 - the engine moves from it
             * with per-block deltas) */
            double fq = -1;
            const AVFrameSideData *ep = av_frame_get_side_data(f, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
            if (ep) {
                const AVVideoEncParams *vp = (const AVVideoEncParams *)ep->data;
                double sum = 0;
                for (unsigned k = 0; k < vp->nb_blocks; k++) sum += vp->qp + av_video_enc_params_block((AVVideoEncParams *)vp, k)->delta_qp;
                fq = vp->nb_blocks ? sum / vp->nb_blocks : vp->qp;
            }
            if (fq >= 0) { if (key) { i_qp += fq; i_qn++; } else { p_qp += fq; p_qn++; } }
            if (key) { i_bytes += sz; ni++; }
            else { p_bytes += sz; npf++; p_cover += cover / px; p_zero += cover > 0 ? zero / cover : 0; p_mag += cover > 0 ? mag / cover : 0; }
            if (per) printf("%5ld %c %7d B  predicted %5.1f%%  zero-vector %5.1f%%  avg |mv| %5.1f px\n", nf, key ? 'I' : 'P', sz,
                            100 * cover / px, cover > 0 ? 100 * zero / cover : 0, cover > 0 ? mag / cover : 0);
            nf++;
        }
    }
    const double fps = 60.0;
    printf("%s: %dx%d, %ld frames (%ld I, %ld P)\n", argv[1], d->width, d->height, nf, ni, npf);
    if (ni) printf("I frames: avg %.0f KB, slice QP %.1f\n", i_bytes / ni / 1024, i_qn ? i_qp / i_qn : -1.0);
    if (npf) {
        printf("P frames: avg %.0f KB (%.0f%% of an I frame), slice QP %.1f\n", p_bytes / npf / 1024, ni ? 100 * (p_bytes / npf) / (i_bytes / ni) : 0, p_qn ? p_qp / p_qn : -1.0);
        printf("P frames: %.1f%% of the picture predicted from the previous frame (the rest coded from scratch),\n"
               "          %.1f%% of that with a zero vector, avg vector %.1f px\n",
               100 * p_cover / npf, 100 * p_zero / npf, p_mag / npf);
    }
    printf("average %.1f Mbps at %.0f fps\n", (i_bytes + p_bytes) * 8 / (double)nf * fps / 1e6, fps);
    return 0;
}
