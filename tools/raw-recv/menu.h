/* menu.h - v0.5: the viewer's main screen while there is no picture: what it
 * is waiting for, the keys (R record, M mute, F11 fullscreen, Esc), and where
 * recordings go. Drawn with SDL2 alone: text comes from font_dejavu.h, a
 * DejaVu Sans atlas built into the program (tools/raw-recv/gen_font.py). */
#include "font_dejavu.h"

enum { MENU_WAITING = 0, MENU_NODRIVER, MENU_CONNECTED, MENU_PAUSED };

static int g_menu = 0;                 /* the menu is on screen (not a frame) */
static int g_menu_state = MENU_WAITING;
static char g_menu_err[64] = "";       /* the libusb error for MENU_NODRIVER */
static SDL_Texture *g_font_tex = NULL;
static SDL_Renderer *g_font_ren = NULL;

static int text_init(SDL_Renderer *ren)
{
    if (g_font_tex && g_font_ren == ren) return 0;
    if (g_font_tex) SDL_DestroyTexture(g_font_tex);
    g_font_tex = NULL;
    g_font_ren = ren;
    Uint32 *px = malloc((size_t)FONT_ATLAS_W * FONT_ATLAS_H * 4);
    if (!px) return -1;
    for (int i = 0; i < FONT_ATLAS_W * FONT_ATLAS_H; i++) {
        const unsigned b = font_atlas[i >> 1];
        const unsigned a = ((i & 1) ? (b & 15) : (b >> 4)) * 17;
        px[i] = (a << 24) | 0xFFFFFFu;             /* white, the colour comes from the mod */
    }
    g_font_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FONT_ATLAS_W, FONT_ATLAS_H);
    if (g_font_tex) {
        SDL_UpdateTexture(g_font_tex, NULL, px, FONT_ATLAS_W * 4);
        SDL_SetTextureBlendMode(g_font_tex, SDL_BLENDMODE_BLEND);
    }
    free(px);
    return g_font_tex ? 0 : -1;
}

static int text_width(const char *s, int px, int bold)
{
    int w = 0;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        w += font_glyphs[bold][c - FONT_FIRST][2];
    }
    return w * px / font_px[bold];
}

/* align: 0 left, 1 centre, 2 right; y is the top of the line */
static void text_draw(SDL_Renderer *ren, const char *s, int x, int y, int px, int bold, SDL_Color col, int align)
{
    if (!g_font_tex) return;
    if (align) x -= text_width(s, px, bold) / (align == 1 ? 2 : 1);
    SDL_SetTextureColorMod(g_font_tex, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(g_font_tex, col.a);
    const int fp = font_px[bold];
    int pen = x * fp;                               /* in font pixels * px, to keep fractions */
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        const unsigned short *g = font_glyphs[bold][c - FONT_FIRST];
        if (c != ' ') {
            SDL_Rect src = { g[0], font_top[bold], g[1], font_h[bold] };
            SDL_Rect dst = { pen / fp, y, g[1] * px / fp + 1, font_h[bold] * px / fp };
            SDL_RenderCopy(ren, g_font_tex, &src, &dst);
        }
        pen += g[2] * px;
    }
}

static void fill(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(ren, &r);
}

/* a key cap: a light box with the key's name in it */
static int key_cap(SDL_Renderer *ren, const char *k, int x, int y)
{
    const SDL_Color edge = { 90, 97, 112, 255 }, face = { 44, 48, 58, 255 }, ink = { 235, 237, 242, 255 };
    const int w = text_width(k, 22, 1) + 28, h = 38;
    fill(ren, x, y, w, h, edge);
    fill(ren, x + 2, y + 2, w - 4, h - 5, face);
    text_draw(ren, k, x + w / 2, y + 6, 22, 1, ink, 1);
    return w;
}

static const char *rec_dir_for_menu(void);
static const char *net_peer_for_menu(void);

