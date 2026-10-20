/* ui.h - v0.7.12: the viewer's look. One home screen while there is no
 * picture, and one settings drawer (Esc, or the Settings button) with every
 * option in it, over the home screen or over the game - mouse or keys. The
 * old shortcuts (R, M, F11, Tab, I, O, V, U) still work; P opens the drawer at
 * the picture controls.
 *
 * Immediate mode: every draw lays the controls out again and remembers where
 * the clickable ones are; the next mouse event is matched against that.
 * Everything is in 720p units ("the height is 720"): the home screen is
 * 1280x720 logical, over the game it is scaled with the picture.
 */
#pragma once

/* ---- the theme -------------------------------------------------------- */
static const SDL_Color UI_BG = { 12, 14, 18, 255 }, UI_SURF = { 21, 24, 30, 255 }, UI_SURF2 = { 31, 35, 43, 255 },
                       UI_SURF3 = { 44, 49, 59, 255 }, UI_LINE = { 255, 255, 255, 20 },
                       UI_INK = { 236, 238, 243, 255 }, UI_DIM = { 152, 160, 175, 255 }, UI_FAINT = { 100, 108, 122, 255 },
                       UI_ACC = { 56, 189, 248, 255 }, UI_GOOD = { 52, 211, 153, 255 }, UI_WARN = { 251, 191, 36, 255 },
                       UI_BAD = { 248, 113, 113, 255 };

static int g_ui_open = 0;                  /* the settings drawer */
static int g_ui_focus = 0, g_ui_kbd = 0;   /* the row the keys are on; shown after a key */
static float g_ui_scroll = 0, g_ui_content_h = 0;
static float g_ui_mx = -1, g_ui_my = -1;   /* the mouse, in units */
static Uint64 g_ui_mouse_t = 0;            /* its last move: the Settings button shows over the game */
static int g_ui_drag = 0, g_ui_drag_arg = 0;
static float g_ui_k = 1, g_ui_w = 1280;    /* units -> logical, and the width in units, at the last draw */
static int g_ui_scroll_to = -1;            /* bring this row into view at the next draw */
static SDL_Window *g_ui_win = NULL;        /* for the fullscreen switch's state (app_events sets it) */

enum { H_NONE, H_SETTINGS, H_CLOSE, H_BACKDROP, H_CONN, H_IP, H_FULL, H_VSYNC, H_UPS, H_HUD, H_HWDEC,
       H_PIC, H_PIC_RESET, H_SOUND, H_REC, H_PANEL, H_FSR_SHARP, H_SUPPORT };

typedef struct { SDL_FRect r; int id, arg; } ui_hit_t;
static ui_hit_t g_ui_hits[96];
static int g_ui_nhits;
/* the drawer's rows in order, for the keys: (id, arg) */
static struct { int id, arg; float y, h; } g_ui_rows[24];
static int g_ui_nrows;

static void ui_hit(float x, float y, float w, float h, int id, int arg)
{
    if (g_ui_nhits < (int)(sizeof(g_ui_hits) / sizeof(g_ui_hits[0])))
        g_ui_hits[g_ui_nhits++] = (ui_hit_t){ { x, y, w, h }, id, arg };
}

static int ui_in(float x, float y, float w, float h) { return g_ui_mx >= x && g_ui_mx < x + w && g_ui_my >= y && g_ui_my < y + h; }

static void ui_row(int id, int arg, float y, float h)
{
    if (g_ui_nrows < (int)(sizeof(g_ui_rows) / sizeof(g_ui_rows[0]))) {
        g_ui_rows[g_ui_nrows].id = id; g_ui_rows[g_ui_nrows].arg = arg;
        g_ui_rows[g_ui_nrows].y = y; g_ui_rows[g_ui_nrows].h = h;
        g_ui_nrows++;
    }
}

static int ui_focused(int id, int arg)
{
    return g_ui_kbd && g_ui_focus < g_ui_nrows && g_ui_rows[g_ui_focus].id == id && g_ui_rows[g_ui_focus].arg == arg;
}

static SDL_Color ui_alpha(SDL_Color c, int a) { c.a = (Uint8)a; return c; }

/* text that does not fit: cut with "..." */
static void ui_text_fit(SDL_Renderer *ren, const char *s, int x, int y, int px, int bold, SDL_Color c, int align, int maxw)
{
    char b[256];
    snprintf(b, sizeof(b), "%s", s);
    if (text_width(b, px, bold) > maxw) {
        size_t n = strlen(b);
        while (n > 3 && text_width(b, px, bold) > maxw) { b[--n] = 0; b[n - 1] = '.'; b[n - 2] = '.'; b[n - 3] = '.'; }
    }
    text_draw(ren, b, x, y, px, bold, c, align);
}

/* centred lines, broken at spaces to fit maxw; returns the height used */
static int ui_wrap(SDL_Renderer *ren, const char *s, int cx, int y, int px, int bold, SDL_Color c, int maxw, int lh)
{
    char line[256];
    int used = 0;
    while (*s) {
        size_t n = 0, last_space = 0;
        while (s[n] && n < sizeof(line) - 1) {
            memcpy(line, s, n + 1); line[n + 1] = 0;
            if (text_width(line, px, bold) > maxw && last_space) break;
            if (s[n] == ' ') last_space = n;
            n++;
        }
        size_t take = s[n] ? (last_space ? last_space : n) : n;
        memcpy(line, s, take); line[take] = 0;
        text_draw(ren, line, cx, y + used, px, bold, c, 1);
        used += lh;
        s += take;
        while (*s == ' ') s++;
    }
    return used;
}

/* ---- the controls ------------------------------------------------------ */

