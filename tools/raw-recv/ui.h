/* ui.h - v0.7.12: the viewer's look. One home screen while there is no
 * picture, and one settings drawer (Esc, or the Settings button) with every
 * option in it, over the home screen or over the game - mouse or keys. The
 * old shortcuts (R, M, F11, Tab, I, O, V, A, B, U) still work; P opens the drawer at
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
static float g_home_scroll = 0, g_home_content_h = 0;
static float g_ui_mx = -1, g_ui_my = -1;   /* the mouse, in UI units (logical / g_ui_k) */
static float g_ui_wx = -1, g_ui_wy = -1;   /* the mouse, in window pixels (for fixed chrome) */
static Uint64 g_ui_mouse_t = 0;            /* last pointer activity inside the window */
static int g_ui_ptr_inside = 0;            /* mouse is over the window: show hover chrome */
static int g_ui_drag = 0, g_ui_drag_arg = 0;
static float g_ui_k = 1, g_ui_w = 1280;    /* units -> logical, and the width in units, at the last draw */
static int g_ui_scroll_to = -1;            /* bring this row into view at the next draw */
/* Active scrollbar geometry (for thumb dragging without a mouse wheel). */
static float g_sb_top = 0, g_sb_h = 0, g_sb_content = 0, g_sb_view = 0;
static float *g_sb_scroll_ptr = NULL;
static SDL_Window *g_ui_win = NULL;        /* for the fullscreen switch's state (app_events sets it) */
static int g_ui_hit_window = 0;            /* next ui_hit() is in window pixels (fixed chrome) */
static int g_ui_popout_map = 0;            /* ui_in: map window mouse → design-space card */
static float g_ui_popout_x0 = 0, g_ui_popout_y0 = 0, g_ui_popout_scale = 1;
/* Settings density: desktop 1.0 / tablet 0.85 / mobile 0.70. Applied by redrawing
 * at native pixels (smaller font px + control sizes) - stays sharp, never blit-scaled. */
static float g_ui_d = 1.f;

static float ui_d(float v) { return v * g_ui_d; }
static int ui_px(int px)
{
    int v = (int)((float)px * g_ui_d + 0.5f);
    return v < 11 ? 11 : v; /* floor: still readable on mobile */
}
static void ui_open(int open);

enum { H_NONE, H_SETTINGS, H_CLOSE, H_BACKDROP, H_CONN, H_IP, H_FULL, H_VSYNC, H_ASPECT, H_CHROMELESS, H_UPS, H_HUD, H_HWDEC,
       H_PIC, H_PIC_RESET, H_SOUND, H_REC, H_PANEL, H_FSR_SHARP, H_SUPPORT, H_SCROLL,
       H_WIN_MIN, H_WIN_MAX, H_WIN_CLOSE, H_UPD, H_UPD_BETA, H_UPD_AUTO, H_PIN, H_REC1 };

typedef struct { SDL_FRect r; int id, arg, window_space; } ui_hit_t;
static ui_hit_t g_ui_hits[96];
static int g_ui_nhits;
/* the drawer's rows in order, for the keys: (id, arg) */
static struct { int id, arg; float y, h; } g_ui_rows[24];
static int g_ui_nrows;

static void ui_hit(float x, float y, float w, float h, int id, int arg)
{
    if (g_ui_nhits < (int)(sizeof(g_ui_hits) / sizeof(g_ui_hits[0])))
        g_ui_hits[g_ui_nhits++] = (ui_hit_t){ { x, y, w, h }, id, arg, g_ui_hit_window };
}

static int ui_in(float x, float y, float w, float h)
{
    float mx = g_ui_mx, my = g_ui_my;
    if (g_ui_hit_window) {
        if (g_ui_popout_map && g_ui_popout_scale > 0.f) {
            mx = (g_ui_wx - g_ui_popout_x0) / g_ui_popout_scale;
            my = (g_ui_wy - g_ui_popout_y0) / g_ui_popout_scale;
        } else {
            mx = g_ui_wx;
            my = g_ui_wy;
        }
    }
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

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
    const float w = ui_d(44), h = ui_d(24), x = xr - w;
    const float knob = ui_d(18), inset = ui_d(3);
    SDL_Color track = on ? UI_ACC : UI_SURF3;
    if (!enabled) track = ui_alpha(track, 90);
    ui_rrect(ren, x, y, w, h, h / 2, track);
    ui_rrect(ren, on ? x + w - knob - inset : x + inset, y + inset, knob, knob, knob / 2, enabled ? UI_INK : UI_DIM);
    if (enabled) ui_hit(x - 4, y - 6, w + 8, h + 12, id, !on);
}

/* segmented buttons, right edge at xr (or centred at cx when xr < 0).
 * maxw > 0 clamps the control so it never overlaps neighbouring content. */
static float ui_seg(SDL_Renderer *ren, float xr, float cx, float y, const char *const *opt, int n, int sel, int id, int px, float maxw)
{
    px = ui_px(px);
    float h = ui_d(32), pad = ui_d(16);
    float wsum = 0, wi[8];
    for (int i = 0; i < n && i < 8; i++) { wi[i] = text_width(opt[i], px, 1) + 2 * pad; wsum += wi[i]; }
    while (maxw > 0 && wsum + 6 > maxw && pad > 6.f) {
        pad -= 2.f;
        wsum = 0;
        for (int i = 0; i < n && i < 8; i++) { wi[i] = text_width(opt[i], px, 1) + 2 * pad; wsum += wi[i]; }
    }
    while (maxw > 0 && wsum + 6 > maxw && px > 11) {
        px -= 1;
        wsum = 0;
        for (int i = 0; i < n && i < 8; i++) { wi[i] = text_width(opt[i], px, 1) + 2 * pad; wsum += wi[i]; }
    }
    float x = xr >= 0 ? xr - wsum - 6 : cx - (wsum + 6) / 2;
    ui_rrect(ren, x, y, wsum + 6, h, ui_d(9), UI_SURF2);
    float sx = x + 3;
    for (int i = 0; i < n && i < 8; i++) {
        const int hov = ui_in(sx, y, wi[i], h);
        if (i == sel) ui_rrect(ren, sx, y + 3, wi[i], h - 6, ui_d(7), UI_ACC);
        else if (hov) ui_rrect(ren, sx, y + 3, wi[i], h - 6, ui_d(7), UI_SURF3);
        text_draw(ren, opt[i], (int)(sx + wi[i] / 2), (int)(y + (h - px * 1.18f) / 2), px, 1, i == sel ? (SDL_Color){ 8, 20, 30, 255 } : UI_INK, 1);
        ui_hit(sx, y, wi[i], h, id, i);
        sx += wi[i];
    }
    return wsum + 6;
}

/* Vertical scrollbar when content overflows the view (mouse-wheel optional). */
static void ui_scrollbar(SDL_Renderer *ren, float track_x, float top, float view_h, float content_h, float *scroll)
{
    if (!scroll || content_h <= view_h + 1.f) { g_sb_scroll_ptr = NULL; return; }
    float maxs = content_h - view_h;
    if (*scroll < 0) *scroll = 0;
    if (*scroll > maxs) *scroll = maxs;
    const float tw = 8.f;
    float thumb_h = view_h * view_h / content_h;
    if (thumb_h < 28.f) thumb_h = 28.f;
    if (thumb_h > view_h) thumb_h = view_h;
    float thumb_y = top + (maxs > 0.f ? (*scroll / maxs) * (view_h - thumb_h) : 0.f);
    ui_rrect(ren, track_x, top, tw, view_h, 4.f, UI_SURF3);
    /* Wide hit pad so chromeless panel-drag does not steal the grab. */
    const int hov = ui_in(track_x - 10, top, tw + 20, view_h);
    ui_rrect(ren, track_x, thumb_y, tw, thumb_h, 4.f, hov || g_ui_drag == H_SCROLL ? UI_INK : UI_DIM);
    g_sb_top = top;
    g_sb_h = view_h;
    g_sb_content = content_h;
    g_sb_view = view_h;
    g_sb_scroll_ptr = scroll;
    ui_hit(track_x - 10, top, tw + 20, view_h, H_SCROLL, 0);
}

