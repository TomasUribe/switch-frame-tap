/*
 * uvc-check - v0.4 webcam mode test: finds the "Switch Frame Tap Camera"
 * V4L2 node, streams H.264 from it for a few seconds, decodes every frame with
 * libavcodec and reports what arrived. No ffmpeg/OBS needed.
 *
 *   uvc-check [--size 1080|720|432] [--seconds N] [--save out.h264] [--shot out.ppm]
 *
 * v0.7.7: webcam-any (an uncompressed NV12 camera) is found by its format:
 * frames are counted as complete when they carry all w*h*3/2 bytes, and
 * --shot writes the first one as a PPM (BT.709, limited range).
 *
 * Needs read access to /dev/video* (the `video` group, or sudo).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <libavcodec/avcodec.h>

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static int find_camera(char *path, size_t cap) {
    int denied = 0;
    for (int i = 0; i < 64; ++i) {
        snprintf(path, cap, "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) { if (errno == EACCES) { denied = 1; } continue; }
        struct v4l2_capability c;
        memset(&c, 0, sizeof(c));
        int ok = xioctl(fd, VIDIOC_QUERYCAP, &c) == 0 && strstr((const char *)c.card, "Switch Frame Tap") != NULL
                 && (c.device_caps & V4L2_CAP_VIDEO_CAPTURE);
        if (ok) { printf("camera: %s  card \"%s\"  driver %s\n", path, c.card, c.driver); return fd; }
        close(fd);
    }
    if (denied) { fprintf(stderr, "no permission on /dev/video* - add yourself to the video group (then log in again) or use sudo\n"); }
    else { fprintf(stderr, "no \"Switch Frame Tap Camera\" found - is the console in webcam mode and plugged in?\n"); }
    return -1;
}

int main(int argc, char **argv) {
    unsigned want_h = 1080, seconds = 10;
    const char *save = NULL, *shot = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) { want_h = (unsigned)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) { seconds = (unsigned)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) { save = argv[++i]; }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; }
        else { fprintf(stderr, "usage: %s [--size 1080|720|432] [--seconds N] [--save out.h264] [--shot out.ppm]\n", argv[0]); return 2; }
    }
    char path[64];
    int fd = find_camera(path, sizeof(path));
    if (fd < 0) { return 1; }

    struct v4l2_fmtdesc fd_;
    memset(&fd_, 0, sizeof(fd_));
    fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int nv12 = 0;
    for (fd_.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fd_) == 0; ++fd_.index) {
        printf("format %u: %.4s \"%s\"\n", fd_.index, (const char *)&fd_.pixelformat, fd_.description);
        if (fd_.pixelformat == V4L2_PIX_FMT_NV12) { nv12 = 1; }
        struct v4l2_frmsizeenum fs;
        memset(&fs, 0, sizeof(fs));
        fs.pixel_format = fd_.pixelformat;
        for (fs.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; ++fs.index) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) { printf("   %ux%u\n", fs.discrete.width, fs.discrete.height); }
        }
    }

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (nv12) {
        f.fmt.pix.width = want_h == 432 ? 768 : 1280;
        f.fmt.pix.height = want_h == 432 ? 432 : 720;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    } else {
        f.fmt.pix.width = want_h == 720 ? 1280 : 1920;
        f.fmt.pix.height = want_h == 720 ? 720 : 1080;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    }
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("VIDIOC_S_FMT"); return 1; }
    printf("negotiated %ux%u %.4s, buffer %u B\n", f.fmt.pix.width, f.fmt.pix.height, (const char *)&f.fmt.pix.pixelformat, f.fmt.pix.sizeimage);
    struct v4l2_streamparm sp;
    memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_PARM, &sp) == 0 && sp.parm.capture.timeperframe.numerator) {
        printf("frame interval %u/%u s (%.1f fps)\n", sp.parm.capture.timeperframe.numerator, sp.parm.capture.timeperframe.denominator,
               (double)sp.parm.capture.timeperframe.denominator / sp.parm.capture.timeperframe.numerator);
    }

    enum { NB = 4 };
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = NB; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("VIDIOC_REQBUFS"); return 1; }
    void *map[NB] = {0};
    size_t len[NB] = {0};
    for (unsigned i = 0; i < rb.count && i < NB; ++i) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = rb.type; b.memory = rb.memory; b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("VIDIOC_QUERYBUF"); return 1; }
        len[i] = b.length;
        map[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (map[i] == MAP_FAILED) { perror("mmap"); return 1; }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); return 1; }
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, NULL) < 0) { fprintf(stderr, "avcodec_open2 failed\n"); return 1; }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    FILE *out = save ? fopen(save, "wb") : NULL;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    const double t_open = now_s();
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return 1; }
    printf("streaming for %u s...\n", seconds);

    unsigned got = 0, decoded = 0, errors = 0, corrupt = 0, idr = 0, dec_w = 0, dec_h = 0, win = 0, shot_done = 0;
    const unsigned fw = f.fmt.pix.width, fh = f.fmt.pix.height, full = fw * fh * 3 / 2;
    unsigned long long bytes = 0;
    double first = 0, t_win = now_s(), worst_gap = 0, last = 0;
    while (now_s() - t_open < seconds) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = { 2, 0 };
        int r = select(fd + 1, &rs, NULL, NULL, &tv);
        if (r <= 0) { printf("   no frame for 2 s\n"); continue; }
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("VIDIOC_DQBUF"); break; }
        const double t = now_s();
        if (got == 0) { first = t; printf("first frame %.0f ms after STREAMON\n", (t - t_open) * 1000); }
        else if (t - last > worst_gap) { worst_gap = t - last; }
        last = t;
        ++got; ++win;
        if (b.flags & V4L2_BUF_FLAG_ERROR) { ++corrupt; }
        const unsigned char *p = map[b.index];
        bytes += b.bytesused;
        if (nv12) {
            /* complete frames count as "decoded"; a short one is an error */
            if (b.bytesused >= full && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
                ++decoded; dec_w = fw; dec_h = fh;
                if (shot && !shot_done) {
                    FILE *o = fopen(shot, "wb");
                    if (o) {
                        fprintf(o, "P6\n%u %u\n255\n", fw, fh);
                        for (unsigned y = 0; y < fh; ++y) {
                            for (unsigned x = 0; x < fw; ++x) {
                                const double Y = (p[y * fw + x] - 16) * 1.164;
                                const unsigned char *c = p + fw * fh + (y / 2) * fw + (x & ~1u);
                                const double U = c[0] - 128.0, V = c[1] - 128.0;
                                double rgb[3] = { Y + 1.793 * V, Y - 0.213 * U - 0.533 * V, Y + 2.112 * U };
                                unsigned char px[3];
                                for (int k = 0; k < 3; ++k) { px[k] = (unsigned char)(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k] + 0.5); }
                                fwrite(px, 1, 3, o);
                            }
                        }
                        fclose(o);
                        printf("   first complete frame saved to %s\n", shot);
                    }
                    shot_done = 1;
                }
            } else { ++errors; }
            if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); break; }
            if (t - t_win >= 1.0) {
                printf("   %5.1f fps, %u complete so far (%ux%u NV12), %llu KB total\n", win / (t - t_win), decoded, fw, fh, bytes / 1024);
                win = 0; t_win = t;
            }
            continue;
        }
        for (unsigned k = 0; k + 4 < b.bytesused && k < 256; ++k) {
            if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1 && (p[k + 3] & 0x1F) == 5) { ++idr; break; }
        }
        if (out) { fwrite(p, 1, b.bytesused, out); }
        pkt->data = (uint8_t *)p;
        pkt->size = (int)b.bytesused;
        if (avcodec_send_packet(ctx, pkt) < 0) { ++errors; }
        while (avcodec_receive_frame(ctx, frm) == 0) {
            ++decoded;
            dec_w = frm->width; dec_h = frm->height;
            if (frm->decode_error_flags) { ++errors; }
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); break; }
        if (t - t_win >= 1.0) {
            printf("   %5.1f fps, %u decoded so far (%ux%u), %llu KB total\n", win / (t - t_win), decoded, dec_w, dec_h, bytes / 1024);
            win = 0; t_win = t;
        }
    }
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    const double span = last > first ? last - first : 0;
    if (nv12) {
        printf("\n%u frames in %.1f s = %.1f fps; %u complete (%ux%u NV12), %u short or flagged, %u flagged corrupt by the driver\n",
               got, span, span > 0 ? (got - 1) / span : 0, decoded, dec_w, dec_h, errors, corrupt);
    } else {
    printf("\n%u frames in %.1f s = %.1f fps; %u decoded (%ux%u), %u IDR, %u decode errors, %u flagged corrupt by the driver\n",
           got, span, span > 0 ? (got - 1) / span : 0, decoded, dec_w, dec_h, idr, errors, corrupt);
    }
    printf("avg %llu KB/frame (%.1f Mbps), worst gap between frames %.0f ms\n",
           got ? bytes / got / 1024 : 0, span > 0 ? bytes * 8 / span / 1e6 : 0, worst_gap * 1000);
    if (out) { fclose(out); printf("saved to %s\n", save); }
    for (unsigned i = 0; i < NB; ++i) { if (map[i]) { munmap(map[i], len[i]); } }
    close(fd);
    return (got > 0 && decoded > 0) ? 0 : 1;
}