/* a toggle switch, its right edge at xr */
static void ui_toggle(SDL_Renderer *ren, float xr, float y, int on, int id, int enabled)
{
    const float w = 44, h = 24, x = xr - w;
    SDL_Color track = on ? UI_ACC : UI_SURF3;
    if (!enabled) track = ui_alpha(track, 90);
    ui_rrect(ren, x, y, w, h, h / 2, track);
    ui_rrect(ren, on ? x + w - 21 : x + 3, y + 3, 18, 18, 9, enabled ? UI_INK : UI_DIM);
    if (enabled) ui_hit(x - 4, y - 6, w + 8, h + 12, id, !on);
}

/* segmented buttons, right edge at xr (or centred at cx when xr < 0) */
static float ui_seg(SDL_Renderer *ren, float xr, float cx, float y, const char *const *opt, int n, int sel, int id, int px)
{
    const float h = 32, pad = 16;
    float wsum = 0, wi[8];
    for (int i = 0; i < n && i < 8; i++) { wi[i] = text_width(opt[i], px, 1) + 2 * pad; wsum += wi[i]; }
    float x = xr >= 0 ? xr - wsum - 6 : cx - (wsum + 6) / 2;
    ui_rrect(ren, x, y, wsum + 6, h, 9, UI_SURF2);
    float sx = x + 3;
    for (int i = 0; i < n && i < 8; i++) {
        const int hov = ui_in(sx, y, wi[i], h);
        if (i == sel) ui_rrect(ren, sx, y + 3, wi[i], h - 6, 7, UI_ACC);
        else if (hov) ui_rrect(ren, sx, y + 3, wi[i], h - 6, 7, UI_SURF3);
        text_draw(ren, opt[i], (int)(sx + wi[i] / 2), (int)(y + (h - px * 1.18f) / 2), px, 1, i == sel ? (SDL_Color){ 8, 20, 30, 255 } : UI_INK, 1);
        ui_hit(sx, y, wi[i], h, id, i);
        sx += wi[i];
    }
    return wsum + 6;
}

/* a pill button, right edge at xr (or centred at cx when xr < 0); returns its width */
static float ui_button(SDL_Renderer *ren, float xr, float cx, float y, const char *label, SDL_Color fill, SDL_Color ink, int id, int arg, int enabled)
{
    const int px = 15;
    const float w = text_width(label, px, 1) + 32, h = 32;
    const float x = xr >= 0 ? xr - w : cx - w / 2;
    SDL_Color f = fill;
    if (enabled && ui_in(x, y, w, h)) { f.r = (Uint8)(f.r + (255 - f.r) / 8); f.g = (Uint8)(f.g + (255 - f.g) / 8); f.b = (Uint8)(f.b + (255 - f.b) / 8); }
    ui_rrect(ren, x, y, w, h, h / 2, enabled ? f : ui_alpha(f, 80));
    text_draw(ren, label, (int)(x + w / 2), (int)(y + 7), px, 1, enabled ? ink : UI_FAINT, 1);
    if (enabled) ui_hit(x, y, w, h, id, arg);
    return w;
}

/* the settings button: a gear in a round button, "Settings" beside it on hover */
static void ui_gear_button(SDL_Renderer *ren, float xr, float y, SDL_Color bg, int id)
{
    const float d = 38, x = xr - d;
    const int hov = ui_in(x, y, d, d);
    SDL_Color b = bg;
    if (hov) { b = UI_SURF3; b.a = bg.a > 230 ? bg.a : 230; }
    ui_rrect(ren, x, y, d, d, d / 2, b);
    ui_gear_init(ren);
    if (g_ui_gear) {
        SDL_SetTextureColorMod(g_ui_gear, UI_INK.r, UI_INK.g, UI_INK.b);
        SDL_SetTextureAlphaMod(g_ui_gear, 255);
        const SDL_FRect g = { x + 8, y + 8, d - 16, d - 16 };
        SDL_RenderCopyF(ren, g_ui_gear, NULL, &g);
    }
    if (hov) {
        const float tw = text_width("Settings", 14, 1) + 20;
        ui_rrect(ren, x - tw - 8, y + 6, tw, 26, 13, (SDL_Color){ 21, 24, 30, 230 });
        text_draw(ren, "Settings", (int)(x - 8 - tw / 2), (int)y + 10, 14, 1, UI_INK, 1);
    }
    ui_hit(x, y, d, d, id, 0);
}

/* a small key hint, like a key cap */
static float ui_key(SDL_Renderer *ren, float x, float y, const char *k)
{
    const int px = 13;
    const float w = text_width(k, px, 1) + 14;
    ui_rrect(ren, x, y, w, 22, 6, UI_SURF3);
    text_draw(ren, k, (int)(x + w / 2), (int)(y + 3), px, 1, UI_INK, 1);
    return w;
}

/* ---- the settings drawer ---------------------------------------------- */

static void ui_section(SDL_Renderer *ren, float x, float *y, const char *name)
{
    *y += 14;
    text_draw(ren, name, (int)x, (int)*y, 13, 1, UI_FAINT, 0);
    *y += 26;
}

/* a row's label (and a smaller line under it); the control goes on the right */
static void ui_label(SDL_Renderer *ren, float x, float y, float h, const char *label, const char *sub, int focus, float w)
{
    if (focus) ui_rrect(ren, x - 12, y - 2, w + 24, h + 4, 10, UI_SURF2);
    if (sub && sub[0]) {
        text_draw(ren, label, (int)x, (int)(y + h / 2 - 20), 17, 0, UI_INK, 0);
        ui_text_fit(ren, sub, (int)x, (int)(y + h / 2 + 2), 13, 0, UI_DIM, 0, (int)(w - 70));
    } else {
        text_draw(ren, label, (int)x, (int)(y + h / 2 - 11), 17, 0, UI_INK, 0);
    }
}