static void ui_scroll_drag(float my)
{
    if (!g_sb_scroll_ptr || g_sb_content <= g_sb_view + 1.f) return;
    float maxs = g_sb_content - g_sb_view;
    float thumb_h = g_sb_view * g_sb_view / g_sb_content;
    if (thumb_h < 28.f) thumb_h = 28.f;
    if (thumb_h > g_sb_h) thumb_h = g_sb_h;
    float span = g_sb_h - thumb_h;
    float t = span > 1.f ? (my - g_sb_top - thumb_h * 0.5f) / span : 0.f;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    *g_sb_scroll_ptr = t * maxs;
}

/* a pill button, right edge at xr (or centred at cx when xr < 0); returns its width */
static float ui_button(SDL_Renderer *ren, float xr, float cx, float y, const char *label, SDL_Color fill, SDL_Color ink, int id, int arg, int enabled)
{
    const int px = ui_px(15);
    const float w = text_width(label, px, 1) + ui_d(32), h = ui_d(32);
    const float x = xr >= 0 ? xr - w : cx - w / 2;
    SDL_Color f = fill;
    if (enabled && ui_in(x, y, w, h)) { f.r = (Uint8)(f.r + (255 - f.r) / 8); f.g = (Uint8)(f.g + (255 - f.g) / 8); f.b = (Uint8)(f.b + (255 - f.b) / 8); }
    ui_rrect(ren, x, y, w, h, h / 2, enabled ? f : ui_alpha(f, 80));
    text_draw(ren, label, (int)(x + w / 2), (int)(y + ui_d(7)), px, 1, enabled ? ink : UI_FAINT, 1);
    if (enabled) ui_hit(x, y, w, h, id, arg);
    return w;
}

/* Chrome density from window width - same tiers as settings (sharp native redraw). */
static float ui_chrome_dens(int ow)
{
    if (ow >= 1100) return 1.00f;
    if (ow >= 800) return 0.85f;
    return 0.70f;
}

/* Expanded = desktop-fullscreen (F11 / middle / double-click) or maximized. */
static int ui_win_expanded(void)
{
    if (!g_ui_win) return 0;
    const Uint32 fl = SDL_GetWindowFlags(g_ui_win);
    return (fl & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)) != 0;
}

/* Chrome hit pad: visual button is d×d; hits are a few px larger so clicks land. */
static void ui_chrome_hit(float x, float y, float d, int id)
{
    const float pad = d * 0.14f;
    ui_hit(x - pad, y - pad, d + pad * 2, d + pad * 2, id, 0);
}

/* the settings button: a gear in a round button, "Settings" beside it on hover */
static void ui_gear_button(SDL_Renderer *ren, float xr, float y, float d, SDL_Color bg, int id)
{
    const float x = xr - d, inset = d * 0.19f;
    const int hov = ui_in(x, y, d, d);
    SDL_Color b = bg;
    if (hov) { b = UI_SURF3; b.a = bg.a > 230 ? bg.a : 230; }
    ui_rrect(ren, x, y, d, d, d / 2, b);
    ui_gear_init(ren);
    if (g_ui_gear) {
        SDL_SetTextureColorMod(g_ui_gear, UI_INK.r, UI_INK.g, UI_INK.b);
        SDL_SetTextureAlphaMod(g_ui_gear, 255);
        const SDL_FRect g = { x + inset, y + inset, d - inset * 2, d - inset * 2 };
        SDL_RenderCopyF(ren, g_ui_gear, NULL, &g);
    }
    if (hov) {
        const int px = d >= 38.f ? 14 : d >= 32.f ? 13 : 12;
        const float tw = text_width("Settings", px, 1) + d * 0.48f;
        const float th = d * 0.62f, ty = y + (d - th) * 0.5f;
        ui_rrect(ren, x - tw - d * 0.19f, ty, tw, th, th * 0.5f, (SDL_Color){ 21, 24, 30, 230 });
        text_draw(ren, "Settings", (int)(x - d * 0.19f - tw * 0.5f), (int)(ty + th * 0.18f), px, 1, UI_INK, 1);
    }
    ui_chrome_hit(x, y, d, id);
}

/* Borderless window controls (min / max / close), drawn left of the gear. */
static void ui_win_btn(SDL_Renderer *ren, float x, float y, float d, int id, int danger)
{
    const int hov = ui_in(x, y, d, d);
    SDL_Color b = (SDL_Color){ 21, 24, 30, 200 };
    if (hov) b = danger ? (SDL_Color){ 185, 28, 28, 230 } : UI_SURF3;
    ui_rrect(ren, x, y, d, d, d / 2, b);
    SDL_SetRenderDrawColor(ren, UI_INK.r, UI_INK.g, UI_INK.b, 255);
    const float cx = x + d * 0.5f, cy = y + d * 0.5f;
    const float arm = d * 0.17f, box = d * 0.33f;
    if (id == H_WIN_MIN) {
        SDL_RenderDrawLineF(ren, cx - arm, cy, cx + arm, cy);
        SDL_RenderDrawLineF(ren, cx - arm, cy + 1, cx + arm, cy + 1);
    } else if (id == H_WIN_MAX) {
        /* Restore glyph when fullscreen (F11/middle/double) or maximized. */
        if (ui_win_expanded()) {
            const float s = box * 0.78f, gap = box * 0.22f;
            SDL_FRect back = { cx - s * 0.35f, cy - s * 0.65f, s, s };
            SDL_FRect front = { cx - s * 0.65f, cy - s * 0.35f, s, s };
            (void)gap;
            SDL_RenderDrawRectF(ren, &back);
            SDL_SetRenderDrawColor(ren, b.r, b.g, b.b, b.a);
            SDL_RenderFillRectF(ren, &front);
            SDL_SetRenderDrawColor(ren, UI_INK.r, UI_INK.g, UI_INK.b, 255);
            SDL_RenderDrawRectF(ren, &front);
        } else {
            SDL_FRect r = { cx - box * 0.5f, cy - box * 0.5f, box, box };
            SDL_RenderDrawRectF(ren, &r);
        }
    } else { /* close */
        SDL_RenderDrawLineF(ren, cx - arm, cy - arm, cx + arm, cy + arm);
        SDL_RenderDrawLineF(ren, cx - arm, cy - arm + 1, cx + arm - 1, cy + arm);
        SDL_RenderDrawLineF(ren, cx + arm, cy - arm, cx - arm, cy + arm);
        SDL_RenderDrawLineF(ren, cx + arm - 1, cy - arm, cx - arm, cy + arm - 1);
    }
    ui_chrome_hit(x, y, d, id);
}

/* Pin / always-on-top: sits right of the gear, left of min/max/close when borderless.
 * Status is the bottom toast only - no hover label (it sat under the gear). */
static void ui_pin_button(SDL_Renderer *ren, float x, float y, float d)
{
    const int hov = ui_in(x, y, d, d);
    const int on = g_always_on_top;
    SDL_Color b = (SDL_Color){ 21, 24, 30, 200 };
    if (on) b = (SDL_Color){ UI_ACC.r, UI_ACC.g, UI_ACC.b, 220 };
    else if (hov) { b = UI_SURF3; b.a = 230; }
    ui_rrect(ren, x, y, d, d, d / 2, b);
    ui_pin_init(ren);
    if (g_ui_pin) {
        if (on) SDL_SetTextureColorMod(g_ui_pin, 12, 14, 18);
        else SDL_SetTextureColorMod(g_ui_pin, UI_INK.r, UI_INK.g, UI_INK.b);
        SDL_SetTextureAlphaMod(g_ui_pin, 255);
        const float inset = d * 0.18f;
        const SDL_FRect g = { x + inset, y + inset, d - inset * 2, d - inset * 2 };
        SDL_RenderCopyF(ren, g_ui_pin, NULL, &g);
    }
    ui_chrome_hit(x, y, d, H_PIN);
}

/* Layout of top-right chrome in window/output pixels. Returns button count.
 * Right→left: [close max min] pin gear  (window controls only when borderless). */
static int ui_chrome_layout(int ow, float *xs, int *ids, float *out_d, float *out_y)
{
    const float dens = ui_chrome_dens(ow);
    const float d = 42.f * dens, gap = 8.f * dens, y = 10.f * dens, margin = 10.f * dens;
    float right = (float)ow - margin;
    int n = 0;
    /* Keep min/max/close in fullscreen too - max shows restore and matches F11. */
    if (g_chromeless) {
        ids[n] = H_WIN_CLOSE; xs[n] = right - d; n++; right -= d + gap;
        ids[n] = H_WIN_MAX;   xs[n] = right - d; n++; right -= d + gap;
        ids[n] = H_WIN_MIN;   xs[n] = right - d; n++; right -= d + gap;
    }
    ids[n] = H_PIN;      xs[n] = right - d; n++; right -= d + gap;
    ids[n] = H_SETTINGS; xs[n] = right - d; n++;
    *out_d = d; *out_y = y;
    return n;
}

