/* report.h - v0.7.24: Settings -> Report a problem.
 *
 * The viewer asks the console for its log ("SFTG"); the console sends
 * config.ini, the log's start and end, the lines it holds in memory, last.txt
 * and the end of the last boot's log, in pieces flagged audio + report (256)
 * with rate 0 - height 1 on the last. With this viewer's own logs they go
 * into one text file, switch-frame-tap-report-<when>.txt, in the recordings
 * folder, and the folder opens. A console older than 0.7.24 does not answer:
 * after 8 s the file holds the viewer's side alone, and says so. */
#define SFT_FLAG_REPORT  256u

enum { REP_IDLE = 0, REP_ASKING, REP_SAVED, REP_FAILED };
static SDL_atomic_t g_rep_state;
static SDL_atomic_t g_want_report;        /* the reader sends "SFTG" */
static SDL_SpinLock g_rep_lock = 0;
static char *g_rep_buf = NULL;            /* the console's side, as it comes */
static size_t g_rep_len = 0, g_rep_cap = 0;
static int g_rep_done = 0;                /* its last piece came */
static Uint64 g_rep_t0 = 0;
static char g_rep_msg[300] = "";
static char g_rep_path[1300] = "";
static int g_report_cli = 0;              /* --report (tests): asked once connected, or 4 s in */
static int g_report_test = 0;             /* ...and the folder is not opened */

/* the reader thread: a piece of the console's report */
static void report_packet(const uint8_t *d, uint32_t n, int last)
{
    SDL_AtomicLock(&g_rep_lock);
    if (SDL_AtomicGet(&g_rep_state) == REP_ASKING && !g_rep_done) {
        if (g_rep_len + n > g_rep_cap && g_rep_len + n <= 4u * 1024 * 1024) {
            size_t cap = g_rep_cap ? g_rep_cap : 256 * 1024;
            while (cap < g_rep_len + n) cap *= 2;
            char *b = realloc(g_rep_buf, cap);
            if (b) { g_rep_buf = b; g_rep_cap = cap; }
        }
        if (g_rep_len + n <= g_rep_cap) { memcpy(g_rep_buf + g_rep_len, d, n); g_rep_len += n; }
        if (last) g_rep_done = 1;
    }
    SDL_AtomicUnlock(&g_rep_lock);
}

/* the reader thread, beside key_send_pending: "SFTG" once asked */
static void report_send_pending(void)
{
    static Uint64 s_retry_at = 0;
    if (s_retry_at != 0 && SDL_GetTicks64() < s_retry_at) return;
    if (!SDL_AtomicCAS(&g_want_report, 1, 0)) return;
    static const unsigned char msg[4] = { 'S', 'F', 'T', 'G' };
    SDL_LockMutex(g_tx_m);
    if (g_sock != SOCK_BAD) { send(g_sock, (const char *)msg, sizeof(msg), 0); }
    else if (g_usb) {
        /* as key_send_pending: the console takes it only while a buffer of
         * its own waits - again 150 ms on, for up to ~2 s */
        static int tries = 0;
        int n = 0;
        const int rc = libusb_bulk_transfer(g_usb, SFT_EP_OUT, (unsigned char *)msg, sizeof(msg), &n, 20);
        if (rc == LIBUSB_ERROR_TIMEOUT && n == 0 && ++tries < 14) { SDL_AtomicSet(&g_want_report, 1); s_retry_at = SDL_GetTicks64() + 150; }
        else { tries = 0; s_retry_at = 0; }
    }
    SDL_UnlockMutex(g_tx_m);
}

static FILE *report_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1200], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1200) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8)) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

/* a file into the report: all of it, or its last `tail` bytes */
static void report_copy(FILE *out, const char *title, const char *path, long tail)
{
    fprintf(out, "\n===== %s =====\n", title);
    FILE *f = path[0] ? report_fopen(path, "rb") : NULL;
    if (!f) { fprintf(out, "(not there)\n"); return; }
    if (tail > 0 && fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        if (size > tail) { fseek(f, size - tail, SEEK_SET); fprintf(out, "(the last %ld of %ld bytes)\n", tail, size); }
        else fseek(f, 0, SEEK_SET);
    }
    char b[16384];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) fwrite(b, 1, n, out);
    fclose(f);
}

static void report_mkdir(const char *dir)
{
#ifdef _WIN32
    wchar_t w[1200];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1200)) CreateDirectoryW(w, NULL);
#else
    mkdir(dir, 0755);
#endif
}

