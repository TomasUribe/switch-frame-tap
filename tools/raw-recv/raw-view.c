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
 *   ./raw-view --file s.sft --paced --seconds 4 --stats full --shot hud.bmp
 *                              # v0.7.2: the stats overlay at a level (off/fps/graphs/full;
 *                              # O or F3 cycles it), and the last frame saved as BMP (tests)
 *   ./raw-view --no-zero-copy  # VAAPI frames are copied to memory and uploaded again
 *                              # (the default on Wayland/EGL draws them straight from the GPU)
 *   ./raw-view --software      # v0.7.11: decode on the CPU, not the GPU's video decoder
 *                              # (viewer.ini hwdec=0 too; the GPU is the default)
 *   ./raw-view --file s.sft --paced --upscaler fsr   # v0.7.11: GPU upscaling (off/fsr/nis;
 *                              # U cycles it, remembered in viewer.ini)
 *   ./raw-view --net 192.168.1.23   # v0.7: the stream over the network (the console
 *                              # with network streaming on); --app finds it by itself
 *   ./raw-view --no-aspect-lock  # free resize (Windows default locks to 16:9 via WM_SIZING;
 *                              # off by default on Linux/macOS so tiling WMs are left alone;
 *                              # viewer.ini lock_aspect= / Settings / A toggles it)
 *   ./raw-view --chromeless    # borderless window; drag the picture to move (B / Settings;
 *                              # viewer.ini chromeless=1). Still resizable from the edges.
 *   R records to MP4 (v0.5) in Videos/Switch Frame Tap: the console's H.264 as it
 *   arrived plus the game audio as AAC; --rec-dir DIR saves elsewhere, --mp4 FILE
 *   records into FILE from the start (tests)
 *   ./raw-view --app           # M97: the desktop launcher's mode - the window opens at
 *                              # once, waits for the Switch, reconnects whenever it comes
 *                              # back, and only closes when you close it. F11, F,
 *                              # double-click, or middle-click toggles fullscreen.
 *                              # On Linux it logs to $XDG_STATE_HOME/switch-frame-tap/viewer.log.
 *   ./raw-view --check         # v0.7.14: session, video/audio driver, udev, USB, network
 *   ./raw-view --check --net 192.168.1.23   # also test the console's TCP 9950 stream
 *   ./raw-view --video-driver wayland       # force wayland (or x11) for this run
 *
 * Decoding: v0.7.11 decodes on one thread by default (no decoder delay):
 * since v0.6 the stream is P frames at ~2-15 Mbps, a few ms per frame on one
 * core. Frame threads (--threads N) hold back N-1 frames (~16 ms each at
 * 60 fps); they paid off for the early IDR-only stream (150+ Mbps). If one
 * thread cannot keep up (over 12 ms a frame on average), the viewer switches
 * itself to two frame threads once, and says so (overlay, log).
 * V: vsync on/off (remembered) - off shows each frame the moment it is
 * decoded, lower latency, may tear.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#ifdef _WIN32
/* v0.2: the Windows build (tools/raw-recv/build-windows.sh). libusb's Windows
 * package puts libusb.h at the top of its include dir; SDL must not replace
 * main (the entry point stays mainCRTStartup -> main, GUI subsystem).
 * v0.7: winsock2 before anything includes windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <libusb.h>
#define SDL_MAIN_HANDLED
#else
#include <libusb-1.0/libusb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "sdl_compat.h"        /* v0.7.15: sdl2-compat's SDL_RenderGetScale */
#ifdef SFT_H264
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#endif

#define SFT_VID 0x1209
#define SFT_PID 0x5F1E
#define SFT_IFACE 0
#define SFT_EP_IN 0x81
#define SFT_EP_OUT 0x01            /* v0.7.12: the viewer's goodbye ("SFTQ") */
#define SFT_MAGIC 0x52544653u
#define SFT_FLAG_PACKED420 1u
#define SFT_FLAG_H264      2u
#define SFT_FLAG_KEY       4u   /* M85: an IDR access unit (H.264 only) */
#define SFT_FLAG_AUDIO     8u   /* v0.3: game audio, 16-bit stereo PCM (width = rate, height = channels) */
#define SFT_FLAG_STATUS   16u   /* v0.7.10: with AUDIO (rate 0, so older viewers drop it): why there is no picture */
#define SFT_FLAG_CROP     32u   /* v0.7.16: with H264 - block_h_log2 is the part of the picture the game drew (w << 16 | h) */
#define SFT_FLAG_CROP_MID 64u   /* v0.7.16: with CROP - that part is in the middle of the picture, not at its top left */

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
static const char *g_shot = NULL;    /* --shot: save the last frame drawn (overlay included) as BMP (tests) */
static int g_open_settings = 0;      /* --settings: the settings drawer open at start (tests) */
static const char *g_mp4 = NULL;     /* --mp4 FILE: record into FILE from the start (tests) */
static const char *g_rec_dir = NULL; /* --rec-dir DIR: where R saves (default Videos/Switch Frame Tap) */
static int g_win_w = 0, g_win_h = 0;  /* --window WxH: keep the window this size (tests) */
/* Remembered window box (viewer.ini). Applied at create; kept across stream start. */
static int g_geom_valid = 0, g_geom_maximized = 0;
static int g_geom_x = SDL_WINDOWPOS_CENTERED, g_geom_y = SDL_WINDOWPOS_CENTERED;
static int g_geom_w = 1280, g_geom_h = 720;
static int g_pre_fs_valid = 0, g_pre_fs_x, g_pre_fs_y, g_pre_fs_w, g_pre_fs_h;
static const char *g_video_driver = NULL; /* --video-driver: wayland, x11, ... */
#ifdef __APPLE__   /* v0.7.11 (DiM3NT0R, PR #12): Retina - the picture at the screen's real pixels */
#define VIDEO_WINDOW_FLAGS (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)
#else
#define VIDEO_WINDOW_FLAGS (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)
#endif

/* Lock the window client area to 16:9 while resizing (letterbox remains the
 * fallback inside a free rectangle / non-16:9 monitors). Default on for
 * Windows (WM_SIZING); off elsewhere so Linux tiling / half-screen snap is not
 * rewritten by aspect_constrain_window on SIZE_CHANGED. */
#ifdef _WIN32
static int g_lock_aspect = 1;
#else
static int g_lock_aspect = 0;
#endif
static int g_lock_aspect_cli = -1;   /* -1 = use prefs; 0/1 from --no-aspect-lock / --aspect-lock */
/* Optional borderless window; drag the client area to move. Default off. */
static int g_chromeless = 0;
static int g_chromeless_cli = -1;
/* Keep the viewer above other windows (chrome pin / viewer.ini). */
static int g_always_on_top = 0;
/* v0.7.19: while recording, ask the console to keep one picture size (the
 * v0.7.18 lock, SFTL) so the MP4 stays one file - it then scales pictures of
 * another size. Off: never scaled; a new file starts when the size changes. */
static int g_rec_one_file = 0;
static int g_drag_pending = 0, g_dragging = 0;
static int g_drag_ox, g_drag_oy, g_drag_wx, g_drag_wy;
static int g_drag_sx, g_drag_sy; /* global mouse at button-down (absolute threshold) */
static int g_in_size_move = 0;             /* Win32 modal resize loop */
static int g_need_aspect_snap = 0;
static SDL_Window *g_paint_win = NULL;
static SDL_Renderer *g_paint_ren = NULL;
static SDL_Texture *g_paint_tex = NULL;
static SDL_Rect g_paint_crop;        /* v0.7.16: the crop of the frame on screen, for repaints (w 0: all) */
static void viewer_repaint(void);
static void window_geom_capture(SDL_Window *win);
static void window_geom_apply(SDL_Window *win);
static void apply_always_on_top(SDL_Window *win);
static void prefs_save(void);

static Uint32 video_window_flags(void)
{
    Uint32 f = VIDEO_WINDOW_FLAGS;
    if (g_chromeless) f |= SDL_WINDOW_BORDERLESS;
    return f;
}

#ifdef _WIN32
static WNDPROC g_prev_wndproc;
static HWND g_aspect_hwnd;

static void aspect_borders(HWND hwnd, int *bw, int *bh)
{
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    *bw = (int)((wr.right - wr.left) - (cr.right - cr.left));
    *bh = (int)((wr.bottom - wr.top) - (cr.bottom - cr.top));
}

static LRESULT CALLBACK aspect_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Borderless: 8px edges still resize (no title-bar chrome). */
    if (msg == WM_NCHITTEST && g_chromeless) {
        LRESULT hit = CallWindowProc(g_prev_wndproc, hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT cr;
            GetClientRect(hwnd, &cr);
            ScreenToClient(hwnd, &pt);
            const int edge = 8;
            const int left = pt.x < edge, right = pt.x >= cr.right - edge;
            const int top = pt.y < edge, bottom = pt.y >= cr.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        return hit;
    }
    if (msg == WM_ENTERSIZEMOVE) {
        g_in_size_move = 1;
        return CallWindowProc(g_prev_wndproc, hwnd, msg, wp, lp);
    }
    if (msg == WM_EXITSIZEMOVE) {
        g_in_size_move = 0;
        g_need_aspect_snap = 1;
        LRESULT r = CallWindowProc(g_prev_wndproc, hwnd, msg, wp, lp);
        if (g_paint_win) {
            window_geom_capture(g_paint_win);
            prefs_save();
        }
        viewer_repaint();
        return r;
    }
    /* Live picture while the user drags the border (modal size loop). */
    if (msg == WM_SIZE && g_in_size_move) {
        LRESULT r = CallWindowProc(g_prev_wndproc, hwnd, msg, wp, lp);
        viewer_repaint();
        return r;
    }
    if (msg == WM_SIZING && g_lock_aspect) {
        RECT *r = (RECT *)lp;
        int bw = 0, bh = 0;
        aspect_borders(hwnd, &bw, &bh);
        int cw = (int)(r->right - r->left) - bw;
        int ch = (int)(r->bottom - r->top) - bh;
        if (cw < 160) cw = 160;
        if (ch < 90) ch = 90;
        const int edge = (int)wp;
        if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
            ch = (cw * 9 + 8) / 16;
        } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
            cw = (ch * 16 + 4) / 9;
        } else {
            const int ch2 = (cw * 9 + 8) / 16;
            const int cw2 = (ch * 16 + 4) / 9;
            if ((ch2 > ch ? ch2 - ch : ch - ch2) <= (cw2 > cw ? cw2 - cw : cw - cw2)) ch = ch2;
            else cw = cw2;
        }
        const int ww = cw + bw, wh = ch + bh;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            r->left = r->right - ww;
        else
            r->right = r->left + ww;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            r->top = r->bottom - wh;
        else
            r->bottom = r->top + wh;
        return TRUE;
    }
    return CallWindowProc(g_prev_wndproc, hwnd, msg, wp, lp);
}
#endif

/* Borderless still needs a thick frame + min/max boxes or Windows won't resize
 * / maximize until something else (e.g. the first stream size) restyles it. */
static void win32_ensure_frame_styles(SDL_Window *win)
{
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!win || !SDL_GetWindowWMInfo(win, &info)) return;
    HWND hwnd = info.info.win.window;
    if (!hwnd) return;
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
#else
    (void)win;
#endif
    SDL_SetWindowResizable(win, SDL_TRUE);
}

static void aspect_hook_window(SDL_Window *win)
{
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!win || !SDL_GetWindowWMInfo(win, &info)) return;
    HWND hwnd = info.info.win.window;
    if (!hwnd || hwnd == g_aspect_hwnd) return;
    if (g_aspect_hwnd && g_prev_wndproc)
        SetWindowLongPtr(g_aspect_hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_wndproc);
    g_aspect_hwnd = hwnd;
    g_prev_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)aspect_wndproc);
    win32_ensure_frame_styles(win);
#else
    (void)win;
#endif
}

/* Portable snap after resize (mac/linux, or if the Win32 hook missed a frame). */
static void aspect_constrain_window(SDL_Window *win)
{
    if (!g_lock_aspect || !win) return;
    const Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)) return;
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    if (w < 160) w = 160;
    const int nh = (w * 9 + 8) / 16;
    if (nh != h && nh >= 90) SDL_SetWindowSize(win, w, nh);
}

#ifdef __linux__
/* v0.7.14: on Linux a window that stops presenting must not become KWin's
 * "not responding" dialog. The watchdog sees the display loop stop beating
 * and closes the process after 15 s, leaving the cause in the log. Linux
 * only: a modal window drag on Windows and macOS blocks the loop inside the
 * OS, which would look like a hang. */