static const char *const ui_hud_names[HUD_MODES] = { "Off", "FPS", "Graphs", "Full" };

static void ui_drawer(SDL_Renderer *ren, float W)
{
    const float pw = W < 900 ? W * 0.5f : 420, x0 = W - pw, pad = 26, cx = x0 + pad, cw = pw - 2 * pad, xr = x0 + pw - pad;
    /* a dim veil over the rest; a click there closes */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 110);
    SDL_FRect veil = { 0, 0, x0, 720 };
    SDL_RenderFillRectF(ren, &veil);
    ui_hit(0, 0, x0, 720, H_BACKDROP, 0);
    ui_hit(x0, 0, pw, 720, H_PANEL, 0);   /* clicks on the panel's empty space stay in it */
    SDL_SetRenderDrawColor(ren, UI_SURF.r, UI_SURF.g, UI_SURF.b, 250);
    SDL_FRect panel = { x0, 0, pw, 720 };
    SDL_RenderFillRectF(ren, &panel);
    SDL_SetRenderDrawColor(ren, UI_LINE.r, UI_LINE.g, UI_LINE.b, UI_LINE.a);
    SDL_FRect edge = { x0, 0, 1, 720 };
    SDL_RenderFillRectF(ren, &edge);

    /* the rows, scrolled, under the header */
    const float top = 76, bottom = 720 - 44;
    if (g_ui_scroll_to >= 0 && g_ui_scroll_to < g_ui_nrows) {
        const float ry = g_ui_rows[g_ui_scroll_to].y + g_ui_scroll, rh = g_ui_rows[g_ui_scroll_to].h;
        if (ry - 40 < top) g_ui_scroll -= top - (ry - 40);
        if (ry + rh + 10 > bottom) g_ui_scroll += ry + rh + 10 - bottom;
    }
    g_ui_scroll_to = -1;
    const float maxs = g_ui_content_h - (bottom - top);
    if (g_ui_scroll > maxs) g_ui_scroll = maxs;
    if (g_ui_scroll < 0) g_ui_scroll = 0;
    float sx, sy;
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_Rect clip = { (int)(x0 * sx), (int)(top * sy), (int)(pw * sx) + 1, (int)((bottom - top) * sy) };
    SDL_Rect clip_units = { (int)x0, (int)top, (int)pw + 1, (int)(bottom - top) };
    (void)clip;
    SDL_RenderSetClipRect(ren, &clip_units);
    const int first_hit = g_ui_nhits;
    g_ui_nrows = 0;
    float y = top - g_ui_scroll;
    const float y_start = y;

    ui_section(ren, cx, &y, "CONNECTION");
    {
        static const char *const conn[] = { "USB", "Network" };
        const float h = 44;
        ui_row(H_CONN, 0, y, h);
        ui_label(ren, cx, y, h, "Connection", NULL, ui_focused(H_CONN, 0), cw);
        ui_seg(ren, xr, 0, y + 6, conn, 2, g_conn == CONN_NET, H_CONN, 14);
        y += h + 6;
        if (g_conn == CONN_NET) {
            char sub[96];
            if (g_sock != SOCK_BAD && net_peer_for_menu()[0]) snprintf(sub, sizeof(sub), "Connected to %s", net_peer_for_menu());
            else snprintf(sub, sizeof(sub), "%s", g_last_ip[0] ? g_last_ip : "Found by itself on the network");
            ui_row(H_IP, 0, y, h);
            ui_label(ren, cx, y, h, "Switch address", sub, ui_focused(H_IP, 0), cw);
            ui_button(ren, xr, 0, y + 6, "Type IP", UI_SURF3, UI_INK, H_IP, 0, act_ip_possible());
            y += h + 6;
        }
    }

    ui_section(ren, cx, &y, "DISPLAY");
    {
        const float h = 48;
        ui_row(H_FULL, 0, y, h);
        ui_label(ren, cx, y, h, "Fullscreen", "F11, or double-click the picture", ui_focused(H_FULL, 0), cw);
        ui_toggle(ren, xr, y + 12, g_ui_win && (SDL_GetWindowFlags(g_ui_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0, H_FULL, 1);
        y += h + 4;
        ui_row(H_VSYNC, 0, y, h);
        ui_label(ren, cx, y, h, "Vsync", g_vsync ? "Smooth, no tearing" : "Off: lowest latency, may tear", ui_focused(H_VSYNC, 0), cw);
        ui_toggle(ren, xr, y + 12, g_vsync, H_VSYNC, 1);
        y += h + 4;
        ui_row(H_HWDEC, 0, y, h);
        char dec[64];
        snprintf(dec, sizeof(dec), "Now: %s", g_dec_mode && g_dec_mode[0] ? g_dec_mode : (g_hwdec ? "the graphics card" : "the CPU"));
#if defined(__APPLE__)
        ui_label(ren, cx, y, h, "Hardware decoding (beta)", dec, ui_focused(H_HWDEC, 0), cw);
#else
        ui_label(ren, cx, y, h, "Hardware decoding", dec, ui_focused(H_HWDEC, 0), cw);
#endif
        ui_toggle(ren, xr, y + 12, g_hwdec, H_HWDEC, 1);
        y += h + 4;
        /* the multi-choice rows: the label, the choices under it */
        const float h2 = 74;
        ui_row(H_UPS, 0, y, h2);
        if (ui_focused(H_UPS, 0)) ui_rrect(ren, cx - 12, y - 2, cw + 24, h2 + 4, 10, UI_SURF2);
        text_draw(ren, "Upscaling", (int)cx, (int)(y + 4), 17, 0, UI_INK, 0);
        char why[48] = "";
        if (g_upscaler != UP_OFF) up_hud_text(why, sizeof(why));
        ui_text_fit(ren, why, (int)xr, (int)(y + 7), 13, 0, UI_DIM, 2, (int)(cw - 110));
        static const char *const ups[] = { "Off", "FSR 1", "NIS", "RTX VSR" };
        ui_seg(ren, xr, 0, y + 32, ups, UP_COUNT, g_upscaler, H_UPS, 13);
        y += h2 + 4;
#ifdef __APPLE__
        if (g_upscaler == UP_FSR) {
            const float sh = 52, ty = y + 34;
            ui_row(H_FSR_SHARP, 0, y, sh);
            if (ui_focused(H_FSR_SHARP, 0)) ui_rrect(ren, cx - 12, y - 2, cw + 24, sh + 4, 10, UI_SURF2);
            text_draw(ren, "FSR sharpness", (int)cx, (int)(y + 4), 16, 0, UI_INK, 0);
            char value[16]; snprintf(value, sizeof(value), "%d%%", g_fsr_sharpness);
            text_draw(ren, value, (int)xr, (int)(y + 4), 15, 1, UI_ACC, 2);
            const float vx = cx + cw * g_fsr_sharpness / 100.0f;
            ui_rrect(ren, cx, ty - 2, cw, 4, 2, UI_SURF3);
            ui_rrect(ren, cx, ty - 2, vx - cx, 4, 2, UI_ACC);
            ui_rrect(ren, vx - 8, ty - 8, 16, 16, 8, UI_INK);
            ui_hit(cx - 8, ty - 14, cw + 16, 28, H_FSR_SHARP, 0);
            y += sh + 4;
        }
#endif
        ui_row(H_HUD, 0, y, h2);
        if (ui_focused(H_HUD, 0)) ui_rrect(ren, cx - 12, y - 2, cw + 24, h2 + 4, 10, UI_SURF2);
        text_draw(ren, "Stats overlay", (int)cx, (int)(y + 4), 17, 0, UI_INK, 0);
        text_draw(ren, "O or F3", (int)xr, (int)(y + 7), 13, 0, UI_DIM, 2);
        ui_seg(ren, xr, 0, y + 32, ui_hud_names, HUD_MODES, g_hud, H_HUD, 13);
        y += h2 + 4;
    }

    ui_section(ren, cx, &y, "PICTURE");
    if (!pic_neutral()) ui_button(ren, xr, 0, y - 36, "Reset", UI_SURF3, UI_INK, H_PIC_RESET, 0, 1);
    for (int r = 0; r < PIC_N; r++) {
        const float h = 52;
        ui_row(H_PIC, r, y, h);
        if (ui_focused(H_PIC, r)) ui_rrect(ren, cx - 12, y - 2, cw + 24, h + 4, 10, UI_SURF2);
        text_draw(ren, pic_rows[r].name, (int)cx, (int)(y + 4), 16, 0, UI_INK, 0);
        char t[16];
        pic_value_text(r, t, sizeof(t));
        const int v = *pic_field(r), lo = pic_rows[r].min, hi = pic_rows[r].max, def = *(&pic_default.bright + r);
        text_draw(ren, t, (int)xr, (int)(y + 4), 15, 1, v == def ? UI_DIM : UI_ACC, 2);
        const float tx = cx, tw = cw, ty = y + 34;
        ui_rrect(ren, tx, ty - 2, tw, 4, 2, UI_SURF3);
        const float fx = tx + (float)(def - lo) / (hi - lo) * tw, vx = tx + (float)(v - lo) / (hi - lo) * tw;
        ui_rrect(ren, vx < fx ? vx : fx, ty - 2, vx < fx ? fx - vx : vx - fx, 4, 2, UI_ACC);
        ui_rrect(ren, fx - 1, ty - 6, 2, 12, 1, UI_FAINT);
        const int hov = ui_in(tx - 8, ty - 14, tw + 16, 28) || (g_ui_drag == H_PIC && g_ui_drag_arg == r);
        ui_rrect(ren, vx - (hov ? 9 : 8), ty - (hov ? 9 : 8), hov ? 18 : 16, hov ? 18 : 16, 9, UI_INK);
        ui_hit(tx - 8, ty - 14, tw + 16, 28, H_PIC, r);
        y += h + 2;
    }

    ui_section(ren, cx, &y, "SOUND");
    {
        const float h = 48;
        ui_row(H_SOUND, 0, y, h);
        ui_label(ren, cx, y, h, "Game sound", g_mute ? "Muted (M)" : "On (M mutes)", ui_focused(H_SOUND, 0), cw);
        ui_toggle(ren, xr, y + 12, !g_mute, H_SOUND, 1);
        y += h + 4;
    }

    ui_section(ren, cx, &y, "RECORDING");
    {
        const float h = 48;
        char rs[200], sub[200];
        rec_status(rs, sizeof(rs));
        snprintf(sub, sizeof(sub), "%s", rec_active() ? rs + 5 : "MP4 with sound (R)");
        ui_row(H_REC, 0, y, h);
        ui_label(ren, cx, y, h, "Record", sub, ui_focused(H_REC, 0), cw);
        if (rec_active()) ui_button(ren, xr, 0, y + 8, "Stop", UI_BAD, (SDL_Color){ 30, 8, 8, 255 }, H_REC, 0, 1);
        else ui_button(ren, xr, 0, y + 8, "Start", UI_SURF3, UI_INK, H_REC, 0, 1);
        y += h + 2;
        const char *dir = rec_dir_for_menu();
        if (dir[0]) {
            char line[1100];
            snprintf(line, sizeof(line), "Saved in %s", dir);
            ui_text_fit(ren, line, (int)cx, (int)y, 13, 0, UI_FAINT, 0, (int)cw);
            y += 24;
        }
    }
    /* the very end of the panel: one faint line, a link only for whoever
     * looks for it (never shown anywhere else, never asks) */
    {
        y += 18;
        const char *pre = "Free and open source  -  ", *link = "support the project";
        const int pw = text_width(pre, 13, 0), lw = text_width(link, 13, 0);
        text_draw(ren, pre, (int)cx, (int)y, 13, 0, UI_FAINT, 0);
        const int hov = ui_in(cx + pw, y - 2, lw, 20);
        text_draw(ren, link, (int)cx + pw, (int)y, 13, 0, hov ? UI_DIM : UI_FAINT, 0);
        if (hov) ui_rrect(ren, cx + pw, y + 17, lw, 1, 0, UI_DIM);
        ui_hit(cx + pw, y - 2, lw, 20, H_SUPPORT, 0);
        y += 22;
    }
    y += 16;
    g_ui_content_h = y - y_start;
    SDL_RenderSetClipRect(ren, NULL);
    /* hits under the header or the footer do not count */
    for (int i = first_hit; i < g_ui_nhits; i++) {
        SDL_FRect *r = &g_ui_hits[i].r;
        if (g_ui_hits[i].id == H_PANEL) continue;
        if (r->y + r->h < top || r->y > bottom) { r->w = 0; r->h = 0; continue; }
        if (r->y < top) { r->h -= top - r->y; r->y = top; }
        if (r->y + r->h > bottom) r->h = bottom - r->y;
    }

    /* the header and the footer over the scrolled rows */
    SDL_SetRenderDrawColor(ren, UI_SURF.r, UI_SURF.g, UI_SURF.b, 255);
    SDL_FRect hb = { x0 + 1, 0, pw - 1, top }, fb = { x0 + 1, bottom, pw - 1, 720 - bottom };
    SDL_RenderFillRectF(ren, &hb);
    SDL_RenderFillRectF(ren, &fb);
    text_draw(ren, "Settings", (int)cx, 24, 24, 1, UI_INK, 0);
    ui_button(ren, xr, 0, 22, "Close", UI_SURF2, UI_INK, H_CLOSE, 0, 1);
    SDL_SetRenderDrawColor(ren, UI_LINE.r, UI_LINE.g, UI_LINE.b, UI_LINE.a);
    SDL_FRect l1 = { x0 + 1, top - 1, pw - 1, 1 }, l2 = { x0 + 1, bottom, pw - 1, 1 };
    SDL_RenderFillRectF(ren, &l1);
    SDL_RenderFillRectF(ren, &l2);
    float kx = cx;
    kx += ui_key(ren, kx, bottom + 12, "Esc") + 6;
    text_draw(ren, "close", (int)kx, (int)bottom + 14, 13, 0, UI_DIM, 0);
    kx += text_width("close", 13, 0) + 14;
    kx += ui_key(ren, kx, bottom + 12, "Up/Down") + 6;
    text_draw(ren, "move", (int)kx, (int)bottom + 14, 13, 0, UI_DIM, 0);
    kx += text_width("move", 13, 0) + 14;
    kx += ui_key(ren, kx, bottom + 12, "Left/Right") + 6;
    text_draw(ren, "change", (int)kx, (int)bottom + 14, 13, 0, UI_DIM, 0);
    if (g_ui_scroll < maxs - 1) {           /* more below */
        SDL_SetRenderDrawColor(ren, UI_SURF.r, UI_SURF.g, UI_SURF.b, 200);
        SDL_FRect f = { x0 + 1, bottom - 18, pw - 1, 18 };
        SDL_RenderFillRectF(ren, &f);
    }
}

/* ---- over the game ------------------------------------------------------ */

/* v0.7.12: while recording, a "REC 1:23" pill at the top left of the picture
 * (drawn on the screen only - the recording keeps the console's frames) */
static void ui_rec_badge(SDL_Renderer *ren, int W, int H)
{
    if (!rec_active() || H <= 0 || text_init(ren) != 0) return;
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(ren, &old);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren, &sx, &sy);
    const float k = (float)H / 720.0f, w720 = W / k;
    SDL_RenderSetScale(ren, sx * k, sy * k);
    const int s = (int)rec_seconds();
    char t[32];
    if (s >= 3600) snprintf(t, sizeof(t), "REC %d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(t, sizeof(t), "REC %d:%02d", s / 60, s % 60);
    /* top left, under the stats overlay when it is on */
    const float tw = text_width(t, 15, 1), w = tw + 46, x = 12;
    const float y = g_hud == HUD_FPS ? 58 : g_hud == HUD_GRAPHS ? 258 : g_hud == HUD_FULL ? 512 : 12;
    (void)w720;
    ui_rrect(ren, x, y, w, 32, 16, (SDL_Color){ 18, 20, 25, 215 });
    /* the dot breathes, once a second */
    const double ph = (double)(SDL_GetTicks() % 1000) / 1000.0;
    const int a = (int)(150 + 105 * (0.5 + 0.5 * cos(ph * 2 * 3.14159265)));
    ui_rrect(ren, x + 14, y + 11, 10, 10, 5, (SDL_Color){ UI_BAD.r, UI_BAD.g, UI_BAD.b, (Uint8)a });
    text_draw(ren, t, (int)(x + 32), (int)y + 7, 15, 1, UI_INK, 0);
    SDL_RenderSetScale(ren, sx, sy);
    SDL_SetRenderDrawBlendMode(ren, old);
}

/* the Settings button (after the mouse moved) and the drawer, over the picture */
static void ui_overlay(SDL_Renderer *ren, int W, int H)
{
    const Uint64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    const int show_btn = g_ui_mouse_t && (double)(now - g_ui_mouse_t) / f < 2.5;
    g_ui_nhits = 0;
    if (!g_ui_open && !show_btn) return;
    if (H <= 0 || text_init(ren) != 0) return;
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(ren, &old);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren, &sx, &sy);
    const float k = (float)H / 720.0f;
    SDL_RenderSetScale(ren, sx * k, sy * k);
    g_ui_k = k; g_ui_w = W / k;
    if (g_ui_open) ui_drawer(ren, g_ui_w);
    else ui_gear_button(ren, g_ui_w - 20, 20, (SDL_Color){ 21, 24, 30, 200 }, H_SETTINGS);
    SDL_RenderSetScale(ren, sx, sy);
    SDL_SetRenderDrawBlendMode(ren, old);
}

