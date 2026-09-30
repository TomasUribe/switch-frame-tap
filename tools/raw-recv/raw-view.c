/* raw-view - live viewer for the switch-frame-tap stream.
 *
 * libusb + SDL2 (+ libavcodec for H.264) in ONE process: no pipe, no player,
 * no buffering beyond the frame being drawn. A pipe into an external player
 * would add latency, which is the thing this prototype exists to minimise.
 *
 * The console sends SFTR packets: a 32-byte header in its own USB transfer,
 * then `length` payload bytes. What the payload is depends on hdr.flags:
 *   bit 1 (2)  H.264 Annex-B access unit: SPS + PPS + one IDR slice (M83
 *              nvstream). BT.709 limited-range YCbCr; decoded with libavcodec.
 *   bit 0 (1)  the VIC's packed 4:2:0 RGB (M67 stream): luma = B, chroma
 *              even/odd = R/G at half resolution.
 *   neither    raw pixels, `stride` bytes per row.
 *
 *   make                       # tools/raw-recv/Makefile; H.264 if libavcodec is found
 *   ./raw-view                 # USB, auto-sizes to whatever the header reports
 *   ./raw-view --record s.sft  # also append every packet to a file
 *   ./raw-view --h264 s.h264   # also write the H.264 elementary stream (ffplay s.h264)
 *   ./raw-view --file s.sft    # replay a recorded stream instead of USB
 *   ./raw-view --file s.sft --headless --frames 10   # decode only (tests)
 *   ./raw-view --swap          # flip R and B for a raw stream if colours look wrong
 *   ./raw-view --low-latency   # one decode thread, frames out immediately
 *   ./raw-view --threads 2     # N frame threads: N-1 frames of decoder delay (default 2;
 *                              # 0 = one per core, which cost ~250 ms in M84 Run J)
 *   ./raw-view --file s.sft --paced --seconds 5   # v0.2: a recording through the LIVE
 *                              # pipeline (reader thread, decoder thread, vsync display)
 *                              # game audio (v0.3) plays on the default sound device; M mutes
 *   R records to MP4 (v0.5) in Videos/Switch Frame Tap: the console's H.264 as it
 *   arrived plus the game audio as AAC; --rec-dir DIR saves elsewhere, --mp4 FILE
 *   records into FILE from the start (tests)
 *   ./raw-view --app           # M97: the desktop launcher's mode - the window opens at
 *                              # once, waits for the Switch, reconnects whenever it comes
 *                              # back, and only closes when you close it. F11 or a
 *                              # double-click toggles fullscreen.
 *
 * Decoding: an IDR-only 720p60 stream at QP 20 is ~150-170 Mbps of CABAC,
 * which one core may not keep up with (a 4-vCPU cloud box: 32 ms/frame on one
 * thread, 10 ms/frame with frame threads). So the default uses FFmpeg's frame
 * threads - throughput, at the cost of a few frames of decoder latency.
 * --low-latency trades that back on a fast CPU, or with a higher nvqp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#ifdef _WIN32
/* v0.2: the Windows build (tools/raw-recv/build-windows.sh). libusb's Windows
 * package puts libusb.h at the top of its include dir; SDL must not replace
 * main (the entry point stays mainCRTStartup -> main, GUI subsystem). */
#include <libusb.h>
#define SDL_MAIN_HANDLED
#else
#include <libusb-1.0/libusb.h>
#endif
#include <SDL2/SDL.h>
#ifdef SFT_H264
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#endif

#define SFT_VID 0x1209
#define SFT_PID 0x5F1E
#define SFT_IFACE 0
#define SFT_EP_IN 0x81
#define SFT_MAGIC 0x52544653u
#define SFT_FLAG_PACKED420 1u
#define SFT_FLAG_H264      2u
#define SFT_FLAG_KEY       4u   /* M85: an IDR access unit (H.264 only) */
#define SFT_FLAG_AUDIO     8u   /* v0.3: game audio, 16-bit stereo PCM (width = rate, height = channels) */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic; uint16_t version; uint16_t flags;
    uint32_t width, height, stride, length, block_h_log2, kind;
} sft_hdr_t;
#pragma pack(pop)

static volatile sig_atomic_t g_quit = 0;
static int g_app = 0;               /* --app: survive the console leaving the bus */
static int g_lost = 0;              /* --app: the console left; wait for it again */
static int g_paced = 0;             /* --paced: a recording fed through the live pipeline at 60 fps, looped (tests) */
static double g_run_secs = 0;       /* --seconds: stop the live pipeline after this long (tests) */
static const char *g_mp4 = NULL;     /* --mp4 FILE: record into FILE from the start (tests) */
static const char *g_rec_dir = NULL; /* --rec-dir DIR: where R saves (default Videos/Switch Frame Tap) */

/* ---- v0.3: game audio ------------------------------------------------------
 * The console sends the sound since the last frame as its own packet before
 * each frame (~16 ms, 3 KB). The reader thread queues it on the sound device
 * directly: playback starts once ~40 ms are buffered (and waits for that again
 * after running dry), and if more than ~120 ms pile up the queue is cleared,
 * so the sound cannot drift behind the picture. M mutes. */
static SDL_AudioDeviceID g_adev = 0;
static int g_a_paused = 1, g_mute = 0;
static volatile long g_a_bytes = 0, g_a_resyncs = 0, g_a_underruns = 0;

static void audio_packet(const uint8_t *d, uint32_t n, uint32_t rate, uint32_t ch)
{
    if (!g_adev || g_mute || rate != 48000 || ch != 2) return;
    const Uint32 bps = 48000 * 4;
    Uint32 q = SDL_GetQueuedAudioSize(g_adev);
    if (!g_a_paused && q == 0) {               /* ran dry: build the cushion again */
        SDL_PauseAudioDevice(g_adev, 1);
        g_a_paused = 1;
        g_a_underruns++;
    }
    if (q > bps * 120 / 1000) {                /* too far behind the picture: start over */
        SDL_ClearQueuedAudio(g_adev);
        q = 0;
        g_a_resyncs++;
    }
    SDL_QueueAudio(g_adev, d, n);
    g_a_bytes += n;
    if (g_a_paused && q + n >= bps * 40 / 1000) {
        SDL_PauseAudioDevice(g_adev, 0);
        g_a_paused = 0;
    }
}
static void on_sigint(int s) { (void)s; g_quit = 1; }
#include "record.h"            /* v0.5: R records the stream to MP4 */
#include "menu.h"              /* v0.5: the main screen while there is no picture */
static libusb_device_handle *g_usb = NULL;
static FILE *g_in = NULL;           /* --file */