static volatile Uint64 g_loop_beat = 0;
static volatile int g_watchdog_stop = 0;
static SDL_Thread *g_watchdog = NULL;
static void beat(void) { g_loop_beat = SDL_GetPerformanceCounter(); }
static int watchdog_main(void *arg)
{
    (void)arg;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    while (!g_watchdog_stop) {
        SDL_Delay(1000);
        const Uint64 last = g_loop_beat;
        if (last && (double)(SDL_GetPerformanceCounter() - last) * 1000.0 / (double)freq > 15000.0) {
            fprintf(stderr, "\ndisplay: the window stopped presenting for 15 s - closing the viewer (video driver: %s)\n",
                    SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
            fflush(stderr);
            _Exit(3);
        }
    }
    return 0;
}
static void watchdog_start(void)
{
    if (g_watchdog) return;
    g_watchdog_stop = 0;
    g_loop_beat = SDL_GetPerformanceCounter();
    g_watchdog = SDL_CreateThread(watchdog_main, "display-watchdog", NULL);
}
static void watchdog_stop(void)
{
    if (!g_watchdog) return;
    g_watchdog_stop = 1;
    SDL_WaitThread(g_watchdog, NULL);
    g_watchdog = NULL;
}
#else
/* Windows and macOS run a modal loop while the user drags the window; the
 * display loop cannot beat then, so the watchdog stays Linux-only. */
static void beat(void) {}
static void watchdog_start(void) {}
static void watchdog_stop(void) {}
#endif

/* v0.7.14: on KDE Wayland the release's static SDL could fall back to X11
 * (XWayland); its GLX/DRI3 present path is what froze the window on the
 * author's 165 Hz desktop. Only KDE is forced: GNOME and the other Wayland
 * sessions keep SDL's own choice. An explicit SDL_VIDEODRIVER /
 * SFT_VIDEO_DRIVER / --video-driver still wins, and Wayland falls back to
 * SDL's default if it cannot start. */
static int video_init(void)
{
    const char *explicit = g_video_driver;
    if (!explicit || !explicit[0]) explicit = getenv("SFT_VIDEO_DRIVER");
#ifndef _WIN32
    const int user_env = getenv("SDL_VIDEODRIVER") != NULL;
    int forced = 0;
    if (explicit && explicit[0]) {
        setenv("SDL_VIDEODRIVER", explicit, 1);
        forced = 1;
    } else if (!user_env) {
        const char *sess = getenv("XDG_SESSION_TYPE");
        const char *desk = getenv("XDG_CURRENT_DESKTOP");
        if (sess && !strcmp(sess, "wayland") && getenv("WAYLAND_DISPLAY") &&
            desk && strstr(desk, "KDE")) {
            setenv("SDL_VIDEODRIVER", "wayland", 1);
            forced = 1;
        }
    }
#else
    (void)explicit;
    const int forced = 0;
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "Switch Frame Tap");
    int rc = SDL_Init(SDL_INIT_VIDEO);
    if (rc != 0 && forced) {
        fprintf(stderr, "video: the requested driver failed (%s) - trying SDL's default\n", SDL_GetError());
#ifndef _WIN32
        unsetenv("SDL_VIDEODRIVER");
#endif
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
        rc = SDL_Init(SDL_INIT_VIDEO);
    }
    if (rc == 0) fprintf(stderr, "video: driver %s\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    else fprintf(stderr, "video: SDL_Init failed: %s\n", SDL_GetError());
    return rc;
}

/* v0.7.11 (DiM3NT0R, PR #12): the window at the picture's size, but never
 * bigger than the screen (a 1080p stream on a 1366x768 laptop) */
static void size_video_window(SDL_Window *win, int width, int height)
{
#ifdef _WIN32
    /* v0.7.13: the viewer is DPI aware on Windows (main), so sizes are real
     * pixels - keep the window as big on screen as before, now sharp */
    float ddpi = 0;
    if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(win), &ddpi, NULL, NULL) == 0 && ddpi > 100.0f) {
        width = (int)(width * ddpi / 96.0f);
        height = (int)(height * ddpi / 96.0f);
    }
#endif
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &usable) == 0) {
        double fit = 1.0;
        const double fw = (usable.w - 32.0) / width, fh = (usable.h - 48.0) / height;
        if (fw < fit) fit = fw;
        if (fh < fit) fit = fh;
        width = width * fit > 1 ? (int)(width * fit) : 1;
        height = height * fit > 1 ? (int)(height * fit) : 1;
    }
    SDL_SetWindowSize(win, width, height);
    aspect_hook_window(win);
    aspect_constrain_window(win);
}
/* v0.7.12: the GPU's video decoder (viewer.ini hwdec=, Settings, --software).
 * v0.7.13: on by default where it has been confirmed - Linux (VAAPI, NVDEC)
 * and Windows (D3D11VA); macOS (VideoToolbox) still starts off until a Mac
 * user confirms it. It falls back to the CPU by itself. */
#if defined(__APPLE__)
static int g_hwdec = 0;
#else
static int g_hwdec = 1;
#endif

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
static const char *net_peer_for_menu(void);
static int conn_for_menu(void);
static const char *net_status_for_menu(void);
static const char *ip_edit_for_menu(void);   /* NULL unless an address is being typed */
#include "upscale.h"           /* v0.7.11: GPU upscaling, FSR 1 / NIS (U) */
#include "menu.h"              /* v0.5: the main screen while there is no picture */
#include "hud.h"               /* v0.7.2: the stats overlay (O / F3) */
#include "picture.h"           /* v0.7.12: brightness, contrast... (P) */
#include "zerocopy.h"          /* VAAPI frames drawn without a copy (Linux, EGL) */
#include "update.h"            /* v0.7.14: the viewer updates itself from GitHub's releases */
static libusb_device_handle *g_usb = NULL;

/* ---- v0.7: the network transport ------------------------------------------
 * With network streaming on (the manager), the console listens on TCP 9950
 * and broadcasts "SFTAP1 <ip> <port> ..." on UDP 9951 once a second while no
 * viewer is connected. The viewer listens for that beacon while it waits for
 * USB, and connects: the stream is the same SFTR packets USB carries. */
#ifdef _WIN32
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#define NET_PORT 9950
#define BEACON_PORT 9951
static sock_t g_sock = SOCK_BAD;       /* the stream, when it comes over the network */
static sock_t g_beacon = SOCK_BAD;     /* listening for the console's beacon */
static char g_net_peer[64] = "";       /* "192.168.1.23", for the menu and the title */

static void net_init(void)
{
#ifdef _WIN32
    static int done = 0;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

/* connect to the console's stream port; 1 = connected (g_sock) */
static int net_connect(const char *host, int port)
{
    net_init();
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return 0;
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) return 0;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { sock_close(s); return 0; }
    const int one = 1, rcv = 8 * 1024 * 1024;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof(rcv));
    g_sock = s;
    snprintf(g_net_peer, sizeof(g_net_peer), "%s", host);
    fprintf(stderr, "network: connected to %s:%d\n", host, port);
    return 1;
}

/* a beacon waiting on UDP 9951? then connect; 1 = connected */
static int net_poll_beacon(void)
{
    net_init();
    if (g_beacon == SOCK_BAD) {
        g_beacon = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_beacon == SOCK_BAD) return 0;
        const int one = 1;
        setsockopt(g_beacon, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(BEACON_PORT);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_beacon, (struct sockaddr *)&a, sizeof(a)) != 0) {
            fprintf(stderr, "network: cannot listen for the console on UDP %d\n", BEACON_PORT);
            sock_close(g_beacon); g_beacon = SOCK_BAD;
            return 0;
        }
    }
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(g_beacon, &rs);
    struct timeval tv = { 0, 0 };
    if (select((int)g_beacon + 1, &rs, NULL, NULL, &tv) <= 0) return 0;
    char msg[128];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = (int)recvfrom(g_beacon, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) return 0;
    msg[n] = 0;
    char ip[64]; int port = 0;
    if (sscanf(msg, "SFTAP1 %63s %d", ip, &port) != 2 || port <= 0) return 0;
    /* connect to where the beacon came from (the address in it may be
     * another interface's) */
    char src[64];
    inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
    return net_connect(src, port);
}

static const char *net_peer_for_menu(void) { return g_net_peer; }

/* ---- v0.7: one connection at a time, picked in the viewer (Tab) -----------
 * USB: only the cable. Network: the last address that worked, the console's
 * beacon, and a scan of this PC's subnet (Run AN: on a Wi-Fi LAN the beacon
 * never arrived - access points often filter broadcasts between wireless
 * clients - so the scan is what finds it there); or an address typed with I.
 * The choice and the last address live in viewer.ini next to viewer.log. */
enum { CONN_USB = 0, CONN_NET = 1 };
static int g_conn = CONN_USB;
static char g_last_ip[64] = "";
static char g_net_status[128] = "";    /* what the finder is doing, for the menu */
/* Second viewer: back off only when the Switch closes us quickly with no
 * stream (refused / reset). Do NOT treat "connected, waiting for picture"
 * as busy - that was a false positive after closing every viewer. */
static Uint64 g_net_busy_until = 0;    /* SDL ticks; 0 = not backing off */
static int g_net_busy_ms = 0;          /* last backoff length (grows) */
static int g_net_got_stream = 0;       /* saw stream bytes on this socket */
static Uint64 g_net_connect_ticks = 0; /* when this TCP session started */
static char g_ip_edit[32] = "";        /* I: the address being typed */
static int g_ip_editing = 0;
static int conn_for_menu(void) { return g_conn; }
static const char *net_status_for_menu(void) { return g_net_status; }
static const char *ip_edit_for_menu(void) { return g_ip_editing ? g_ip_edit : NULL; }

static void prefs_path(char *out, size_t cap)
{
    char *dir = SDL_GetPrefPath("switch-frame-tap", "viewer");
    snprintf(out, cap, "%sviewer.ini", dir ? dir : "");
    if (dir) SDL_free(dir);
}

static void prefs_load(void)
{
    char path[1024];
    prefs_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char v[128];
        if (sscanf(line, "connection=%127s", v) == 1) g_conn = strcmp(v, "network") == 0 ? CONN_NET : CONN_USB;
        if (sscanf(line, "last_ip=%63s", v) == 1) snprintf(g_last_ip, sizeof(g_last_ip), "%.63s", v);
        int hm;
        if (sscanf(line, "stats_overlay=%d", &hm) == 1 && hm >= 0 && hm < HUD_MODES) g_hud = hm;
        if (sscanf(line, "vsync=%d", &hm) == 1) g_vsync = hm != 0;
        if (sscanf(line, "upscaler=%127s", v) == 1) up_set_by_key(v);
        if (sscanf(line, "fsr_sharpness=%d", &hm) == 1 && hm >= 0 && hm <= 100) g_fsr_sharpness = hm;
        if (sscanf(line, "hwdec=%d", &hm) == 1) g_hwdec = hm != 0;
        if (sscanf(line, "lock_aspect=%d", &hm) == 1) g_lock_aspect = hm != 0;
        if (sscanf(line, "chromeless=%d", &hm) == 1) g_chromeless = hm != 0;
        if (sscanf(line, "always_on_top=%d", &hm) == 1) g_always_on_top = hm != 0;
        if (sscanf(line, "one_file_recording=%d", &hm) == 1) g_rec_one_file = hm != 0;
        if (sscanf(line, "picture=%127s", v) == 1) pic_parse(v);
        if (sscanf(line, "window=%d,%d,%d,%d", &g_geom_x, &g_geom_y, &g_geom_w, &g_geom_h) == 4 &&
            g_geom_w >= 160 && g_geom_h >= 90) g_geom_valid = 1;
        if (sscanf(line, "maximized=%d", &hm) == 1) g_geom_maximized = hm != 0;
        if (sscanf(line, "update_betas=%d", &hm) == 1) g_upd_betas = hm != 0;
        if (sscanf(line, "update_check=%d", &hm) == 1) g_upd_check = hm != 0;
    }
    fclose(f);
}

static void window_geom_capture(SDL_Window *win)
{
    if (!win) return;
    const Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) return;
    if (fl & SDL_WINDOW_MINIMIZED) return;
    if (fl & SDL_WINDOW_MAXIMIZED) {
        g_geom_maximized = 1;
        g_geom_valid = 1;
        return;
    }
    int x, y, w, h;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    if (w < 160 || h < 90) return;
    g_geom_x = x; g_geom_y = y; g_geom_w = w; g_geom_h = h;
    g_geom_maximized = 0;
    g_geom_valid = 1;
}

static void window_geom_default_size(int *w, int *h)
{
    *w = 1280; *h = 720;
#ifdef _WIN32
    float ddpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) == 0 && ddpi > 100.0f) {
        *w = (int)(*w * ddpi / 96.0f);
        *h = (int)(*h * ddpi / 96.0f);
    }
#endif
}

static void window_geom_apply(SDL_Window *win)
{
    if (!win) return;
    if (!g_geom_valid) {
        int w, h;
        window_geom_default_size(&w, &h);
        SDL_SetWindowSize(win, w, h);
    } else {
        SDL_SetWindowSize(win, g_geom_w, g_geom_h);
    }
    if (g_lock_aspect) aspect_constrain_window(win);
    /* Styles before position: FRAMECHANGED can shove the window; set x/y last. */
    win32_ensure_frame_styles(win);
    if (g_geom_valid && !g_geom_maximized) {
        SDL_SetWindowPosition(win, g_geom_x, g_geom_y);
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(win, &info) && info.info.win.window)
            SetWindowPos(info.info.win.window, NULL, g_geom_x, g_geom_y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    }
    if (g_geom_maximized) SDL_MaximizeWindow(win);
    apply_always_on_top(win);
}