/* Hit-test chrome from geometry alone (does not depend on the last paint). */
static int ui_chrome_at(SDL_Renderer *ren, float wx, float wy, int *out_id)
{
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow < 80 || oh < 40) return 0;
    float xs[5], d = 42.f, y = 10.f;
    int ids[5];
    const int n = ui_chrome_layout(ow, xs, ids, &d, &y);
    const float pad = d * 0.14f;
    for (int i = 0; i < n; i++) {
        if (wx >= xs[i] - pad && wx < xs[i] + d + pad &&
            wy >= y - pad && wy < y + d + pad) {
            *out_id = ids[i];
            return 1;
        }
    }
    return 0;
}

static void ui_chrome_activate(SDL_Window *win, int id)
{
    switch (id) {
    case H_SETTINGS: ui_open(1); break;
    case H_PIN: act_always_on_top(win, !g_always_on_top); break;
    case H_WIN_MIN: if (win) SDL_MinimizeWindow(win); break;
    case H_WIN_MAX:
        /* Same expand/restore path as F11, double-click, and middle-click. */
        if (win) {
            const Uint32 fl = SDL_GetWindowFlags(win);
            if (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) act_fullscreen(win);
            else if (fl & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win);
            else act_fullscreen(win);
        }
        break;
    case H_WIN_CLOSE: g_quit = 1; break;
    default: break;
    }
}

static void ui_top_chrome(SDL_Renderer *ren, float xr, float y, SDL_Color gear_bg, int paint)
{
    (void)xr; (void)y;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    float xs[5], d = 42.f, cy = 10.f;
    int ids[5];
    const int n = ui_chrome_layout(ow, xs, ids, &d, &cy);
    for (int i = 0; i < n; i++) {
        if (ids[i] == H_SETTINGS) {
            if (paint) ui_gear_button(ren, xs[i] + d, cy, d, gear_bg, H_SETTINGS);
            else ui_chrome_hit(xs[i], cy, d, H_SETTINGS);
        } else if (ids[i] == H_PIN) {
            if (paint) ui_pin_button(ren, xs[i], cy, d);
            else ui_chrome_hit(xs[i], cy, d, H_PIN);
        } else {
            if (paint) ui_win_btn(ren, xs[i], cy, d, ids[i], ids[i] == H_WIN_CLOSE);
            else ui_chrome_hit(xs[i], cy, d, ids[i]);
        }
    }
}

/* Settings / window controls in real window pixels so they don't grow with resize.
 * paint=0 still registers hit targets so clicks stay reliable between hover frames. */
static void ui_top_chrome_fixed(SDL_Renderer *ren, SDL_Color gear_bg, int paint)
{
    int ow = 0, oh = 0, lw = 0, lh = 0;
    float sx = 1, sy = 1;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow < 80 || oh < 40) return;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1, 1);
    g_ui_hit_window = 1;
    ui_top_chrome(ren, (float)ow - 12.0f, 12.0f, gear_bg, paint);
    g_ui_hit_window = 0;
    if (lw > 0 && lh > 0) SDL_RenderSetLogicalSize(ren, lw, lh);
    SDL_RenderSetScale(ren, sx, sy);
}

/* a small key hint, like a key cap */
static float ui_key(SDL_Renderer *ren, float x, float y, const char *k)
{
    const int px = ui_px(13);
    const float w = text_width(k, px, 1) + ui_d(14);
    ui_rrect(ren, x, y, w, ui_d(22), ui_d(6), UI_SURF3);
    text_draw(ren, k, (int)(x + w / 2), (int)(y + ui_d(3)), px, 1, UI_INK, 1);
    return w;
}

/* ---- the settings drawer ---------------------------------------------- */

static void ui_section(SDL_Renderer *ren, float x, float *y, const char *name)
{
    *y += ui_d(14);
    text_draw(ren, name, (int)x, (int)*y, ui_px(13), 1, UI_FAINT, 0);
    *y += ui_d(26);
}

/* a row's label (and a smaller line under it); the control goes on the right.
 * Titles are width-clipped so a narrow column cannot bleed into its neighbour. */
static void ui_label(SDL_Renderer *ren, float x, float y, float h, const char *label, const char *sub, int focus, float w)
{
    if (focus) ui_rrect(ren, x - ui_d(12), y - 2, w + ui_d(24), h + 4, ui_d(10), UI_SURF2);
    const int tw = (int)(w - ui_d(56)); /* leave room for the toggle / button on the right */
    if (tw < 40) return;
    if (sub && sub[0]) {
        ui_text_fit(ren, label, (int)x, (int)(y + h / 2 - ui_d(20)), ui_px(17), 0, UI_INK, 0, tw);
        ui_text_fit(ren, sub, (int)x, (int)(y + h / 2 + ui_d(2)), ui_px(13), 0, UI_DIM, 0, tw);
    } else {
        ui_text_fit(ren, label, (int)x, (int)(y + h / 2 - ui_d(11)), ui_px(17), 0, UI_INK, 0, tw);
    }
}

static const char *const ui_hud_names[HUD_MODES] = { "Off", "FPS", "Graphs", "Full" };

/* Settings card at a fixed pixel size (x0,y0,pw,ph). Caller draws the veil.
 * Two columns only when the card is wide enough; narrower cards stay one column
 * so controls never collide. Header/footer shrink on short cards so the scroll
 * viewport never goes invalid (that was the "mush together" failure mode).
 * Layout/fonts use g_ui_d (desktop/tablet/mobile) so smaller tiers stay sharp. */