static void report_save(void)
{
    SDL_AtomicLock(&g_rep_lock);
    char *console = g_rep_buf;
    const size_t clen = g_rep_len;
    const int complete = g_rep_done;
    g_rep_buf = NULL; g_rep_len = g_rep_cap = 0;
    SDL_AtomicUnlock(&g_rep_lock);

#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32], when[48];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

    /* the recordings folder (Videos/Switch Frame Tap), else beside viewer.log */
    char dir[1200] = "";
    snprintf(dir, sizeof(dir), "%s", rec_dir_for_menu());
    FILE *f = NULL;
    for (int pass = 0; pass < 2 && !f; pass++) {
        if (pass == 1) {
            snprintf(dir, sizeof(dir), "%s", g_viewer_log_path);
            char *cut = strrchr(dir, sep);
            if (cut) *cut = 0; else dir[0] = 0;
        }
        if (!dir[0]) continue;
        report_mkdir(dir);
        snprintf(g_rep_path, sizeof(g_rep_path), "%s%cswitch-frame-tap-report-%s.txt", dir, sep, stamp);
        f = report_fopen(g_rep_path, "wb");
    }
    if (!f) {
        snprintf(g_rep_path, sizeof(g_rep_path), "switch-frame-tap-report-%s.txt", stamp);
        f = report_fopen(g_rep_path, "wb");
    }
    if (!f) {
        snprintf(g_rep_msg, sizeof(g_rep_msg), "Could not write the report (%.200s)", g_rep_path);
        fprintf(stderr, "report: %s\n", g_rep_msg);
        free(console);
        SDL_AtomicSet(&g_rep_state, REP_FAILED);
        return;
    }
    SDL_version sv;
    SDL_GetVersion(&sv);
    fprintf(f, "Switch Frame Tap - problem report\nsaved: %s\nviewer: %s on %s (SDL %u.%u.%u)\n",
            when, SFT_VERSION, SDL_GetPlatform(), sv.major, sv.minor, sv.patch);
    fprintf(f, "connection: %s%s\n", g_sock != SOCK_BAD ? "network " : g_usb ? "USB" : "none (the Switch was not connected)", g_sock != SOCK_BAD ? g_net_peer : "");
    if (clen) fprintf(f, "console: %s (%zu bytes)\n", complete ? "its log follows" : "part of its log follows - it stopped answering", clen);
    else if (g_sock != SOCK_BAD || g_usb) fprintf(f, "console: did not answer - Switch Frame Tap on the console is older than 0.7.24, or it is busy\n");
    else fprintf(f, "console: not connected - copy config/switch-frame-tap/log.txt from the SD card if you can\n");
    if (clen) fwrite(console, 1, clen, f);
    free(console);
    fflush(stderr);
    report_copy(f, "viewer.log (this session)", g_viewer_log_path, 1024 * 1024);
    char prev[1200] = "";
    if (g_viewer_log_path[0]) {
        snprintf(prev, sizeof(prev), "%s", g_viewer_log_path);
        char *dot = strrchr(prev, '.');
        if (dot && !strcmp(dot, ".log")) snprintf(dot, sizeof(prev) - (size_t)(dot - prev), ".prev.log");
        else prev[0] = 0;
    }
    report_copy(f, "viewer.prev.log (the session before)", prev, 64 * 1024);
    fclose(f);

    const char *name = strrchr(g_rep_path, sep);
    snprintf(g_rep_msg, sizeof(g_rep_msg), "Saved %.200s", name ? name + 1 : g_rep_path);
    fprintf(stderr, "report: saved %s (console: %zu bytes%s)\n", g_rep_path, clen, complete ? "" : ", not all");
    SDL_AtomicSet(&g_rep_state, REP_SAVED);
    /* show it (not in the tests) */
    if (g_report_test) return;
#ifdef _WIN32
    {
        wchar_t wp[1200], args[1300];
        if (MultiByteToWideChar(CP_UTF8, 0, g_rep_path, -1, wp, 1200)) {
            _snwprintf(args, 1290, L"/select,\"%ls\"", wp);
            args[1290] = 0;
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        }
    }
#else
    {
        char url[1300];
        snprintf(url, sizeof(url), "file://%s", dir[0] ? dir : ".");
        SDL_OpenURL(url);
    }
#endif
}

/* the button */
static void report_start(void)
{
    if (SDL_AtomicGet(&g_rep_state) == REP_ASKING) return;
    SDL_AtomicLock(&g_rep_lock);
    free(g_rep_buf);
    g_rep_buf = NULL; g_rep_len = g_rep_cap = 0; g_rep_done = 0;
    SDL_AtomicUnlock(&g_rep_lock);
    g_rep_t0 = SDL_GetTicks64();
    snprintf(g_rep_msg, sizeof(g_rep_msg), "Asking the console for its log...");
    SDL_AtomicSet(&g_rep_state, REP_ASKING);
    fprintf(stderr, "report: asked\n");
    if (g_sock != SOCK_BAD || g_usb) SDL_AtomicSet(&g_want_report, 1);
    else report_save();                  /* no console: the viewer's side now */
}

/* the main thread, every pass: saved once the console has sent it all, or after 8 s */
static void report_tick(void)
{
    if (g_report_cli && (g_sock != SOCK_BAD || g_usb || SDL_GetTicks64() > 4000)) { g_report_cli = 0; report_start(); }
    if (SDL_AtomicGet(&g_rep_state) != REP_ASKING) return;
    SDL_AtomicLock(&g_rep_lock);
    const int done = g_rep_done;
    SDL_AtomicUnlock(&g_rep_lock);
    if (done || SDL_GetTicks64() - g_rep_t0 > 8000) report_save();
}

static int report_state(void) { return SDL_AtomicGet(&g_rep_state); }
/* Settings: what it did last, under the row ("" before the first) */
static const char *report_status(void) { return report_state() == REP_IDLE ? "" : g_rep_msg; }