static void prefs_save(void)
{
    char path[1024];
    prefs_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "connection=%s\nlast_ip=%s\nstats_overlay=%d\nvsync=%d\nupscaler=%s\nhwdec=%d\nlock_aspect=%d\nchromeless=%d\nalways_on_top=%d\npicture=%d,%d,%d,%d,%d\n", g_conn == CONN_NET ? "network" : "usb", g_last_ip, g_hud, g_vsync, up_key[g_upscaler], g_hwdec, g_lock_aspect, g_chromeless, g_always_on_top,
            g_pic.bright, g_pic.contrast, g_pic.sat, g_pic.gamma10, g_pic.sharp);
    fprintf(f, "fsr_sharpness=%d\n", g_fsr_sharpness);
    fprintf(f, "one_file_recording=%d\n", g_rec_one_file);
    fprintf(f, "update_betas=%d\nupdate_check=%d\n", g_upd_betas, g_upd_check);
    if (g_geom_valid)
        fprintf(f, "window=%d,%d,%d,%d\nmaximized=%d\n", g_geom_x, g_geom_y, g_geom_w, g_geom_h, g_geom_maximized);
    fclose(f);
}

#if defined(__linux__)
/* v0.7.14: a launcher-started viewer has no terminal. Keep the same log the
 * Windows app writes, under $XDG_STATE_HOME (normally ~/.local/state). The
 * previous run is kept as viewer.prev.log, so a crash survives a relaunch. */
static void log_mkdir(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void viewer_log_init(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char dir[1024], path[1200], prev[1200];
    if (state && state[0]) snprintf(dir, sizeof(dir), "%s/switch-frame-tap", state);
    else if (home && home[0]) snprintf(dir, sizeof(dir), "%s/.local/state/switch-frame-tap", home);
    else return;
    log_mkdir(dir);
    snprintf(path, sizeof(path), "%s/viewer.log", dir);
    snprintf(prev, sizeof(prev), "%s/viewer.prev.log", dir);
    remove(prev);
    rename(path, prev);
    FILE *f = freopen(path, "w", stderr);
    if (!f) return;
    setvbuf(stderr, NULL, _IONBF, 0);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char when[64];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "Switch Frame Tap viewer log\nwhen: %s\nsession: %s  desktop: %s\nwayland: %s  display: %s\n",
            when,
            getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "?",
            getenv("XDG_CURRENT_DESKTOP") ? getenv("XDG_CURRENT_DESKTOP") : "?",
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "-",
            getenv("DISPLAY") ? getenv("DISPLAY") : "-");
}
#endif

static void sock_nonblock(sock_t s, int on)
{
#ifdef _WIN32
    u_long v = (u_long)on;
    ioctlsocket(s, FIONBIO, &v);
#else
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* connect to many addresses at once, ms to answer; the first that does (on
 * the console's port) comes back connected and blocking, the rest closed */
static int g_last_refused = 0;      /* the last connect_any: a host said no (nothing listening) */
static sock_t connect_any(const uint32_t *ips, int n, int port, int ms, uint32_t *which)
{
    g_last_refused = 0;
    sock_t sk[64];
    uint32_t sip[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; i++) {
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == SOCK_BAD) continue;
        sock_nonblock(s, 1);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)port);
        a.sin_addr.s_addr = ips[i];
        connect(s, (struct sockaddr *)&a, sizeof(a));
        sk[m] = s;
        sip[m] = ips[i];
        m++;
    }
    sock_t won = SOCK_BAD;
    const Uint64 t_end = SDL_GetTicks64() + (Uint64)ms;
    while (won == SOCK_BAD && SDL_GetTicks64() < t_end && !g_quit) {
        fd_set ws, es;
        FD_ZERO(&ws); FD_ZERO(&es);
        sock_t mx = 0;
        for (int i = 0; i < m; i++) if (sk[i] != SOCK_BAD) { FD_SET(sk[i], &ws); FD_SET(sk[i], &es); if (sk[i] > mx) mx = sk[i]; }
        struct timeval tv = { 0, 50000 };
        if (select((int)mx + 1, NULL, &ws, &es, &tv) <= 0) continue;
        for (int i = 0; i < m && won == SOCK_BAD; i++) {
            if (sk[i] == SOCK_BAD) continue;
            if (FD_ISSET(sk[i], &es)) { sock_close(sk[i]); sk[i] = SOCK_BAD; continue; }
            if (!FD_ISSET(sk[i], &ws)) continue;
            int err = 1; socklen_t el = sizeof(err);
            getsockopt(sk[i], SOL_SOCKET, SO_ERROR, (char *)&err, &el);
            if (err == 0) { won = sk[i]; sk[i] = SOCK_BAD; if (which) *which = sip[i]; }
            else {
#ifdef _WIN32
                if (err == WSAECONNREFUSED) g_last_refused = 1;
#else
                if (err == ECONNREFUSED) g_last_refused = 1;
#endif
                sock_close(sk[i]); sk[i] = SOCK_BAD;
            }
        }
    }
    for (int i = 0; i < m; i++) if (sk[i] != SOCK_BAD) sock_close(sk[i]);
    if (won != SOCK_BAD) sock_nonblock(won, 0);
    return won;
}

/* this PC's address on the LAN (the interface a packet to the internet
 * would leave by - nothing is sent) */
static uint32_t local_ip(void)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_BAD) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    uint32_t ip = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        socklen_t l = sizeof(a);
        if (getsockname(s, (struct sockaddr *)&a, &l) == 0) ip = a.sin_addr.s_addr;
    }
    sock_close(s);
    return ip;
}

/* the finder thread: the last address, then the subnet; a connection it
 * makes is handed over in g_found_sock */
static SDL_Thread *g_finder = NULL;
static volatile int g_finder_stop = 0;
static volatile sock_t g_found_sock = SOCK_BAD;
static char g_found_ip[64] = "";

static int net_backing_off(void)
{
    if (!g_net_busy_until) return 0;
    const Uint64 now = SDL_GetTicks64();
    if (now >= g_net_busy_until) { g_net_busy_until = 0; return 0; }
    const int left = (int)((g_net_busy_until - now + 999) / 1000);
    snprintf(g_net_status, sizeof(g_net_status),
             "Another viewer may be connected - retrying in %d s", left > 0 ? left : 1);
    return 1;
}

static void net_mark_busy(void)
{
    if (g_net_busy_ms < 3000) g_net_busy_ms = 3000;
    else if (g_net_busy_ms < 20000) g_net_busy_ms = g_net_busy_ms * 2;
    if (g_net_busy_ms > 20000) g_net_busy_ms = 20000;
    g_net_busy_until = SDL_GetTicks64() + (Uint64)g_net_busy_ms;
    snprintf(g_net_status, sizeof(g_net_status),
             "Another viewer may be connected - retrying in %d s", (g_net_busy_ms + 999) / 1000);
    fprintf(stderr, "\nnetwork: Switch closed us before any stream - backing off %d ms\n", g_net_busy_ms);
}

static void net_clear_busy(void)
{
    g_net_busy_until = 0;
    g_net_busy_ms = 0;
}

/* Call when a net session ends with no stream bytes. Quick close = refused by
 * an already-serving Switch; long wait then drop = normal (no picture yet). */
static void net_on_lost_without_stream(void)
{
    const Uint64 lived = g_net_connect_ticks ? (SDL_GetTicks64() - g_net_connect_ticks) : 0;
    if (lived > 0 && lived < 1500) net_mark_busy();
    else net_clear_busy();
}

static void net_session_begin(void)
{
    g_net_got_stream = 0;
    g_net_connect_ticks = SDL_GetTicks64();
    net_clear_busy();
}

static void use_found(sock_t s, uint32_t ip)
{
    const int one = 1, rcv = 8 * 1024 * 1024;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof(rcv));
    struct in_addr ia; ia.s_addr = ip;
    inet_ntop(AF_INET, &ia, g_found_ip, sizeof(g_found_ip));
    g_found_sock = s;
}

static int finder_main(void *arg)
{
    (void)arg;
    net_init();
    while (!g_finder_stop && !g_quit && g_found_sock == SOCK_BAD) {
        if (net_backing_off()) {
            SDL_Delay(200);
            continue;
        }
        if (g_last_ip[0]) {
            struct in_addr ia;
            if (inet_pton(AF_INET, g_last_ip, &ia) == 1) {
                uint32_t ip = ia.s_addr, w = 0;
                snprintf(g_net_status, sizeof(g_net_status), "Trying %s (the last address)...", g_last_ip);
                sock_t s = connect_any(&ip, 1, NET_PORT, 700, &w);
                if (s != SOCK_BAD) { use_found(s, w); break; }
                /* refused: the Switch is there but set to USB (#24) */
                if (g_last_refused) {
                    snprintf(g_net_status, sizeof(g_net_status), "%.15s is there but set to USB - choose Network on the Switch, then restart it", g_last_ip);
                    for (int t = 0; t < 40 && !g_finder_stop && !g_quit; t++) SDL_Delay(100);
                }
            }
        }
        const uint32_t me = local_ip();
        if (me) {
            const uint32_t base = ntohl(me) & 0xFFFFFF00u;
            char net[32];
            struct in_addr ia; ia.s_addr = htonl(base);
            inet_ntop(AF_INET, &ia, net, sizeof(net));
            snprintf(g_net_status, sizeof(g_net_status), "Looking on %s/24...", net);
            for (int b = 1; b < 255 && !g_finder_stop && g_found_sock == SOCK_BAD; b += 64) {
                uint32_t ips[64]; int n = 0;
                for (int h = b; h < b + 64 && h < 255; h++) ips[n++] = htonl(base | (uint32_t)h);
                uint32_t w = 0;
                sock_t s = connect_any(ips, n, NET_PORT, 600, &w);
                if (s != SOCK_BAD) { use_found(s, w); break; }
            }
        } else {
            snprintf(g_net_status, sizeof(g_net_status), "This PC is not on a network");
        }
        for (int i = 0; i < 30 && !g_finder_stop && g_found_sock == SOCK_BAD; i++) SDL_Delay(100);
    }
    return 0;
}

static void finder_start(void)
{
    if (g_finder) return;
    g_finder_stop = 0;
    g_finder = SDL_CreateThread(finder_main, "net-finder", NULL);
}

static void finder_stop(void)
{
    if (!g_finder) return;
    g_finder_stop = 1;
    SDL_WaitThread(g_finder, NULL);
    g_finder = NULL;
    if (g_found_sock != SOCK_BAD) { sock_close(g_found_sock); g_found_sock = SOCK_BAD; }
}

/* the finder, the beacon or a typed address produced a connection? */
static int net_try_connect(void)
{
    if (net_backing_off()) return 0;
    if (g_found_sock != SOCK_BAD) {
        g_sock = g_found_sock;
        g_found_sock = SOCK_BAD;
        snprintf(g_net_peer, sizeof(g_net_peer), "%s", g_found_ip);
        finder_stop();
        fprintf(stderr, "network: connected to %s:%d\n", g_net_peer, NET_PORT);
    } else if (!net_poll_beacon()) {
        return 0;
    } else {
        finder_stop();
    }
    net_session_begin();
    snprintf(g_last_ip, sizeof(g_last_ip), "%s", g_net_peer);
    prefs_save();
    return 1;
}

/* close whichever transport is open */
static void transport_close(void)
{
    if (g_usb) {
        /* v0.7.12: goodbye - the console ends the stream now (its screen comes
         * back with screen_off) instead of noticing after its 5 s timeout */
        unsigned char bye[4] = { 'S', 'F', 'T', 'Q' };
        int n = 0;
        libusb_bulk_transfer(g_usb, SFT_EP_OUT, bye, sizeof(bye), &n, 200);
        libusb_release_interface(g_usb, 0); libusb_close(g_usb); g_usb = NULL;
    }
    if (g_sock != SOCK_BAD) { sock_close(g_sock); g_sock = SOCK_BAD; g_net_peer[0] = 0; }
}
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
    if (g_sock != SOCK_BAD) {
        /* v0.7: the network - the same semantics as the USB reads below */
        size_t done = 0;
        while (done < n && !g_quit) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(g_sock, &rs);
            struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
            const int r = select((int)g_sock + 1, &rs, NULL, NULL, &tv);
            if (r == 0) return (done > 0) ? -2 : 0;
            if (r < 0) { if (g_app) g_lost = 1; else g_quit = 1; return -3; }
            const int got = (int)recv(g_sock, (char *)dst + done, (int)((n - done) > 0x40000 ? 0x40000 : (n - done)), 0);
            if (got <= 0) {
                /* the console closed it (the viewer went back to the menu,
                 * the console slept or rebooted) - as a cable pulled out */
                fprintf(stderr, "\nnetwork: the connection closed\n");
                if (g_app) g_lost = 1; else g_quit = 1;
                return -3;
            }
            done += (size_t)got;
        }
        return g_quit ? -1 : (int)done;
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

/* v0.7.10: the console's reason for sending no picture (a status packet after
 * each idle probe), shown on the menu while it is fresh */
typedef struct { uint32_t version, reason; char head[64]; char detail[120]; } sft_status_t;
static SDL_SpinLock g_status_lock = 0;
static sft_status_t g_status;
static Uint64 g_status_t = 0;

static void status_packet(const uint8_t *d, uint32_t n)
{
    if (n < sizeof(sft_status_t)) return;
    sft_status_t st;
    memcpy(&st, d, sizeof(st));
    st.head[sizeof(st.head) - 1] = 0;
    st.detail[sizeof(st.detail) - 1] = 0;
    SDL_AtomicLock(&g_status_lock);
    g_status = st;
    g_status_t = SDL_GetTicks64();
    SDL_AtomicUnlock(&g_status_lock);
}