/* Read exactly n bytes from USB (tolerating short bulk transfers and idle
 * timeouts) or from the replay file. 0 = idle, <0 = error, n = done. */
static int read_exact(uint8_t *dst, size_t n, int ms)
{
    if (g_in) {
        size_t got = fread(dst, 1, n, g_in);
        if (got == 0 && g_paced) { rewind(g_in); got = fread(dst, 1, n, g_in); }
        if (got != n) { g_quit = 1; return -3; }
        return (int)n;
    }
    size_t done = 0;
    while (done < n && !g_quit) {
        int got = 0;
        int chunk = (int)((n - done) > 0x40000 ? 0x40000 : (n - done));
        int rc = libusb_bulk_transfer(g_usb, SFT_EP_IN, dst + done, chunk, &got, ms);
        if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) {
            if (got == 0 && rc == LIBUSB_ERROR_TIMEOUT) return (done > 0) ? -2 : 0;
            done += (size_t)got;
            continue;
        }
        /* NO_DEVICE means the console left the bus (powered off, rebooted):
         * exit cleanly instead of spinning on errors */
        if (rc == LIBUSB_ERROR_NO_DEVICE) { if (g_app) g_lost = 1; else g_quit = 1; return -3; }
        fprintf(stderr, "bulk read: %s\n", libusb_error_name(rc));
        return -1;
    }
    return g_quit ? -1 : (int)done;
}

/* ---- v0.2: the USB reader thread ------------------------------------------
 * The console keeps one frame transfer outstanding: while the PC is decoding
 * or drawing instead of reading, the console waits, and its whole pipeline
 * with it. On Linux reads and presents are quick enough to hide that; on
 * Windows (WinUSB, Direct3D + the compositor) they were not - the first
 * Windows test ran "a lot" below Linux's frame rate. A thread now does
 * nothing but read packets into this queue, so the cable is always drained;
 * the main thread decodes every packet and draws only the newest frame. */
#define QN 8
typedef struct { sft_hdr_t hdr; uint8_t *data; size_t cap; Uint64 t_hdr; } pkt_t;
static pkt_t g_q[QN];
static int g_qh = 0, g_qn = 0, g_qmax = 0;
static SDL_mutex *g_qm = NULL;
static SDL_cond *g_q_put = NULL, *g_q_get = NULL;
static volatile int g_rd_stop = 0;
static SDL_Thread *g_rd = NULL;

static int reader_main(void *arg)
{
    (void)arg;
    while (!g_quit && !g_rd_stop && !g_lost) {
        sft_hdr_t hdr;
        const int r = read_exact((uint8_t *)&hdr, sizeof(hdr), 100);
        if (r <= 0) continue;
        const Uint64 t = SDL_GetPerformanceCounter();
        if (hdr.magic != SFT_MAGIC) { fprintf(stderr, "bad magic 0x%08x, resyncing\n", hdr.magic); continue; }
        if (hdr.length == 0 || hdr.length > 64u*1024*1024) continue;
        if (hdr.flags & SFT_FLAG_AUDIO) {
            static uint8_t abuf[64 * 1024];
            if (hdr.length > sizeof(abuf)) { static uint8_t sink[4096]; for (uint32_t k = 0; k < hdr.length; k += sizeof(sink)) read_exact(sink, (hdr.length - k) < sizeof(sink) ? (hdr.length - k) : sizeof(sink), 5000); continue; }
            if (read_exact(abuf, hdr.length, 5000) != (int)hdr.length) continue;
            audio_packet(abuf, hdr.length, hdr.width, hdr.height);
            rec_audio(abuf, hdr.length, hdr.width, hdr.height, t);
            continue;
        }
        if (g_paced) SDL_Delay(16);         /* a recording at roughly the console's pace */
        SDL_LockMutex(g_qm);
        while (g_qn == QN && !g_quit && !g_rd_stop) SDL_CondWaitTimeout(g_q_put, g_qm, 100);
        const int slot = (g_qh + g_qn) % QN;
        const int stop = g_quit || g_rd_stop;
        SDL_UnlockMutex(g_qm);
        if (stop) break;
        pkt_t *p = &g_q[slot];
        if (hdr.length > p->cap) {
            /* libavcodec wants readable padding past the end of a packet */
            uint8_t *d = realloc(p->data, (size_t)hdr.length + 64);
            if (!d) break;
            p->data = d; p->cap = hdr.length;
        }
        if (read_exact(p->data, hdr.length, 5000) != (int)hdr.length) continue;
        memset(p->data + hdr.length, 0, 64);
        p->hdr = hdr;
        p->t_hdr = t;
        SDL_LockMutex(g_qm);
        g_qn++;
        if (g_qn > g_qmax) g_qmax = g_qn;
        SDL_CondSignal(g_q_get);
        SDL_UnlockMutex(g_qm);
    }
    return 0;
}

static void reader_start(void)
{
    if (!g_qm) { g_qm = SDL_CreateMutex(); g_q_put = SDL_CreateCond(); g_q_get = SDL_CreateCond(); }
    g_qh = g_qn = 0;
    g_rd_stop = 0;
    g_rd = SDL_CreateThread(reader_main, "usb-reader", NULL);
}

static void reader_stop(void)
{
    if (!g_rd) return;
    g_rd_stop = 1;
    SDL_LockMutex(g_qm); SDL_CondSignal(g_q_put); SDL_UnlockMutex(g_qm);
    SDL_WaitThread(g_rd, NULL);
    g_rd = NULL;
    g_qh = g_qn = 0;
}

/* the oldest queued packet, copied out (waits up to ms); *left = still queued */
static int reader_pop(sft_hdr_t *hdr, uint8_t **payload, size_t *cap, Uint64 *t_hdr, int ms, int *left)
{
    SDL_LockMutex(g_qm);
    if (g_qn == 0) SDL_CondWaitTimeout(g_q_get, g_qm, (Uint32)ms);
    if (g_qn == 0) { SDL_UnlockMutex(g_qm); return 0; }
    pkt_t *p = &g_q[g_qh];
    if (p->hdr.length > *cap) {
        uint8_t *d = realloc(*payload, (size_t)p->hdr.length + 64);
        if (!d) { SDL_UnlockMutex(g_qm); return 0; }
        *payload = d; *cap = p->hdr.length;
    }
    memcpy(*payload, p->data, (size_t)p->hdr.length + 64);
    *hdr = p->hdr;
    *t_hdr = p->t_hdr;
    g_qh = (g_qh + 1) % QN;
    g_qn--;
    *left = g_qn;
    SDL_CondSignal(g_q_put);
    SDL_UnlockMutex(g_qm);
    return 1;
}

