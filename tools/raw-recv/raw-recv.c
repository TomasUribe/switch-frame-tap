/* raw-recv - read raw captured frames from switch-frame-tap over USB bulk.
 *
 * Option A of the transport plan: no codec, no protocol negotiation. The
 * sysmodule sends a 32-byte header then the raw block-linear frame bytes; we
 * write the payload to a .bin that tools/deswizzle.py turns into a PNG.
 *
 *   cc -O2 -o raw-recv raw-recv.c $(pkg-config --cflags --libs libusb-1.0)
 *   ./raw-recv                 # writes frame_000.bin, frame_001.bin, ...
 *   ./raw-recv -o /tmp/cap     # prefix
 *
 * The existing switch-stream/receiver speaks sw_hdr_t and pipes through
 * libavcodec, which expects an ENCODED stream - that is option B and needs
 * NVENC. This tool deliberately handles only the raw case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>

#define SFT_VID   0x1209      /* pid.codes open range - matches applet-mitm.json */
#define SFT_PID   0x5F1E
#define SFT_IFACE 0           /* bInterfaceNumber from the M53 descriptor dump */
#define SFT_EP_IN 0x81

#define SFT_MAGIC 0x52544653u /* "SFTR" little-endian */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;        /* bytes per row */
    uint32_t length;        /* payload bytes following this header */
    uint32_t block_h_log2;  /* Tegra block-linear gob height, 4 for our frames */
    uint32_t kind;          /* nvmap kind, 0xFE = Generic_16BX2 */
} sft_hdr_t;
#pragma pack(pop)

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int s) { (void)s; g_quit = 1; }

/* Read exactly n bytes, tolerating short bulk transfers and idle timeouts. */
static int read_exact(libusb_device_handle *h, uint8_t *dst, size_t n, int timeout_ms)
{
    size_t done = 0;
    while (done < n && !g_quit) {
        int got = 0;
        int chunk = (int)((n - done) > 0x40000 ? 0x40000 : (n - done));
        int rc = libusb_bulk_transfer(h, SFT_EP_IN, dst + done, chunk, &got, timeout_ms);
        if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) {
            if (got == 0 && rc == LIBUSB_ERROR_TIMEOUT) return (done > 0) ? -2 : 0;
            done += (size_t)got;
            continue;
        }
        fprintf(stderr, "bulk read failed: %s\n", libusb_error_name(rc));
        return -1;
    }
    return g_quit ? -1 : (int)done;
}

int main(int argc, char **argv)
{
    const char *prefix = "frame";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
    }

    signal(SIGINT, on_sigint);

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
    if (!h) {
        fprintf(stderr,
                "device %04x:%04x not found.\n"
                "  - is the console booted with applet-mitm installed and 'usb' in the arm file?\n"
                "  - check: lsusb -d %04x:%04x\n",
                SFT_VID, SFT_PID, SFT_VID, SFT_PID);
        libusb_exit(ctx);
        return 1;
    }

    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, SFT_IFACE) != 0) {
        fprintf(stderr, "cannot claim interface %d - permissions?\n"
                        "  sudo cp 99-switch-frame-tap.rules /etc/udev/rules.d/\n"
                        "  sudo udevadm control --reload-rules && sudo udevadm trigger\n", SFT_IFACE);
        libusb_close(h); libusb_exit(ctx);
        return 1;
    }

    fprintf(stderr, "listening on %04x:%04x ep 0x%02x - Ctrl-C to stop\n",
            SFT_VID, SFT_PID, SFT_EP_IN);

    int n_frames = 0;
    uint8_t *payload = NULL;
    size_t payload_cap = 0;

    while (!g_quit) {
        sft_hdr_t hdr;
        int r = read_exact(h, (uint8_t *)&hdr, sizeof(hdr), 2000);
        if (r == 0) continue;                    /* idle, keep waiting */
        if (r < 0) { if (r == -2) fprintf(stderr, "short header, resyncing\n"); continue; }

        if (hdr.magic != SFT_MAGIC) {
            fprintf(stderr, "bad magic 0x%08x (want 0x%08x) - resyncing\n", hdr.magic, SFT_MAGIC);
            continue;
        }
        if (hdr.length == 0 || hdr.length > 64u * 1024 * 1024) {
            fprintf(stderr, "implausible length %u - skipping\n", hdr.length);
            continue;
        }

        if (hdr.length > payload_cap) {
            uint8_t *p = realloc(payload, hdr.length);
            if (!p) { fprintf(stderr, "out of memory for %u bytes\n", hdr.length); break; }
            payload = p; payload_cap = hdr.length;
        }

        fprintf(stderr, "frame %d: %ux%u stride=%u kind=0x%02x blk_h_log2=%u payload=%u B ... ",
                n_frames, hdr.width, hdr.height, hdr.stride, hdr.kind, hdr.block_h_log2, hdr.length);
        fflush(stderr);

        r = read_exact(h, payload, hdr.length, 5000);
        if (r != (int)hdr.length) {
            fprintf(stderr, "incomplete (%d/%u)\n", r, hdr.length);
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s_%03d.bin", prefix, n_frames);
        FILE *f = fopen(path, "wb");
        if (!f) { perror("fopen"); break; }
        fwrite(payload, 1, hdr.length, f);
        fclose(f);
        fprintf(stderr, "wrote %s\n", path);
        n_frames++;
    }

    fprintf(stderr, "\n%d frame(s) received\n", n_frames);
    if (n_frames > 0) {
        fprintf(stderr, "de-swizzle with:\n"
                        "  python3 tools/deswizzle.py %s_000.bin out.png\n", prefix);
    }

    free(payload);
    libusb_release_interface(h, SFT_IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