static void ui_drawer(SDL_Renderer *ren, float x0, float y0, float pw, float ph)
{
    const float min_col = ui_d(280), two_col_w = ui_d(720);
    const int cols = pw >= two_col_w ? 2 : 1;
    const float pad = pw >= ui_d(500) ? ui_d(26) : ui_d(16), gap = ui_d(22);
    float col_w = cols == 2 ? (pw - pad * 2 - gap) * 0.5f : pw - 2 * pad;
    if (col_w < ui_d(120)) col_w = pw - 2 * pad;
    const int use_cols = (cols == 2 && col_w >= min_col) ? 2 : 1;
    if (use_cols == 1) col_w = pw - 2 * pad;
    const float cx_l = x0 + pad, xr_l = cx_l + col_w;
    const float cx_r = use_cols == 2 ? cx_l + col_w + gap : cx_l;
    const float xr_r = cx_r + col_w;
    const float cx = cx_l, xr = x0 + pw - pad;

    ui_hit(x0, y0, pw - 22.f, ph, H_PANEL, 0); /* leave scrollbar strip out of panel-drag */
    ui_rrect(ren, x0, y0, pw, ph, ui_d(16), (SDL_Color){ UI_SURF.r, UI_SURF.g, UI_SURF.b, 250 });
    SDL_SetRenderDrawColor(ren, UI_LINE.r, UI_LINE.g, UI_LINE.b, UI_LINE.a);
    SDL_FRect edge = { x0, y0, 1, ph };
    SDL_RenderFillRectF(ren, &edge);

    /* Chrome sizes: reclaim vertical space before the scroll clip breaks. */
    const int show_footer = ph >= ui_d(360);
    float head_h = ph >= ui_d(280) ? ui_d(76) : ui_d(52);
    float foot_h = show_footer ? ui_d(44) : ui_d(6);
    float top = y0 + head_h, bottom = y0 + ph - foot_h;
    if (bottom < top + ui_d(120)) { head_h = ui_d(48); foot_h = ui_d(4); top = y0 + head_h; bottom = y0 + ph - foot_h; }
    if (bottom <= top + ui_d(48)) {
        text_draw(ren, "Settings", (int)cx, (int)(y0 + ui_d(16)), ui_px(20), 1, UI_INK, 0);
        ui_button(ren, xr, 0, y0 + ui_d(14), "Close", UI_SURF2, UI_INK, H_CLOSE, 0, 1);
        text_draw(ren, "Resize the window larger to use settings", (int)(x0 + pw * 0.5f), (int)(y0 + ph * 0.5f), ui_px(15), 0, UI_DIM, 1);
        return;
    }

    if (g_ui_scroll_to >= 0 && g_ui_scroll_to < g_ui_nrows) {
        const float ry = g_ui_rows[g_ui_scroll_to].y + g_ui_scroll, rh = g_ui_rows[g_ui_scroll_to].h;
        if (ry - 40 < top) g_ui_scroll -= top - (ry - 40);
        if (ry + rh + 10 > bottom) g_ui_scroll += ry + rh + 10 - bottom;
    }
    g_ui_scroll_to = -1;
    const float view_h = bottom - top;
    const float maxs = g_ui_content_h - view_h;
    if (g_ui_scroll > maxs) g_ui_scroll = maxs;
    if (g_ui_scroll < 0) g_ui_scroll = 0;
    SDL_Rect clip_units = { (int)x0, (int)top, (int)pw + 1, (int)view_h };
    SDL_RenderSetClipRect(ren, &clip_units);
    const int first_hit = g_ui_nhits;
    g_ui_nrows = 0;
    float y_l = top - g_ui_scroll, y_r = top - g_ui_scroll;
    const float y_start = y_l;

    /* ---- left column (or full width): connection + display ---- */
    {
        float y = y_l, cxi = cx_l, cwi = col_w, xri = xr_l;
        ui_section(ren, cxi, &y, "CONNECTION");
        {
            static const char *const conn[] = { "USB", "Network" };
            const float h = ui_d(44), gap_y = ui_d(6);
            ui_row(H_CONN, 0, y, h);
            ui_label(ren, cxi, y, h, "Connection", NULL, ui_focused(H_CONN, 0), cwi);
            ui_seg(ren, xri, 0, y + gap_y, conn, 2, g_conn == CONN_NET, H_CONN, 14, cwi * 0.55f);
            y += h + gap_y;
            if (g_conn == CONN_NET) {
                char sub[96];
                if (g_sock != SOCK_BAD && net_peer_for_menu()[0]) snprintf(sub, sizeof(sub), "Connected to %s", net_peer_for_menu());
                else snprintf(sub, sizeof(sub), "%s", g_last_ip[0] ? g_last_ip : "Found by itself on the network");
                ui_row(H_IP, 0, y, h);
                ui_label(ren, cxi, y, h, "Switch address", sub, ui_focused(H_IP, 0), cwi);
                ui_button(ren, xri, 0, y + gap_y, "Type IP", UI_SURF3, UI_INK, H_IP, 0, act_ip_possible());
                y += h + gap_y;
            }
        }

        ui_section(ren, cxi, &y, "DISPLAY");
        {
            const float h = ui_d(48), gap_y = ui_d(4), tog_y = ui_d(12);
            ui_row(H_FULL, 0, y, h);
            ui_label(ren, cxi, y, h, "Fullscreen", "F11, double-click, or middle-click", ui_focused(H_FULL, 0), cwi);
            ui_toggle(ren, xri, y + tog_y, g_ui_win && (SDL_GetWindowFlags(g_ui_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0, H_FULL, 1);
            y += h + gap_y;
            ui_row(H_VSYNC, 0, y, h);
            ui_label(ren, cxi, y, h, "Vsync", g_vsync ? "Smooth, no tearing" : "Off: lowest latency, may tear", ui_focused(H_VSYNC, 0), cwi);
            ui_toggle(ren, xri, y + tog_y, g_vsync, H_VSYNC, 1);
            y += h + gap_y;
            ui_row(H_ASPECT, 0, y, h);
            ui_label(ren, cxi, y, h, "Lock aspect ratio", g_lock_aspect ? "Window stays 16:9 while resizing (A)" : "Free resize; picture letterboxed (A)", ui_focused(H_ASPECT, 0), cwi);
            ui_toggle(ren, xri, y + tog_y, g_lock_aspect, H_ASPECT, 1);
            y += h + gap_y;
            ui_row(H_CHROMELESS, 0, y, h);
            ui_label(ren, cxi, y, h, "Borderless window", g_chromeless ? "No title bar; drag the picture to move (B)" : "Normal window frame (B)", ui_focused(H_CHROMELESS, 0), cwi);
            ui_toggle(ren, xri, y + tog_y, g_chromeless, H_CHROMELESS, 1);
            y += h + gap_y;
            ui_row(H_HWDEC, 0, y, h);
            char dec[64];
            snprintf(dec, sizeof(dec), "Now: %s", g_dec_mode && g_dec_mode[0] ? g_dec_mode : (g_hwdec ? "the graphics card" : "the CPU"));
#if defined(__APPLE__)
            ui_label(ren, cxi, y, h, "Hardware decoding (beta)", dec, ui_focused(H_HWDEC, 0), cwi);
#else
            ui_label(ren, cxi, y, h, "Hardware decoding", dec, ui_focused(H_HWDEC, 0), cwi);
#endif
            ui_toggle(ren, xri, y + tog_y, g_hwdec, H_HWDEC, 1);
            y += h + gap_y;
            /* Dedicated blurb under the title for every mode (not the HUD "-" fallback). */
            char why[120];
            if (g_upscaler == UP_OFF)
                snprintf(why, sizeof(why), "No upscaling (U)");
            else if (g_upscaler == UP_FSR)
                snprintf(why, sizeof(why), "AMD FidelityFX Super Resolution (U)");
            else if (g_upscaler == UP_NIS)
                snprintf(why, sizeof(why), "NVIDIA Image Scaling (U)");
#ifdef _WIN32
            else if (g_upscaler == UP_RTX)
                snprintf(why, sizeof(why), "NVIDIA RTX Video Super Resolution (U)");
#endif
            else
                snprintf(why, sizeof(why), "%s (U)", up_name[g_upscaler]);
            /* When live, prefer the real status (GPU / output size / restart note). */
            if (g_upscaler != UP_OFF) {
                char live[120];
                up_describe(ren, live, sizeof(live));
                if (live[0] && strcmp(live, "-") != 0) snprintf(why, sizeof(why), "%s", live);
            }
            const float h_ups = ui_d(94);
            ui_row(H_UPS, 0, y, h_ups);
            if (ui_focused(H_UPS, 0)) ui_rrect(ren, cxi - ui_d(12), y - 2, cwi + ui_d(24), h_ups + 4, ui_d(10), UI_SURF2);
            text_draw(ren, "Upscaling", (int)cxi, (int)(y + ui_d(2)), ui_px(17), 0, UI_INK, 0);
            ui_text_fit(ren, why, (int)cxi, (int)(y + ui_d(24)), ui_px(13), 0, UI_DIM, 0, (int)cwi);
            static const char *const ups[] = { "Off", "FSR 1", "NIS", "RTX VSR" };
            ui_seg(ren, xri, 0, y + ui_d(48), ups, UP_COUNT, g_upscaler, H_UPS, 13, cwi);
            y += h_ups + gap_y;
#ifdef __APPLE__
            if (g_upscaler == UP_FSR) {
                const float sh = ui_d(52), ty = y + ui_d(34);
                ui_row(H_FSR_SHARP, 0, y, sh);
                if (ui_focused(H_FSR_SHARP, 0)) ui_rrect(ren, cxi - ui_d(12), y - 2, cwi + ui_d(24), sh + 4, ui_d(10), UI_SURF2);
                text_draw(ren, "FSR sharpness", (int)cxi, (int)(y + ui_d(4)), ui_px(16), 0, UI_INK, 0);
                char value[16]; snprintf(value, sizeof(value), "%d%%", g_fsr_sharpness);
                text_draw(ren, value, (int)xri, (int)(y + ui_d(4)), ui_px(15), 1, UI_ACC, 2);
                const float vx = cxi + cwi * g_fsr_sharpness / 100.0f;
                ui_rrect(ren, cxi, ty - 2, cwi, 4, 2, UI_SURF3);
                ui_rrect(ren, cxi, ty - 2, vx - cxi, 4, 2, UI_ACC);
                ui_rrect(ren, vx - ui_d(8), ty - ui_d(8), ui_d(16), ui_d(16), ui_d(8), UI_INK);
                ui_hit(cxi - ui_d(8), ty - ui_d(14), cwi + ui_d(16), ui_d(28), H_FSR_SHARP, 0);
                y += sh + gap_y;
            }
#endif
            const float h_hud = ui_d(74);
            ui_row(H_HUD, 0, y, h_hud);
            if (ui_focused(H_HUD, 0)) ui_rrect(ren, cxi - ui_d(12), y - 2, cwi + ui_d(24), h_hud + 4, ui_d(10), UI_SURF2);
            text_draw(ren, "Stats overlay", (int)cxi, (int)(y + ui_d(4)), ui_px(17), 0, UI_INK, 0);
            text_draw(ren, "O or F3", (int)xri, (int)(y + ui_d(7)), ui_px(13), 0, UI_DIM, 2);
            ui_seg(ren, xri, 0, y + ui_d(32), ui_hud_names, HUD_MODES, g_hud, H_HUD, 13, cwi);
            y += h_hud + gap_y;
        }
        y_l = y;
    }

    /* ---- right column when wide; else continue below left ---- */
    {
        float y = use_cols == 2 ? y_r : y_l;
        float cxi = use_cols == 2 ? cx_r : cx_l, cwi = col_w, xri = use_cols == 2 ? xr_r : xr_l;

        ui_section(ren, cxi, &y, "PICTURE");
        if (!pic_neutral()) ui_button(ren, xri, 0, y - ui_d(36), "Reset", UI_SURF3, UI_INK, H_PIC_RESET, 0, 1);
        for (int r = 0; r < PIC_N; r++) {
            const float h = ui_d(52);
            ui_row(H_PIC, r, y, h);
            if (ui_focused(H_PIC, r)) ui_rrect(ren, cxi - ui_d(12), y - 2, cwi + ui_d(24), h + 4, ui_d(10), UI_SURF2);
            text_draw(ren, pic_rows[r].name, (int)cxi, (int)(y + ui_d(4)), ui_px(16), 0, UI_INK, 0);
            char t[16];
            pic_value_text(r, t, sizeof(t));
            const int v = *pic_field(r), lo = pic_rows[r].min, hi = pic_rows[r].max, def = *(&pic_default.bright + r);
            text_draw(ren, t, (int)xri, (int)(y + ui_d(4)), ui_px(15), 1, v == def ? UI_DIM : UI_ACC, 2);
            const float tx = cxi, tw = cwi, ty = y + ui_d(34);
            ui_rrect(ren, tx, ty - 2, tw, 4, 2, UI_SURF3);
            const float fx = tx + (float)(def - lo) / (hi - lo) * tw, vx = tx + (float)(v - lo) / (hi - lo) * tw;
            ui_rrect(ren, vx < fx ? vx : fx, ty - 2, vx < fx ? fx - vx : vx - fx, 4, 2, UI_ACC);
            ui_rrect(ren, fx - 1, ty - ui_d(6), 2, ui_d(12), 1, UI_FAINT);
            const int hov = ui_in(tx - ui_d(8), ty - ui_d(14), tw + ui_d(16), ui_d(28)) || (g_ui_drag == H_PIC && g_ui_drag_arg == r);
            const float ks = hov ? ui_d(18) : ui_d(16);
            ui_rrect(ren, vx - ks * 0.5f, ty - ks * 0.5f, ks, ks, ks * 0.5f, UI_INK);
            ui_hit(tx - ui_d(8), ty - ui_d(14), tw + ui_d(16), ui_d(28), H_PIC, r);
            y += h + ui_d(2);
        }

        ui_section(ren, cxi, &y, "SOUND");
        {
            const float h = ui_d(48);
            ui_row(H_SOUND, 0, y, h);
            ui_label(ren, cxi, y, h, "Game sound", g_mute ? "Muted (M)" : "On (M mutes)", ui_focused(H_SOUND, 0), cwi);
            ui_toggle(ren, xri, y + ui_d(12), !g_mute, H_SOUND, 1);
            y += h + ui_d(4);
        }

        ui_section(ren, cxi, &y, "RECORDING");
        {
            const float h = ui_d(48);
            char rs[200], sub[200];
            rec_status(rs, sizeof(rs));
            snprintf(sub, sizeof(sub), "%s", rec_active() ? rs + 5 : "MP4 with sound (R)");
            ui_row(H_REC, 0, y, h);
            ui_label(ren, cxi, y, h, "Record", sub, ui_focused(H_REC, 0), cwi);
            if (rec_active()) ui_button(ren, xri, 0, y + ui_d(8), "Stop", UI_BAD, (SDL_Color){ 30, 8, 8, 255 }, H_REC, 0, 1);
            else ui_button(ren, xri, 0, y + ui_d(8), "Start", UI_SURF3, UI_INK, H_REC, 0, 1);
            y += h + ui_d(2);
            /* v0.7.19: the v0.7.18 size lock, now a choice (off by default) */
            ui_row(H_REC1, 0, y, h);
            ui_label(ren, cxi, y, h, "One file per recording",
                     g_rec_one_file ? "Keeps the size it started at; other sizes are scaled" : "A new file when the picture size changes; never scaled",
                     ui_focused(H_REC1, 0), cwi);
            ui_toggle(ren, xri, y + ui_d(12), g_rec_one_file, H_REC1, 1);
            y += h + ui_d(2);
            const char *dir = rec_dir_for_menu();
            if (dir[0]) {
                char line[1100];
                snprintf(line, sizeof(line), "Saved in %s", dir);
                ui_text_fit(ren, line, (int)cxi, (int)y, ui_px(13), 0, UI_FAINT, 0, (int)cwi);
                y += ui_d(24);
            }
        }
        /* v0.7.14: updates from GitHub (update.h) */
        ui_section(ren, cxi, &y, "UPDATES");
        {
            const float h = ui_d(48);
            char sb[220];
            const int us = upd_state();
            const char *btn = us == UPD_FOUND ? "Update" : us == UPD_READY ? "Restart" : (us == UPD_ERROR && g_upd.failed_update) ? "Try again" : "Check";
            const int can = us != UPD_CHECKING && us != UPD_GETTING && us != UPD_UNSUPPORTED;
            ui_row(H_UPD, 0, y, h);
            ui_label(ren, cxi, y, h, "Switch Frame Tap " SFT_VERSION, upd_status(sb, sizeof(sb)), ui_focused(H_UPD, 0), cwi);
            ui_button(ren, xri, 0, y + ui_d(8), btn, us == UPD_FOUND || us == UPD_READY ? UI_ACC : UI_SURF3,
                      us == UPD_FOUND || us == UPD_READY ? (SDL_Color){ 8, 20, 30, 255 } : UI_INK, H_UPD, 0, can);
            y += h + ui_d(4);
            ui_row(H_UPD_BETA, 0, y, h);
            ui_label(ren, cxi, y, h, "Include betas", g_upd_betas ? "Offers test versions too" : "Only full releases", ui_focused(H_UPD_BETA, 0), cwi);
            ui_toggle(ren, xri, y + ui_d(12), g_upd_betas, H_UPD_BETA, us != UPD_UNSUPPORTED);
            y += h + ui_d(4);
            ui_row(H_UPD_AUTO, 0, y, h);
            ui_label(ren, cxi, y, h, "Check at start-up", "Looks for a new version when the viewer opens", ui_focused(H_UPD_AUTO, 0), cwi);
            ui_toggle(ren, xri, y + ui_d(12), g_upd_check, H_UPD_AUTO, us != UPD_UNSUPPORTED);
            y += h + ui_d(4);
        }

        /* faint support line at the end of this column */
        {
            y += ui_d(18);
            const char *pre = "Free and open source  -  ", *link = "support the project";
            const int tw = text_width(pre, ui_px(13), 0), lw = text_width(link, ui_px(13), 0);
            text_draw(ren, pre, (int)cxi, (int)y, ui_px(13), 0, UI_FAINT, 0);
            const int hov = ui_in(cxi + tw, y - 2, lw, ui_d(20));
            text_draw(ren, link, (int)cxi + tw, (int)y, ui_px(13), 0, hov ? UI_DIM : UI_FAINT, 0);
            if (hov) ui_rrect(ren, cxi + tw, y + ui_d(17), lw, 1, 0, UI_DIM);
            ui_hit(cxi + tw, y - 2, lw, ui_d(20), H_SUPPORT, 0);
            y += ui_d(22);
        }
        y += ui_d(16);
        if (use_cols == 2) y_r = y; else y_l = y;
    }

    g_ui_content_h = (use_cols == 2 ? (y_l > y_r ? y_l : y_r) : y_l) - y_start;
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
    SDL_FRect hb = { x0 + 1, y0, pw - 1, top - y0 }, fb = { x0 + 1, bottom, pw - 1, y0 + ph - bottom };
    SDL_RenderFillRectF(ren, &hb);
    SDL_RenderFillRectF(ren, &fb);
    text_draw(ren, "Settings", (int)cx, (int)(y0 + (head_h >= ui_d(70) ? ui_d(24) : ui_d(14))),
              head_h >= ui_d(70) ? ui_px(24) : ui_px(20), 1, UI_INK, 0);
    ui_button(ren, xr, 0, y0 + (head_h >= ui_d(70) ? ui_d(22) : ui_d(12)), "Close", UI_SURF2, UI_INK, H_CLOSE, 0, 1);
    SDL_SetRenderDrawColor(ren, UI_LINE.r, UI_LINE.g, UI_LINE.b, UI_LINE.a);
    SDL_FRect l1 = { x0 + 1, top - 1, pw - 1, 1 }, l2 = { x0 + 1, bottom, pw - 1, 1 };
    SDL_RenderFillRectF(ren, &l1);
    SDL_RenderFillRectF(ren, &l2);
    if (show_footer) {
        float kx = cx;
        kx += ui_key(ren, kx, bottom + ui_d(12), "Esc") + ui_d(6);
        text_draw(ren, "close", (int)kx, (int)(bottom + ui_d(14)), ui_px(13), 0, UI_DIM, 0);
        kx += text_width("close", ui_px(13), 0) + ui_d(14);
        kx += ui_key(ren, kx, bottom + ui_d(12), "Up/Down") + ui_d(6);
        text_draw(ren, "move", (int)kx, (int)(bottom + ui_d(14)), ui_px(13), 0, UI_DIM, 0);
        kx += text_width("move", ui_px(13), 0) + ui_d(14);
        kx += ui_key(ren, kx, bottom + ui_d(12), "Left/Right") + ui_d(6);
        text_draw(ren, "change", (int)kx, (int)(bottom + ui_d(14)), ui_px(13), 0, UI_DIM, 0);
    }
    /* Scrollbar on the right of the content view (works without a mouse wheel). */
    ui_scrollbar(ren, x0 + pw - ui_d(14), top, view_h, g_ui_content_h, &g_ui_scroll);
}

/* Settings popout: native pixels + 3 sharp density tiers (desktop / tablet / mobile).
 * Density shrinks fonts and controls by redrawing them smaller - not by scaling a bitmap. */
static void ui_settings_popout(SDL_Renderer *ren)
{
    int ow = 0, oh = 0, lw = 0, lh = 0;
    float sx = 1, sy = 1;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow < 40 || oh < 40) return;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1, 1);

    const float margin = 12.f;
    float avail_w = (float)ow - 2.f * margin;
    float avail_h = (float)oh - 2.f * margin;
    if (avail_w < 280.f) avail_w = (float)ow > 8 ? (float)ow - 8 : (float)ow;
    if (avail_h < 200.f) avail_h = (float)oh > 8 ? (float)oh - 8 : (float)oh;

    /* Pick density from how the desktop card (820×640) fits the window. */
    const float fit_w = avail_w / 820.f, fit_h = avail_h / 640.f;
    const float fit = fit_w < fit_h ? fit_w : fit_h;
    if (fit >= 1.0f) g_ui_d = 1.0f;        /* desktop */
    else if (fit >= 0.78f) g_ui_d = 0.85f; /* tablet */
    else g_ui_d = 0.70f;                   /* mobile */

    const float design_w = 820.f * g_ui_d, design_h = 640.f * g_ui_d;
    float pw = design_w < avail_w ? design_w : avail_w;
    float ph = design_h < avail_h ? design_h : avail_h;
    if (pw < 360.f * g_ui_d) pw = avail_w;
    if (ph < 240.f * g_ui_d) ph = avail_h;

    float x0 = ((float)ow - pw) * 0.5f;
    float y0 = ((float)oh - ph) * 0.5f;
    if (x0 < margin) x0 = margin;
    if (y0 < margin) y0 = margin;
    if (x0 + pw > (float)ow) x0 = (float)ow - pw;
    if (y0 + ph > (float)oh) y0 = (float)oh - ph;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    g_ui_k = 1;
    g_ui_w = (float)ow;
    g_ui_popout_x0 = x0;
    g_ui_popout_y0 = y0;
    g_ui_popout_scale = 1.f; /* 1:1 window pixels - density is in the draw sizes */
    g_ui_popout_map = 0;

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 140);
    SDL_FRect veil = { 0, 0, (float)ow, (float)oh };
    SDL_RenderFillRectF(ren, &veil);
    g_ui_hit_window = 1;
    /* Backdrop is registered but does not close the panel (Esc / Close only).
     * Clicks on it fall through so chromeless drag can still move the window. */
    ui_hit(0, 0, (float)ow, (float)oh, H_BACKDROP, 0);
    ui_drawer(ren, x0, y0, pw, ph);

    g_ui_hit_window = 0;
    if (lw > 0 && lh > 0) SDL_RenderSetLogicalSize(ren, lw, lh);
    SDL_RenderSetScale(ren, sx, sy);
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