static int reader_pending(void)
{
    SDL_LockMutex(g_qm);
    const int n = g_qn;
    SDL_UnlockMutex(g_qm);
    return n;
}

#ifdef SFT_H264
typedef struct { AVCodecContext *c; AVPacket *pkt; AVFrame *frm; } h264_t;

static int h264_open(h264_t *d, int low_latency, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return -1;
    d->c = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    if (!d->c || !d->pkt || !d->frm) return -1;
    if (low_latency) {
        /* every packet is a whole IDR frame: output it the moment it decodes */
        d->c->flags |= AV_CODEC_FLAG_LOW_DELAY;
        d->c->thread_count = 1;
    } else {
        /* frame threading holds back about one frame per extra thread: on a
         * 20-core PC the default (one per core, capped at 16) is ~250 ms */
        d->c->thread_count = threads;        /* 0 = one per core */
        d->c->thread_type = FF_THREAD_FRAME;
    }
    return avcodec_open2(d->c, codec, NULL);
}

/* one access unit in, at most one frame out (1 = frame in d->frm) */
static int h264_decode(h264_t *d, uint8_t *buf, int len, int64_t pts)
{
    d->pkt->data = buf;
    d->pkt->size = len;
    d->pkt->pts = pts;   /* the console's frame number, to pair output with arrival */
    int rc = avcodec_send_packet(d->c, d->pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) return rc;
    rc = avcodec_receive_frame(d->c, d->frm);
    if (rc == AVERROR(EAGAIN)) return 0;
    return rc < 0 ? rc : 1;
}

static void h264_close(h264_t *d)
{
    av_frame_free(&d->frm);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->c);
}
#endif

/* --app: window events while there is nothing to draw (and between frames) */
static void app_events(SDL_Window *win, SDL_Renderer *ren, SDL_Texture *tex)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            /* leave fullscreen first; Esc in a window closes it */
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) SDL_SetWindowFullscreen(win, 0);
            else g_quit = 1;
        } else if ((e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_F11 || e.key.keysym.sym == SDLK_f)) ||
                   (e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2)) {
            const int fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r && !e.key.repeat) {
            rec_toggle();
            char t[256], rs[200];
            rec_status(rs, sizeof(rs));
            snprintf(t, sizeof(t), "Switch Frame Tap%s", rs[0] ? rs : "  |  recording stopped");
            SDL_SetWindowTitle(win, t);
            if (g_menu) menu_draw(ren);
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_m) {
            g_mute = !g_mute;
            if (g_adev) { SDL_ClearQueuedAudio(g_adev); SDL_PauseAudioDevice(g_adev, 1); g_a_paused = 1; }
            fprintf(stderr, "\naudio %s\n", g_mute ? "muted" : "on");
            if (g_menu) menu_draw(ren);
        } else if (e.type == SDL_WINDOWEVENT) {
            /* resized or uncovered with no new frame: draw the last one again */
            if (g_menu) { menu_draw(ren); continue; }
            SDL_RenderClear(ren);
            if (tex) SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
    }
}

/* v0.2: is the Switch on the bus, and if so why can it not be opened? A
 * present device that will not open is a missing driver on Windows (WinUSB,
 * installed once with Zadig) or a missing udev rule on Linux. */
static int switch_on_bus(libusb_context *ctx, int *open_err)
{
    libusb_device **list = NULL;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == SFT_VID && dd.idProduct == SFT_PID) {
            libusb_device_handle *h = NULL;
            *open_err = libusb_open(list[i], &h);
            if (h) libusb_close(h);
            found = 1;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    return found;
}

static void app_title_cannot_open(SDL_Window *win, int err)
{
    char t[256];
#ifdef _WIN32
    snprintf(t, sizeof(t), "Switch Frame Tap - the Switch is connected but has no USB driver: install WinUSB for it once with Zadig (see README.txt)  [%s]", libusb_error_name(err));
#else
    snprintf(t, sizeof(t), "Switch Frame Tap - the Switch is connected but cannot be opened: install the udev rule (see the README)  [%s]", libusb_error_name(err));
#endif
    SDL_SetWindowTitle(win, t);
    g_menu_state = MENU_NODRIVER;
    snprintf(g_menu_err, sizeof(g_menu_err), "%s", libusb_error_name(err));
}

/* --app: open and claim the console, keeping the window alive meanwhile */
static libusb_device_handle *app_wait_device(libusb_context *ctx, SDL_Window *win, SDL_Renderer *ren, SDL_Texture *tex)
{
    SDL_SetWindowTitle(win, "Switch Frame Tap - waiting for the Switch (USB cable, a game running)");
    int said = 0;
    g_menu = 1;
    g_menu_state = MENU_WAITING;
    for (int tick = 0;; tick++) {
        /* v0.5: the menu, redrawn often enough for the keys and the REC timer;
         * the bus is looked at every 250 ms as before */
        app_events(win, ren, tex);
        if (g_quit) return NULL;
        if (tick % 5 != 0) { menu_draw(ren); SDL_Delay(50); continue; }
        libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
        if (h) {
            libusb_set_auto_detach_kernel_driver(h, 1);
            if (libusb_claim_interface(h, SFT_IFACE) == 0) {
                SDL_SetWindowTitle(win, "Switch Frame Tap - connected, waiting for the picture");
                g_menu_state = MENU_CONNECTED;
                menu_draw(ren);
                return h;
            }
            libusb_close(h);
            app_title_cannot_open(win, LIBUSB_ERROR_BUSY);
            said = 1;
        } else {
            int err = 0;
            if (switch_on_bus(ctx, &err)) { app_title_cannot_open(win, err); said = 1; }
            else if (said) { SDL_SetWindowTitle(win, "Switch Frame Tap - waiting for the Switch (USB cable, a game running)"); said = 0; g_menu_state = MENU_WAITING; }
        }
        menu_draw(ren);
        SDL_Delay(50);
    }
}

#ifdef SFT_H264
/* ---- v0.2 test3: the live pipeline - reader -> decoder -> paced display -----
 *
 * Test build 2 (Windows, logs/v020-win-test2): decoding ~0.1-3 ms and drawing
 * ~0.75 ms a frame, yet it stuttered: packets arrived in bursts (the USB
 * queue hit 8 of 8), and "draw only the newest" then threw whole bursts away
 * (472 frames decoded, never shown), while presenting unsynchronised to the
 * display left the compositor to show frames at uneven moments.
 *
 * Now three threads: the reader drains the cable (reader_main), a decoder
 * turns every packet into a frame in a small FIFO, and the display loop shows
 * the frames IN ORDER, one per display refresh (vsync). Only when more than
 * two are waiting does it drop the oldest - a two-frame latency cap instead
 * of throwing a burst away. Statistics are per second, not since the start. */