/* 1 with the console's status if one arrived in the last 4 s */
static int status_for_menu(char *head, size_t hn, char *detail, size_t dn, uint32_t *reason)
{
    int fresh = 0;
    SDL_AtomicLock(&g_status_lock);
    if (g_status_t != 0 && SDL_GetTicks64() - g_status_t < 4000 && g_status.reason != 0) {
        snprintf(head, hn, "%s", g_status.head);
        snprintf(detail, dn, "%s", g_status.detail);
        *reason = g_status.reason;
        fresh = 1;
    }
    SDL_AtomicUnlock(&g_status_lock);
    return fresh;
}

/* v0.7.13: ask the console for a keyframe now - the decoder reopened, a
 * recording starts - instead of waiting for its next one (up to 4 s over the
 * network, where the console spaces them out for viewers that can ask: the
 * first ask, at connect, says this one can). Sent by the reader thread, which
 * owns the connection; consoles before v0.7.13 ignore it. */
static SDL_atomic_t g_want_key;
static void key_request(void) { SDL_AtomicSet(&g_want_key, 1); }
static void key_send_pending(void)
{
    /* v0.7.18: a retry (below) waits 150 ms - while streaming, the console has
     * its buffer back within ~8 frames, and each try may hold the reader 20 ms */
    static Uint64 s_retry_at = 0;
    if (s_retry_at != 0 && SDL_GetTicks64() < s_retry_at) return;
    if (!SDL_AtomicCAS(&g_want_key, 1, 0)) return;
    /* v0.7.16: + "SFTC" - this viewer draws crops itself (SFT_FLAG_CROP); a
     * console only sends them to a viewer that said so. Older consoles read
     * the first four bytes and ignore the rest.
     * v0.7.17: + "SFTD" instead while recording - the MP4 gets the stream as
     * it comes, and a crop drawn here never reached the file (Mario Galaxy's
     * dynamic resolution: old picture beside the part it drew); the console
     * crops the pictures itself meanwhile. */
    /* v0.7.18: + "SFTL" while recording - the console keeps the stream at the
     * size the recording started at (it scales the picture), so the MP4 stays
     * one file; "SFTU" lets it follow the game again (#44). */
    /* v0.7.19: "SFTL" only with "One file per recording" on (Settings) */
    static const unsigned char msg_crop[12] = { 'S', 'F', 'T', 'K', 'S', 'F', 'T', 'C', 'S', 'F', 'T', 'U' };
    static const unsigned char msg_nocrop[12] = { 'S', 'F', 'T', 'K', 'S', 'F', 'T', 'D', 'S', 'F', 'T', 'L' };
    static const unsigned char msg_nocrop_free[12] = { 'S', 'F', 'T', 'K', 'S', 'F', 'T', 'D', 'S', 'F', 'T', 'U' };
    const unsigned char *msg = rec_active() ? (g_rec_one_file ? msg_nocrop : msg_nocrop_free) : msg_crop;
    if (g_sock != SOCK_BAD) { send(g_sock, (const char *)msg, sizeof(msg_crop), 0); }
    else if (g_usb) {
        /* v0.7.18: the console takes a request only while a buffer of its own
         * waits for it - between two, the 20 ms run out and the request was
         * lost (#45: SFTC at connect). Again on the next pass, for up to ~2 s;
         * tries 150 ms apart. */
        static int tries = 0;
        int n = 0;
        const int rc = libusb_bulk_transfer(g_usb, SFT_EP_OUT, (unsigned char *)msg, sizeof(msg_crop), &n, 20);
        if (rc == LIBUSB_ERROR_TIMEOUT && n == 0 && ++tries < 14) { SDL_AtomicSet(&g_want_key, 1); s_retry_at = SDL_GetTicks64() + 150; }
        else { tries = 0; s_retry_at = 0; }
    }
}

static int reader_main(void *arg)
{
    (void)arg;
    /* After a Switch reboot the old TCP session often goes half-open: recv
     * only times out, never errors, so g_lost never trips and --app sits on
     * the waiting menu until the user toggles USB/network. Treat sustained
     * silence as a disconnect once a stream has started - and not while the
     * waiting/paused menu is up. Over the network the console sends nothing
     * with no game on screen (status packets are USB-only today), so arming
     * on the HOME menu would reconnect every 5 s. */
    Uint64 idle_since = 0;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    while (!g_quit && !g_rd_stop && !g_lost) {
        key_send_pending();
        sft_hdr_t hdr;
        const int r = read_exact((uint8_t *)&hdr, sizeof(hdr), 100);
        if (r <= 0) {
            if (g_app && r == 0 && g_net_got_stream && !g_menu &&
                (g_sock != SOCK_BAD || g_usb)) {
                const Uint64 now = SDL_GetPerformanceCounter();
                if (!idle_since) { idle_since = now; }
                else if ((double)(now - idle_since) / (double)freq >= 5.0) {
                    fprintf(stderr, "\ntransport: no data for 5 s - reconnecting\n");
                    g_lost = 1;
                    break;
                }
            } else {
                idle_since = 0;
            }
            continue;
        }
        idle_since = 0;
        g_net_got_stream = 1;
        const Uint64 t = SDL_GetPerformanceCounter();
        /* after a resync, ask for a keyframe at once: the bytes skipped may
         * have held a frame, and live USB spaces keyframes 4 s apart
         * (usb_keyframe_interval) - the picture would stay broken till then */
        static int resynced = 0;
        if (hdr.magic != SFT_MAGIC) { fprintf(stderr, "bad magic 0x%08x, resyncing\n", hdr.magic); resynced = 1; continue; }
        if (resynced) { resynced = 0; key_request(); }
        if (hdr.length == 0 || hdr.length > 64u*1024*1024) continue;
        if (hdr.flags & SFT_FLAG_AUDIO) {
            static uint8_t abuf[64 * 1024];
            if (hdr.length > sizeof(abuf)) { static uint8_t sink[4096]; for (uint32_t k = 0; k < hdr.length; k += sizeof(sink)) read_exact(sink, (hdr.length - k) < sizeof(sink) ? (hdr.length - k) : sizeof(sink), 5000); continue; }
            if (read_exact(abuf, hdr.length, 5000) != (int)hdr.length) continue;
            if (hdr.flags & SFT_FLAG_STATUS) { status_packet(abuf, hdr.length); continue; }
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
    key_request();                     /* v0.7.13: tells the console this viewer can ask */
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
/* v0.7.11: the GPU's video decoder when there is one (the macOS part after
 * DiM3NT0R's PR #12): VideoToolbox on macOS, D3D11VA on Windows (NVIDIA, AMD,
 * Intel), NVDEC or VAAPI on Linux. One frame in, one frame out, no thread
 * delay; the frame comes back to memory as NV12 (~0.3 ms at 1080p) and is
 * drawn as it is. Anything that fails goes back to the software decoder. */
typedef struct {
    AVCodecContext *c; AVPacket *pkt; AVFrame *frm, *swfrm;
    AVBufferRef *hwdev;
    enum AVPixelFormat hwfmt;               /* AV_PIX_FMT_NONE: software */
    const char *hwname;                     /* "d3d11va", "videotoolbox", ... */
} h264_t;

static const enum AVHWDeviceType h264_hw_order[] = {
#if defined(_WIN32)
    AV_HWDEVICE_TYPE_D3D11VA,
#elif defined(__APPLE__)
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
    AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI,
#endif
};

#ifdef SFT_VA_SHIM
/* v0.7.13: the portable Linux build reaches the system's libva through
 * gen_va_shim.py's loader; 1 = it is there, VAAPI can be tried */
int sft_va_load(void);
#endif

static enum AVPixelFormat h264_get_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    h264_t *d = c->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) if (*p == d->hwfmt) return *p;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            fprintf(stderr, "decode: %s refused this stream - software\n", d->hwname ? d->hwname : "GPU");
            d->hwfmt = AV_PIX_FMT_NONE;
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

static int h264_open(h264_t *d, int low_latency, int threads, int hw)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return -1;
    const int want_low = low_latency;
    d->c = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    d->swfrm = av_frame_alloc();
    if (!d->c || !d->pkt || !d->frm || !d->swfrm) return -1;
    d->hwfmt = AV_PIX_FMT_NONE;
    d->hwname = NULL;
    for (size_t k = 0; hw && k < sizeof(h264_hw_order) / sizeof(h264_hw_order[0]) && d->hwfmt == AV_PIX_FMT_NONE; k++) {
#ifdef SFT_VA_SHIM
        if (h264_hw_order[k] == AV_HWDEVICE_TYPE_VAAPI && !sft_va_load()) continue;
#endif
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (cfg->device_type != h264_hw_order[k] || !(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
            if (av_hwdevice_ctx_create(&d->hwdev, h264_hw_order[k], NULL, NULL, 0) == 0) {
                d->hwfmt = cfg->pix_fmt;
                d->hwname = av_hwdevice_get_type_name(h264_hw_order[k]);
                d->c->hw_device_ctx = av_buffer_ref(d->hwdev);
                d->c->opaque = d;
                d->c->get_format = h264_get_format;
                d->c->extra_hw_frames = 8;   /* the display queue may hold GPU frames (zerocopy.h) */
            }
            break;
        }
    }
    if (d->hwfmt != AV_PIX_FMT_NONE) low_latency = 1;   /* the GPU decodes it whole: no threads to hold frames */
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
    int rc = avcodec_open2(d->c, codec, NULL);
    if (rc < 0 && d->hwfmt != AV_PIX_FMT_NONE) {
        char err[128];
        av_strerror(rc, err, sizeof(err));
        fprintf(stderr, "decode: %s could not open the stream (%s) - software\n", d->hwname ? d->hwname : "GPU", err);
        avcodec_free_context(&d->c);
        av_buffer_unref(&d->hwdev);
        d->hwfmt = AV_PIX_FMT_NONE;
        d->hwname = NULL;
        d->c = avcodec_alloc_context3(codec);
        if (!d->c) return -1;
        low_latency = want_low;
        if (low_latency) {
            d->c->flags |= AV_CODEC_FLAG_LOW_DELAY;
            d->c->thread_count = 1;
        } else {
            d->c->thread_count = threads;
            d->c->thread_type = FF_THREAD_FRAME;
        }
        rc = avcodec_open2(d->c, codec, NULL);
    }
    if (rc == 0) fprintf(stderr, "decode: %s\n", d->hwname ? d->hwname : "software");
    else {
        char err[128];
        av_strerror(rc, err, sizeof(err));
        fprintf(stderr, "decode: no H.264 decoder (%s)\n", err);
    }
    return rc;
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
    if (rc < 0) return rc;
#ifdef SFT_ZEROCOPY
    if (g_zc_ready && d->frm->format == AV_PIX_FMT_VAAPI) {   /* stays on the GPU; the display imports it */
        zc_sync(d->frm);
        return 1;
    }
#endif
    if (d->hwfmt != AV_PIX_FMT_NONE && d->frm->format == d->hwfmt) {   /* back from the GPU, as NV12 */
        av_frame_unref(d->swfrm);
        rc = av_hwframe_transfer_data(d->swfrm, d->frm, 0);
        if (rc < 0) return rc;
        av_frame_copy_props(d->swfrm, d->frm);
        av_frame_unref(d->frm);
        av_frame_move_ref(d->frm, d->swfrm);
    }
    if (d->frm->format != AV_PIX_FMT_YUV420P && d->frm->format != AV_PIX_FMT_YUVJ420P && d->frm->format != AV_PIX_FMT_NV12) {
        av_frame_unref(d->frm);
        return AVERROR(ENOSYS);                /* a format the display cannot draw */
    }
    return 1;
}

static void h264_close(h264_t *d)
{
    av_frame_free(&d->frm);
    av_frame_free(&d->swfrm);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->c);
    av_buffer_unref(&d->hwdev);
    d->hwfmt = AV_PIX_FMT_NONE;
}
#endif

/* --app: window events while there is nothing to draw (and between frames) */
/* v0.7.11: U picked an upscaler that draws with the other graphics API
 * (Windows: FSR 1 and NIS with OpenGL, RTX VSR with Direct3D 11). The live
 * loop makes a new renderer between frames; a window that had OpenGL gets a
 * new window too (Direct3D's swap chain wants one of its own). */
static int g_ren_swap = 0;

static void ren_swap(SDL_Window **winp, SDL_Renderer **renp, SDL_Texture **texp)
{
    g_ren_swap = 0;
    SDL_Window *win = *winp;
    const char *want = up_driver_for(g_upscaler);
    SDL_RendererInfo ri;
    const int was_gl = SDL_GetRendererInfo(*renp, &ri) == 0 && strcmp(ri.name, "opengl") == 0;
    if (*texp) { SDL_DestroyTexture(*texp); *texp = NULL; }
    if (g_font_tex) SDL_DestroyTexture(g_font_tex);
    g_font_tex = NULL; g_font_ren = NULL;
    if (g_ui_disc) SDL_DestroyTexture(g_ui_disc);
    g_ui_disc = NULL; g_ui_disc_ren = NULL;
    if (g_ui_gear) SDL_DestroyTexture(g_ui_gear);
    g_ui_gear = NULL; g_ui_gear_ren = NULL;
    if (g_ui_pin) SDL_DestroyTexture(g_ui_pin);
    g_ui_pin = NULL; g_ui_pin_ren = NULL;
    up_reset();
    SDL_DestroyRenderer(*renp);
    *renp = NULL;
    if (was_gl && strcmp(want, "opengl") != 0) {
        int x, y, w, h;
        char title[256];
        const Uint32 fl = SDL_GetWindowFlags(win);
        SDL_GetWindowPosition(win, &x, &y);
        SDL_GetWindowSize(win, &w, &h);
        snprintf(title, sizeof(title), "%s", SDL_GetWindowTitle(win));
        SDL_DestroyWindow(win);
        win = SDL_CreateWindow(title, x, y, w, h, video_window_flags() |
                               (fl & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)));
        *winp = win;
        if (!win) { fprintf(stderr, "\nSDL window: %s\n", SDL_GetError()); g_quit = 1; return; }
        aspect_hook_window(win);
        apply_always_on_top(win);
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, want);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {                                /* that one would not start: any renderer */
        fprintf(stderr, "\n%s renderer: %s\n", want, SDL_GetError());
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    }
    *renp = ren;
    if (!ren) { fprintf(stderr, "\nSDL renderer: %s\n", SDL_GetError()); g_quit = 1; return; }
    if (!g_vsync) SDL_RenderSetVSync(ren, 0);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    if (SDL_GetRendererInfo(ren, &ri) == 0) fprintf(stderr, "\nrenderer: %s (for %s)\n", ri.name, up_name[g_upscaler]);
    char t[112];
    up_describe(ren, t, sizeof(t));
    hud_toast(t);
    fprintf(stderr, "upscaler: %s\n", t);
}

