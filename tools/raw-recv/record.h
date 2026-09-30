/* record.h - v0.5: recording the stream to MP4 from the viewer (R).
 *
 * The console's H.264 goes into the file untouched - no re-encoding, the
 * exact picture that arrived - and the game audio (16-bit PCM, which MP4
 * cannot carry) is encoded to AAC with FFmpeg's own encoder. Both are
 * timestamped with the PC's arrival clock, the same clock live playback
 * keeps them in step with.
 *
 *  - A recording starts at the next keyframe (the console sends one every
 *    second): H.264 cannot start anywhere else.
 *  - Fragmented MP4 (a fragment per keyframe): a recording cut short by a
 *    crash, a closed window or a pulled cable still plays.
 *  - The picture size can change mid-game (ReverseNX-RT, 720p <-> 1080p);
 *    one MP4 video track cannot, so the recording continues in a new file.
 *  - Its own thread writes the file: a slow disk never stalls the USB reader
 *    or the display. Packets wait in a queue of at most RecQueueBytes; a disk
 *    that cannot keep up stops the recording rather than the stream.
 *  - It refuses to start with less than 1 GB free and stops below 500 MB.
 *
 * Used from three threads: rec_video (the decoder thread), rec_audio (the USB
 * reader thread), rec_toggle / rec_status (the display thread).
 */
#if defined(SFT_H264) && defined(SFT_MP4)
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <direct.h>
#else
#include <sys/statvfs.h>
#endif

#define RecQueueBytes (192u * 1024 * 1024)
#define RecMinFreeStart (1024ull * 1024 * 1024)
#define RecMinFreeKeep  (500ull * 1024 * 1024)

typedef struct rec_item {
    struct rec_item *next;
    int audio, key;
    uint32_t w, h;
    Uint64 t;                  /* arrival, SDL performance counter */
    size_t n;
    uint8_t data[];
} rec_item_t;

enum { REC_OFF = 0, REC_WAIT_KEY, REC_ON };

static struct {
    SDL_mutex *m;
    SDL_cond *cv;
    SDL_Thread *th;
    volatile int state;        /* REC_* - what the producers see */
    volatile int stop_req;
    rec_item_t *head, *tail;
    size_t queued;
    char dir[1024];
    char path[1100];           /* the current file */
    char msg[256];             /* the last thing worth showing (errors, "saved ...") */
    Uint64 msg_t;
    volatile double secs;      /* of the current recording */
    volatile double mb;
    int fixed_path;            /* --mp4 FILE: exactly this file */
} g_rec;

static void rec_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_rec.msg, sizeof(g_rec.msg), fmt, ap);
    va_end(ap);
    g_rec.msg_t = SDL_GetPerformanceCounter();
    fprintf(stderr, "\nrecording: %s\n", g_rec.msg);
}

#ifdef _WIN32
/* paths are UTF-8 throughout (what FFmpeg's avio_open expects on Windows);
 * the Win32 calls get them as UTF-16, so a name like "José" works */
static void rec_wide(const char *utf8, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap)) out[0] = 0;
}
#endif

static unsigned long long rec_free_bytes(const char *dir)
{
#ifdef _WIN32
    ULARGE_INTEGER avail;
    wchar_t w[1024];
    rec_wide(dir, w, 1024);
    if (GetDiskFreeSpaceExW(w, &avail, NULL, NULL)) return avail.QuadPart;
    return ~0ull;
#else
    struct statvfs s;
    if (statvfs(dir, &s) == 0) return (unsigned long long)s.f_bavail * s.f_frsize;
    return ~0ull;
#endif
}

static void rec_mkdir(const char *p)
{
#ifdef _WIN32
    wchar_t w[1024];
    rec_wide(p, w, 1024);
    _wmkdir(w);
#else
    mkdir(p, 0755);
#endif
}

/* Videos/Switch Frame Tap (Windows: the Videos folder, wherever the user
 * or OneDrive moved it; Linux: ~/Videos) */