#define FQ 6
static AVFrame *g_fq[FQ];
static int g_fh = 0, g_fn = 0;
static SDL_mutex *g_fm = NULL;
static SDL_cond *g_f_put = NULL;
static volatile int g_dec_stop = 0;
static Uint64 g_t_rx[256];           /* header arrival per frame number */
static double g_age_ms[256];         /* the console's own age of that frame */

/* counters the decoder writes and the display loop reads (benign races) */
static volatile long s_packets = 0, s_decoded = 0, s_undecoded = 0, s_lost = 0, s_keyframes = 0, s_sessions = 1;
static volatile double s_bytes = 0, s_dec_ms = 0, s_dec_max = 0;

typedef struct { h264_t *dec; FILE *rec; FILE *es; } dec_args_t;

static int decoder_main(void *arg)
{
    dec_args_t *a = arg;
    uint8_t *payload = NULL; size_t cap = 0;
    uint32_t last_kind = 0; int have_kind = 0;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    while (!g_quit && !g_dec_stop) {
        sft_hdr_t hdr; Uint64 t_hdr = 0; int left = 0;
        if (!reader_pop(&hdr, &payload, &cap, &t_hdr, 50, &left)) continue;
        s_packets++;
        s_bytes += hdr.length;
        if (a->rec) { fwrite(&hdr, 1, sizeof(hdr), a->rec); fwrite(payload, 1, hdr.length, a->rec); }
        if (!(hdr.flags & SFT_FLAG_H264)) { s_undecoded++; continue; }   /* the old raw formats: file replay only */
        if (have_kind && hdr.kind > last_kind + 1) s_lost += hdr.kind - last_kind - 1;
        if (have_kind && hdr.kind < last_kind) { s_sessions++; fprintf(stderr, "\nnew stream session %ld (the console reattached)\n", (long)s_sessions); }
        last_kind = hdr.kind; have_kind = 1;
        if (a->es) fwrite(payload, 1, hdr.length, a->es);
        rec_video(&hdr, payload, t_hdr);
        if (hdr.flags & SFT_FLAG_KEY) s_keyframes++;
        g_t_rx[hdr.kind & 255] = t_hdr;
        g_age_ms[hdr.kind & 255] = (hdr.stride != 0 && hdr.stride < 10000000u) ? hdr.stride / 1000.0 : -1.0;
        const Uint64 d0 = SDL_GetPerformanceCounter();
        const int got = h264_decode(a->dec, payload, (int)hdr.length, (int64_t)hdr.kind);
        const double dm = (double)(SDL_GetPerformanceCounter() - d0) * 1000.0 / (double)freq;
        s_dec_ms += dm;
        if (dm > s_dec_max) s_dec_max = dm;
        if (got < 0) { s_undecoded++; continue; }
        if (got == 0) continue;
        s_decoded++;
        SDL_LockMutex(g_fm);
        if (g_fn == FQ) {                      /* the display is far behind: make room */
            av_frame_unref(g_fq[g_fh]);
            g_fh = (g_fh + 1) % FQ;
            g_fn--;
        }
        av_frame_move_ref(g_fq[(g_fh + g_fn) % FQ], a->dec->frm);
        g_fn++;
        SDL_CondSignal(g_f_put);
        SDL_UnlockMutex(g_fm);
    }
    free(payload);
    return 0;
}