/* ---- v0.7.12: what the keys, the settings drawer and the mouse do ------- */
static int g_dec_reopen = 0;                  /* hardware decoding switched: the decoder thread reopens */

static void act_conn(int net)                 /* USB / network; a stream on the old one ends */
{
    if (net == (g_conn == CONN_NET)) return;
    g_conn = net ? CONN_NET : CONN_USB;
    prefs_save();
    fprintf(stderr, "\nconnection: %s\n", g_conn == CONN_NET ? "network" : "USB");
    g_net_status[0] = 0;
    if (g_usb || g_sock != SOCK_BAD) g_lost = 1;
}

static int act_ip_possible(void) { return g_conn == CONN_NET && g_menu && g_sock == SOCK_BAD && !g_ip_editing; }

static void act_ip(void)                      /* type the Switch's address */
{
    if (!act_ip_possible()) return;
    snprintf(g_ip_edit, sizeof(g_ip_edit), "%.31s", g_last_ip);
    g_ip_editing = 1;
    SDL_StartTextInput();
}

static void act_fullscreen(SDL_Window *win)
{
    const int fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (!fs) {
        /* Remember the windowed box so leaving fullscreen doesn't leave a
         * desktop-sized client with letterbox bars ("black blocks"). */
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) {
            SDL_GetWindowPosition(win, &g_pre_fs_x, &g_pre_fs_y);
            SDL_GetWindowSize(win, &g_pre_fs_w, &g_pre_fs_h);
            g_pre_fs_valid = (g_pre_fs_w >= 160 && g_pre_fs_h >= 90);
        } else {
            g_pre_fs_valid = 0;
        }
        window_geom_capture(win);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        /* v0.7.13: a screen whose mode is not 16:9 shows the picture with bars
         * at the sides - often a TV Windows put on 4096x2160 instead of 3840x2160,
         * which the TV then squeezes into its 16:9 panel (a narrower picture) */
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 && dm.h > 0) {
            const double a = (double)dm.w / dm.h;
            fprintf(stderr, "\nfullscreen: the screen is %dx%d at %d Hz\n", dm.w, dm.h, dm.refresh_rate);
            if (a > 16.0 / 9.0 * 1.03 || a < 16.0 / 9.0 * 0.97) {
                char t[112];
                snprintf(t, sizeof(t), "This screen runs at %dx%d (not 16:9) - pick a 16:9 mode like 3840x2160 for no side bars", dm.w, dm.h);
                hud_toast(t);
            }
        }
    } else {
        SDL_SetWindowFullscreen(win, 0);
        if (g_pre_fs_valid) {
            SDL_SetWindowSize(win, g_pre_fs_w, g_pre_fs_h);
            SDL_SetWindowPosition(win, g_pre_fs_x, g_pre_fs_y);
        } else if (g_geom_valid) {
            SDL_SetWindowSize(win, g_geom_w, g_geom_h);
            SDL_SetWindowPosition(win, g_geom_x, g_geom_y);
        }
        if (g_lock_aspect) aspect_constrain_window(win);
        win32_ensure_frame_styles(win);
        window_geom_capture(win);
        prefs_save();
        /* Leaving desktop-fullscreen often leaves uncleared black on the desktop
         * until something else paints there. Force a desktop + window refresh. */
#ifdef _WIN32
        RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        {
            HWND hwnd = NULL;
            SDL_SysWMinfo wmi;
            SDL_VERSION(&wmi.version);
            if (SDL_GetWindowWMInfo(win, &wmi)) hwnd = wmi.info.win.window;
            if (hwnd) {
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
            }
        }
#endif
    }
    /* Redraw so the expand/restore chrome icon matches the new FS state. */
    viewer_repaint();
}

static void act_record(SDL_Window *win)
{
    rec_toggle();
    /* v0.7.13: it starts at the next keyframe - now. v0.7.17: and stopping
     * says so too (SFTC again: the crops are drawn here once more) */
    key_request();
    char t[256], rs[200];
    rec_status(rs, sizeof(rs));
    snprintf(t, sizeof(t), "Switch Frame Tap%s", rs[0] ? rs : "  |  recording stopped");
    SDL_SetWindowTitle(win, t);
}

/* v0.7.14: the update button - check, download, or restart (update.h); a
 * restart would cut a recording short, so it waits for the recording */
static void act_update(void)
{
    switch (upd_state()) {
    case UPD_FOUND: upd_get(); break;
    case UPD_READY:
        if (rec_active()) { upd_note("Stop the recording first (R), then restart"); break; }
        upd_restart();
        break;
    case UPD_ERROR: if (g_upd.failed_update) upd_get(); else upd_check(); break;
    default: upd_check(); break;
    }
}

static void act_hud(int mode)                 /* the stats overlay; -1 = the next level */
{
    if (mode < 0) hud_cycle();
    else { g_hud = mode; hud_toast_mode(); }
    prefs_save();
    fprintf(stderr, "\nstats overlay: %s\n", hud_mode_name[g_hud]);
}

static void act_vsync(SDL_Renderer *ren, int on)
{
    g_vsync = on;
    SDL_RenderSetVSync(ren, g_vsync);
    prefs_save();
    hud_toast(g_vsync ? "Vsync on" : "Vsync off: lower latency, may tear");
    fprintf(stderr, "\nvsync %s\n", g_vsync ? "on" : "off");
}

static void act_rec_one_file(int on)
{
    g_rec_one_file = on != 0;
    prefs_save();
    if (rec_active()) key_request();   /* the console hears it now (SFTL / SFTU) */
    hud_toast(g_rec_one_file ? "One file per recording" : "Recordings follow the picture size");
    fprintf(stderr, "\none file per recording %s\n", g_rec_one_file ? "on" : "off");
}

static void act_lock_aspect(SDL_Window *win, int on)
{
    g_lock_aspect = on != 0;
    prefs_save();
    if (g_lock_aspect && win) aspect_constrain_window(win);
    hud_toast(g_lock_aspect ? "Aspect lock on (16:9)" : "Aspect lock off");
    fprintf(stderr, "\naspect lock %s\n", g_lock_aspect ? "on" : "off");
}

static void apply_chromeless(SDL_Window *win)
{
    if (!win) return;
    SDL_SetWindowBordered(win, g_chromeless ? SDL_FALSE : SDL_TRUE);
    aspect_hook_window(win);
    win32_ensure_frame_styles(win);
    if (g_lock_aspect) aspect_constrain_window(win);
}

static void act_chromeless(SDL_Window *win, int on)
{
    g_chromeless = on != 0;
    g_drag_pending = g_dragging = 0;
    prefs_save();
    apply_chromeless(win);
    hud_toast(g_chromeless ? "Borderless on - drag the picture to move" : "Window frame on");
    fprintf(stderr, "\nchromeless %s\n", g_chromeless ? "on" : "off");
}

static void apply_always_on_top(SDL_Window *win)
{
    if (!win) return;
    SDL_SetWindowAlwaysOnTop(win, g_always_on_top ? SDL_TRUE : SDL_FALSE);
}

static void act_always_on_top(SDL_Window *win, int on)
{
    g_always_on_top = on != 0;
    prefs_save();
    apply_always_on_top(win);
    hud_toast(g_always_on_top ? "Always on top" : "Always on top off");
    fprintf(stderr, "\nalways on top %s\n", g_always_on_top ? "on" : "off");
}

static void act_upscaler(SDL_Renderer *ren, int mode)   /* -1 = the next one */
{
    char t[112];
    g_upscaler = mode < 0 ? (g_upscaler + 1) % UP_COUNT : mode;
    prefs_save();
    if (up_needs_swap(ren)) {                 /* the other graphics API: the live loop switches */
        g_ren_swap = 1;
        snprintf(t, sizeof(t), "Upscaling: %s - switching the graphics mode...", up_name[g_upscaler]);
    } else {
        up_describe(ren, t, sizeof(t));
    }
    hud_toast(t);
    fprintf(stderr, "\nupscaler: %s\n", t);
}

static void act_mute(int mute)
{
    g_mute = mute;
    if (g_adev) { SDL_ClearQueuedAudio(g_adev); SDL_PauseAudioDevice(g_adev, 1); g_a_paused = 1; }
    fprintf(stderr, "\naudio %s\n", g_mute ? "muted" : "on");
}

static void act_hwdec(int on)
{
    if (on == g_hwdec) return;         /* v0.7.13: the choice already made - no decoder restart */
    g_hwdec = on;
    g_dec_reopen = 1;
    prefs_save();
    hud_toast(on ? "Hardware decoding on" : "Hardware decoding off - decoding on the CPU");
}

#include "ui.h"                /* v0.7.12: the home screen and the settings drawer */

static void viewer_repaint(void)
{
    if (!g_paint_ren) return;
    if (g_menu) { menu_draw(g_paint_ren); return; }
    SDL_SetRenderDrawColor(g_paint_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_paint_ren);
    if (g_paint_tex) {
#ifdef __APPLE__
        if (g_upscaler != UP_FSR || !g_up.active ||
            !clarity_draw(g_paint_ren, NULL, NULL, 0, 0, g_fsr_sharpness / 100.0f, 0))
#endif
        if (!zc_repaint(g_paint_ren))   /* a zero-copy frame never reached the texture */
            SDL_RenderCopy(g_paint_ren, g_paint_tex, g_paint_crop.w ? &g_paint_crop : NULL, NULL);
    }
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(g_paint_ren, &lw, &lh);
    if (lw <= 0) lw = 1280;
    if (lh <= 0) lh = 720;
    ui_overlay(g_paint_ren, lw, lh);
    SDL_SetRenderDrawColor(g_paint_ren, 0, 0, 0, 255);
    SDL_RenderPresent(g_paint_ren);
}

static int chromeless_can_drag(SDL_Window *win)
{
    /* Settings may be open: controls keep their clicks; empty panel chrome and
     * the dimmed backdrop return 0 from ui_event so drag still moves the window. */
    if (!g_chromeless || !win || g_ip_editing) return 0;
    const Uint32 fl = SDL_GetWindowFlags(win);
    return (fl & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)) == 0;
}

