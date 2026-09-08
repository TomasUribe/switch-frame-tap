/* capture.h - thin wrapper over the grc:d game-recording service. */
#ifndef SWITCH_STREAM_CAPTURE_H
#define SWITCH_STREAM_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* grc:d limits. A 720p IDR at ~8 Mbps stays well under 384 KiB. */
#define CAP_VIDEO_BUF   0x60000
#define CAP_AUDIO_BUF   0x2000

int  cap_init(void);
void cap_exit(void);

/* Blocking read of one batch of H.264 Annex-B NAL units.
 * Returns byte count (>0), 0 if nothing was available (retry), <0 on hard error.
 * *ts  = source timestamp in nanoseconds.
 * *key = true if the batch contains an SPS or IDR NAL. */
int cap_read_video(void *buf, size_t cap, uint64_t *ts, bool *key);

/* Blocking read of one chunk of interleaved S16 stereo 48 kHz PCM.
 * Same return convention. */
int cap_read_audio(void *buf, size_t cap, uint64_t *ts);

#endif
