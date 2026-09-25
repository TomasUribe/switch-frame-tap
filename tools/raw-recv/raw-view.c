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
 *   ./raw-view --threads 2     # N frame threads: N-1 frames of decoder delay
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
#include <libusb-1.0/libusb.h>
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

#pragma pack(push, 1)
typedef struct {
    uint32_t magic; uint16_t version; uint16_t flags;
    uint32_t width, height, stride, length, block_h_log2, kind;
} sft_hdr_t;
#pragma pack(pop)

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int s) { (void)s; g_quit = 1; }
static libusb_device_handle *g_usb = NULL;
static FILE *g_in = NULL;           /* --file */

/* Read exactly n bytes from USB (tolerating short bulk transfers and idle
 * timeouts) or from the replay file. 0 = idle, <0 = error, n = done. */
static int read_exact(uint8_t *dst, size_t n, int ms)
{
    if (g_in) {
        size_t got = fread(dst, 1, n, g_in);
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
        if (rc == LIBUSB_ERROR_NO_DEVICE) { g_quit = 1; return -3; }
        fprintf(stderr, "bulk read: %s\n", libusb_error_name(rc));
        return -1;
    }
    return g_quit ? -1 : (int)done;
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
static int h264_decode(h264_t *d, uint8_t *buf, int len)
{
    d->pkt->data = buf;
    d->pkt->size = len;
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

int main(int argc, char **argv)
{
    int swap_rb = 0, scale = 1, headless = 0, low_latency = 0, threads = 0;
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
        else { fprintf(stderr, "unknown option %s (see the comment at the top of raw-view.c)\n", argv[i]); return 2; }
    }
    if (scale < 1) scale = 1;
    /* Ctrl-C before the first frame still ends with the summary below */
    signal(SIGINT, on_sigint);

    libusb_context *ctx = NULL;
    if (file) {
        g_in = fopen(file, "rb");
        if (!g_in) { perror(file); return 1; }
    } else {
        if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
        g_usb = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
        if (!g_usb) {
            fprintf(stderr, "device %04x:%04x not found - is the console booted with 'usb' and a stream flag armed?\n",
                    SFT_VID, SFT_PID);
            libusb_exit(ctx); return 1;
        }
        libusb_set_auto_detach_kernel_driver(g_usb, 1);
        if (libusb_claim_interface(g_usb, SFT_IFACE) != 0) {
            fprintf(stderr, "cannot claim interface %d (udev rule installed?)\n", SFT_IFACE);
            libusb_close(g_usb); libusb_exit(ctx); return 1;
        }
        fprintf(stderr, "waiting for frames on %04x:%04x ep 0x%02x...\n", SFT_VID, SFT_PID, SFT_EP_IN);
    }
    FILE *rec = record ? fopen(record, "wb") : NULL;
    FILE *es = h264_out ? fopen(h264_out, "wb") : NULL;
    if ((record && !rec) || (h264_out && !es)) { perror("output file"); return 1; }

#ifdef SFT_H264
    h264_t dec = {0};
    int dec_ok = (h264_open(&dec, low_latency, threads) == 0);
    if (!dec_ok) fprintf(stderr, "libavcodec H.264 decoder unavailable - H.264 packets will be skipped\n");
#endif

    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Texture *tex = NULL;
    uint32_t W = 0, H = 0, fmt = 0;
    uint8_t *payload = NULL; size_t cap = 0;
    uint8_t *rgb = NULL; size_t rgbcap = 0;   /* unpacked RGB24 for packed420 */
    long frames = 0, packets = 0, lost = 0, undecoded = 0;
    uint32_t last_kind = 0; int have_kind = 0;
    uint64_t bytes = 0;
    Uint64 t_first = 0, t_prev = 0, freq = SDL_GetPerformanceFrequency();
    double worst_gap = 0.0, dec_ms_total = 0.0;

    while (!g_quit && (max_frames == 0 || packets < max_frames)) {
        if (!headless && win) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) g_quit = 1;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
            }
        }

        sft_hdr_t hdr;
        int r = read_exact((uint8_t *)&hdr, sizeof(hdr), 1000);
        if (r <= 0) continue;
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
        packets++;
        bytes += hdr.length;
        if (rec) { fwrite(&hdr, 1, sizeof(hdr), rec); fwrite(payload, 1, hdr.length, rec); }

        const int is_h264 = (hdr.flags & SFT_FLAG_H264) != 0;
        const int packed420 = !is_h264 && (hdr.flags & SFT_FLAG_PACKED420) != 0;
        if (is_h264) {
            /* kind carries the console's frame number: gaps are frames it
             * skipped (an encode error) or that never arrived */
            if (have_kind && hdr.kind > last_kind + 1) lost += hdr.kind - last_kind - 1;
            last_kind = hdr.kind; have_kind = 1;
            if (es) fwrite(payload, 1, hdr.length, es);
        }

        const uint8_t *planes[3] = {0}; int pitches[3] = {0};
#ifdef SFT_H264
        if (is_h264) {
            if (!dec_ok) { undecoded++; continue; }
            Uint64 d0 = SDL_GetPerformanceCounter();
            int got = h264_decode(&dec, payload, (int)hdr.length);
            dec_ms_total += (double)(SDL_GetPerformanceCounter() - d0) * 1000.0 / (double)freq;
            if (got < 0) { undecoded++; continue; }
            if (got == 0) continue;          /* frame threads still filling up */
            for (int k = 0; k < 3; k++) { planes[k] = dec.frm->data[k]; pitches[k] = dec.frm->linesize[k]; }
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

        const uint32_t want_fmt = is_h264 ? SDL_PIXELFORMAT_IYUV
                                : packed420 ? SDL_PIXELFORMAT_RGB24
                                : (swap_rb ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888);
        if (!win) {
            if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); break; }
            /* the stream is BT.709 limited range (VUI says so too) */
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
            win = SDL_CreateWindow("switch-frame-tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)(hdr.width*scale), (int)(hdr.height*scale), SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            t_first = SDL_GetPerformanceCounter(); t_prev = t_first;
        }
        /* the console can change size or format mid-run: rebuild on change */
        if (!tex || hdr.width != W || hdr.height != H || want_fmt != fmt) {
            if (tex) SDL_DestroyTexture(tex);
            W = hdr.width; H = hdr.height; fmt = want_fmt;
            SDL_SetWindowSize(win, (int)(W*scale), (int)(H*scale));
            tex = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
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

        Uint64 now = SDL_GetPerformanceCounter();
        double gap = (double)(now - t_prev) * 1000.0 / (double)freq;
        if (frames > 1 && gap > worst_gap) worst_gap = gap;
        t_prev = now;
        if ((frames % 30) == 0) {
            double secs = (double)(now - t_first) / (double)freq;
            char title[200];
            snprintf(title, sizeof(title), "switch-frame-tap  %ux%u  %.1f fps  %.0f Mbps  (worst gap %.1f ms, %ld lost)",
                     W, H, frames / secs, (double)bytes * 8.0 / 1e6 / secs, worst_gap, lost);
            SDL_SetWindowTitle(win, title);
            fprintf(stderr, "\r%ld frames  %.1f fps  %.0f Mbps  worst gap %.1f ms  %ld lost   ",
                    frames, frames / secs, (double)bytes * 8.0 / 1e6 / secs, worst_gap, lost);
            fflush(stderr);
        }
    }

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