static int run_live(libusb_context *ctx, SDL_Window *win, SDL_Renderer *ren, int app,
                    int low_latency, int threads, FILE *rec, FILE *es)
{
    h264_t dec = {0};
    if (h264_open(&dec, low_latency, threads) != 0) { fprintf(stderr, "libavcodec H.264 decoder unavailable\n"); return 1; }
    if (!win) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        win = SDL_CreateWindow("Switch Frame Tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!ren) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    }
    /* v0.3: the sound device (opened paused; audio_packet starts it) */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {0}, have = {0};
        want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 512;
        g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        fprintf(stderr, "audio: %s\n", g_adev ? "48 kHz stereo" : SDL_GetError());
    } else {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
    }
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(ren, &ri) == 0) {
        fprintf(stderr, "renderer: %s%s\n", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? ", vsync" : ", NO vsync");
    }
    for (int i = 0; i < FQ; i++) g_fq[i] = av_frame_alloc();
    if (!g_fm) { g_fm = SDL_CreateMutex(); g_f_put = SDL_CreateCond(); }
    rec_init(g_rec_dir, g_mp4);
    if (g_mp4) rec_toggle();
    dec_args_t da = { &dec, rec, es };
    g_dec_stop = 0;
    SDL_Thread *dth = SDL_CreateThread(decoder_main, "decoder", &da);

    SDL_Texture *tex = NULL;
    AVFrame *show = av_frame_alloc();
    uint32_t W = 0, H = 0;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 t_start = SDL_GetPerformanceCounter();
    Uint64 t_prev_show = 0, t_win = t_start, t_menu = 0;
    long shown = 0, skipped = 0, stale = 0;
    double lat_sum = 0, lat_max = 0, age_sum = 0, age_max = 0; long lat_n = 0, age_n = 0;
    /* this second's window */
    long w_shown = 0, w_skipped = 0, w_dec0 = 0, w_lat_n = 0, w_age_n = 0; double w_bytes0 = 0, w_lat = 0, w_age = 0, w_gap = 0, w_draw = 0, w_dec_ms0 = 0;
    int w_qmax_seen = 0;
    long w_a_bytes0 = 0;
    char title[512];

    while (!g_quit) {
        app_events(win, ren, tex);
        if (g_run_secs > 0 && (double)(SDL_GetPerformanceCounter() - t_start) / (double)freq >= g_run_secs) break;
        if (g_lost) {
            /* the Switch left the bus (rebooted, cable out) */
            reader_stop();
            libusb_release_interface(g_usb, SFT_IFACE);
            libusb_close(g_usb);
            g_usb = NULL;
            fprintf(stderr, "\nthe Switch disconnected%s\n", app ? " - waiting for it" : "");
            if (!app) break;
            g_usb = app_wait_device(ctx, win, ren, tex);
            if (!g_usb) break;
            g_lost = 0;
            reader_start();
            continue;
        }
        SDL_LockMutex(g_fm);
        if (g_fn == 0) SDL_CondWaitTimeout(g_f_put, g_fm, 20);
        if (g_fn == 0) {
            SDL_UnlockMutex(g_fm);
        no_picture:
            {
                /* v0.5: no new picture for 1.5 s (no game yet, the game closed
                 * or in the background, the stream turned off): the menu
                 * instead of a frozen frame */
                const Uint64 nowm = SDL_GetPerformanceCounter();
                if (!g_menu && (!t_prev_show || (double)(nowm - t_prev_show) / (double)freq > 1.5)) {
                    g_menu = 1;
                    g_menu_state = t_prev_show ? MENU_PAUSED : MENU_CONNECTED;
                    if (t_prev_show) fprintf(stderr, "\nno new picture for 1.5 s - showing the menu\n");
                }
                if (g_menu && (double)(nowm - t_menu) / (double)freq > 0.2) { menu_draw(ren); t_menu = nowm; }
            }
            goto stats;
        }
        while (g_fn > 2) {                     /* the latency cap: at most two frames behind */
            av_frame_unref(g_fq[g_fh]);
            g_fh = (g_fh + 1) % FQ;
            g_fn--;
            skipped++; w_skipped++;
        }
        av_frame_move_ref(show, g_fq[g_fh]);
        g_fh = (g_fh + 1) % FQ;
        g_fn--;
        SDL_UnlockMutex(g_fm);
        /* v0.5: while the game is not on screen (HOME menu, suspended) the
         * console keeps re-sending its last frame, ageing: frames older than
         * 800 ms on the console are not new picture. Live frames are 5-60 ms
         * old; a loading stall that long does not last the 1.5 s the menu
         * waits for. (Recordings from before M85 carry no age: -1.) */
        if (g_age_ms[show->pts & 255] > 800.0) {
            av_frame_unref(show);
            stale++;
            goto no_picture;
        }
        {
            const Uint64 r0 = SDL_GetPerformanceCounter();
            if (g_menu) {                      /* back from the menu: the picture's own size again */
                g_menu = 0;
                if (tex) SDL_RenderSetLogicalSize(ren, (int)W, (int)H);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            }
            if (!tex || (uint32_t)show->width != W || (uint32_t)show->height != H) {
                if (tex) SDL_DestroyTexture(tex);
                W = (uint32_t)show->width; H = (uint32_t)show->height;
                if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED))) SDL_SetWindowSize(win, (int)W, (int)H);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
                SDL_RenderSetLogicalSize(ren, (int)W, (int)H);
                fprintf(stderr, "\nstream: %ux%u H.264\n", W, H);
            }
            SDL_UpdateYUVTexture(tex, NULL, show->data[0], show->linesize[0], show->data[1], show->linesize[1], show->data[2], show->linesize[2]);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            if (rec_active()) {                /* v0.5: a red dot while recording (not in the file) */
                const int d = (int)(H / 40 > 12 ? H / 40 : 12);
                SDL_SetRenderDrawColor(ren, 230, 40, 40, 255);
                SDL_Rect r = { d, d, d, d };
                SDL_RenderFillRect(ren, &r);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            }
            SDL_RenderPresent(ren);            /* waits for the display's refresh (vsync) */
            const Uint64 now = SDL_GetPerformanceCounter();
            w_draw += (double)(now - r0) * 1000.0 / (double)freq;
            shown++; w_shown++;
            const int k = (int)(show->pts & 255);
            const double l = (double)(now - g_t_rx[k]) * 1000.0 / (double)freq;
            if (l >= 0 && l < 5000) { lat_sum += l; lat_n++; w_lat += l; w_lat_n++; if (l > lat_max) lat_max = l; }
            if (g_age_ms[k] >= 0) { age_sum += g_age_ms[k]; age_n++; w_age += g_age_ms[k]; w_age_n++; if (g_age_ms[k] > age_max) age_max = g_age_ms[k]; }
            if (t_prev_show) { const double gap = (double)(now - t_prev_show) * 1000.0 / (double)freq; if (gap > w_gap) w_gap = gap; }
            t_prev_show = now;
            av_frame_unref(show);
        }
    stats:
        {
            const Uint64 now = SDL_GetPerformanceCounter();
            const double secs = (double)(now - t_win) / (double)freq;
            if (g_qmax > w_qmax_seen) w_qmax_seen = g_qmax;
            if (secs >= 1.0) {
                const long dec = s_decoded - w_dec0;
                const double mbps = (s_bytes - w_bytes0) * 8.0 / 1e6 / secs;
                const double dms = dec ? (s_dec_ms - w_dec_ms0) / dec : 0.0;
                char rs[200];
                rec_status(rs, sizeof(rs));
                snprintf(title, sizeof(title), "Switch Frame Tap  %ux%u  %.1f fps  %.0f Mbps  pc %.1f ms  console %.1f ms%s",
                         W, H, w_shown / secs, mbps, w_lat_n ? w_lat / w_lat_n : 0.0, w_age_n ? w_age / w_age_n : 0.0, rs);
                if (W) SDL_SetWindowTitle(win, title);
                const double a_kbs = (g_a_bytes - w_a_bytes0) / 1024.0 / secs;
                const double a_q = g_adev ? SDL_GetQueuedAudioSize(g_adev) / 192.0 : 0.0;
                fprintf(stderr, "[%7.1f s] decoded %.1f fps, shown %.1f fps, skipped %ld, %.0f Mbps | pc %.1f ms, console %.1f ms | decode %.2f ms (max %.1f), draw+vsync %.1f ms, worst gap %.1f ms, usb queue max %d | audio %.0f KB/s, %.0f ms queued, %ld underruns, %ld resyncs%s\n",
                        (double)(now - t_start) / (double)freq, dec / secs, w_shown / secs, w_skipped, mbps,
                        w_lat_n ? w_lat / w_lat_n : 0.0, w_age_n ? w_age / w_age_n : 0.0,
                        dms, s_dec_max, w_shown ? w_draw / w_shown : 0.0, w_gap, w_qmax_seen,
                        a_kbs, a_q, (long)g_a_underruns, (long)g_a_resyncs, g_mute ? " (muted)" : "");
                w_a_bytes0 = g_a_bytes;
                t_win = now; w_shown = w_skipped = 0; w_dec0 = s_decoded; w_bytes0 = s_bytes; w_dec_ms0 = s_dec_ms;
                w_lat = w_age = w_gap = w_draw = 0; w_lat_n = w_age_n = 0; s_dec_max = 0; g_qmax = 0; w_qmax_seen = 0;
            }
        }
    }

    reader_stop();
    g_dec_stop = 1;
    SDL_WaitThread(dth, NULL);
    rec_join();                               /* v0.5: finish and close a recording */
    const double secs = (double)(SDL_GetPerformanceCounter() - t_start) / (double)freq;
    fprintf(stderr, "\n%ld packets, %ld decoded, %ld shown, %ld skipped (latency cap), %ld not decoded, %ld lost, %.1f MB in %.0f s\n",
            (long)s_packets, (long)s_decoded, shown, skipped, (long)s_undecoded, (long)s_lost, s_bytes / 1e6, secs);
    if (stale) fprintf(stderr, "%ld re-sent frames not shown (the game was not on screen)\n", stale);
    if (s_keyframes) fprintf(stderr, "%ld keyframes (IDR)\n", (long)s_keyframes);
    if (lat_n) fprintf(stderr, "PC latency (header arrival -> on screen): avg %.1f ms, max %.1f ms\n", lat_sum / lat_n, lat_max);
    if (age_n) fprintf(stderr, "console latency (present -> header sent): avg %.1f ms, max %.1f ms\n", age_sum / age_n, age_max);
    printf("packets=%ld frames=%ld undecoded=%ld lost=%ld\n", (long)s_packets, (long)s_decoded, (long)s_undecoded, (long)s_lost);
    if (g_a_bytes) fprintf(stderr, "game audio: %.1f MB, %ld underruns, %ld resyncs\n", g_a_bytes / 1e6, (long)g_a_underruns, (long)g_a_resyncs);
    if (g_adev) { SDL_CloseAudioDevice(g_adev); g_adev = 0; }
    av_frame_free(&show);
    for (int i = 0; i < FQ; i++) av_frame_free(&g_fq[i]);
    h264_close(&dec);
    if (rec) fclose(rec);
    if (es) fclose(es);
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_usb) { libusb_release_interface(g_usb, SFT_IFACE); libusb_close(g_usb); }
    if (ctx) libusb_exit(ctx);
    if (g_in) fclose(g_in);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    int swap_rb = 0, scale = 1, headless = 0, low_latency = 0, threads = 2;
    long max_frames = 0;
    const char *file = NULL, *record = NULL, *h264_out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--swap")) swap_rb = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--h264") && i + 1 < argc) h264_out = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--low-latency")) low_latency = 1;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app")) g_app = 1;
        else if (!strcmp(argv[i], "--paced")) g_paced = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_run_secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mp4") && i + 1 < argc) g_mp4 = argv[++i];
        else if (!strcmp(argv[i], "--rec-dir") && i + 1 < argc) g_rec_dir = argv[++i];
        else { fprintf(stderr, "unknown option %s (see the comment at the top of raw-view.c)\n", argv[i]); return 2; }
    }
    if (scale < 1) scale = 1;