static void app_events(SDL_Window *win, SDL_Renderer *ren, SDL_Texture *tex)
{
    g_ui_win = win;
    g_paint_win = win;
    g_paint_ren = ren;
    g_paint_tex = tex;
    if (g_need_aspect_snap) {
        g_need_aspect_snap = 0;
        aspect_constrain_window(win);
        viewer_repaint();
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            window_geom_capture(win);
            prefs_save();
            g_quit = 1;
            continue;
        }
        /* v0.7.11 (DiM3NT0R, PR #12): a non-Latin keyboard layout (Cyrillic,
         * Greek...) gives letter keys non-ASCII codes - use the key's place,
         * so R, M, O, V, U and the rest still work */
        if (e.type == SDL_KEYDOWN && (e.key.keysym.sym > 0x7f || e.key.keysym.sym == SDLK_UNKNOWN) &&
            e.key.keysym.scancode >= SDL_SCANCODE_A && e.key.keysym.scancode <= SDL_SCANCODE_Z)
            e.key.keysym.sym = SDLK_a + (e.key.keysym.scancode - SDL_SCANCODE_A);
        /* Borderless: drag the picture (or home screen) to move the window. */
        if (g_dragging || g_drag_pending) {
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                const int was_click = g_drag_pending && !g_dragging;
                const int was_drag = g_dragging;
                g_dragging = g_drag_pending = 0;
                if (was_drag) {
                    window_geom_capture(win);
                    prefs_save();
                }
                /* Click with no drag: if the down missed UI (stale hits), retry as a click
                 * so the cog / window buttons still fire. */
                if (was_click && !g_ip_editing) {
                    SDL_Event click = e;
                    click.type = SDL_MOUSEBUTTONDOWN;
                    click.button.state = SDL_PRESSED;
                    if (ui_event(win, ren, &click)) {
                        if (g_menu) menu_draw(ren);
                        else viewer_repaint();
                    }
                }
                continue;
            }
            if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK) && chromeless_can_drag(win)) {
                int mx, my;
                SDL_GetGlobalMouseState(&mx, &my);
                if (g_drag_pending && !g_dragging) {
                    const int dx = mx - g_drag_sx, dy = my - g_drag_sy;
                    if (dx * dx + dy * dy >= 9) g_dragging = 1;
                }
                if (g_dragging) SDL_SetWindowPosition(win, mx - g_drag_ox, my - g_drag_oy);
                continue;
            }
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN)
                g_dragging = g_drag_pending = 0;
        }
        /* v0.7.12: the settings drawer and the mouse first.
         * While streaming, the display loop already draws ui_overlay each frame -
         * do not viewer_repaint() on every mouse move (extra vsync present without
         * the upscaler/stats stalls and flickers). Home and Win32 live-resize still
         * need an immediate redraw. */
        if (!g_ip_editing && ui_event(win, ren, &e)) {
            if (g_menu) menu_draw(ren);
            else if (g_in_size_move || e.type != SDL_MOUSEMOTION)
                viewer_repaint();
            continue;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 1 &&
            chromeless_can_drag(win)) {
            int mx, my;
            SDL_GetGlobalMouseState(&mx, &my);
            SDL_GetWindowPosition(win, &g_drag_wx, &g_drag_wy);
            g_drag_ox = mx - g_drag_wx;
            g_drag_oy = my - g_drag_wy;
            g_drag_sx = mx;
            g_drag_sy = my;
            g_drag_pending = 1;
            g_dragging = 0;
        }
        /* v0.7: typing the Switch's address (I) takes every key */
        if (g_ip_editing) {
            if (e.type == SDL_TEXTINPUT) {
                for (const char *c = e.text.text; *c; c++) {
                    const size_t l = strlen(g_ip_edit);
                    if (((*c >= '0' && *c <= '9') || *c == '.') && l + 1 < sizeof(g_ip_edit)) { g_ip_edit[l] = *c; g_ip_edit[l + 1] = 0; }
                }
            } else if (e.type == SDL_KEYDOWN) {
                const SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_BACKSPACE) { const size_t l = strlen(g_ip_edit); if (l) g_ip_edit[l - 1] = 0; }
                else if (k == SDLK_ESCAPE) { g_ip_editing = 0; SDL_StopTextInput(); }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    g_ip_editing = 0;
                    SDL_StopTextInput();
                    snprintf(g_net_status, sizeof(g_net_status), "Connecting to %s...", g_ip_edit);
                    if (g_menu) menu_draw(ren);
                    struct in_addr ia;
                    uint32_t w = 0;
                    if (inet_pton(AF_INET, g_ip_edit, &ia) != 1) snprintf(g_net_status, sizeof(g_net_status), "\"%s\" is not an IPv4 address", g_ip_edit);
                    else {
                        uint32_t ip = ia.s_addr;
                        finder_stop();
                        sock_t sk = connect_any(&ip, 1, NET_PORT, 3000, &w);
                        if (sk != SOCK_BAD) use_found(sk, w);
                        else snprintf(g_net_status, sizeof(g_net_status), "No Switch Frame Tap at %s (is Connection set to Network on the Switch?)", g_ip_edit);
                    }
                }
            }
            if (g_menu) menu_draw(ren);
            continue;
        }
        /* the shortcuts (the drawer has all of them too) */
        const SDL_Keycode key = e.type == SDL_KEYDOWN && !e.key.repeat ? e.key.keysym.sym : SDLK_UNKNOWN;
        if (key == SDLK_TAB) { act_conn(g_conn != CONN_NET); if (g_menu) menu_draw(ren); }
        else if (key == SDLK_i && act_ip_possible()) { act_ip(); menu_draw(ren); }
        else if (key == SDLK_F11 || key == SDLK_f ||
                 (e.type == SDL_MOUSEBUTTONDOWN && (e.button.clicks == 2 || e.button.button == SDL_BUTTON_MIDDLE))) {
            act_fullscreen(win);
            /* Keep chrome painted after FS toggle so the restore icon stays in sync. */
            g_ui_ptr_inside = 1;
            g_ui_mouse_t = SDL_GetPerformanceCounter();
            viewer_repaint();
        }
        else if (key == SDLK_r) { act_record(win); if (g_menu) menu_draw(ren); }
        else if (key == SDLK_o || key == SDLK_F3) act_hud(-1);
        else if (key == SDLK_v) { act_vsync(ren, !g_vsync); if (g_menu) menu_draw(ren); }
        else if (key == SDLK_a) { act_lock_aspect(win, !g_lock_aspect); if (g_menu) menu_draw(ren); }
        else if (key == SDLK_b) { act_chromeless(win, !g_chromeless); if (g_menu) menu_draw(ren); }
        else if (key == SDLK_u) { act_upscaler(ren, -1); if (g_menu) menu_draw(ren); }
#ifdef __APPLE__
        else if (key == SDLK_F9) {
            g_fsr_sharpness = g_fsr_sharpness < 40 ? 40 : g_fsr_sharpness < 70 ? 70 : g_fsr_sharpness < 100 ? 100 : 40;
            act_upscaler(ren, UP_FSR);
        }
#endif
        else if (key == SDLK_m) { act_mute(!g_mute); if (g_menu) menu_draw(ren); }
        else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_ENTER) {
                g_ui_ptr_inside = 1;
                g_ui_mouse_t = SDL_GetPerformanceCounter();
            } else if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
                g_ui_ptr_inside = 0;
                /* Keep an in-progress drag alive when the cursor briefly leaves the
                 * client area; only drop a not-yet-started pending click. */
                if (!g_dragging) g_drag_pending = 0;
            } else if ((e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED) && !g_in_size_move) {
                /* Win32: WM_SIZING locks aspect during the drag; EXITSIZEMOVE snaps
                 * and saves. Do not aspect_constrain here on Linux - tiling / half-
                 * screen snap would fight the WM. Capture geom only; prefs on
                 * exit-size-move / mouse-up / quit. */
#ifdef _WIN32
                aspect_constrain_window(win);
#endif
                window_geom_capture(win);
            } else if (e.window.event == SDL_WINDOWEVENT_MOVED && !g_in_size_move) {
                window_geom_capture(win);
            } else if (e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                       e.window.event == SDL_WINDOWEVENT_RESTORED) {
                window_geom_capture(win);
                prefs_save(); /* discrete user action, not a drag stream */
            }
            /* resized, uncovered, or hover chrome: draw the last picture again */
            viewer_repaint();
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

/* v0.7.14: the console can present another USB gadget first. SysDVR is the
 * common one (18d1:4ee0): its module owns usb:ds, so the viewer waits forever
 * for a 1209:5f1e that will never appear. Say which program is in the way. */