/* ---- the home screen ---------------------------------------------------- */

static void menu_draw(SDL_Renderer *ren)
{
    text_init(ren);
    SDL_RenderSetLogicalSize(ren, 1280, 720);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(ren);                  /* the letterbox bars too (DiM3NT0R, PR #12) */
    g_ui_k = 1; g_ui_w = 1280;
    g_ui_nhits = 0;

    /* the top bar */
    ui_rrect(ren, 32, 28, 12, 12, 6, UI_ACC);
    text_draw(ren, "Switch Frame Tap", 54, 22, 20, 1, UI_INK, 0);
    if (!g_ui_open) ui_gear_button(ren, 1248, 16, UI_SURF2, H_SETTINGS);

    /* what it is waiting for */
    const char *head = "", *l1 = "", *l2 = "";
    SDL_Color hc = UI_ACC;
    char errline[200];
    switch (g_menu_state) {
    case MENU_WAITING:
        if (conn_for_menu()) {
            head = "Looking for the Switch";
            l1 = "Set Connection to Network in the Switch Frame Tap app on the Switch too.";
            if (!ip_edit_for_menu()) { snprintf(errline, sizeof(errline), "%s", net_status_for_menu()); l2 = errline; }
        } else {
            head = "Waiting for the Switch";
            l1 = "Connect it to this PC with a USB-C cable, in handheld mode.";
            l2 = "Playing docked? Choose Network.";
        }
        break;
    case MENU_NODRIVER:
        hc = UI_WARN;
#ifdef _WIN32
        head = "The Switch needs its USB driver";
        l1 = "Install WinUSB for it once with Zadig - see README-WINDOWS.txt.";
#elif defined(__APPLE__)
        head = "The Switch cannot be opened";
        l1 = "Is another program using it (another viewer, OBS)? Close it, or plug the cable in again.";
#else
        head = "The Switch cannot be opened";
        l1 = "Install the USB rule (99-switch-frame-tap.rules) - see the README.";
#endif
        snprintf(errline, sizeof(errline), "(%s)", g_menu_err);
        l2 = errline;
        break;
    case MENU_WRONG_DEVICE:
        hc = UI_WARN;
        head = "Another USB program has the Switch";
        snprintf(errline, sizeof(errline),
                 "The PC sees \"%s\". Remove or disable it on the SD card, install Switch Frame Tap, and reboot the console.",
                 g_menu_other[0] ? g_menu_other : "SysDVR");
        l1 = errline;
        l2 = "Or set Connection to Network in the Switch Frame Tap app and use Network here (Tab).";
        break;
    case MENU_PAUSED:
        hc = UI_GOOD;
        head = "Connected - the game is not on screen";
        l1 = "Go back to the game, or start one, and the picture returns here.";
        l2 = "Nothing? Check that streaming is on in the overlay or the app on the Switch.";
        break;
    default:
        hc = UI_GOOD;
        head = net_peer_for_menu()[0] ? "Connected over the network" : "Connected";
        l1 = "Start a game on the Switch and the picture appears here.";
        l2 = "Nothing? Check that streaming is on in the overlay or the app on the Switch.";
        break;
    }
    char st_head[64], st_detail[120];
    uint32_t st_reason = 0;
    if ((g_menu_state == MENU_CONNECTED || g_menu_state == MENU_PAUSED) &&
        status_for_menu(st_head, sizeof(st_head), st_detail, sizeof(st_detail), &st_reason)) {
        head = st_head;
        l1 = st_detail;
        l2 = "";
        hc = (st_reason == 1 || st_reason == 2 || st_reason == 10) ? UI_GOOD : UI_WARN;
    }
    /* "Paused - a manual, news or the eShop is open": a short title, the
     * rest as the first line under it */
    char title[96], rest[96], first[200];
    snprintf(title, sizeof(title), "%s", head);
    {
        char *dash = strstr(title, " - ");
        if (dash) {
            *dash = 0;
            snprintf(rest, sizeof(rest), "%s", dash + 3);
            if (rest[0] >= 'a' && rest[0] <= 'z') rest[0] = (char)(rest[0] - 32);
            snprintf(first, sizeof(first), "%s", l1);
            l2 = first;
            l1 = rest;
        }
    }

    /* the card */
    const float cw = 640, cx = 640, card_x = cx - cw / 2, card_y = 150, card_h = 330;
    ui_rrect(ren, card_x, card_y, cw, card_h, 20, UI_SURF);
    /* a breathing status light */
    const double t = (double)SDL_GetTicks() / 1000.0;
    const int pulse = (g_menu_state == MENU_WAITING) ? (int)(60 + 50 * (0.5 + 0.5 * sin(t * 3.0))) : 70;
    ui_rrect(ren, cx - 22, card_y + 40, 44, 44, 22, ui_alpha(hc, pulse));
    ui_rrect(ren, cx - 9, card_y + 53, 18, 18, 9, hc);
    {
        int tpx = 28;
        while (tpx > 20 && text_width(title, tpx, 1) > cw - 60) tpx -= 2;
        text_draw(ren, title, (int)cx, (int)card_y + 104 + (28 - tpx) / 2, tpx, 1, UI_INK, 1);
    }
    int y = (int)card_y + 150;
    y += ui_wrap(ren, l1, (int)cx, y, 17, 0, UI_DIM, (int)cw - 80, 24);
    if (l2[0]) y += ui_wrap(ren, l2, (int)cx, y, 15, 0, UI_FAINT, (int)cw - 80, 22);

    /* USB / Network, and the address */
    {
        static const char *const conn[] = { "USB", "Network" };
        const float by = card_y + card_h - (conn_for_menu() ? 92 : 64);
        if (ip_edit_for_menu()) {
            const float fw = 300;
            ui_rrect(ren, cx - fw / 2, by - 2, fw, 40, 10, UI_ACC);
            ui_rrect(ren, cx - fw / 2 + 2, by, fw - 4, 36, 9, UI_SURF2);
            char ipl[64];
            snprintf(ipl, sizeof(ipl), "%s%s", ip_edit_for_menu(), ((SDL_GetTicks() / 500) & 1) ? "_" : " ");
            text_draw(ren, ipl, (int)(cx - fw / 2 + 16), (int)by + 8, 18, 0, UI_INK, 0);
            text_draw(ren, "Enter to connect, Esc to cancel", (int)cx, (int)by + 46, 13, 0, UI_DIM, 1);
        } else {
            const float sw = ui_seg(ren, -1, cx, by, conn, 2, conn_for_menu(), H_CONN, 15);
            (void)sw;
            if (conn_for_menu() && act_ip_possible()) {
                ui_button(ren, -1, cx, by + 42, "Type the Switch's IP", (SDL_Color){ 0, 0, 0, 0 }, UI_ACC, H_IP, 0, 1);
            }
        }
    }

    /* the key hints under the card */
    {
        struct { const char *k, *what; } hints[] = { { "Esc", "Settings" }, { "F11", "Fullscreen" }, { "R", "Record" }, { "Tab", "USB / Network" } };
        float total = 0;
        for (int i = 0; i < 4; i++) total += text_width(hints[i].k, 13, 1) + 14 + 8 + text_width(hints[i].what, 14, 0) + 28;
        float x = 640 - (total - 28) / 2;
        for (int i = 0; i < 4; i++) {
            x += ui_key(ren, x, 520, hints[i].k) + 8;
            text_draw(ren, hints[i].what, (int)x, 522, 14, 0, UI_DIM, 0);
            x += text_width(hints[i].what, 14, 0) + 28;
        }
    }

    /* the bottom line: recording, sound, where recordings go */
    {
        char rs[200];
        rec_status(rs, sizeof(rs));
        if (rec_active()) {
            const char *r = rs + 5;
            const float w = text_width(r, 14, 1) + 40;
            ui_rrect(ren, 640 - w / 2, 572, w, 30, 15, ui_alpha(UI_BAD, 40));
            ui_rrect(ren, 640 - w / 2 + 12, 583, 8, 8, 4, UI_BAD);
            text_draw(ren, r, (int)(640 - w / 2 + 28), 578, 14, 1, UI_BAD, 0);
        }
        if (g_mute) text_draw(ren, "Sound muted (M)", 1248, 690, 13, 0, UI_WARN, 2);
        const char *dir = rec_dir_for_menu();
        if (dir[0]) {
            char line[1100];
            snprintf(line, sizeof(line), "Recordings: %s", dir);
            ui_text_fit(ren, line, 32, 690, 13, 0, UI_FAINT, 0, 900);
        }
    }
    if (g_ui_open) ui_drawer(ren, 1280);
    SDL_RenderPresent(ren);
}