#ifdef _WIN32
    /* double-clicked from Explorer: the app mode */
    if (argc == 1) g_app = 1;
    SDL_SetMainReady();
    /* a GUI program has no console: the statistics go to a log file,
     * %APPDATA%\switch-frame-tap\viewer\viewer.log */
    if (g_app) {
        char *dir = SDL_GetPrefPath("switch-frame-tap", "viewer");
        if (dir) {
            char path[1024];
            snprintf(path, sizeof(path), "%sviewer.log", dir);
            if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
            SDL_free(dir);
        }
    }
#endif
    /* Ctrl-C before the first frame still ends with the summary below */
    signal(SIGINT, on_sigint);

    libusb_context *ctx = NULL;
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Texture *tex = NULL;
    if (g_app && !file) {
        headless = 0;
        /* a double-clicked program has no console: say it in a box */
        if (libusb_init(&ctx) != 0) {
            fprintf(stderr, "libusb_init failed\n");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Switch Frame Tap", "USB support could not be started (libusb_init failed).", NULL);
            return 1;
        }
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Switch Frame Tap", SDL_GetError(), NULL);
            return 1;
        }
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        win = SDL_CreateWindow("Switch Frame Tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!ren) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Switch Frame Tap", SDL_GetError(), NULL);
            return 1;
        }
        rec_init(g_rec_dir, g_mp4);
        menu_draw(ren);
        g_usb = app_wait_device(ctx, win, ren, NULL);
        if (!g_usb) { SDL_Quit(); libusb_exit(ctx); return 0; }
    } else if (file) {
        g_in = fopen(file, "rb");
        if (!g_in) { perror(file); return 1; }
    } else {
        if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
        /* the console enumerates a few seconds into its boot: wait for it
         * (Ctrl-C to give up) rather than making the user start us after it */
        g_usb = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
        if (!g_usb) {
            fprintf(stderr, "waiting for the console (%04x:%04x) - boot it with 'usb' in sdmc:/applet-mitm.armed...\n",
                    SFT_VID, SFT_PID);
            while (!g_usb && !g_quit) {
                SDL_Delay(500);
                g_usb = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
            }
            if (!g_usb) { libusb_exit(ctx); return 1; }
        }
        libusb_set_auto_detach_kernel_driver(g_usb, 1);
        if (libusb_claim_interface(g_usb, SFT_IFACE) != 0) {
            fprintf(stderr, "cannot claim interface %d (udev rule installed?)\n", SFT_IFACE);
            libusb_close(g_usb); libusb_exit(ctx); return 1;
        }
        fprintf(stderr, "waiting for frames on %04x:%04x ep 0x%02x...\n", SFT_VID, SFT_PID, SFT_EP_IN);
    }
    if (g_usb) reader_start();
    FILE *rec = record ? fopen(record, "wb") : NULL;
    FILE *es = h264_out ? fopen(h264_out, "wb") : NULL;
    if ((record && !rec) || (h264_out && !es)) { perror("output file"); return 1; }
#ifdef SFT_H264
    /* v0.2 test3: live USB goes through the paced three-thread pipeline;
     * the loop below is file replay (and the tests) */
    if (g_usb || (g_in && g_paced)) {
        (void)headless; (void)max_frames;
        if (!g_usb) reader_start();            /* --paced: the recording goes through the same reader thread */
        return run_live(ctx, win, ren, g_app, low_latency, threads, rec, es);
    }
#endif

#ifdef SFT_H264
    h264_t dec = {0};
    int dec_ok = (h264_open(&dec, low_latency, threads) == 0);
    if (!dec_ok) fprintf(stderr, "libavcodec H.264 decoder unavailable - H.264 packets will be skipped\n");
