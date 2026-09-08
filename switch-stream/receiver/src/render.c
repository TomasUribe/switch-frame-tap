#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <libavutil/pixdesc.h>

struct render {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    int tex_w, tex_h;
    Uint32 tex_fmt;

    int fullscreen;

    /* audio */
    int audio_on;
    SDL_AudioDeviceID adev;
    int arate, ach;
    Uint32 amax_bytes;      /* drop threshold */

    /* stats */
    Uint32 t0;
    int frames;
};

render_t *render_open(int vsync, int fullscreen, int audio_on)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS |
                 (audio_on ? SDL_INIT_AUDIO : 0)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_SetHint(SDL_HINT_RENDER_VSYNC, vsync ? "1" : "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   /* linear */

    struct render *r = calloc(1, sizeof(*r));
    r->audio_on = audio_on;
    r->fullscreen = fullscreen;

    r->win = SDL_CreateWindow("switch-stream",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_RESIZABLE |
        (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!r->win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); goto fail; }

    r->ren = SDL_CreateRenderer(r->win, -1,
        SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!r->ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); goto fail; }

    SDL_RenderSetLogicalSize(r->ren, 1280, 720);
    r->t0 = SDL_GetTicks();
    return r;

fail:
    render_close(r);
    return NULL;
}

void render_configure_audio(render_t *r, int rate, int channels)
{
    if (!r->audio_on || r->adev) return;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = 512;                    /* ~10.7 ms at 48 kHz */
    want.callback = NULL;                  /* use SDL_QueueAudio */

    r->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!r->adev) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return; }

    r->arate = have.freq;
    r->ach = have.channels;
    /* keep at most ~150 ms queued; beyond that we flush to stay in sync */
    r->amax_bytes = (Uint32)(have.freq * have.channels * 2 * 0.15);
    SDL_PauseAudioDevice(r->adev, 0);
}

void render_audio(render_t *r, const void *buf, int bytes)
{
    if (!r->adev || bytes <= 0) return;
    if (SDL_GetQueuedAudioSize(r->adev) > r->amax_bytes)
        SDL_ClearQueuedAudio(r->adev);
    SDL_QueueAudio(r->adev, buf, (Uint32)bytes);
}

static Uint32 sdl_fmt_for(enum AVPixelFormat f)
{
    switch (f) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: return SDL_PIXELFORMAT_IYUV;
        case AV_PIX_FMT_NV12:     return SDL_PIXELFORMAT_NV12;
        default:                  return 0;
    }
}

void render_frame(render_t *r, AVFrame *f)
{
    Uint32 fmt = sdl_fmt_for(f->format);
    if (fmt == 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "render: unsupported pixfmt %s; "
                            "run without --hw or add a swscale path\n",
                    av_get_pix_fmt_name(f->format));
        }
        return;
    }

    if (!r->tex || r->tex_w != f->width || r->tex_h != f->height || r->tex_fmt != fmt) {
        if (r->tex) SDL_DestroyTexture(r->tex);
        r->tex = SDL_CreateTexture(r->ren, fmt, SDL_TEXTUREACCESS_STREAMING,
                                   f->width, f->height);
        r->tex_w = f->width; r->tex_h = f->height; r->tex_fmt = fmt;
        SDL_RenderSetLogicalSize(r->ren, f->width, f->height);
    }

    if (fmt == SDL_PIXELFORMAT_IYUV) {
        SDL_UpdateYUVTexture(r->tex, NULL,
            f->data[0], f->linesize[0],
            f->data[1], f->linesize[1],
            f->data[2], f->linesize[2]);
    } else {
#if SDL_VERSION_ATLEAST(2, 0, 16)
        SDL_UpdateNVTexture(r->tex, NULL,
            f->data[0], f->linesize[0],
            f->data[1], f->linesize[1]);
#else
        static int warned = 0;
        if (!warned) { warned = 1; fprintf(stderr, "render: SDL too old for NV12\n"); }
        return;
#endif
    }

    SDL_RenderClear(r->ren);
    SDL_RenderCopy(r->ren, r->tex, NULL, NULL);
    SDL_RenderPresent(r->ren);

    if (++r->frames >= 120) {
        Uint32 now = SDL_GetTicks();
        double fps = r->frames * 1000.0 / (now - r->t0);
        Uint32 aq = r->adev ? SDL_GetQueuedAudioSize(r->adev) : 0;
        fprintf(stderr, "\r%.1f fps  audio_queue=%u B   ", fps, aq);
        fflush(stderr);
        r->frames = 0;
        r->t0 = now;
    }
}

int render_pump(render_t *r)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 1;
        if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    return 1;
                case SDLK_f:
                    r->fullscreen = !r->fullscreen;
                    SDL_SetWindowFullscreen(r->win,
                        r->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                case SDLK_s:
                    fprintf(stderr, "\n[stats] tex %dx%d fmt=%u audio_queue=%u B\n",
                            r->tex_w, r->tex_h, r->tex_fmt,
                            r->adev ? SDL_GetQueuedAudioSize(r->adev) : 0);
                    break;
                default: break;
            }
        }
    }
    return 0;
}

void render_close(render_t *r)
{
    if (!r) return;
    if (r->adev) SDL_CloseAudioDevice(r->adev);
    if (r->tex)  SDL_DestroyTexture(r->tex);
    if (r->ren)  SDL_DestroyRenderer(r->ren);
    if (r->win)  SDL_DestroyWindow(r->win);
    SDL_Quit();
    free(r);
}