/* Hover chrome (Settings + borderless window buttons) and the drawer, over the picture */
static void ui_overlay(SDL_Renderer *ren, int W, int H)
{
    g_ui_nhits = 0;
    if (H <= 0) return;
    if (g_ui_open) {
        if (text_init(ren) != 0) return;
        SDL_BlendMode old;
        SDL_GetRenderDrawBlendMode(ren, &old);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        (void)W;
        ui_settings_popout(ren);
        SDL_SetRenderDrawBlendMode(ren, old);
        return;
    }
    /* Hits always - paint only when the pointer is over the window. Hits do not
     * need fonts, so chrome stays clickable even if text_init is late/fails. */
    ui_top_chrome_fixed(ren, (SDL_Color){ 21, 24, 30, 200 }, 0);
    if (!g_ui_ptr_inside || text_init(ren) != 0) return;
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(ren, &old);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    /* Re-register while painting so hit pads match the drawn buttons. */
    g_ui_nhits = 0;
    ui_top_chrome_fixed(ren, (SDL_Color){ 21, 24, 30, 200 }, 1);
    SDL_SetRenderDrawBlendMode(ren, old);
}

/* ---- the home screen ---------------------------------------------------- */

/* Floating status panel (card + key legend), centered like settings - always
 * kept on-screen with desktop / tablet / mobile density. Brand + recordings
 * stay as light chrome in the corners; the black field is just backdrop. */