/* ---- input -------------------------------------------------------------- */

static void ui_open(int open)
{
    g_ui_open = open;
    g_ui_drag = 0;
    if (open) { g_ui_kbd = 0; }
}

/* change the row's value: d = -1 / +1 (Left / Right), 0 = Enter */
static void ui_change(SDL_Window *win, SDL_Renderer *ren, int id, int arg, int d)
{
    switch (id) {
    case H_CONN: act_conn(d == 0 ? g_conn != CONN_NET : d > 0); break;
    case H_IP: if (d == 0) { act_ip(); ui_open(0); } break;
    case H_FULL: act_fullscreen(win); break;
    case H_VSYNC: act_vsync(ren, !g_vsync); break;
    case H_HWDEC: act_hwdec(!g_hwdec); break;
    case H_UPS: act_upscaler(ren, d < 0 ? (g_upscaler + UP_COUNT - 1) % UP_COUNT : (g_upscaler + 1) % UP_COUNT); break;
    case H_HUD: act_hud(d < 0 ? (g_hud + HUD_MODES - 1) % HUD_MODES : (g_hud + 1) % HUD_MODES); break;
    case H_FSR_SHARP:
        g_fsr_sharpness = d == 0 ? 70 : SDL_clamp(g_fsr_sharpness + d * 5, 0, 100);
        prefs_save();
        break;
    case H_PIC: {
        int *f = pic_field(arg);
        if (d < 0 && *f > pic_rows[arg].min) *f -= pic_rows[arg].step;
        if (d > 0 && *f < pic_rows[arg].max) *f += pic_rows[arg].step;
        if (d == 0) *f = *(&pic_default.bright + arg);
        prefs_save();
        break;
    }
    case H_SOUND: act_mute(!g_mute); break;
    case H_REC: if (d == 0) act_record(win); break;
    default: break;
    }
}