static void rec_default_dir(char *out, size_t cap)
{
#ifdef _WIN32
    wchar_t wv[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYVIDEO | CSIDL_FLAG_CREATE, NULL, 0, wv))) {
        char v[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, wv, -1, v, sizeof(v), NULL, NULL)) {
            snprintf(out, cap, "%s\\Switch Frame Tap", v);
            rec_mkdir(out);
            return;
        }
    }
    const char *home = getenv("USERPROFILE");
    const char sep = '\\';
#else
    const char *home = getenv("HOME");
    const char sep = '/';
#endif
    if (!home) home = ".";
    char videos[900];
    snprintf(videos, sizeof(videos), "%s%cVideos", home, sep);
    rec_mkdir(videos);
    snprintf(out, cap, "%s%cSwitch Frame Tap", videos, sep);
    rec_mkdir(out);
}

/* ---- the H.264 side: the console's access units are Annex-B SPS + PPS +
 * one slice. The parameter sets go into the MP4 header (extradata) once;
 * the samples keep only the slices. */
static const uint8_t *nal_next(const uint8_t *p, const uint8_t *end, const uint8_t **nal)
{
    /* finds the next start code at or after p; *nal = the NAL's first byte */
    for (; p + 3 <= end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { *nal = p + 3; return p; }
    }
    *nal = NULL;
    return end;
}

/* the SPS and PPS NALs (with start codes) at the front of an AU */
static size_t h264_param_sets(const uint8_t *d, size_t n, uint8_t *out, size_t cap)
{
    const uint8_t *end = d + n, *nal = NULL;
    nal_next(d, end, &nal);
    size_t o = 0;
    while (nal) {
        const uint8_t *nxt = NULL;
        const uint8_t *sc = nal_next(nal, end, &nxt);
        const uint8_t *stop = sc;
        while (stop > nal && stop[-1] == 0) stop--;     /* the 00 of a 4-byte start code */
        const int type = nal[0] & 0x1F;
        if (type == 7 || type == 8) {
            const size_t len = (size_t)(stop - nal);
            if (o + 4 + len <= cap) { out[o] = 0; out[o + 1] = 0; out[o + 2] = 0; out[o + 3] = 1; memcpy(out + o + 4, nal, len); o += 4 + len; }
        }
        nal = nxt;
    }
    return o;
}

/* the AU without its SPS / PPS (Annex-B, 4-byte start codes) */
static size_t h264_strip_params(const uint8_t *d, size_t n, uint8_t *out)
{
    const uint8_t *end = d + n, *nal = NULL;
    nal_next(d, end, &nal);
    size_t o = 0;
    while (nal) {
        const uint8_t *nxt = NULL;
        const uint8_t *sc = nal_next(nal, end, &nxt);
        const uint8_t *stop = nxt ? sc : end;
        if (nxt) while (stop > nal && stop[-1] == 0) stop--;
        const int type = nal[0] & 0x1F;
        if (type != 7 && type != 8 && type != 9) {
            const size_t len = (size_t)(stop - nal);
            out[o] = 0; out[o + 1] = 0; out[o + 2] = 0; out[o + 3] = 1;
            memcpy(out + o + 4, nal, len);
            o += 4 + len;
        }
        nal = nxt;
    }
    return o;
}

/* ---- the writer thread's file ------------------------------------------- */
typedef struct {
    AVFormatContext *fc;
    AVStream *vs, *as;
    AVCodecContext *ac;
    AVFrame *af;
    AVPacket *pkt;
    uint32_t w, h;
    Uint64 t0;                 /* arrival of the first keyframe */
    int64_t last_vpts;
    int64_t a_next;            /* the next audio sample's pts (48 kHz) */
    int64_t a_off, v_off;      /* the AAC encoder's priming, in each track's time base:
                                  both tracks start after it, so no timestamp is ever
                                  negative and the muxer never shifts one of them */
    int a_fill;                /* samples waiting in af */
    int64_t bytes;
    int part;
    uint8_t *tmp; size_t tmp_cap;
} rec_file_t;

static int64_t rec_us(Uint64 t, Uint64 t0)
{
    return (int64_t)((double)(t - t0) * 1e6 / (double)SDL_GetPerformanceFrequency());
}