static void menu_draw(SDL_Renderer *ren)
{
    text_init(ren);
    int ow = 1280, oh = 720;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow < 40 || oh < 40) { ow = 1280; oh = 720; }
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(ren);

    g_ui_k = 1;
    g_ui_w = (float)ow;
    g_ui_nhits = 0;
    g_ui_hit_window = 1;

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
        l1 = "Install the USB rule (70-switch-frame-tap.rules) - see the README.";
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

    /* Floating panel sized to content (previous frame's measure), capped by the
     * window - scrolls only when content cannot fit. */
    const float margin = 16.f, chrome_top = 52.f, chrome_bot = 36.f;
    float avail_w = (float)ow - 2.f * margin;
    float avail_h = (float)oh - chrome_top - chrome_bot;
    if (avail_w < 200.f) avail_w = (float)ow > 8 ? (float)ow - 8 : (float)ow;
    if (avail_h < 160.f) avail_h = (float)oh > 8 ? (float)oh - 8 : (float)oh;

    const float design_w = 640.f;
    /* Density from width first; height follows content rather than a fixed card. */
    const float fit_w = avail_w / design_w;
    if (fit_w >= 1.0f) g_ui_d = 1.0f;
    else if (fit_w >= 0.78f) g_ui_d = 0.85f;
    else g_ui_d = 0.70f;

    float pw = design_w * g_ui_d;
    if (pw > avail_w) pw = avail_w;

    const float pad_y = ui_d(32); /* top+bottom padding inside the panel */
    float ph;
    if (g_home_content_h > 1.f) {
        ph = g_home_content_h + pad_y;
        if (ph < ui_d(200)) ph = ui_d(200);
    } else {
        ph = ui_d(360); /* first frame before measure */
    }
    if (ph > avail_h) ph = avail_h;

    float x0 = ((float)ow - pw) * 0.5f;
    float y0 = chrome_top + (avail_h - ph) * 0.5f;
    if (x0 < margin) x0 = margin;
    if (y0 < chrome_top) y0 = chrome_top;
    if (x0 + pw > (float)ow - margin) x0 = (float)ow - margin - pw;
    if (y0 + ph > (float)oh - chrome_bot) y0 = (float)oh - chrome_bot - ph;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    const float cx = x0 + pw * 0.5f;
    const float pad = ui_d(28);
    const float top = y0 + ui_d(16), bottom = y0 + ph - ui_d(16);
    const float view_h = bottom - top;
    if (g_home_scroll > g_home_content_h - view_h) g_home_scroll = g_home_content_h - view_h;
    if (g_home_scroll < 0) g_home_scroll = 0;

    /* Panel hit excludes the scrollbar strip so drag does not steal thumb clicks. */
    const float sb_reserve = 22.f;
    ui_hit(x0, y0, pw - sb_reserve, ph, H_PANEL, 0);
    ui_rrect(ren, x0, y0, pw, ph, ui_d(20), UI_SURF);

    SDL_Rect clip = { (int)x0, (int)top, (int)pw + 1, (int)(view_h > 1.f ? view_h : 1.f) };
    SDL_RenderSetClipRect(ren, &clip);
    const int first_hit = g_ui_nhits;
    float y = top - g_home_scroll;
    const float y_start = y;

    const double t = (double)SDL_GetTicks() / 1000.0;
    const int pulse = (g_menu_state == MENU_WAITING) ? (int)(60 + 50 * (0.5 + 0.5 * sin(t * 3.0))) : 70;
    ui_rrect(ren, cx - ui_d(22), y + ui_d(8), ui_d(44), ui_d(44), ui_d(22), ui_alpha(hc, pulse));
    ui_rrect(ren, cx - ui_d(9), y + ui_d(21), ui_d(18), ui_d(18), ui_d(9), hc);
    y += ui_d(64);
    {
        int tpx = ui_px(28);
        const int tmin = ui_px(18);
        while (tpx > tmin && text_width(title, tpx, 1) > (int)(pw - pad * 2)) tpx -= 2;
        text_draw(ren, title, (int)cx, (int)y, tpx, 1, UI_INK, 1);
        y += ui_d(36);
    }
    y += ui_wrap(ren, l1, (int)cx, (int)y, ui_px(16), 0, UI_DIM, (int)(pw - pad * 2), (int)ui_d(22));
    if (l2[0]) y += ui_wrap(ren, l2, (int)cx, (int)y, ui_px(14), 0, UI_FAINT, (int)(pw - pad * 2), (int)ui_d(20));
    y += ui_d(18);

    /* USB / Network - in document flow, not pinned to the bottom */
    {
        static const char *const conn[] = { "USB", "Network" };
        if (ip_edit_for_menu()) {
            const float fw = ui_d(280) < pw - pad * 2 ? ui_d(280) : pw - pad * 2;
            ui_rrect(ren, cx - fw / 2, y - 2, fw, ui_d(36), ui_d(10), UI_ACC);
            ui_rrect(ren, cx - fw / 2 + 2, y, fw - 4, ui_d(32), ui_d(9), UI_SURF2);
            char ipl[64];
            snprintf(ipl, sizeof(ipl), "%s%s", ip_edit_for_menu(), ((SDL_GetTicks() / 500) & 1) ? "_" : " ");
            text_draw(ren, ipl, (int)(cx - fw / 2 + ui_d(14)), (int)(y + ui_d(6)), ui_px(16), 0, UI_INK, 0);
            y += ui_d(40);
            text_draw(ren, "Enter to connect, Esc to cancel", (int)cx, (int)y, ui_px(12), 0, UI_DIM, 1);
            y += ui_d(28);
        } else {
            ui_seg(ren, -1, cx, y, conn, 2, conn_for_menu(), H_CONN, 14, pw - pad * 2);
            y += ui_d(40);
            if (conn_for_menu() && act_ip_possible()) {
                ui_button(ren, -1, cx, y, "Type the Switch's IP", (SDL_Color){ 0, 0, 0, 0 }, UI_ACC, H_IP, 0, 1);
                y += ui_d(40);
            }
        }
    }

    /* v0.7.14: a newer viewer on GitHub (update.h) */
    {
        const int us = upd_state();
        if (us == UPD_FOUND || us == UPD_GETTING || us == UPD_READY || (us == UPD_ERROR && g_upd.failed_update)) {
            char lb[96];
            if (us == UPD_FOUND) snprintf(lb, sizeof(lb), "Update to %s", g_upd.tag);
            else if (us == UPD_GETTING) snprintf(lb, sizeof(lb), "Downloading %s...", g_upd.tag);
            else if (us == UPD_READY) snprintf(lb, sizeof(lb), "Restart to use %s", g_upd.tag);
            else snprintf(lb, sizeof(lb), "Update failed - try again");
            y += ui_d(4);
            ui_button(ren, -1, cx, y, lb, us == UPD_ERROR ? UI_SURF3 : UI_ACC, us == UPD_ERROR ? UI_WARN : (SDL_Color){ 8, 20, 30, 255 }, H_UPD, 0, us != UPD_GETTING);
            y += ui_d(40);
            if (us == UPD_ERROR) {
                y += ui_wrap(ren, g_upd.msg, (int)cx, (int)y, ui_px(13), 0, UI_FAINT, (int)(pw - pad * 2), (int)ui_d(18));
            }
        }
    }

    y += ui_d(12);
    {
        struct { const char *k, *what; } hints[] = {
            { "Esc", "Settings" }, { "F11", "Fullscreen" }, { "R", "Record" }, { "Tab", "USB / Network" }
        };
        float widths[4], gap = ui_d(16), total = 0;
        for (int i = 0; i < 4; i++) {
            widths[i] = text_width(hints[i].k, ui_px(12), 1) + ui_d(12) + ui_d(6) + text_width(hints[i].what, ui_px(13), 0);
            total += widths[i] + (i ? gap : 0);
        }
        const float inner = pw - pad * 2.f - sb_reserve;
        int per_row = total <= inner ? 4 : 2;
        float row_y = y;
        for (int start = 0; start < 4; start += per_row) {
            float row_w = 0;
            int n = start + per_row <= 4 ? per_row : 4 - start;
            for (int j = 0; j < n; j++) row_w += widths[start + j] + (j ? gap : 0);
            float x = cx - row_w * 0.5f;
            for (int j = 0; j < n; j++) {
                int i = start + j;
                x += ui_key(ren, x, row_y, hints[i].k) + ui_d(6);
                text_draw(ren, hints[i].what, (int)x, (int)(row_y + ui_d(2)), ui_px(13), 0, UI_DIM, 0);
                x += text_width(hints[i].what, ui_px(13), 0) + gap;
            }
            row_y += ui_d(30);
        }
        y = row_y + ui_d(6);
    }
    y += ui_d(12);
    g_home_content_h = y - y_start;
    SDL_RenderSetClipRect(ren, NULL);

    for (int i = first_hit; i < g_ui_nhits; i++) {
        SDL_FRect *r = &g_ui_hits[i].r;
        if (g_ui_hits[i].id == H_PANEL) continue;
        if (r->y + r->h < top || r->y > bottom) { r->w = 0; r->h = 0; continue; }
        if (r->y < top) { r->h -= top - r->y; r->y = top; }
        if (r->y + r->h > bottom) r->h = bottom - r->y;
    }
    ui_scrollbar(ren, x0 + pw - ui_d(16), top, view_h, g_home_content_h, &g_home_scroll);

    /* Corner chrome drawn last so it stays above the connection panel.
     * Brand uses the same density tier as the home panel. */
    {
        const float dot = ui_d(10), bx = ui_d(16), by = ui_d(16);
        ui_rrect(ren, bx, by, dot, dot, dot * 0.5f, UI_ACC);
        text_draw(ren, "Switch Frame Tap", (int)(bx + ui_d(20)), (int)(by - ui_d(4)), ui_px(18), 1, UI_INK, 0);
    }
    {
        const char *dir = rec_dir_for_menu();
        if (dir[0]) {
            char line[1100];
            snprintf(line, sizeof(line), "Recordings: %s", dir);
            ui_text_fit(ren, line, (int)ui_d(16), oh - (int)ui_d(28), ui_px(12), 0, UI_FAINT, 0, ow > 40 ? ow - 32 : 200);
        }
        if (g_mute) text_draw(ren, "Sound muted (M)", ow - (int)ui_d(16), oh - (int)ui_d(28), ui_px(12), 0, UI_WARN, 2);
        if (rec_active()) {
            char rs[200];
            rec_status(rs, sizeof(rs));
            const char *r = rs + 5;
            const float w = text_width(r, ui_px(13), 1) + ui_d(36);
            ui_rrect(ren, (ow - w) * 0.5f, ui_d(16), w, ui_d(26), ui_d(13), ui_alpha(UI_BAD, 40));
            ui_rrect(ren, (ow - w) * 0.5f + ui_d(10), ui_d(24), ui_d(8), ui_d(8), ui_d(4), UI_BAD);
            text_draw(ren, r, (int)((ow - w) * 0.5f + ui_d(24)), (int)ui_d(20), ui_px(13), 1, UI_BAD, 0);
        }
    }
    /* Home: always paint + hit-test chrome (connection screen needs it visible). */
    if (!g_ui_open) ui_top_chrome_fixed(ren, UI_SURF2, 1);

    g_ui_hit_window = 0;
    if (g_ui_open) ui_settings_popout(ren);
    else g_ui_d = 1.f;
    /* tests: --shot with --seconds also saves the home screen (no picture came) */
    {
        static Uint32 t0 = 0;
        if (!t0) t0 = SDL_GetTicks();
        if (g_shot && g_run_secs > 0 && SDL_GetTicks() - t0 >= (Uint32)(g_run_secs * 1000.0)) {
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (sf && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
                SDL_SaveBMP(sf, g_shot);
                fprintf(stderr, "\nsaved the home screen %dx%d to %s\n", ow, oh, g_shot);
            }
            if (sf) SDL_FreeSurface(sf);
            exit(0);
        }
    }
    SDL_RenderPresent(ren);
}