static int conflict_on_bus(libusb_context *ctx, char *out, size_t cap)
{
    libusb_device **list = NULL;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    out[0] = 0;
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (dd.idVendor != 0x18d1 || dd.idProduct != 0x4ee0) continue;
        char product[96] = "";
        libusb_device_handle *h = NULL;
        if (libusb_open(list[i], &h) == 0) {
            if (dd.iProduct) libusb_get_string_descriptor_ascii(h, dd.iProduct, (unsigned char *)product, sizeof(product));
            libusb_close(h);
        }
        snprintf(out, cap, "%s", product[0] ? product : "SysDVR (18d1:4ee0)");
        found = 1;
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
static int app_wait_device(libusb_context *ctx, SDL_Window *win, SDL_Renderer *ren, SDL_Texture *tex)
{
    SDL_SetWindowTitle(win, "Switch Frame Tap - waiting for the Switch (USB cable, a game running)");
    int said = 0;
    g_menu = 1;
    g_menu_state = MENU_WAITING;
    if (g_open_settings) { ui_open(1); g_open_settings = 0; }
    for (int tick = 0;; tick++) {
        /* v0.5: the menu, redrawn often enough for the keys and the REC timer;
         * the bus is looked at every 250 ms as before */
        beat();
        app_events(win, ren, tex);
        if (g_quit) { finder_stop(); return 0; }
        g_lost = 0;                        /* a Tab while waiting is not a disconnect */
        if (g_conn == CONN_NET) {
            /* v0.7: network only - the finder (last address, subnet scan),
             * the beacon, or a typed address */
            g_menu_state = MENU_WAITING;
            finder_start();
            if (net_try_connect()) {
                char t[160];
                snprintf(t, sizeof(t), "Switch Frame Tap - connected over the network (%s), waiting for the picture", g_net_peer);
                SDL_SetWindowTitle(win, t);
                g_menu_state = MENU_CONNECTED;
                menu_draw(ren);
                return 1;
            }
            menu_draw(ren); SDL_Delay(50); continue;
        }
        finder_stop();                     /* USB only */
        if (tick % 5 != 0) { menu_draw(ren); SDL_Delay(50); continue; }
        libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, SFT_VID, SFT_PID);
        if (h) {
            libusb_set_auto_detach_kernel_driver(h, 1);
            if (libusb_claim_interface(h, SFT_IFACE) == 0) {
                SDL_SetWindowTitle(win, "Switch Frame Tap - connected, waiting for the picture");
                g_menu_state = MENU_CONNECTED;
                menu_draw(ren);
                g_usb = h;
                return 1;
            }
            libusb_close(h);
            app_title_cannot_open(win, LIBUSB_ERROR_BUSY);
            said = 1;
        } else {
            int err = 0;
            char other[96];
            if (switch_on_bus(ctx, &err)) {
                if (said != 1) fprintf(stderr, "usb: the Switch is present but cannot be opened (%s)\n", libusb_error_name(err));
                app_title_cannot_open(win, err);
                said = 1;
            } else if (conflict_on_bus(ctx, other, sizeof(other))) {
                snprintf(g_menu_other, sizeof(g_menu_other), "%s", other);
                if (said != 2) {
                    fprintf(stderr, "usb: the Switch is presenting %s, not Switch Frame Tap - remove/disable that program and reboot\n", other);
                    char t[220];
                    snprintf(t, sizeof(t), "Switch Frame Tap - the Switch is presenting %s, not Switch Frame Tap", other);
                    SDL_SetWindowTitle(win, t);
                }
                g_menu_state = MENU_WRONG_DEVICE;
                said = 2;
            } else if (said) {
                SDL_SetWindowTitle(win, "Switch Frame Tap - waiting for the Switch (USB cable, a game running)");
                said = 0;
                g_menu_state = MENU_WAITING;
            }
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
static uint32_t g_crop_wh[256];      /* v0.7.16: its dynamic-resolution crop (w << 16 | h), 0: all of it */
static uint8_t g_crop_mid[256];      /* v0.7.16: that crop sits in the middle */

/* v0.7.16: dynamic resolution (Super Mario Galaxy) - the console sends the
 * frame 1:1 and says which part the game drew; only that part is stretched
 * over the picture, as the Switch's screen does. Sets the crop for the
 * upscaler and repaints; 1 when this frame has one */
static int frame_crop(const AVFrame *f, uint32_t W, uint32_t H)
{
    const uint32_t c = g_crop_wh[f->pts & 255];
    const int cw = (int)(c >> 16) & ~1, ch = (int)(c & 0xFFFF) & ~1;
    const int ok = c != 0 && cw >= 64 && ch >= 64 && cw <= (int)W && ch <= (int)H && (cw < (int)W || ch < (int)H);
    const int mid = ok && g_crop_mid[f->pts & 255];   /* Kingdom Hearts: the middle */
    const int cx = mid ? (((int)W - cw) / 2) & ~1 : 0, cy = mid ? (((int)H - ch) / 2) & ~1 : 0;
    g_up_crop_w = ok ? cw : 0; g_up_crop_h = ok ? ch : 0;
    g_up_crop_x = cx; g_up_crop_y = cy;
    g_paint_crop = (SDL_Rect){ cx, cy, ok ? cw : 0, ok ? ch : 0 };
    return ok;
}

static float g_kb_pts[256], g_dec_pts[256];   /* v0.7.2 overlay: its size, its decode time */
static unsigned char g_key_pts[256];          /* and whether it was a keyframe */

/* counters the decoder writes and the display loop reads (benign races) */
static volatile long s_packets = 0, s_decoded = 0, s_undecoded = 0, s_lost = 0, s_keyframes = 0, s_sessions = 1;
static volatile double s_bytes = 0, s_dec_ms = 0, s_dec_max = 0;
static int g_dec_low = 1, g_dec_fallback = 0;   /* v0.7.11: one decode thread, and whether it fell back */
static char g_dec_name[48] = "";                /* v0.7.11: "GPU (d3d11va)" and the like, for g_dec_mode */

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
        g_crop_wh[hdr.kind & 255] = (hdr.flags & SFT_FLAG_CROP) ? hdr.block_h_log2 : 0;
        g_crop_mid[hdr.kind & 255] = (hdr.flags & SFT_FLAG_CROP_MID) != 0;
        if (g_dec_reopen) {                    /* v0.7.12: hardware decoding switched in the drawer */
            g_dec_reopen = 0;
            h264_close(a->dec);
            key_request();                     /* v0.7.13: a new decoder starts at a keyframe */
            if (h264_open(a->dec, 1, 1, g_hwdec) == 0) {
                if (a->dec->hwname) snprintf(g_dec_name, sizeof(g_dec_name), "GPU (%s)", a->dec->hwname);
                else snprintf(g_dec_name, sizeof(g_dec_name), "1 thread");
                g_dec_mode = g_dec_name;
                g_dec_low = 1; g_dec_fallback = 0;
            }
        }
        const Uint64 d0 = SDL_GetPerformanceCounter();
        const int got = h264_decode(a->dec, payload, (int)hdr.length, (int64_t)hdr.kind);
        const double dm = (double)(SDL_GetPerformanceCounter() - d0) * 1000.0 / (double)freq;
        g_kb_pts[hdr.kind & 255] = hdr.length / 1024.0f;
        g_dec_pts[hdr.kind & 255] = (float)dm;
        g_key_pts[hdr.kind & 255] = (hdr.flags & SFT_FLAG_KEY) != 0;
        s_dec_ms += dm;
        if (dm > s_dec_max) s_dec_max = dm;
        /* v0.7.11: one decode thread by default; if this PC cannot keep up
         * (over 12 ms a frame for 120 frames), two frame threads, once */
        {
            static long fb_n = 0; static double fb_ms = 0;
            if (g_dec_low && !g_dec_fallback && !a->dec->hwname) {
                fb_ms += dm;
                if (++fb_n >= 120) {
                    if (fb_ms / fb_n > 12.0) {
                        fprintf(stderr, "\ndecode: %.1f ms a frame on one thread - switching to two frame threads (one frame more latency)\n", fb_ms / fb_n);
                        h264_close(a->dec);
                        key_request();
                        if (h264_open(a->dec, 0, 2, 0) == 0) { g_dec_fallback = 1; g_dec_mode = "2 threads (slow PC)"; hud_toast("Decoding on 2 threads: this PC is slow for 1"); }
                        continue;
                    }
                    fb_n = 0; fb_ms = 0;
                }
            }
        }
        /* v0.7.11: the GPU decoder keeps failing (a driver that took the
         * stream but gives nothing back): the software decoder, for good */
        {
            static int hw_err = 0;
            if (a->dec->hwname && got < 0 && ++hw_err >= 30) {
                fprintf(stderr, "\ndecode: %s failed %d frames in a row - software from now on\n", a->dec->hwname, hw_err);
                h264_close(a->dec);
                key_request();
                if (h264_open(a->dec, 1, 1, 0) == 0) { snprintf(g_dec_name, sizeof(g_dec_name), "1 thread (GPU failed)"); g_dec_mode = g_dec_name; hud_toast("GPU decoding failed - decoding on the CPU"); }
                hw_err = 0;
                continue;
            }
            if (got > 0) hw_err = 0;
        }
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
    if (h264_open(&dec, low_latency, threads, g_hwdec) != 0) { fprintf(stderr, "libavcodec H.264 decoder unavailable\n"); return 1; }
    g_dec_low = low_latency || dec.hwname;
    if (dec.hwname) { snprintf(g_dec_name, sizeof(g_dec_name), "GPU (%s)", dec.hwname); g_dec_mode = g_dec_name; }
    else g_dec_mode = low_latency ? "1 thread" : "frame threads (--threads)";
    fprintf(stderr, "decode: %s\n", dec.hwname ? g_dec_name : low_latency ? "one thread, no decoder delay" : "frame threads (--threads)");
    if (!win) {
        if (video_init() != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        win = SDL_CreateWindow("Switch Frame Tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, video_window_flags());
        aspect_hook_window(win);
        apply_always_on_top(win);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!ren) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    } else {
        aspect_hook_window(win);
        apply_always_on_top(win);
    }
    watchdog_start();
    /* v0.3: the sound device (opened paused; audio_packet starts it) */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {0}, have = {0};
        want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 512;
        g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        fprintf(stderr, "audio: %s\n", g_adev ? "48 kHz stereo" : SDL_GetError());
    } else {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
    }
    if (!g_vsync) SDL_RenderSetVSync(ren, 0);   /* v0.7.11: V, remembered */
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(ren, &ri) == 0) {
        fprintf(stderr, "renderer: %s%s\n", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? ", vsync" : ", NO vsync");
    }
    for (int i = 0; i < FQ; i++) g_fq[i] = av_frame_alloc();
    if (!g_fm) { g_fm = SDL_CreateMutex(); g_f_put = SDL_CreateCond(); }
    rec_init(g_rec_dir, g_mp4);
    if (g_mp4) rec_toggle();
#ifdef SFT_ZEROCOPY
    if (dec.hwname && !strcmp(dec.hwname, "vaapi")) {
        g_zc_ready = zc_probe(ren);
        if (g_zc_ready) fprintf(stderr, "decode: zero-copy (VAAPI -> EGL/OpenGL, no copy to memory)\n");
        else fprintf(stderr, "decode: zero-copy unavailable - %s\n", zc.why);
    }
#endif
    dec_args_t da = { &dec, rec, es };
    g_dec_stop = 0;
    SDL_Thread *dth = SDL_CreateThread(decoder_main, "decoder", &da);

    SDL_Texture *tex = NULL;
    AVFrame *show = av_frame_alloc();
    uint32_t W = 0, H = 0;
    int tex_nv12 = 0;
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
        beat();
        app_events(win, ren, tex);
        if (g_ren_swap) {                      /* v0.7.11: the upscaler wants the other renderer */
            ren_swap(&win, &ren, &tex);
            W = H = 0;
            if (g_quit) break;
            if (g_menu) menu_draw(ren);
        }
        if (g_run_secs > 0 && (double)(SDL_GetPerformanceCounter() - t_start) / (double)freq >= g_run_secs) break;
        if (g_lost) {
            /* the Switch left the bus (rebooted, cable out) */
            const int never_streamed = (g_conn == CONN_NET && !g_net_got_stream);
            reader_stop();
            transport_close();
            fprintf(stderr, "\nthe Switch disconnected%s\n", app ? " - waiting for it" : "");
            if (!app) break;
            if (never_streamed) net_on_lost_without_stream();
            else net_clear_busy();
            g_menu = 1;
            if (!app_wait_device(ctx, win, ren, tex)) break;
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
                /* v0.7.16: 8 s, not 1.5 - a loading screen draws nothing for
                 * a while too (Duke Nukem: ~5 s, Doom 3), and the TV keeps
                 * the last picture meanwhile; the menu only for a longer gap
                 * (the HOME menu itself streams at once with home_menu on) */
                if (!g_menu && (!t_prev_show || (double)(nowm - t_prev_show) / (double)freq > 8.0)) {
                    g_menu = 1;
                    g_menu_state = t_prev_show ? MENU_PAUSED : MENU_CONNECTED;
                    if (t_prev_show) fprintf(stderr, "\nno new picture for 8 s - showing the menu\n");
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
            int zc_drawn = 0;
#ifdef SFT_ZEROCOPY
            if (show->format == AV_PIX_FMT_VAAPI) {   /* zerocopy.h */
                if (g_zc_ready && pic_neutral() && (uint32_t)show->width == W && (uint32_t)show->height == H && tex
                    && !frame_crop(show, W, H)) {    /* a dynamic-resolution crop is drawn from the copy */
                    SDL_RenderClear(ren);
                    zc_drawn = zc_draw(ren, show, (int)W, (int)H, g_upscaler);
                    if (!zc_drawn) { g_zc_ready = 0; fprintf(stderr, "\ndecode: zero-copy off - %s\n", zc.why); }
                }
                if (!zc_drawn) {                       /* P settings, a new size, or zero-copy failed: to memory */
                    AVFrame *m = av_frame_alloc();
                    if (m && av_hwframe_transfer_data(m, show, 0) == 0) {
                        av_frame_copy_props(m, show);
                        av_frame_unref(show);
                        av_frame_move_ref(show, m);
                    }
                    av_frame_free(&m);
                }
            }
            zc.last = zc_drawn;
#endif
            const int nv12 = show->format == AV_PIX_FMT_NV12 || show->format == AV_PIX_FMT_VAAPI;   /* v0.7.11: from the GPU decoder */
            if (!tex || (uint32_t)show->width != W || (uint32_t)show->height != H || nv12 != tex_nv12) {
                if (tex) SDL_DestroyTexture(tex);
                W = (uint32_t)show->width; H = (uint32_t)show->height; tex_nv12 = nv12;
                /* Keep the user's window (and remembered geometry). Only auto-fit
                 * to the stream when nothing was saved and --window was not set. */
                if (g_win_w > 0) SDL_SetWindowSize(win, g_win_w, g_win_h);
                else if (!g_geom_valid &&
                         !(SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)))
                    size_video_window(win, (int)W, (int)H);
                tex = SDL_CreateTexture(ren, nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);
                SDL_RenderSetLogicalSize(ren, (int)W, (int)H);
                fprintf(stderr, "\nstream: %ux%u H.264%s\n", W, H, nv12 ? " (NV12)" : "");
            }
            if (!zc_drawn) {
            uint8_t *pd[3] = { show->data[0], show->data[1], show->data[2] };
            int pl[3] = { show->linesize[0], show->linesize[1], show->linesize[2] };
            pic_apply(pd, pl, (int)W, (int)H, nv12);   /* v0.7.12: P - nothing at the defaults */
            if (nv12) SDL_UpdateNVTexture(tex, NULL, pd[0], pl[0], pd[1], pl[1]);
            else SDL_UpdateYUVTexture(tex, NULL, pd[0], pl[0], pd[1], pl[1], pd[2], pl[2]);
            SDL_RenderClear(ren);
            frame_crop(show, W, H);
            if (!up_draw(ren, pd, pl, (int)W, (int)H, g_upscaler, nv12))   /* v0.7.11: U */
                SDL_RenderCopy(ren, tex, g_paint_crop.w ? &g_paint_crop : NULL, NULL);
            }
            if (rec_active()) {                /* v0.5: shown while recording (not in the file); v0.7.12: with the time */
                ui_rec_badge(ren, (int)W, (int)H);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            }
            if (g_hud != HUD_OFF || g_hud_toast) {     /* v0.7.2: the stats overlay */
                char rs[200], cs[96];
                rec_status(rs, sizeof(rs));
                if (g_conn == CONN_NET) snprintf(cs, sizeof(cs), "Network%s%s", net_peer_for_menu()[0] ? " " : "", net_peer_for_menu());
                else snprintf(cs, sizeof(cs), "USB");
                hud_draw(ren, (int)W, (int)H, cs, rs[0] ? rs + 5 : "");
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            }
            ui_overlay(ren, (int)W, (int)H);   /* v0.7.12: the Settings button and the drawer */
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            if (g_shot && g_run_secs > 0 && (double)(SDL_GetPerformanceCounter() - t_start) / (double)freq >= g_run_secs - 1.0) {
                int ow = 0, oh = 0;
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (sf && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
                    SDL_SaveBMP(sf, g_shot);
                    fprintf(stderr, "\nsaved %dx%d to %s\n", ow, oh, g_shot);
                }
                if (sf) SDL_FreeSurface(sf);
                g_shot = NULL;
            }
            SDL_RenderPresent(ren);            /* waits for the display's refresh (vsync) */
            const Uint64 now = SDL_GetPerformanceCounter();
            w_draw += (double)(now - r0) * 1000.0 / (double)freq;
            shown++; w_shown++;
            const int k = (int)(show->pts & 255);
            const double l = (double)(now - g_t_rx[k]) * 1000.0 / (double)freq;
            if (l >= 0 && l < 5000) { lat_sum += l; lat_n++; w_lat += l; w_lat_n++; if (l > lat_max) lat_max = l; }
            if (g_age_ms[k] >= 0) { age_sum += g_age_ms[k]; age_n++; w_age += g_age_ms[k]; w_age_n++; if (g_age_ms[k] > age_max) age_max = g_age_ms[k]; }
            {
                const double gap = t_prev_show ? (double)(now - t_prev_show) * 1000.0 / (double)freq : 0.0;
                if (gap > w_gap) w_gap = gap;
                hud_frame((float)gap, (l >= 0 && l < 5000) ? (float)l : 0.0f, (float)g_age_ms[k], g_kb_pts[k], g_key_pts[k], g_dec_pts[k]);
            }
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
                {
                    hud_sec hs = { 0 };
                    hs.fps = w_shown / secs; hs.dec_fps = dec / secs; hs.mbps = mbps;
                    hs.pc = w_lat_n ? w_lat / w_lat_n : 0.0; hs.con = w_age_n ? w_age / w_age_n : 0.0;
                    hs.dec_ms = dms; hs.dec_max = s_dec_max; hs.draw_ms = w_shown ? w_draw / w_shown : 0.0; hs.gap_max = w_gap;
                    hs.a_kbs = a_kbs; hs.a_q = a_q; hs.skipped = w_skipped; hs.lost = s_lost; hs.keyframes = s_keyframes;
                    hs.underruns = g_a_underruns; hs.resyncs = g_a_resyncs; hs.sessions = s_sessions; hs.packets = s_packets;
                    hs.qmax = w_qmax_seen; hs.w = (int)W; hs.h = (int)H;
                    hud_second(&hs);
                }
                /* v0.7.13: upscaling this GPU cannot keep up with (FSR to a 4K
                 * TV on a laptop's integrated graphics: 60 fps decoded, 30
                 * shown) - off for this session, said once; viewer.ini keeps
                 * the choice, so a smaller screen next time tries again */
                {
                    static int slow_secs = 0;
                    const double shown_fps = w_shown / secs, dec_fps = dec / secs;
                    if (g_upscaler != UP_OFF && g_up.active && dec_fps >= 40 && dms < 10.0 && w_skipped > 0 && shown_fps < 0.8 * dec_fps) {
                        if (++slow_secs >= 2) {
                            char t[112];
                            snprintf(t, sizeof(t), "Upscaling to %dx%d is too slow on this graphics card - turned off (U to try again)", g_up.out_w, g_up.out_h);
                            fprintf(stderr, "\nupscaler: %s (%.0f of %.0f fps shown)\n", t, shown_fps, dec_fps);
                            g_upscaler = UP_OFF;
                            if (up_needs_swap(ren)) g_ren_swap = 1;
                            hud_toast(t);
                            slow_secs = 0;
                        }
                    } else {
                        slow_secs = 0;
                    }
                }
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
    up_reset();
    watchdog_stop();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    transport_close();
    if (ctx) libusb_exit(ctx);
    if (g_in) fclose(g_in);
    return 0;
}
#endif

/* v0.7.14: --check, the one command to run on a Linux/KDE machine that has no
 * picture. It touches nothing on the console and prints what the viewer sees. */
static void check_usb_product(libusb_device *dev, char *out, size_t cap)
{
    out[0] = 0;
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) != 0 || !dd.iProduct) return;
    libusb_device_handle *h = NULL;
    if (libusb_open(dev, &h) == 0) {
        libusb_get_string_descriptor_ascii(h, dd.iProduct, (unsigned char *)out, cap);
        libusb_close(h);
    }
}