static void rec_audio_flush_frame(rec_file_t *f)
{
    if (!f->ac || f->a_fill == 0) return;
    f->af->nb_samples = f->a_fill;
    f->af->pts = f->a_next - f->a_fill + f->a_off;
    if (avcodec_send_frame(f->ac, f->af) == 0) {
        while (avcodec_receive_packet(f->ac, f->pkt) == 0) {
            av_packet_rescale_ts(f->pkt, f->ac->time_base, f->as->time_base);
            f->pkt->stream_index = f->as->index;
            f->bytes += f->pkt->size;
            av_interleaved_write_frame(f->fc, f->pkt);
        }
    }
    f->a_fill = 0;
    av_frame_make_writable(f->af);
}

/* 16-bit interleaved stereo -> the AAC encoder's planar float, 1024 at a time */
static void rec_audio_samples(rec_file_t *f, const int16_t *s, int n)
{
    const int fs = f->ac->frame_size ? f->ac->frame_size : 1024;
    for (int i = 0; i < n; i++) {
        ((float *)f->af->data[0])[f->a_fill] = s[2 * i] / 32768.0f;
        ((float *)f->af->data[1])[f->a_fill] = s[2 * i + 1] / 32768.0f;
        f->a_fill++;
        f->a_next++;
        if (f->a_fill == fs) rec_audio_flush_frame(f);
    }
}

static void rec_close_file(rec_file_t *f, const char *why)
{
    if (!f->fc) return;
    if (f->ac) {
        rec_audio_flush_frame(f);
        avcodec_send_frame(f->ac, NULL);
        while (avcodec_receive_packet(f->ac, f->pkt) == 0) {
            av_packet_rescale_ts(f->pkt, f->ac->time_base, f->as->time_base);
            f->pkt->stream_index = f->as->index;
            av_interleaved_write_frame(f->fc, f->pkt);
        }
    }
    av_write_trailer(f->fc);
    avio_closep(&f->fc->pb);
    avformat_free_context(f->fc);
    f->fc = NULL;
    if (f->ac) avcodec_free_context(&f->ac);
    av_frame_free(&f->af);
    av_packet_free(&f->pkt);
    rec_msg("saved %s (%.0f MB, %.0f s)%s%s", g_rec.path, g_rec.mb, g_rec.secs, why ? " - " : "", why ? why : "");
}