/* ---- input -------------------------------------------------------------- */

static void ui_open(int open)
{
    g_ui_open = open;
    g_ui_drag = 0;
    if (open) { g_ui_kbd = 0; }
    else g_ui_d = 1.f;
}

/* change the row's value: d = -1 / +1 (Left / Right), 0 = Enter */
static void ui_change(SDL_Window *win, SDL_Renderer *ren, int id, int arg, int d)
{
    switch (id) {
    case H_CONN: act_conn(d == 0 ? g_conn != CONN_NET : d > 0); break;
    case H_IP: if (d == 0) { act_ip(); ui_open(0); } break;
    case H_FULL: act_fullscreen(win); break;
    case H_VSYNC: act_vsync(ren, !g_vsync); break;
    case H_ASPECT: act_lock_aspect(win, !g_lock_aspect); break;
    case H_CHROMELESS: act_chromeless(win, !g_chromeless); break;
    case H_HWDEC: act_hwdec(!g_hwdec); break;
    case H_REC1: act_rec_one_file(!g_rec_one_file); break;
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
    case H_UPD: if (d == 0) act_update(); break;
    case H_UPD_BETA: g_upd_betas = !g_upd_betas; prefs_save(); if (upd_state() == UPD_LATEST || upd_state() == UPD_FOUND) upd_check(); break;
    case H_UPD_AUTO: g_upd_check = !g_upd_check; prefs_save(); break;
    default: break;
    }
}

