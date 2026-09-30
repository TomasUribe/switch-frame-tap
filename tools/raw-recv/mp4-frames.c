/*
 * mp4-frames - frames out of a recording, for the docs (stills and the
 * README's animated preview): decodes FILE from START to END seconds and
 * writes every Nth frame as a binary PPM, scaled to WIDTH.
 *
 *   mp4-frames FILE START END N WIDTH OUTDIR      -> OUTDIR/f00000.ppm ...
 *   then e.g. python3 (PIL) to make a JPEG or an animated WebP
 */
#include <stdio.h>
#include <stdlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: %s FILE START END N WIDTH OUTDIR\n", argv[0]); return 2; }
    const double t0 = atof(argv[2]), t1 = atof(argv[3]);
    const int every = atoi(argv[4]) > 0 ? atoi(argv[4]) : 1, ow = atoi(argv[5]);
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot open\n"); return 1; }
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) return 1;
    const AVCodec *c = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
    AVCodecContext *d = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(d, fc->streams[vs]->codecpar);
    if (avcodec_open2(d, c, NULL) < 0) return 1;
    const AVRational tb = fc->streams[vs]->time_base;
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    struct SwsContext *sw = NULL;
    int oh = 0, n = 0, written = 0;
    uint8_t *rgb = NULL;
    int64_t first = AV_NOPTS_VALUE;
    while (av_read_frame(fc, p) >= 0) {
        if (p->stream_index != vs) { av_packet_unref(p); continue; }
        avcodec_send_packet(d, p);
        av_packet_unref(p);
        while (avcodec_receive_frame(d, f) == 0) {
            if (first == AV_NOPTS_VALUE) first = f->pts;
            const double t = (double)(f->pts - first) * tb.num / tb.den;
            if (t < t0 || t > t1 || (n++ % every) != 0) continue;
            if (!sw) {
                oh = (int)((double)ow * f->height / f->width + 0.5) & ~1;
                sw = sws_getContext(f->width, f->height, f->format, ow, oh, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
                /* the console's picture is BT.709, limited range */
                sws_setColorspaceDetails(sw, sws_getCoefficients(SWS_CS_ITU709), 0, sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);
                rgb = malloc((size_t)ow * oh * 3);
            }
            uint8_t *dst[1] = { rgb };
            int ls[1] = { ow * 3 };
            sws_scale(sw, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, ls);
            char path[1024];
            snprintf(path, sizeof(path), "%s/f%05d.ppm", argv[6], written++);
            FILE *o = fopen(path, "wb");
            if (!o) return 1;
            fprintf(o, "P6\n%d %d\n255\n", ow, oh);
            fwrite(rgb, 1, (size_t)ow * oh * 3, o);
            fclose(o);
        }
    }
    printf("%d frames written (%dx%d)\n", written, ow, oh);
    return 0;
}
