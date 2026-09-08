#include "transport.h"
#include "proto.h"
#include "capture.h"

#include <switch.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static tr_mode_t g_mode;
static uint16_t  g_port = SW_PROTO_PORT;

static Mutex     g_lock;              /* serialises writers onto the pipe   */
static volatile bool g_connected;
static int       g_listen_fd = -1;
static int       g_client_fd = -1;

/* fixed capture format */
static const sw_hello_t g_hello = {
    .width = 1280, .height = 720,
    .fps_num = 30, .fps_den = 1,
    .audio_rate = 48000, .audio_channels = 2,
};

/* Scratch buffer for header+payload assembly. All writers are serialised by
 * g_lock, so one shared buffer is enough. Sized for the largest video batch. */
#define TX_MAX_PAYLOAD CAP_VIDEO_BUF
static uint8_t g_tx[sizeof(sw_hdr_t) + TX_MAX_PAYLOAD];

/* -------------------------------------------------------------------------- */

int tr_init(const char *mode)
{
    mutexInit(&g_lock);
    g_connected = false;

    if (mode && strncmp(mode, "tcp", 3) == 0) {
        g_mode = TR_TCP;
        const char *c = strchr(mode, ':');
        if (c && c[1]) g_port = (uint16_t)atoi(c + 1);

        g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (g_listen_fd < 0) return -1;

        int one = 1;
        setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = INADDR_ANY;
        sa.sin_port = htons(g_port);

        if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) return -1;
        if (listen(g_listen_fd, 1) < 0) return -1;
        return 0;
    }

    /* default: USB */
    g_mode = TR_USB;
    return R_SUCCEEDED(usbCommsInitialize()) ? 0 : -1;
}

void tr_exit(void)
{
    tr_drop_client();
    if (g_mode == TR_TCP) {
        if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    } else {
        usbCommsExit();
    }
}

tr_mode_t tr_get_mode(void) { return g_mode; }
bool      tr_connected(void) { return g_connected; }

void tr_drop_client(void)
{
    mutexLock(&g_lock);
    if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
    g_connected = false;
    mutexUnlock(&g_lock);
}

/* -------------------------------------------------------------------------- */

static int write_all_tcp(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

static int write_all_usb(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        size_t w = usbCommsWrite(b, n);   /* blocks until the host reads */
        if (w == 0) return -1;            /* host detached / disabled */
        b += w; n -= w;
    }
    return 0;
}

static int pipe_write_locked(const void *p, size_t n)
{
    if (g_mode == TR_TCP) {
        if (g_client_fd < 0) return -1;
        return write_all_tcp(g_client_fd, p, n);
    }
    return write_all_usb(p, n);
}

/* -------------------------------------------------------------------------- */

int tr_send(uint8_t type, uint8_t flags, uint64_t ts,
            const void *payload, uint32_t len)
{
    if (!g_connected) return -1;
    if (len > TX_MAX_PAYLOAD) return -1;

    mutexLock(&g_lock);

    sw_hdr_t *h = (sw_hdr_t *)g_tx;
    h->magic = SW_MAGIC;
    h->type  = type;
    h->flags = flags;
    h->_rsvd = 0;
    h->size  = len;
    h->ts    = ts;
    if (len) memcpy(g_tx + sizeof(*h), payload, len);

    int rc = pipe_write_locked(g_tx, sizeof(*h) + len);
    if (rc < 0) {
        if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
        g_connected = false;
    }
    mutexUnlock(&g_lock);
    return rc;
}

int tr_send_hello(void)
{
    return tr_send(SW_PKT_HELLO, 0, 0, &g_hello, sizeof(g_hello));
}

/* -------------------------------------------------------------------------- */

int tr_wait_client(void)
{
    if (g_mode == TR_TCP) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(g_listen_fd, (struct sockaddr *)&ca, &cl);
        if (fd < 0) return -1;

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int snd = 128 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        mutexLock(&g_lock);
        g_client_fd = fd;
        g_connected = true;
        mutexUnlock(&g_lock);

        return tr_send_hello();
    }

    /* USB: the blocking HELLO write itself is our "client attached" signal. */
    g_connected = true;
    if (tr_send_hello() < 0) { g_connected = false; return -1; }
    return 0;
}