static int rec_open_file(rec_file_t *f, const rec_item_t *key)
{
    char stamp[64];
    const time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", localtime(&now));
    if (g_rec.fixed_path) {
        snprintf(g_rec.path, sizeof(g_rec.path), "%s", g_rec.dir);
    } else {
#ifdef _WIN32
        snprintf(g_rec.path, sizeof(g_rec.path), "%s\\switch_%s.mp4", g_rec.dir, stamp);
#else
        snprintf(g_rec.path, sizeof(g_rec.path), "%s/switch_%s.mp4", g_rec.dir, stamp);
#endif
    }
    memset(f, 0, sizeof(*f));
    f->w = key->w; f->h = key->h;
    f->t0 = key->t;
    f->last_vpts = -1;
    if (avformat_alloc_output_context2(&f->fc, NULL, "mp4", g_rec.path) < 0 || !f->fc) return -1;

    /* video: the console's stream as it is */
    f->vs = avformat_new_stream(f->fc, NULL);
    f->vs->time_base = (AVRational){ 1, 90000 };
    f->vs->avg_frame_rate = (AVRational){ 60, 1 };
    AVCodecParameters *vp = f->vs->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_H264;
    vp->width = (int)key->w; vp->height = (int)key->h;
    vp->color_primaries = AVCOL_PRI_BT709; vp->color_trc = AVCOL_TRC_BT709; vp->color_space = AVCOL_SPC_BT709; vp->color_range = AVCOL_RANGE_MPEG;
    uint8_t ps[512];
    const size_t psn = h264_param_sets(key->data, key->n, ps, sizeof(ps));
    if (psn == 0) return -1;
    vp->extradata = av_mallocz(psn + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(vp->extradata, ps, psn);
    vp->extradata_size = (int)psn;

    /* audio: AAC-LC 192 kbps, 48 kHz stereo */
    const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (aac && (f->ac = avcodec_alloc_context3(aac)) != NULL) {
        f->ac->sample_rate = 48000;
        f->ac->sample_fmt = AV_SAMPLE_FMT_FLTP;
        f->ac->bit_rate = 192000;
        f->ac->time_base = (AVRational){ 1, 48000 };
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_default(&f->ac->ch_layout, 2);
#else
        f->ac->channel_layout = AV_CH_LAYOUT_STEREO;
        f->ac->channels = 2;
#endif
        if (f->fc->oformat->flags & AVFMT_GLOBALHEADER) f->ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(f->ac, aac, NULL) < 0) { avcodec_free_context(&f->ac); }
    }
    if (f->ac) {
        f->as = avformat_new_stream(f->fc, NULL);
        f->as->time_base = (AVRational){ 1, 48000 };
        avcodec_parameters_from_context(f->as->codecpar, f->ac);
        f->af = av_frame_alloc();
        f->af->format = AV_SAMPLE_FMT_FLTP;
        f->af->sample_rate = 48000;
        f->af->nb_samples = f->ac->frame_size ? f->ac->frame_size : 1024;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_copy(&f->af->ch_layout, &f->ac->ch_layout);
#else
        f->af->channel_layout = AV_CH_LAYOUT_STEREO;
        f->af->channels = 2;
#endif
        av_frame_get_buffer(f->af, 0);
    } else {
        rec_msg("no AAC encoder in this FFmpeg - recording without sound");
    }
    f->pkt = av_packet_alloc();
    if (f->ac) {
        f->a_off = f->ac->initial_padding;
        f->v_off = av_rescale(f->a_off, 90000, 48000);
    }

    if (avio_open(&f->fc->pb, g_rec.path, AVIO_FLAG_WRITE) < 0) return -2;
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    const int hr = avformat_write_header(f->fc, &opt);
    av_dict_free(&opt);
    if (hr < 0) return -1;
    g_rec.secs = 0;
    g_rec.mb = 0;
    rec_msg("recording to %s (%ux%u)", g_rec.path, key->w, key->h);
    return 0;
}

static void rec_video_write(rec_file_t *f, const rec_item_t *it)
{
    if (f->tmp_cap < it->n + 64) { free(f->tmp); f->tmp_cap = it->n + 1024; f->tmp = malloc(f->tmp_cap); }
    if (!f->tmp) return;
    const size_t n = h264_strip_params(it->data, it->n, f->tmp);
    int64_t pts = av_rescale_q(rec_us(it->t, f->t0), (AVRational){ 1, 1000000 }, f->vs->time_base)
                + av_rescale_q(f->v_off, (AVRational){ 1, 90000 }, f->vs->time_base);
    if (pts <= f->last_vpts) pts = f->last_vpts + 1;
    f->last_vpts = pts;
    av_packet_unref(f->pkt);
    f->pkt->data = f->tmp;
    f->pkt->size = (int)n;
    f->pkt->pts = f->pkt->dts = pts;
    f->pkt->duration = 0;
    f->pkt->stream_index = f->vs->index;
    f->pkt->flags = it->key ? AV_PKT_FLAG_KEY : 0;
    f->bytes += (int64_t)n;
    av_interleaved_write_frame(f->fc, f->pkt);
    f->pkt->data = NULL; f->pkt->size = 0;
    g_rec.secs = (double)rec_us(it->t, f->t0) / 1e6;
    g_rec.mb = (double)f->bytes / 1e6;
}

static void rec_audio_write(rec_file_t *f, const rec_item_t *it)
{
    if (!f->ac || it->t < f->t0) return;
    /* where this packet belongs by the arrival clock; audio that the console
     * did not send (a game with capture off, a hiccup) becomes silence, so
     * the sound never slides earlier than the picture */
    const int64_t want = rec_us(it->t, f->t0) * 48 / 1000 - (int64_t)(it->n / 4);
    if (want > f->a_next + 4800) {
        static const int16_t zero[2 * 480] = { 0 };
        while (f->a_next < want) {
            const int64_t gap = want - f->a_next;
            rec_audio_samples(f, zero, gap > 480 ? 480 : (int)gap);
        }
    }
    rec_audio_samples(f, (const int16_t *)it->data, (int)(it->n / 4));
}

