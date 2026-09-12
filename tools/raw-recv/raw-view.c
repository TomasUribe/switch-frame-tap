/* raw-view - live viewer for the switch-frame-tap raw stream.
 *
 * libusb + SDL2 in ONE process: no pipe, no codec, no buffering beyond the
 * frame being drawn. A pipe into an external player would add latency, which
 * is the thing this prototype exists to minimise.
 *
 *   cc -O2 -o raw-view raw-view.c $(pkg-config --cflags --libs libusb-1.0 sdl2)
 *   ./raw-view                 # auto-sizes to whatever the header reports
 *   ./raw-view --swap          # flip R and B if colours look wrong
 *
 * Pixel order: the VIC writes B,G,R,A bytes (M44 proved the blit is lossless
 * with only R and B transposed). As a little-endian u32 that is ARGB8888,
 * which is the default below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>
#include <SDL2/SDL.h>

#define SFT_VID 0x1209
#define SFT_PID 0x5F1E
#define SFT_IFACE 0
#define SFT_EP_IN 0x81
#define SFT_MAGIC 0x52544653u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic; uint16_t version; uint16_t flags;
    uint32_t width, height, stride, length, block_h_log2, kind;
} sft_hdr_t;
#pragma pack(pop)

static volatile int g_quit = 0;

static int read_exact(libusb_device_handle *h, uint8_t *dst, size_t n, int ms)
{
    size_t done = 0;
    while (done < n && !g_quit) {
        int got = 0;
        int chunk = (int)((n - done) > 0x40000 ? 0x40000 : (n - done));
        int rc = libusb_bulk_transfer(h, SFT_EP_IN, dst + done, chunk, &got, ms);
        if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) {
            if (got == 0 && rc == LIBUSB_ERROR_TIMEOUT) return (done > 0) ? -2 : 0;
            done += (size_t)got;
            continue;
        }
        /* NO_DEVICE means the console left the bus (powered off, rebooted).
         * Returning -1 here used to spin the caller forever printing errors;
         * -3 tells it to exit cleanly instead. */
        if (rc == LIBUSB_ERROR_NO_DEVICE) { g_quit = 1; return -3; }
        fprintf(stderr, "bulk read: %s\n", libusb_error_name(rc));
        return -1;
    }
    return g_quit ? -1 : (int)done;
}

int main(int argc, char **argv)
{
    int swap_rb = 0, scale = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--swap")) swap_rb = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    }

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
    if (!h) {
        fprintf(stderr, "device %04x:%04x not found - is the console booted with 'usb stream' armed?\n",
                SFT_VID, SFT_PID);
        libusb_exit(ctx); return 1;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, SFT_IFACE) != 0) {
        fprintf(stderr, "cannot claim interface %d (udev rule installed?)\n", SFT_IFACE);
        libusb_close(h); libusb_exit(ctx); return 1;
    }
    fprintf(stderr, "waiting for frames on %04x:%04x ep 0x%02x...\n", SFT_VID, SFT_PID, SFT_EP_IN);

    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Texture *tex = NULL;
    uint32_t W = 0, H = 0;
    uint8_t *payload = NULL; size_t cap = 0;
    uint8_t *rgb = NULL; size_t rgbcap = 0;   /* unpacked RGB24 for the texture */
    int packed420 = 0;
    int frames = 0;
    Uint64 t_first = 0, t_prev = 0, freq = SDL_GetPerformanceFrequency();
    double worst_gap = 0.0;

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
        }

        sft_hdr_t hdr;
        int r = read_exact(h, (uint8_t *)&hdr, sizeof(hdr), 1000);
        if (r == 0) continue;
        if (r < 0) continue;
        if (hdr.magic != SFT_MAGIC) { fprintf(stderr, "bad magic 0x%08x, resyncing\n", hdr.magic); continue; }
        if (hdr.length == 0 || hdr.length > 64u*1024*1024) continue;

        if (hdr.length > cap) {
            uint8_t *p = realloc(payload, hdr.length);
            if (!p) break;
            payload = p; cap = hdr.length;
        }
        if (read_exact(h, payload, hdr.length, 5000) != (int)hdr.length) continue;

        /* flags bit 0: the VIC's packed 4:2:0 RGB. It writes the NV12 plane
         * layout but performs no colour conversion, so the planes carry raw
         * channels: luma = B at full res, chroma even/odd = R/G at half res in
         * both axes. Undoing that here costs one pass and buys 2.67x on the
         * wire against RGBA. */
        packed420 = (hdr.flags & 1u) != 0;

        /* M70: the console can sweep resolutions inside one run, so rebuild
         * the texture and window whenever the header geometry changes. */
        if (win && (hdr.width != W || hdr.height != H)) {
            fprintf(stderr, "\nresolution -> %ux%u\n", hdr.width, hdr.height);
            if (tex) SDL_DestroyTexture(tex);
            W = hdr.width; H = hdr.height;
            SDL_SetWindowSize(win, (int)(W*scale), (int)(H*scale));
            tex = SDL_CreateTexture(ren,
                                    packed420 ? SDL_PIXELFORMAT_RGB24
                                              : (swap_rb ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888),
                                    SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
            frames = 0; t_first = SDL_GetPerformanceCounter(); t_prev = t_first; worst_gap = 0.0;
        }

        if (!win) {
            W = hdr.width; H = hdr.height;
            if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); break; }
            win = SDL_CreateWindow("switch-frame-tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)(W*scale), (int)(H*scale), SDL_WINDOW_SHOWN);
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            tex = SDL_CreateTexture(ren,
                                    packed420 ? SDL_PIXELFORMAT_RGB24
                                              : (swap_rb ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888),
                                    SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
            fprintf(stderr, "stream: %ux%u, %u B/frame\n", W, H, hdr.length);
            t_first = SDL_GetPerformanceCounter(); t_prev = t_first;
        }

        if (packed420) {
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

        frames++;
        Uint64 now = SDL_GetPerformanceCounter();
        double gap = (double)(now - t_prev) * 1000.0 / (double)freq;
        if (gap > worst_gap) worst_gap = gap;
        t_prev = now;
        if ((frames % 30) == 0) {
            double secs = (double)(now - t_first) / (double)freq;
            char title[160];
            snprintf(title, sizeof(title), "switch-frame-tap  %ux%u  %.1f fps  (worst gap %.1f ms)",
                     W, H, frames / secs, worst_gap);
            SDL_SetWindowTitle(win, title);
            fprintf(stderr, "\r%d frames  %.1f fps  worst gap %.1f ms   ", frames, frames / secs, worst_gap);
            fflush(stderr);
        }
    }

    if (frames > 1) {
        double secs = (double)(SDL_GetPerformanceCounter() - t_first) / (double)freq;
        fprintf(stderr, "\n%d frames in %.2f s -> %.1f fps average, worst gap %.1f ms\n",
                frames, secs, frames / secs, worst_gap);
    }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    free(payload);
    free(rgb);
    libusb_release_interface(h, SFT_IFACE);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