/* a slider follows the mouse */
static void ui_slide(float mx)
{
    for (int i = 0; i < g_ui_nhits; i++) {
        if (g_ui_hits[i].id != g_ui_drag || g_ui_hits[i].arg != g_ui_drag_arg) continue;
        const SDL_FRect r = g_ui_hits[i].r;
        const float tx = r.x + 8, tw = r.w - 16;
        const int a = g_ui_drag_arg;
        const int fsr = g_ui_drag == H_FSR_SHARP;
        const int lo = fsr ? 0 : pic_rows[a].min, hi = fsr ? 100 : pic_rows[a].max, st = fsr ? 1 : pic_rows[a].step;
        float f = (mx - tx) / tw;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        int v = lo + (int)lrintf(f * (hi - lo) / st) * st;
        if (fsr) g_fsr_sharpness = v;
        else *pic_field(a) = v;
        return;
    }
}

/* 1 = the event was the UI's */
static int ui_event(SDL_Window *win, SDL_Renderer *ren, const SDL_Event *e)
{
    if (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEWHEEL) {
        int wx, wy;
        SDL_GetMouseState(&wx, &wy);
        float lx = (float)wx, ly = (float)wy;
        SDL_RenderWindowToLogical(ren, wx, wy, &lx, &ly);
        g_ui_mx = lx / g_ui_k; g_ui_my = ly / g_ui_k;
    }
    if (e->type == SDL_MOUSEMOTION) {
        g_ui_mouse_t = SDL_GetPerformanceCounter();
        g_ui_kbd = 0;
        if (g_ui_drag == H_PIC || g_ui_drag == H_FSR_SHARP) { ui_slide(g_ui_mx); prefs_save(); }
        return g_menu;                      /* the home screen redraws for the hover */
    }
    if (e->type == SDL_MOUSEBUTTONUP) { if (g_ui_drag) { g_ui_drag = 0; return 1; } return 0; }
    if (e->type == SDL_MOUSEWHEEL && g_ui_open) { g_ui_scroll -= e->wheel.y * 40; return 1; }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        for (int i = g_ui_nhits - 1; i >= 0; i--) {
            const ui_hit_t *h = &g_ui_hits[i];
            if (!(g_ui_mx >= h->r.x && g_ui_mx < h->r.x + h->r.w && g_ui_my >= h->r.y && g_ui_my < h->r.y + h->r.h)) continue;
            switch (h->id) {
            case H_SETTINGS: ui_open(1); break;
            case H_CLOSE: case H_BACKDROP: ui_open(0); break;
            case H_PANEL: break;
            case H_CONN: act_conn(h->arg); break;
            case H_IP: act_ip(); ui_open(0); break;
            case H_FULL: act_fullscreen(win); break;
            case H_VSYNC: act_vsync(ren, h->arg); break;
            case H_HWDEC: act_hwdec(h->arg); break;
            case H_UPS: act_upscaler(ren, h->arg); break;
            case H_HUD: act_hud(h->arg); break;
            case H_PIC: g_ui_drag = H_PIC; g_ui_drag_arg = h->arg; ui_slide(g_ui_mx); prefs_save(); break;
            case H_FSR_SHARP: g_ui_drag = H_FSR_SHARP; g_ui_drag_arg = 0; ui_slide(g_ui_mx); prefs_save(); break;
            case H_PIC_RESET: g_pic = pic_default; prefs_save(); break;
            case H_SOUND: act_mute(!h->arg); break;
            case H_REC: act_record(win); break;
            case H_SUPPORT: SDL_OpenURL("https://buymeacoffee.com/papux200"); break;
            }
            return 1;
        }
        return g_ui_open;                   /* with the drawer open, nothing else takes clicks */
    }
    if (e->type != SDL_KEYDOWN) return 0;
    const SDL_Keycode k = e->key.keysym.sym;
    if (k == SDLK_ESCAPE) { ui_open(!g_ui_open); return 1; }
    if (k == SDLK_p && !e->key.repeat) {    /* the picture controls */
        if (g_ui_open && g_ui_kbd && g_ui_focus < g_ui_nrows && g_ui_rows[g_ui_focus].id == H_PIC) { ui_open(0); return 1; }
        ui_open(1);
        g_ui_kbd = 1;
        for (int i = 0; i < g_ui_nrows; i++) if (g_ui_rows[i].id == H_PIC) { g_ui_focus = i; g_ui_scroll_to = i + PIC_N - 1; break; }
        if (!g_ui_nrows) { g_ui_focus = 0; }
        return 1;
    }
    if (!g_ui_open) return 0;
    if (g_ui_nrows == 0) return 1;
    if (g_ui_focus >= g_ui_nrows) g_ui_focus = g_ui_nrows - 1;
    switch (k) {
    case SDLK_UP: if (g_ui_kbd) g_ui_focus = (g_ui_focus + g_ui_nrows - 1) % g_ui_nrows; g_ui_kbd = 1; g_ui_scroll_to = g_ui_focus; return 1;
    case SDLK_DOWN: if (g_ui_kbd) g_ui_focus = (g_ui_focus + 1) % g_ui_nrows; g_ui_kbd = 1; g_ui_scroll_to = g_ui_focus; return 1;
    case SDLK_LEFT: case SDLK_RIGHT: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (!g_ui_kbd) { g_ui_kbd = 1; return 1; }
        ui_change(win, ren, g_ui_rows[g_ui_focus].id, g_ui_rows[g_ui_focus].arg,
                  k == SDLK_LEFT ? -1 : k == SDLK_RIGHT ? 1 : 0);
        return 1;
    case SDLK_BACKSPACE: case SDLK_DELETE:
        if (g_ui_kbd && g_ui_rows[g_ui_focus].id == H_PIC) ui_change(win, ren, H_PIC, g_ui_rows[g_ui_focus].arg, 0);
        if (g_ui_kbd && g_ui_rows[g_ui_focus].id == H_FSR_SHARP) ui_change(win, ren, H_FSR_SHARP, 0, 0);
        return 1;
    case SDLK_0: case SDLK_KP_0: g_pic = pic_default; prefs_save(); return 1;
    default: return 0;                      /* the shortcuts still work with the drawer open */
    }
}