static int rec_main(void *arg)
{
    (void)arg;
    rec_file_t f;
    memset(&f, 0, sizeof(f));
    int check = 0;
    for (;;) {
        SDL_LockMutex(g_rec.m);
        while (!g_rec.head && !g_rec.stop_req) SDL_CondWaitTimeout(g_rec.cv, g_rec.m, 200);
        rec_item_t *it = g_rec.head;
        if (it) { g_rec.head = it->next; if (!g_rec.head) g_rec.tail = NULL; g_rec.queued -= it->n; }
        const int stopping = g_rec.stop_req && !it;
        SDL_UnlockMutex(g_rec.m);
        if (stopping) break;
        if (!it) {
            /* stopped by itself (a full or slow disk): close the file now */
            if (g_rec.state == REC_OFF && f.fc) rec_close_file(&f, NULL);
            continue;
        }
        if (!it->audio) {
            if (f.fc && (it->w != f.w || it->h != f.h)) {
                rec_close_file(&f, "the picture size changed, continuing in a new file");
                if (g_rec.fixed_path) { free(it); g_rec.state = REC_OFF; continue; }
            }
            if (!f.fc) {
                if (!it->key) { free(it); continue; }
                const int r = rec_open_file(&f, it);
                if (r != 0) {
                    rec_msg("could not create %s", g_rec.path);
                    if (f.fc) { if (f.fc->pb) avio_closep(&f.fc->pb); avformat_free_context(f.fc); f.fc = NULL; }
                    if (f.ac) avcodec_free_context(&f.ac);
                    av_frame_free(&f.af); av_packet_free(&f.pkt);
                    g_rec.state = REC_OFF;
                    free(it);
                    continue;
                }
            }
            rec_video_write(&f, it);
            if (++check % 120 == 0 && !g_rec.fixed_path && rec_free_bytes(g_rec.dir) < RecMinFreeKeep) {
                rec_close_file(&f, "stopped: the disk is almost full");
                g_rec.state = REC_OFF;
            }
        } else if (f.fc) {
            rec_audio_write(&f, it);
        }
        free(it);
    }
    rec_close_file(&f, NULL);
    free(f.tmp);
    return 0;
}

static void rec_push(int audio, int key, uint32_t w, uint32_t h, Uint64 t, const uint8_t *d, size_t n)
{
    rec_item_t *it = malloc(sizeof(*it) + n);
    if (!it) return;
    it->next = NULL; it->audio = audio; it->key = key; it->w = w; it->h = h; it->t = t; it->n = n;
    memcpy(it->data, d, n);
    SDL_LockMutex(g_rec.m);
    if (g_rec.queued + n > RecQueueBytes) {
        SDL_UnlockMutex(g_rec.m);
        free(it);
        if (g_rec.state != REC_OFF) { g_rec.state = REC_OFF; g_rec.stop_req = 0; rec_msg("stopped: the disk could not keep up"); }
        return;
    }
    if (g_rec.tail) g_rec.tail->next = it; else g_rec.head = it;
    g_rec.tail = it;
    g_rec.queued += n;
    SDL_CondSignal(g_rec.cv);
    SDL_UnlockMutex(g_rec.m);
}

/* ---- the producers' and the UI's side ------------------------------------ */

/* decoder thread: every H.264 access unit */
static void rec_video(const sft_hdr_t *hdr, const uint8_t *d, Uint64 t)
{
    if (g_rec.state == REC_OFF) return;
    /* an IDR slice (NAL 5) in the AU - not the header flag, which older
     * streams do not set */
    int key = 0;
    {
        const uint8_t *end = d + hdr->length, *nal = NULL;
        nal_next(d, end, &nal);
        while (nal && !key) {
            if ((nal[0] & 0x1F) == 5) key = 1;
            const uint8_t *nxt = NULL;
            nal_next(nal, end, &nxt);
            nal = nxt;
        }
    }
    if (g_rec.state == REC_WAIT_KEY) { if (!key) return; g_rec.state = REC_ON; }
    rec_push(0, key, hdr->width, hdr->height, t, d, hdr->length);
}

