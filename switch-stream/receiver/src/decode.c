#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>

struct decoder {
    const AVCodec     *codec;
    AVCodecContext    *ctx;
    AVPacket          *pkt;
    AVFrame           *frame;      /* decode output (may be hw) */
    AVFrame           *sw;         /* hw->sw download target    */
    enum AVPixelFormat hw_pix_fmt; /* AV_PIX_FMT_NONE if pure sw */
    decoder_frame_cb   cb;
    void              *user;
};

static enum AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_NONE;

static enum AVPixelFormat get_format(AVCodecContext *ctx,
                                     const enum AVPixelFormat *fmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == s_hw_pix_fmt)
            return *p;
    return fmts[0];   /* fall back to software */
}

static int try_setup_hw(struct decoder *d)
{
    static const enum AVHWDeviceType order[] = {
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_NONE,
    };

    for (int i = 0; order[i] != AV_HWDEVICE_TYPE_NONE; i++) {
        for (int j = 0;; j++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(d->codec, j);
            if (!cfg) break;
            if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
            if (cfg->device_type != order[i]) continue;

            AVBufferRef *hw = NULL;
            if (av_hwdevice_ctx_create(&hw, order[i], NULL, NULL, 0) < 0)
                break;   /* type unavailable, try next type */

            d->ctx->hw_device_ctx = hw;
            d->hw_pix_fmt = cfg->pix_fmt;
            s_hw_pix_fmt  = cfg->pix_fmt;
            d->ctx->get_format = get_format;
            fprintf(stderr, "decode: hw accel = %s (%s)\n",
                    av_hwdevice_get_type_name(order[i]),
                    av_get_pix_fmt_name(cfg->pix_fmt));
            return 0;
        }
    }
    fprintf(stderr, "decode: no hw accel available, using software\n");
    return -1;
}

decoder_t *decoder_open(int try_hw, decoder_frame_cb cb, void *user)
{
    av_log_set_level(AV_LOG_ERROR);

    struct decoder *d = calloc(1, sizeof(*d));
    d->cb = cb;
    d->user = user;
    d->hw_pix_fmt = AV_PIX_FMT_NONE;

    d->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!d->codec) { fprintf(stderr, "decode: no H.264 decoder\n"); goto fail; }

    d->ctx = avcodec_alloc_context3(d->codec);
    if (!d->ctx) goto fail;

    /* latency knobs: no reordering, slice- not frame-threading (no pipeline delay) */
    d->ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    d->ctx->thread_type  = FF_THREAD_SLICE;
    d->ctx->thread_count = 4;
    d->ctx->has_b_frames = 0;
    d->ctx->time_base = (AVRational){ 1, 1000000000 };  /* pts in ns */

    if (try_hw)
        try_setup_hw(d);

    if (avcodec_open2(d->ctx, d->codec, NULL) < 0) {
        fprintf(stderr, "decode: avcodec_open2 failed\n");
        goto fail;
    }

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->sw    = av_frame_alloc();
    if (!d->pkt || !d->frame || !d->sw) goto fail;
    return d;

fail:
    decoder_close(d);
    return NULL;
}

int decoder_submit(decoder_t *d, const uint8_t *data, int len, int64_t pts_ns)
{
    d->pkt->data = (uint8_t *)data;   /* consumed within send_packet */
    d->pkt->size = len;
    d->pkt->pts  = pts_ns;
    d->pkt->dts  = pts_ns;

    int rc = avcodec_send_packet(d->ctx, d->pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        char e[128]; av_strerror(rc, e, sizeof(e));
        fprintf(stderr, "decode: send_packet: %s\n", e);
        return rc;
    }

    for (;;) {
        rc = avcodec_receive_frame(d->ctx, d->frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) { fprintf(stderr, "decode: receive_frame failed\n"); return rc; }

        AVFrame *out = d->frame;
        if (d->hw_pix_fmt != AV_PIX_FMT_NONE &&
            d->frame->format == d->hw_pix_fmt) {
            av_frame_unref(d->sw);
            if (av_hwframe_transfer_data(d->sw, d->frame, 0) < 0) {
                fprintf(stderr, "decode: hwframe download failed\n");
                av_frame_unref(d->frame);
                continue;
            }
            av_frame_copy_props(d->sw, d->frame);
            out = d->sw;
        }

        d->cb(d->user, out);
        av_frame_unref(d->frame);
        av_frame_unref(d->sw);
    }
    return 0;
}

void decoder_close(decoder_t *d)
{
    if (!d) return;
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->frame) av_frame_free(&d->frame);
    if (d->sw)    av_frame_free(&d->sw);
    if (d->ctx)   avcodec_free_context(&d->ctx);
    free(d);
}
