/* switch-stream-recv - decode and display the Switch stream with minimal buffering.
 *
 *   ./switch-stream-recv --usb
 *   ./switch-stream-recv --tcp <console-ip> [--port 9899]
 *
 * options: --vsync --hw --buffer N --no-audio --fullscreen
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <libavutil/frame.h>
#include <SDL2/SDL.h>

#include "proto.h"
#include "source.h"
#include "decode.h"
#include "render.h"

#define PAYLOAD_CAP 0x60000   /* must match the sender's CAP_VIDEO_BUF */

struct opts {
    int use_usb;
    const char *host;
    int port;
    int vsync, hw, fullscreen, audio;
    int buffer_frames;
};

/* deliberate jitter buffer (default 0 = present as soon as decoded) */
struct sink {
    render_t *r;
    AVFrame  *q[8];
    int       qn;
    int       max;   /* buffer_frames */
};

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int s) { (void)s; g_quit = 1; }

static void on_frame(void *user, AVFrame *f)
{
    struct sink *k = user;
    if (k->qn == (int)(sizeof(k->q) / sizeof(k->q[0]))) {
        /* overflow guard: render+drop the oldest */
        render_frame(k->r, k->q[0]);
        av_frame_free(&k->q[0]);
        memmove(&k->q[0], &k->q[1], (k->qn - 1) * sizeof(k->q[0]));
        k->qn--;
    }
    AVFrame *c = av_frame_alloc();
    av_frame_ref(c, f);
    k->q[k->qn++] = c;

    while (k->qn > k->max) {
        render_frame(k->r, k->q[0]);
        av_frame_free(&k->q[0]);
        memmove(&k->q[0], &k->q[1], (k->qn - 1) * sizeof(k->q[0]));
        k->qn--;
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s (--usb | --tcp HOST) [--port N] [--vsync] [--hw]\n"
        "          [--buffer N] [--no-audio] [--fullscreen]\n", p);
}

int main(int argc, char **argv)
{
    struct opts o = { .port = SW_PROTO_PORT, .audio = 1 };

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--usb"))        o.use_usb = 1;
        else if (!strcmp(argv[i], "--tcp") && i+1 < argc) o.host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1 < argc) o.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vsync"))      o.vsync = 1;
        else if (!strcmp(argv[i], "--hw"))         o.hw = 1;
        else if (!strcmp(argv[i], "--buffer") && i+1 < argc) o.buffer_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio"))   o.audio = 0;
        else if (!strcmp(argv[i], "--fullscreen")) o.fullscreen = 1;
        else { usage(argv[0]); return 2; }
    }
    if (o.use_usb == (o.host != NULL)) { usage(argv[0]); return 2; }
    if (o.buffer_frames < 0 || o.buffer_frames > 6) o.buffer_frames = 0;

    signal(SIGINT, on_sigint);

    render_t *r = render_open(o.vsync, o.fullscreen, o.audio);
    if (!r) return 1;

    struct sink sink = { .r = r, .max = o.buffer_frames };
    decoder_t *dec = decoder_open(o.hw, on_frame, &sink);
    if (!dec) { render_close(r); return 1; }

    uint8_t *buf = malloc(PAYLOAD_CAP);

    while (!g_quit) {
        source_t *s = o.use_usb ? source_open_usb()
                                : source_open_tcp(o.host, o.port);
        if (!s) {
            if (render_pump(r) || g_quit) break;
            fprintf(stderr, "waiting for source...\n");
            SDL_Delay(1000);
            continue;
        }
        fprintf(stderr, "connected (%s)\n", o.use_usb ? "usb" : "tcp");

        int seen_key = 0;

        for (;;) {
            if (render_pump(r)) { g_quit = 1; break; }

            sw_hdr_t h;
            int rc = source_read(s, &h, buf, PAYLOAD_CAP);
            if (rc < 0) { fprintf(stderr, "\nsource lost\n"); break; }
            if (rc == 0) continue;

            if (h.type == SW_PKT_HELLO && h.size >= sizeof(sw_hello_t)) {
                sw_hello_t hi;
                memcpy(&hi, buf, sizeof(hi));
                fprintf(stderr, "stream: %ux%u @ %u/%u fps, audio %u Hz x%u\n",
                        hi.width, hi.height, hi.fps_num, hi.fps_den,
                        hi.audio_rate, hi.audio_channels);
                render_configure_audio(r, hi.audio_rate, hi.audio_channels);
            }
            else if (h.type == SW_PKT_VIDEO) {
                if (!seen_key) {
                    if (!(h.flags & SW_FLAG_KEYFRAME)) continue;
                    seen_key = 1;
                }
                decoder_submit(dec, buf, (int)h.size, (int64_t)h.ts);
            }
            else if (h.type == SW_PKT_AUDIO) {
                render_audio(r, buf, (int)h.size);
            }
        }

        source_close(s);
        /* drop any queued frames so we don't show stale video after a reconnect */
        for (int i = 0; i < sink.qn; i++) av_frame_free(&sink.q[i]);
        sink.qn = 0;

        if (g_quit) break;
        SDL_Delay(700);
    }

    free(buf);
    decoder_close(dec);
    render_close(r);
    return 0;
}