/* reader thread: every audio packet (recorded even while muted) */
static void rec_audio(const uint8_t *d, uint32_t n, uint32_t rate, uint32_t ch, Uint64 t)
{
    if (g_rec.state != REC_ON || rate != 48000 || ch != 2) return;
    rec_push(1, 0, 0, 0, t, d, n);
}

static void rec_start_thread(void)
{
    if (g_rec.th) return;
    g_rec.stop_req = 0;
    g_rec.th = SDL_CreateThread(rec_main, "recorder", NULL);
}

/* the writer finishes what is queued, closes the file, and exits */
static void rec_join(void)
{
    g_rec.state = REC_OFF;
    if (!g_rec.th) return;
    SDL_LockMutex(g_rec.m);
    g_rec.stop_req = 1;
    SDL_CondSignal(g_rec.cv);
    SDL_UnlockMutex(g_rec.m);
    SDL_WaitThread(g_rec.th, NULL);
    g_rec.th = NULL;
}

static void rec_init(const char *dir_or_null, const char *fixed_file)
{
    if (g_rec.m) return;               /* once: the menu needs the folder before a stream starts */
    g_rec.m = SDL_CreateMutex();
    g_rec.cv = SDL_CreateCond();
    if (fixed_file) { snprintf(g_rec.dir, sizeof(g_rec.dir), "%s", fixed_file); g_rec.fixed_path = 1; }
    else if (dir_or_null) { snprintf(g_rec.dir, sizeof(g_rec.dir), "%s", dir_or_null); rec_mkdir(g_rec.dir); }
    else rec_default_dir(g_rec.dir, sizeof(g_rec.dir));
}

/* R: start (from the next keyframe) or stop */
static void rec_toggle(void)
{
    if (g_rec.state == REC_OFF) {
        if (!g_rec.fixed_path && rec_free_bytes(g_rec.dir) < RecMinFreeStart) {
            rec_msg("not started: less than 1 GB free in %s", g_rec.dir);
            return;
        }
        rec_join();                    /* a previous recording's writer */
        rec_start_thread();
        g_rec.state = REC_WAIT_KEY;
        g_rec.secs = 0; g_rec.mb = 0;
        fprintf(stderr, "\nrecording: starts at the next keyframe\n");
    } else {
        rec_join();
    }
}

static int rec_active(void) { return g_rec.state != REC_OFF; }
static const char *rec_dir_for_menu(void) { return g_rec.fixed_path ? "" : g_rec.dir; }

/* for the window title: "" when idle and nothing recent to say */
static void rec_status(char *out, size_t cap)
{
    const double since = g_rec.msg_t ? (double)(SDL_GetPerformanceCounter() - g_rec.msg_t) / (double)SDL_GetPerformanceFrequency() : 1e9;
    if (g_rec.state == REC_WAIT_KEY) snprintf(out, cap, "  |  REC starting...");
    else if (g_rec.state == REC_ON) snprintf(out, cap, "  |  REC %d:%02d  %.0f MB", (int)g_rec.secs / 60, (int)g_rec.secs % 60, g_rec.mb);
    else if (since < 6.0 && g_rec.msg[0]) snprintf(out, cap, "  |  %.150s", g_rec.msg);
    else out[0] = 0;
}

#else
static void rec_video(const void *h, const uint8_t *d, Uint64 t) { (void)h; (void)d; (void)t; }
static void rec_audio(const uint8_t *d, uint32_t n, uint32_t r, uint32_t c, Uint64 t) { (void)d; (void)n; (void)r; (void)c; (void)t; }
static void rec_toggle(void) { fprintf(stderr, "\nrecording: this build has no MP4 support (libavformat)\n"); }
static int rec_active(void) { return 0; }
static void rec_status(char *out, size_t cap) { (void)cap; out[0] = 0; }
static void rec_join(void) { }
static void rec_init(const char *d, const char *f) { (void)d; (void)f; }
static const char *rec_dir_for_menu(void) { return ""; }
#endif
