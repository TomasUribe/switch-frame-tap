/* decode.h - low-latency H.264 decode via libavcodec. */
#ifndef SWITCH_STREAM_DECODE_H
#define SWITCH_STREAM_DECODE_H

#include <stdint.h>
#include <libavcodec/avcodec.h>

typedef struct decoder decoder_t;

/* cb is called synchronously from decoder_submit for each finished frame.
 * The AVFrame is owned by the decoder; copy what you need or av_frame_ref it. */
typedef void (*decoder_frame_cb)(void *user, AVFrame *frame);

decoder_t *decoder_open(int try_hw, decoder_frame_cb cb, void *user);

/* Feed one access unit (1+ Annex-B NAL units). Returns 0 ok, <0 on decode error. */
int  decoder_submit(decoder_t *d, const uint8_t *data, int len, int64_t pts_ns);

void decoder_close(decoder_t *d);

#endif