static void menu_draw(SDL_Renderer *ren)
{
    const SDL_Color bg = { 18, 20, 24, 255 }, panel = { 28, 31, 38, 255 }, ink = { 235, 237, 242, 255 },
                    dim = { 150, 156, 168, 255 }, accent = { 0, 184, 230, 255 }, warn = { 255, 196, 0, 255 },
                    red = { 235, 64, 52, 255 }, good = { 76, 217, 100, 255 };
    text_init(ren);
    SDL_RenderSetLogicalSize(ren, 1280, 720);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    fill(ren, 0, 0, 1280, 720, bg);

    text_draw(ren, "Switch Frame Tap", 640, 34, 56, 1, ink, 1);

    /* v0.7: the connection, one at a time (Tab) */
    {
        const int net = conn_for_menu();
        const int y = 112;
        const int wl = text_width("Connection:", 22, 0), wu = text_width("USB", 22, 1), wn = text_width("Network", 22, 1);
        int x = 640 - (wl + 24 + wu + 44 + wn + 24 + 44 + 60) / 2;
        text_draw(ren, "Connection:", x, y + 7, 22, 0, dim, 0);
        x += wl + 24;
        const SDL_Color on_bg = { 0, 120, 150, 255 }, off_bg = { 36, 40, 48, 255 };
        fill(ren, x - 12, y, wu + 24, 38, net ? off_bg : on_bg);
        text_draw(ren, "USB", x, y + 6, 22, 1, net ? dim : ink, 0);
        x += wu + 44;
        fill(ren, x - 12, y, wn + 24, 38, net ? on_bg : off_bg);
        text_draw(ren, "Network", x, y + 6, 22, 1, net ? ink : dim, 0);
        x += wn + 44;
        text_draw(ren, "(Tab)", x, y + 8, 20, 0, dim, 0);
    }

    const char *head = "", *l1 = "", *l2 = "";
    SDL_Color hc = accent;
    char errline[200];
    switch (g_menu_state) {
    case MENU_WAITING:
        if (conn_for_menu()) {
            head = "Looking for the Switch on the network";
            l1 = "Set Connection to Network in the Switch's manager app (then restart the Switch).";
            if (ip_edit_for_menu()) {
                snprintf(errline, sizeof(errline), "Switch IP: %s_     (Enter to connect, Esc to cancel)", ip_edit_for_menu());
            } else {
                snprintf(errline, sizeof(errline), "%s  Not found? Press I to type its IP address.", net_status_for_menu());
            }
            l2 = errline;
        } else {
            head = "Waiting for the Switch";
            l1 = "Connect it with the USB-C cable (handheld mode).";
            l2 = "For docked play: Connection = Network, here and in the Switch's manager app.";
        }
        break;
    case MENU_NODRIVER:
        hc = warn;
#ifdef _WIN32
        head = "The Switch is connected but has no USB driver";
        l1 = "Install WinUSB for it once with Zadig - see README-WINDOWS.txt.";
#else
        head = "The Switch is connected but cannot be opened";
        l1 = "Install the udev rule (99-switch-frame-tap.rules) - see the README.";
#endif
        snprintf(errline, sizeof(errline), "(%s)", g_menu_err);
        l2 = errline;
        break;
    case MENU_PAUSED:
        hc = good;
        head = "Connected - the game is not on screen";
        l1 = "Go back to the game (or start one) and the picture returns here.";
        l2 = "Nothing? Check that streaming is on in the overlay or the manager app.";
        break;
    default:
        hc = good;
        head = net_peer_for_menu()[0] ? "Connected over the network - waiting for a game" : "Connected - waiting for a game";
        l1 = "Start a game on the Switch and the picture appears here.";
        l2 = "Nothing? Check that streaming is on in the overlay or the manager app.";
        break;
    }
    text_draw(ren, head, 640, 170, 32, 1, hc, 1);
    text_draw(ren, l1, 640, 220, 22, 0, dim, 1);
    text_draw(ren, l2, 640, 250, 22, 0, ip_edit_for_menu() ? ink : dim, 1);

    /* the keys */
    const int px0 = 250, pw = 780, py0 = 296, ph = 332;
    fill(ren, px0, py0, pw, ph, panel);
    text_draw(ren, "KEYS", px0 + 30, py0 + 18, 18, 1, dim, 0);
    struct { const char *k, *what; } rows[] = {
        { "R", "Record / stop recording" },
        { "M", "Mute / unmute the game sound" },
        { "F11", "Fullscreen - or double-click the window" },
        { "Esc", "Leave fullscreen / close" },
        { "Tab", "Switch the connection: USB / Network" },
        { "I", "Type the Switch's IP address (Network)" },
        { "O", "Stats overlay: off / FPS / graphs / full (or F3)" },
    };
    for (int i = 0; i < 7; i++) {
        const int y = py0 + 46 + i * 40;
        key_cap(ren, rows[i].k, px0 + 30, y);
        text_draw(ren, rows[i].what, px0 + 140, y + 5, 24, 0, (i == 5 && !conn_for_menu()) ? dim : ink, 0);
    }
    /* live state next to R and M */
    {
        char rs[200];
        rec_status(rs, sizeof(rs));
        const char *r = rs[0] ? rs + 5 : "";             /* rec_status starts with "  |  " */
        if (rec_active()) text_draw(ren, r, px0 + pw - 30, py0 + 51, 22, 1, red, 2);
        if (g_mute) text_draw(ren, "muted", px0 + pw - 30, py0 + 91, 22, 1, warn, 2);
        if (!rec_active() && r[0]) text_draw(ren, r, 640, 690, 18, 0, dim, 1);
    }

    if (net_peer_for_menu()[0] && g_menu_state != MENU_WAITING && g_menu_state != MENU_NODRIVER) {
        char nl[128];
        snprintf(nl, sizeof(nl), "Network: the Switch at %s", net_peer_for_menu());
        text_draw(ren, nl, 640, 662, 18, 0, dim, 1);
    }
    const char *dir = rec_dir_for_menu();
    if (dir[0]) {
        char line[1200];
        snprintf(line, sizeof(line), "Recordings (MP4 with sound) are saved in %s", dir);
        text_draw(ren, line, 640, 634, 18, 0, dim, 1);
    }
    SDL_RenderPresent(ren);
}