/* a slider follows the mouse */
static void ui_slide(float mx)
{
    for (int i = 0; i < g_ui_nhits; i++) {
        if (g_ui_hits[i].id != g_ui_drag || g_ui_hits[i].arg != g_ui_drag_arg) continue;
        const SDL_FRect r = g_ui_hits[i].r;
        if (g_ui_hits[i].window_space) mx = g_ui_wx;
        const float tx = r.x + ui_d(8), tw = r.w - ui_d(16);
        const int a = g_ui_drag_arg;
        const int fsr = g_ui_drag == H_FSR_SHARP;
        const int lo = fsr ? 0 : pic_rows[a].min, hi = fsr ? 100 : pic_rows[a].max, st = fsr ? 1 : pic_rows[a].step;
        float f = tw > 1.f ? (mx - tx) / tw : 0.f;
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
        int wx, wy, ww = 1, wh = 1, ow = 1, oh = 1;
        SDL_GetMouseState(&wx, &wy);
        /* Map window coords → renderer output pixels (HiDPI). Hits are in output space. */
        if (win) SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        if (ww < 1) ww = 1;
        if (wh < 1) wh = 1;
        g_ui_wx = (float)wx * (float)ow / (float)ww;
        g_ui_wy = (float)wy * (float)oh / (float)wh;
        float lx = g_ui_wx, ly = g_ui_wy;
        SDL_RenderWindowToLogical(ren, wx, wy, &lx, &ly);
        g_ui_mx = lx / g_ui_k; g_ui_my = ly / g_ui_k;
    }
    if (e->type == SDL_MOUSEMOTION) {
        g_ui_ptr_inside = 1;
        g_ui_mouse_t = SDL_GetPerformanceCounter();
        g_ui_kbd = 0;
        if (g_ui_drag == H_PIC || g_ui_drag == H_FSR_SHARP) { ui_slide(g_ui_mx); prefs_save(); }
        else if (g_ui_drag == H_SCROLL) ui_scroll_drag(g_ui_wy);
        return 1;                           /* redraw so hover chrome tracks the pointer */
    }
    if (e->type == SDL_MOUSEBUTTONUP) { if (g_ui_drag) { g_ui_drag = 0; return 1; } return 0; }
    if (e->type == SDL_MOUSEWHEEL) {
        if (g_ui_open) { g_ui_scroll -= e->wheel.y * (40.f * g_ui_d); return 1; }
        if (g_menu) { g_home_scroll -= e->wheel.y * (40.f * g_ui_d); return 1; }
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        /* Chrome first, from live geometry - not the last paint's hit list. */
        if (!g_ui_open) {
            int cid = 0;
            if (ui_chrome_at(ren, g_ui_wx, g_ui_wy, &cid)) {
                ui_chrome_activate(win, cid);
                return 1;
            }
        }
        for (int i = g_ui_nhits - 1; i >= 0; i--) {
            const ui_hit_t *h = &g_ui_hits[i];
            const float hx = h->window_space ? g_ui_wx : g_ui_mx;
            const float hy = h->window_space ? g_ui_wy : g_ui_my;
            if (!(hx >= h->r.x && hx < h->r.x + h->r.w && hy >= h->r.y && hy < h->r.y + h->r.h)) continue;
            switch (h->id) {
            case H_SETTINGS: ui_open(1); break;
            case H_CLOSE: ui_open(0); break;
            case H_BACKDROP: return 0; /* don't close; allow window drag underneath */
            case H_PIN:
            case H_WIN_MIN:
            case H_WIN_MAX:
            case H_WIN_CLOSE:
                ui_chrome_activate(win, h->id);
                break;
            case H_PANEL: return 0; /* empty panel chrome: allow chromeless window drag */
            case H_SCROLL:
                g_ui_drag = H_SCROLL;
                ui_scroll_drag(g_ui_wy); /* thumb follows pointer Y */
                break;
            case H_CONN: act_conn(h->arg); break;
            case H_IP: act_ip(); ui_open(0); break;
            case H_FULL: act_fullscreen(win); break;
            case H_VSYNC: act_vsync(ren, h->arg); break;
            case H_ASPECT: act_lock_aspect(win, h->arg); break;
            case H_REC1: act_rec_one_file(!g_rec_one_file); break;
            case H_CHROMELESS: act_chromeless(win, h->arg); break;
            case H_HWDEC: act_hwdec(h->arg); break;
            case H_UPS: act_upscaler(ren, h->arg); break;
            case H_HUD: act_hud(h->arg); break;
            case H_PIC: g_ui_drag = H_PIC; g_ui_drag_arg = h->arg; ui_slide(g_ui_mx); prefs_save(); break;
            case H_FSR_SHARP: g_ui_drag = H_FSR_SHARP; g_ui_drag_arg = 0; ui_slide(g_ui_mx); prefs_save(); break;
            case H_PIC_RESET: g_pic = pic_default; prefs_save(); break;
            case H_SOUND: act_mute(!h->arg); break;
            case H_REC: act_record(win); break;
            case H_SUPPORT: SDL_OpenURL("https://buymeacoffee.com/papux200"); break;
            case H_UPD: act_update(); break;
            case H_UPD_BETA: g_upd_betas = !g_upd_betas; prefs_save(); if (upd_state() == UPD_LATEST || upd_state() == UPD_FOUND) upd_check(); break;
            case H_UPD_AUTO: g_upd_check = !g_upd_check; prefs_save(); break;
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