#endif

    uint32_t W = 0, H = 0, fmt = 0;
    uint8_t *payload = NULL; size_t cap = 0;
    uint8_t *rgb = NULL; size_t rgbcap = 0;   /* unpacked RGB24 for packed420 */
    long frames = 0, packets = 0, lost = 0, undecoded = 0;
    uint32_t last_kind = 0; int have_kind = 0;
    uint64_t bytes = 0;
    Uint64 t_first = 0, t_prev = 0, freq = SDL_GetPerformanceFrequency();
    double worst_gap = 0.0, dec_ms_total = 0.0;
    /* M85 latency: arrival time of each frame's header, indexed by frame
     * number, so a frame that comes out of the decoder later can be paired
     * with it; and the console's own age of the frame (present -> header). */
    static Uint64 t_rx[256];
    double lat_sum = 0.0, lat_max = 0.0, age_sum = 0.0, age_max = 0.0;
    long lat_n = 0, age_n = 0, keyframes = 0, sessions = 1;
    int64_t out_kind = -1;
    /* v0.2: frames decoded but not drawn (a newer one was already waiting),
     * and the time spent drawing - the Windows diagnosis */
    long shown = 0, skipped = 0;
    double draw_ms_total = 0.0;
    /* v0.5: --mp4 from a file replay records on a 60 fps clock (the file
     * is read as fast as it decodes, so arrival times mean nothing) */
    Uint64 t_synth = SDL_GetPerformanceCounter();
    const Uint64 frame_ticks = SDL_GetPerformanceFrequency() / 60;
    if (g_mp4) { rec_init(NULL, g_mp4); rec_toggle(); }

    while (!g_quit && (max_frames == 0 || packets < max_frames)) {
        if (g_app && g_lost) {
            /* the Switch left the bus (rebooted, cable out): keep the window */
            reader_stop();
            libusb_release_interface(g_usb, SFT_IFACE);
            libusb_close(g_usb);
            g_usb = NULL; g_lost = 0;
            fprintf(stderr, "\nthe Switch disconnected - waiting for it\n");
            g_usb = app_wait_device(ctx, win, ren, tex);
            if (!g_usb) break;
            g_lost = 0;
            reader_start();
            have_kind = 0;
            continue;
        }
        if (g_app) {
            app_events(win, ren, tex);
        } else if (!headless && win) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) g_quit = 1;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
            }
        }

        sft_hdr_t hdr;
        Uint64 t_hdr = 0;
        int left = 0;
        if (g_in) {
            /* a recording: read it in line, as always */
            int r = read_exact((uint8_t *)&hdr, sizeof(hdr), 1000);
            if (r <= 0) continue;
            t_hdr = SDL_GetPerformanceCounter();
            if (hdr.magic != SFT_MAGIC) { fprintf(stderr, "bad magic 0x%08x, resyncing\n", hdr.magic); continue; }
            if (hdr.length == 0 || hdr.length > 64u*1024*1024) continue;
            if (hdr.length > cap) {
                /* libavcodec wants readable padding past the end of a packet */
                uint8_t *p = realloc(payload, (size_t)hdr.length + 64);
                if (!p) break;
                payload = p; cap = hdr.length;
            }
            if (read_exact(payload, hdr.length, 5000) != (int)hdr.length) continue;
            memset(payload + hdr.length, 0, 64);
            if (g_mp4) t_hdr = t_synth;
            if (hdr.flags & SFT_FLAG_AUDIO) { rec_audio(payload, hdr.length, hdr.width, hdr.height, t_hdr); continue; }   /* v0.3: sound is the live pipeline's */
        } else if (!reader_pop(&hdr, &payload, &cap, &t_hdr, g_app ? 50 : 200, &left)) {
            continue;
        }
        packets++;
        bytes += hdr.length;
        if (rec) { fwrite(&hdr, 1, sizeof(hdr), rec); fwrite(payload, 1, hdr.length, rec); }

        const int is_h264 = (hdr.flags & SFT_FLAG_H264) != 0;
        const int packed420 = !is_h264 && (hdr.flags & SFT_FLAG_PACKED420) != 0;
        if (is_h264) {
            /* kind carries the console's frame number: gaps are frames it
             * skipped (an encode error) or that never arrived */
            if (have_kind && hdr.kind > last_kind + 1) lost += hdr.kind - last_kind - 1;
            /* M86 live mode: a new console session restarts at frame 0 */
            if (have_kind && hdr.kind < last_kind) {
                sessions++;
                fprintf(stderr, "\nnew stream session %ld (the console reattached)\n", sessions);
            }
            last_kind = hdr.kind; have_kind = 1;
            if (es) fwrite(payload, 1, hdr.length, es);
            if (g_in && g_mp4) { rec_video(&hdr, payload, t_hdr); t_synth += frame_ticks; }
            t_rx[hdr.kind & 255] = t_hdr;
            if (hdr.flags & SFT_FLAG_KEY) keyframes++;
            /* M85 builds put the console-side age (present -> header, us) in
             * the otherwise unused stride; older recordings carry 0 */
            if (hdr.stride != 0 && hdr.stride < 10000000u) {
                const double a = hdr.stride / 1000.0;
                age_sum += a; age_n++;
                if (a > age_max) age_max = a;
            }
        }

        const uint8_t *planes[3] = {0}; int pitches[3] = {0};
#ifdef SFT_H264
        if (is_h264) {
            if (!dec_ok) { undecoded++; continue; }
            Uint64 d0 = SDL_GetPerformanceCounter();
            int got = h264_decode(&dec, payload, (int)hdr.length, (int64_t)hdr.kind);
            dec_ms_total += (double)(SDL_GetPerformanceCounter() - d0) * 1000.0 / (double)freq;
            if (got < 0) { undecoded++; continue; }
            if (got == 0) continue;          /* frame threads still filling up */
            for (int k = 0; k < 3; k++) { planes[k] = dec.frm->data[k]; pitches[k] = dec.frm->linesize[k]; }
            out_kind = dec.frm->pts;
            hdr.width = (uint32_t)dec.frm->width;
            hdr.height = (uint32_t)dec.frm->height;
        }
#else
        if (is_h264) { undecoded++; continue; }   /* built without libavcodec */
