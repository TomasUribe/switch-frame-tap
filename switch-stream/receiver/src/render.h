/* render.h - SDL2 video output + audio playback, tuned for low latency. */
#ifndef SWITCH_STREAM_RENDER_H
#define SWITCH_STREAM_RENDER_H

#include <libavutil/frame.h>

typedef struct render render_t;

render_t *render_open(int vsync, int fullscreen, int audio_on);

/* Called once from the HELLO packet. */
void render_configure_audio(render_t *r, int rate, int channels);

void render_frame(render_t *r, AVFrame *f);              /* upload + present */
void render_audio(render_t *r, const void *buf, int bytes);

/* Pump SDL events. Returns 1 when the user asked to quit, else 0. */
int  render_pump(render_t *r);

void render_close(render_t *r);

#endif
