/*
 * mp4-check - checks a recording made with the viewer's R key (v0.5): opens it
 * with libavformat, decodes every video and audio frame, and reports the
 * streams, frame counts, decode errors, duration, the video frame rate and the
 * audio's dominant pitch (zero crossings - the PC tests record a 440 Hz tone).
 *
 *   mp4-check FILE.mp4 [--expect-tone 440]      exit 0 = everything decoded
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s FILE.mp4 [--expect-tone HZ]\n", argv[0]); return 2; }
    double expect_tone = 0;
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--expect-tone")) expect_tone = atof(argv[i + 1]);
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    if (avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "no stream info\n"); return 1; }
    AVCodecContext *dec[8] = {0};
    long pkts[8] = {0}, frames[8] = {0}, errs[8] = {0};
    int64_t first_pts[8], last_pts[8];
    for (unsigned i = 0; i < fc->nb_streams && i < 8; i++) {
        AVCodecParameters *p = fc->streams[i]->codecpar;
        first_pts[i] = INT64_MIN; last_pts[i] = INT64_MIN;
        const AVCodec *c = avcodec_find_decoder(p->codec_id);
        printf("stream %u: %s %s", i, av_get_media_type_string(p->codec_type), c ? c->name : "?");
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) printf(" %dx%d", p->width, p->height);
        if (p->codec_type == AVMEDIA_TYPE_AUDIO) printf(" %d Hz", p->sample_rate);
        printf("\n");
        if (!c) continue;
        dec[i] = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec[i], p);
        dec[i]->thread_count = 1;
        if (avcodec_open2(dec[i], c, NULL) < 0) { avcodec_free_context(&dec[i]); }
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    /* zero crossings with hysteresis (+-0.02): the encoder's near-silent
     * priming and quiet noise would otherwise count as crossings */
    long crossings = 0; double a_samples = 0; int side = 0;
    double idx = 0, first_x = -1, last_x = -1;   /* sample positions of the first and last crossing */
    while (av_read_frame(fc, pkt) >= 0) {
        const int s = pkt->stream_index;
        if (s < 8 && dec[s]) {
            pkts[s]++;
            if (pkt->pts != AV_NOPTS_VALUE) { if (first_pts[s] == INT64_MIN) first_pts[s] = pkt->pts; last_pts[s] = pkt->pts; }
            if (avcodec_send_packet(dec[s], pkt) < 0) errs[s]++;
            while (avcodec_receive_frame(dec[s], frm) == 0) {
                frames[s]++;
                if (frm->decode_error_flags) errs[s]++;
                if (dec[s]->codec_type == AVMEDIA_TYPE_AUDIO && frm->format == AV_SAMPLE_FMT_FLTP) {
                    const float *l = (const float *)frm->data[0];
                    for (int k = 0; k < frm->nb_samples; k++) {
                        const int now = l[k] > 0.02f ? 1 : (l[k] < -0.02f ? -1 : 0);
                        if (now != 0 && side != 0 && now != side) { crossings++; if (first_x < 0) first_x = idx; last_x = idx; }
                        if (now != 0) side = now;
                        idx++;
                    }
                    a_samples += frm->nb_samples;
                }
            }
        }
        av_packet_unref(pkt);
    }
    int bad = 0;
    for (unsigned i = 0; i < fc->nb_streams && i < 8; i++) {
        const AVRational tb = fc->streams[i]->time_base;
        const double dur = (last_pts[i] != INT64_MIN) ? (double)(last_pts[i] - first_pts[i]) * tb.num / tb.den : 0;
        printf("stream %u: %ld packets, %ld frames decoded, %ld errors, %.2f s", i, pkts[i], frames[i], errs[i], dur);
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && dur > 0) printf(", %.1f fps", (pkts[i] - 1) / dur);
        printf("\n");
        if (errs[i] || (pkts[i] && !frames[i])) bad = 1;
    }
    if (a_samples > 0) {
        const double hz = (crossings > 1 && last_x > first_x) ? (crossings - 1) / 2.0 / ((last_x - first_x) / 48000.0) : 0;
        printf("audio: %.2f s decoded, dominant pitch ~%.0f Hz\n", a_samples / 48000.0, hz);
        if (expect_tone > 0 && (hz < expect_tone * 0.95 || hz > expect_tone * 1.05)) { printf("FAIL: expected ~%.0f Hz\n", expect_tone); bad = 1; }
    } else if (expect_tone > 0) { printf("FAIL: no audio\n"); bad = 1; }
    printf("%s\n", bad ? "FAILED" : "OK");
    av_frame_free(&frm);
    av_packet_free(&pkt);
    for (int i = 0; i < 8; i++) if (dec[i]) avcodec_free_context(&dec[i]);
    avformat_close_input(&fc);
    return bad;
}