#endif
        frames++;
        if (headless) {
            Uint64 now = SDL_GetPerformanceCounter();
            if (frames == 1) { t_first = now; t_prev = now; }
            t_prev = now;
            continue;
        }

        if (g_app && frames == 1) { t_first = SDL_GetPerformanceCounter(); t_prev = t_first; }
        /* v0.2: a newer packet is already waiting - decode it next and draw
         * that instead; drawing this one would only add latency */
        if (!g_in && win && is_h264 && reader_pending() > 0) { skipped++; continue; }
        const Uint64 r0 = SDL_GetPerformanceCounter();
        const uint32_t want_fmt = is_h264 ? SDL_PIXELFORMAT_IYUV
                                : packed420 ? SDL_PIXELFORMAT_RGB24
                                : (swap_rb ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888);
        if (!win) {
            if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); break; }
            /* the stream is BT.709 limited range (VUI says so too) */
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
            win = SDL_CreateWindow("switch-frame-tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)(hdr.width*scale), (int)(hdr.height*scale), SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            /* M86b: SDL scales nearest-neighbour by default, which made edges
             * look pixelated in a resized or maximized window. Linear, and keep
             * 16:9 with letterboxing instead of stretching. */
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            t_first = SDL_GetPerformanceCounter(); t_prev = t_first;
        }
        /* the console can change size or format mid-run: rebuild on change */
        if (!tex || hdr.width != W || hdr.height != H || want_fmt != fmt) {
            if (tex) SDL_DestroyTexture(tex);
            W = hdr.width; H = hdr.height; fmt = want_fmt;
            SDL_SetWindowSize(win, (int)(W*scale), (int)(H*scale));
            tex = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
            SDL_RenderSetLogicalSize(ren, (int)W, (int)H);
            fprintf(stderr, "\nstream: %ux%u %s\n", W, H, is_h264 ? "H.264" : packed420 ? "packed 4:2:0" : "raw");
            worst_gap = 0.0;
        }

        if (is_h264) {
            SDL_UpdateYUVTexture(tex, NULL, planes[0], pitches[0], planes[1], pitches[1], planes[2], pitches[2]);
        } else if (packed420) {
            const size_t need = (size_t)W * H * 3;
            if (need > rgbcap) {
                uint8_t *p = realloc(rgb, need);
                if (!p) break;
                rgb = p; rgbcap = need;
            }
            const uint8_t *bpl = payload;                 /* B, full resolution */
            const uint8_t *uv  = payload + (size_t)W * H; /* R,G interleaved at half res */
            for (uint32_t j = 0; j < H; j++) {
                const uint8_t *uvrow = uv + (size_t)(j >> 1) * W;
                uint8_t *dst = rgb + (size_t)j * W * 3;
                for (uint32_t i = 0; i < W; i++) {
                    const uint32_t k = i & ~1u;
                    dst[i*3+0] = uvrow[k];                /* R */
                    dst[i*3+1] = uvrow[k+1];              /* G */
                    dst[i*3+2] = bpl[(size_t)j * W + i];  /* B */
                }
            }
            SDL_UpdateTexture(tex, NULL, rgb, (int)(W * 3));
        } else {
            SDL_UpdateTexture(tex, NULL, payload, (int)hdr.stride);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        shown++;

        Uint64 now = SDL_GetPerformanceCounter();
        draw_ms_total += (double)(now - r0) * 1000.0 / (double)freq;
        /* header arrival -> on screen, for the frame actually shown (with
         * frame threads that is an older one than the packet just read) */
        if (is_h264 && out_kind >= 0 && !g_in) {
            const double l = (double)(now - t_rx[out_kind & 255]) * 1000.0 / (double)freq;
            if (l >= 0 && l < 5000) { lat_sum += l; lat_n++; if (l > lat_max) lat_max = l; }
        }
        double gap = (double)(now - t_prev) * 1000.0 / (double)freq;
        if (frames > 1 && gap > worst_gap) worst_gap = gap;
        t_prev = now;
        if ((frames % 30) == 0) {
            double secs = (double)(now - t_first) / (double)freq;
            char title[200];
            snprintf(title, sizeof(title), "Switch Frame Tap  %ux%u  %.1f fps  %.0f Mbps  (worst gap %.1f ms, %ld lost)  pc %.1f ms  console %.1f ms",
                     W, H, frames / secs, (double)bytes * 8.0 / 1e6 / secs, worst_gap, lost,
                     lat_n ? lat_sum / lat_n : 0.0, age_n ? age_sum / age_n : 0.0);
            SDL_SetWindowTitle(win, title);
            fprintf(stderr, "\r%ld frames  %.1f fps  %.0f Mbps  worst gap %.1f ms  %ld lost  pc %.1f ms  console %.1f ms  |  decode %.2f ms  draw %.2f ms  drawn %ld skipped %ld  queue max %d   ",
                    frames, frames / secs, (double)bytes * 8.0 / 1e6 / secs, worst_gap, lost,
                    lat_n ? lat_sum / lat_n : 0.0, age_n ? age_sum / age_n : 0.0,
                    frames ? dec_ms_total / frames : 0.0, shown ? draw_ms_total / shown : 0.0, shown, skipped, g_qmax);
            fflush(stderr);
        }
    }

    reader_stop();
#ifdef SFT_H264
    if (dec_ok) {
        /* frame threads hold the last few frames: drain them */
        avcodec_send_packet(dec.c, NULL);
        while (avcodec_receive_frame(dec.c, dec.frm) == 0) frames++;
    }
#endif
    double secs = (double)(t_prev - t_first) / (double)freq;
    fprintf(stderr, "\n%ld packets, %ld frames shown/decoded, %ld not decoded, %ld lost (frame-number gaps), %.1f MB",
            packets, frames, undecoded, lost, (double)bytes / 1e6);
    if (frames > 1 && secs > 0) fprintf(stderr, ", %.1f fps, worst gap %.1f ms", frames / secs, worst_gap);
    if (frames > 0 && dec_ms_total > 0) fprintf(stderr, ", decode avg %.2f ms", dec_ms_total / frames);
    fprintf(stderr, "\n");
    if (keyframes) fprintf(stderr, "%ld keyframes (IDR), %ld other\n", keyframes, packets - keyframes);
    if (lat_n) fprintf(stderr, "PC latency (header arrival -> on screen): avg %.1f ms, max %.1f ms over %ld frames\n",
                       lat_sum / lat_n, lat_max, lat_n);
    if (age_n) fprintf(stderr, "console latency (present -> header sent): avg %.1f ms, max %.1f ms\n",
                       age_sum / age_n, age_max);
    if (shown) fprintf(stderr, "drawn %ld, skipped %ld (a newer frame was waiting), draw avg %.2f ms, USB queue max %d of %d\n",
                       shown, skipped, draw_ms_total / shown, g_qmax, QN);
    rec_join();                               /* v0.5: --mp4: finish the file */
    /* machine-readable last line, for tests */
    printf("packets=%ld frames=%ld undecoded=%ld lost=%ld\n", packets, frames, undecoded, lost);

#ifdef SFT_H264
    h264_close(&dec);
#endif
    if (rec) fclose(rec);
    if (es) fclose(es);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    free(payload);
    free(rgb);
    if (g_in) fclose(g_in);
    if (g_usb) { libusb_release_interface(g_usb, SFT_IFACE); libusb_close(g_usb); }
    if (ctx) libusb_exit(ctx);
    return 0;
}