static int run_check(const char *net_host)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Switch Frame Tap viewer check\n");
    printf("session: %s  desktop: %s\n",
           getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "?",
           getenv("XDG_CURRENT_DESKTOP") ? getenv("XDG_CURRENT_DESKTOP") : "?");
    printf("wayland: %s  display: %s\n",
           getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "-",
           getenv("DISPLAY") ? getenv("DISPLAY") : "-");
#ifndef _WIN32
    /* the rule must sort before 73-seat-late.rules, which applies uaccess:
     * the 99- name of v0.7.19 and older leaves the Switch root-only */
    if (access("/etc/udev/rules.d/70-switch-frame-tap.rules", R_OK) == 0) printf("udev rule: installed\n");
    else if (access("/etc/udev/rules.d/99-switch-frame-tap.rules", R_OK) == 0)
        printf("udev rule: OLD NAME (99-switch-frame-tap.rules is applied after uaccess - rename it to 70-switch-frame-tap.rules)\n");
    else printf("udev rule: missing\n");
#endif

    if (video_init() == 0) {
        printf("video driver: %s\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        SDL_Window *w = SDL_CreateWindow("Switch Frame Tap check", 0, 0, 320, 180, SDL_WINDOW_HIDDEN);
        if (!w) printf("window: %s\n", SDL_GetError());
        else {
            SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!r) printf("renderer: FAILED (%s)\n", SDL_GetError());
            else {
                SDL_RendererInfo ri;
                if (SDL_GetRendererInfo(r, &ri) == 0) printf("renderer: %s%s\n", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? " (vsync)" : "");
                SDL_DestroyRenderer(r);
            }
            SDL_DestroyWindow(w);
        }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            printf("audio driver: %s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        } else printf("audio: FAILED (%s)\n", SDL_GetError());
    } else {
        printf("video: FAILED (%s)\n", SDL_GetError());
    }
    SDL_Quit();

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        printf("usb: libusb_init failed\n");
    } else {
        libusb_device **list = NULL;
        const ssize_t n = libusb_get_device_list(ctx, &list);
        int sft = 0, wrong = 0;
        for (ssize_t i = 0; i < n; i++) {
            struct libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor != SFT_VID || dd.idProduct != SFT_PID) continue;
            char product[96];
            check_usb_product(list[i], product, sizeof(product));
            int err = 0;
            libusb_device_handle *h = NULL;
            err = libusb_open(list[i], &h);
            if (h) libusb_close(h);
            printf("usb: Switch Frame Tap found (%s) - open: %s\n", product[0] ? product : "1209:5f1e",
                   err == 0 ? "yes" : libusb_error_name(err));
            sft++;
        }
        for (ssize_t i = 0; i < n; i++) {
            struct libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor == 0x18d1 && dd.idProduct == 0x4ee0) {
                char product[96];
                check_usb_product(list[i], product, sizeof(product));
                printf("usb: CONFLICT - %s (18d1:4ee0) is presenting the USB gadget\n", product[0] ? product : "SysDVR");
                wrong++;
            }
        }
        if (!sft && !wrong) printf("usb: no Switch Frame Tap device (is the console on USB mode and the cable in?)\n");
        if (list) libusb_free_device_list(list, 1);
        libusb_exit(ctx);
    }

    net_init();
    const uint32_t me = local_ip();
    char here[64] = "?";
    if (me) { struct in_addr ia; ia.s_addr = me; inet_ntop(AF_INET, &ia, here, sizeof(here)); }
    printf("network: this PC is %s\n", here);
    if (net_host && net_host[0]) {
        struct in_addr ia;
        uint32_t which = 0;
        if (inet_pton(AF_INET, net_host, &ia) != 1) {
            printf("network: \"%s\" is not an IPv4 address\n", net_host);
        } else {
            sock_t s = connect_any(&ia.s_addr, 1, NET_PORT, 3000, &which);
            if (s != SOCK_BAD) {
                printf("network: connected to %s:%d (TCP stream port)\n", net_host, NET_PORT);
                sock_close(s);
            } else {
                printf("network: cannot connect to %s:%d (Connection=Network on the Switch?)\n", net_host, NET_PORT);
            }
        }
    } else {
        printf("network: video uses TCP %d (UDP %d beacon), not the SD-card FTP server\n", NET_PORT, BEACON_PORT);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int swap_rb = 0, scale = 1, headless = 0, low_latency = 1, threads = 2;   /* v0.7.11: one decode thread unless --threads */
    long max_frames = 0;
    int check = 0;
    const char *file = NULL, *record = NULL, *h264_out = NULL, *net_host = NULL;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);          /* v0.7.13: the viewer sends (key_request); a closed socket must not end it */
#else
    /* v0.7.13: per-monitor DPI aware - without it Windows drew the viewer at
     * 96 dpi and stretched it on a scaled display (a 4K TV at 150-300 %):
     * a soft picture and soft text */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--swap")) swap_rb = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--h264") && i + 1 < argc) h264_out = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--low-latency")) low_latency = 1;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) { threads = atoi(argv[++i]); low_latency = 0; }
        else if (!strcmp(argv[i], "--app")) g_app = 1;
        else if (!strcmp(argv[i], "--paced")) g_paced = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_run_secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) g_shot = argv[++i];
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc) {
            const char *m = argv[++i];
            for (int k = 0; k < HUD_MODES; k++) if (!strcmp(m, hud_mode_name[k]) || (k == HUD_FPS && !strcmp(m, "fps"))) g_hud = k;
        }
        else if (!strcmp(argv[i], "--upscaler") && i + 1 < argc) up_set_by_key(argv[++i]);
        else if (!strcmp(argv[i], "--software")) g_hwdec = 0;
        else if (!strcmp(argv[i], "--picture") && i + 1 < argc) pic_parse(argv[++i]);   /* v0.7.12: B,C,S,G,SH as in viewer.ini (tests) */
        else if (!strcmp(argv[i], "--no-zero-copy")) g_zc = 0;      /* VAAPI frames copied back to memory and uploaded again */
        else if (!strcmp(argv[i], "--hwdec")) g_hwdec = 1;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &g_win_w, &g_win_h);
        else if (!strcmp(argv[i], "--mp4") && i + 1 < argc) g_mp4 = argv[++i];
        else if (!strcmp(argv[i], "--rec-dir") && i + 1 < argc) g_rec_dir = argv[++i];
        else if (!strcmp(argv[i], "--net") && i + 1 < argc) net_host = argv[++i];
        else if (!strcmp(argv[i], "--no-aspect-lock")) g_lock_aspect_cli = 0;
        else if (!strcmp(argv[i], "--aspect-lock")) g_lock_aspect_cli = 1;
        else if (!strcmp(argv[i], "--chromeless")) g_chromeless_cli = 1;
        else if (!strcmp(argv[i], "--no-chromeless")) g_chromeless_cli = 0;
        else if (!strcmp(argv[i], "--update-check")) { prefs_load(); return upd_cli(argv, 0); }   /* v0.7.14 */
        else if (!strcmp(argv[i], "--settings")) g_open_settings = 1;   /* tests: the drawer open at start */
        else if (!strcmp(argv[i], "--update")) { prefs_load(); return upd_cli(argv, 1); }
        else if (!strcmp(argv[i], "--video-driver") && i + 1 < argc) g_video_driver = argv[++i];
        else if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "--doctor")) check = 1;
        else { fprintf(stderr, "unknown option %s (see the comment at the top of raw-view.c)\n", argv[i]); return 2; }
    }
    if (scale < 1) scale = 1;
    if (g_lock_aspect_cli >= 0) g_lock_aspect = g_lock_aspect_cli;
    if (g_chromeless_cli >= 0) g_chromeless = g_chromeless_cli;
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
#elif defined(__linux__)
    /* v0.7.14: the KDE application launcher has no terminal. Keep the viewer's
     * log under $XDG_STATE_HOME so a failure is visible after the fact. */
    if (g_app && !file && !check) viewer_log_init();
#endif
    /* Ctrl-C before the first frame still ends with the summary below */
    signal(SIGINT, on_sigint);
    if (check) return run_check(net_host);

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
        if (video_init() != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Switch Frame Tap", SDL_GetError(), NULL);
            return 1;
        }
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        prefs_load();                      /* v0.7: USB or network, the last address; v0.7.11: before the renderer */
        if (!headless && !file) upd_init(argv);   /* v0.7.14: where this viewer is, and (update_check) the start-up check */
        if (g_lock_aspect_cli >= 0) g_lock_aspect = g_lock_aspect_cli;
        up_pick_renderer();
        if (g_chromeless_cli >= 0) g_chromeless = g_chromeless_cli;
        {
            int iw = 1280, ih = 720;
            if (g_geom_valid) { iw = g_geom_w; ih = g_geom_h; }
            else window_geom_default_size(&iw, &ih);
            /* Create centered; apply saved x/y after the renderer exists - early
             * SetWindowPosition is often ignored on DPI-aware Win32. */
            win = SDL_CreateWindow("Switch Frame Tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   iw, ih, video_window_flags());
        }
        aspect_hook_window(win);
        win32_ensure_frame_styles(win);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (win && !ren) {                 /* v0.7.14: no GPU renderer (a VM, a broken driver): slower, but a picture */
            fprintf(stderr, "SDL window: %s - drawing in software\n", SDL_GetError());
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!ren) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Switch Frame Tap", SDL_GetError(), NULL);
            return 1;
        }
        if (win) {
            window_geom_apply(win);
            SDL_PumpEvents();
            window_geom_apply(win);          /* second pass after first layout */
        }
        rec_init(g_rec_dir, g_mp4);
        net_init();
        watchdog_start();
        menu_draw(ren);
        if (!app_wait_device(ctx, win, ren, NULL)) { watchdog_stop(); SDL_Quit(); libusb_exit(ctx); return 0; }
    } else if (net_host) {
        /* v0.7: straight to a console on the network (tests; or a network
         * where broadcasts do not arrive) */
        if (!net_connect(net_host, NET_PORT)) { fprintf(stderr, "cannot connect to %s:%d\n", net_host, NET_PORT); return 1; }
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
    if (g_usb || g_sock != SOCK_BAD) reader_start();
    FILE *rec = record ? fopen(record, "wb") : NULL;
    FILE *es = h264_out ? fopen(h264_out, "wb") : NULL;
    if ((record && !rec) || (h264_out && !es)) { perror("output file"); return 1; }
#ifdef SFT_H264
    /* v0.2 test3: live USB goes through the paced three-thread pipeline;
     * the loop below is file replay (and the tests) */
    if (g_usb || g_sock != SOCK_BAD || (g_in && g_paced)) {
        (void)headless; (void)max_frames;
        if (!g_usb && g_sock == SOCK_BAD) reader_start();            /* --paced: the recording goes through the same reader thread */
        return run_live(ctx, win, ren, g_app, low_latency, threads, rec, es);
    }
#endif

#ifdef SFT_H264
    h264_t dec = {0};
    int dec_ok = (h264_open(&dec, low_latency, threads, 0) == 0);   /* file replay and the tests: software */
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
        beat();
        if (g_app && g_lost) {
            /* the Switch left the bus (rebooted, cable out): keep the window */
            const int never_streamed = (g_conn == CONN_NET && !g_net_got_stream);
            reader_stop();
            transport_close();
            g_lost = 0;
            fprintf(stderr, "\nthe Switch disconnected - waiting for it\n");
            if (never_streamed) net_on_lost_without_stream();
            else net_clear_busy();
            if (!app_wait_device(ctx, win, ren, tex)) break;
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
            if (video_init() != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); break; }
            /* the stream is BT.709 limited range (VUI says so too) */
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
            win = SDL_CreateWindow("switch-frame-tap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)(hdr.width*scale), (int)(hdr.height*scale), video_window_flags());
            aspect_hook_window(win);
            /* M86b: SDL scales nearest-neighbour by default, which made edges
             * look pixelated in a resized or maximized window. Linear, and keep
             * 16:9 with letterboxing instead of stretching. */
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            watchdog_start();
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
    up_reset();
    watchdog_stop();
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    free(payload);
    free(rgb);
    if (g_in) fclose(g_in);
    transport_close();
    if (ctx) libusb_exit(ctx);
    return 0;
}
