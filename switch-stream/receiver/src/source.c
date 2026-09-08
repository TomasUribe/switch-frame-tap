#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include <libusb-1.0/libusb.h>

/* libnx usb_comms device descriptor + endpoints. */
#define SW_USB_VID   0x057E
#define SW_USB_PID   0x3000
#define SW_USB_EP_IN 0x81
#define SW_USB_IFACE 0

struct source {
    int kind;                       /* 0 = tcp, 1 = usb */

    int fd;                         /* tcp */

    libusb_context       *uctx;     /* usb */
    libusb_device_handle *uh;

    uint8_t rb[8192];               /* small ring: headers + resync slack only */
    size_t  head, len;
};

/* ---- backends -------------------------------------------------------- */

static int raw_read(source_t *s, uint8_t *dst, size_t n)
{
    if (s->kind == 0) {
        for (;;) {
            ssize_t r = recv(s->fd, dst, n, 0);
            if (r > 0) return (int)r;
            if (r == 0) return -1;                       /* peer closed */
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* timeout */
            return -1;
        }
    } else {
        int got = 0;
        int rc = libusb_bulk_transfer(s->uh, SW_USB_EP_IN, dst, (int)n, &got, 1000);
        if (rc == 0) return got;
        if (rc == LIBUSB_ERROR_TIMEOUT) return got;      /* maybe partial, maybe 0 */
        return -1;
    }
}

/* ---- ring helpers -------------------------------------------------------- */

static void consume(source_t *s, size_t k) { s->head += k; s->len -= k; }

/* Ensure at least `need` bytes are buffered. 1 ok, 0 timeout, -1 fatal. */
static int ensure(source_t *s, size_t need)
{
    while (s->len < need) {
        if (s->head) { memmove(s->rb, s->rb + s->head, s->len); s->head = 0; }
        size_t space = sizeof(s->rb) - s->len;
        if (space == 0) return -1;
        int r = raw_read(s, s->rb + s->len, space);
        if (r < 0) return -1;
        if (r == 0) return 0;
        s->len += (size_t)r;
    }
    return 1;
}

/* ---- public ----------------------------------------------------------- */

int source_read(source_t *s, sw_hdr_t *hdr, uint8_t *buf, size_t cap)
{
    /* 1. resync to a header */
    for (;;) {
        int r = ensure(s, sizeof(sw_hdr_t));
        if (r <= 0) return r;

        uint32_t magic;
        memcpy(&magic, s->rb + s->head, 4);
        if (magic != SW_MAGIC) { consume(s, 1); continue; }   /* slide one byte */

        memcpy(hdr, s->rb + s->head, sizeof(*hdr));
        consume(s, sizeof(*hdr));
        break;
    }

    if (hdr->size > cap) {
        fprintf(stderr, "source: packet %u > buffer %zu, dropping link\n",
                hdr->size, cap);
        return -1;
    }

    /* 2. payload: whatever is already in the ring, then straight from the wire */
    size_t got = 0;
    size_t from_ring = s->len < hdr->size ? s->len : hdr->size;
    if (from_ring) { memcpy(buf, s->rb + s->head, from_ring); consume(s, from_ring); got = from_ring; }

    while (got < hdr->size) {
        int r = raw_read(s, buf + got, hdr->size - got);
        if (r < 0) return -1;
        if (r == 0) continue;                 /* mid-packet stall: keep waiting */
        got += (size_t)r;
    }
    return 1;
}

/* ---- open / close --------------------------------------------------------- */

source_t *source_open_tcp(const char *host, int port)
{
    char ports[16];
    snprintf(ports, sizeof(ports), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) {
        fprintf(stderr, "tcp: cannot resolve %s\n", host);
        return NULL;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("tcp: connect");
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    source_t *s = calloc(1, sizeof(*s));
    s->kind = 0;
    s->fd = fd;
    return s;
}

source_t *source_open_usb(void)
{
    source_t *s = calloc(1, sizeof(*s));
    s->kind = 1;

    if (libusb_init(&s->uctx) != 0) { free(s); return NULL; }

    s->uh = libusb_open_device_with_vid_pid(s->uctx, SW_USB_VID, SW_USB_PID);
    if (!s->uh) {
        fprintf(stderr, "usb: Switch not found (VID %04x PID %04x). "
                        "Is the sysmodule in 'usb' mode and the console in handheld?\n",
                SW_USB_VID, SW_USB_PID);
        libusb_exit(s->uctx);
        free(s);
        return NULL;
    }

    libusb_set_auto_detach_kernel_driver(s->uh, 1);
    if (libusb_claim_interface(s->uh, SW_USB_IFACE) != 0) {
        fprintf(stderr, "usb: cannot claim interface %d (permissions? see udev rule)\n",
                SW_USB_IFACE);
        libusb_close(s->uh);
        libusb_exit(s->uctx);
        free(s);
        return NULL;
    }
    return s;
}

void source_close(source_t *s)
{
    if (!s) return;
    if (s->kind == 0) {
        if (s->fd >= 0) close(s->fd);
    } else {
        if (s->uh) { libusb_release_interface(s->uh, SW_USB_IFACE); libusb_close(s->uh); }
        if (s->uctx) libusb_exit(s->uctx);
    }
    free(s);
}
