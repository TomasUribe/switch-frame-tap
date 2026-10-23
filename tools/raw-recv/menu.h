/* menu.h - v0.5: the viewer's text and drawing basics, and the home screen's
 * state (v0.7.12: the screen itself is drawn in ui.h). Drawn with SDL2 alone:
 * text comes from font_dejavu.h, a DejaVu Sans atlas built into the program
 * (tools/raw-recv/gen_font.py). */
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
        /* DPI-aware windows scale the 720p UI; nearest sampling made glyphs look
         * pixelated/garbled after per-monitor DPI (v0.7.13). Linear matches gear/disc. */
        SDL_SetTextureScaleMode(g_font_tex, SDL_ScaleModeLinear);
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

/* v0.7.12: rounded panels - a smooth (anti-aliased) disc made once per
 * renderer, its quarters for the corners, plain rectangles for the rest.
 * Coordinates in the current units (float: the overlay draws scaled). */
static SDL_Texture *g_ui_disc = NULL;
static SDL_Renderer *g_ui_disc_ren = NULL;

static void ui_disc_init(SDL_Renderer *ren)
{
    if (g_ui_disc && g_ui_disc_ren == ren) return;
    if (g_ui_disc && g_ui_disc_ren != ren) g_ui_disc = NULL;   /* went with its renderer */
    enum { D = 96 };
    static Uint32 px[D * D];
    for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
            /* 4x4 samples a pixel: the coverage of the disc */
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float fx = x + (sx + 0.5f) / 4 - D / 2.0f, fy = y + (sy + 0.5f) / 4 - D / 2.0f;
                    in += fx * fx + fy * fy <= (D / 2.0f) * (D / 2.0f);
                }
            px[y * D + x] = ((Uint32)(in * 255 / 16) << 24) | 0xFFFFFFu;
        }
    g_ui_disc = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, D, D);
    g_ui_disc_ren = ren;
    if (g_ui_disc) {
        SDL_UpdateTexture(g_ui_disc, NULL, px, D * 4);
        SDL_SetTextureBlendMode(g_ui_disc, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_ui_disc, SDL_ScaleModeLinear);
    }
}

/* v0.7.12: the settings icon - a gear (8 teeth, a hole), smooth like the disc */
static SDL_Texture *g_ui_gear = NULL;
static SDL_Renderer *g_ui_gear_ren = NULL;
/* Always-on-top pin (map-pin silhouette, same AA style as the gear). */
static SDL_Texture *g_ui_pin = NULL;
static SDL_Renderer *g_ui_pin_ren = NULL;

static void ui_gear_init(SDL_Renderer *ren)
{
    if (g_ui_gear && g_ui_gear_ren == ren) return;
    enum { D = 96 };
    static Uint32 px[D * D];
    const float c = D / 2.0f, rb = D * 0.34f, rt = D * 0.47f, rh = D * 0.15f;
    for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float fx = x + (sx + 0.5f) / 4 - c, fy = y + (sy + 0.5f) / 4 - c;
                    const float r = sqrtf(fx * fx + fy * fy);
                    if (r < rh) continue;                      /* the hole */
                    if (r <= rb) { in++; continue; }           /* the body */
                    if (r > rt) continue;
                    /* a tooth: 8 of them, a little narrower at the tip */
                    float a = atan2f(fy, fx) / (2 * 3.14159265f) * 8.0f;
                    a -= floorf(a);                            /* 0..1 within one tooth period */
                    const float half = 0.25f - 0.07f * (r - rb) / (rt - rb);
                    if (fabsf(a - 0.5f) < half) in++;
                }
            px[y * D + x] = ((Uint32)(in * 255 / 16) << 24) | 0xFFFFFFu;
        }
    g_ui_gear = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, D, D);
    g_ui_gear_ren = ren;
    if (g_ui_gear) {
        SDL_UpdateTexture(g_ui_gear, NULL, px, D * 4);
        SDL_SetTextureBlendMode(g_ui_gear, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_ui_gear, SDL_ScaleModeLinear);
    }
}

static void ui_pin_init(SDL_Renderer *ren)
{
    if (g_ui_pin && g_ui_pin_ren == ren) return;
    enum { D = 96 };
    static Uint32 px[D * D];
    /* Upright map pin: round head with a hole, tapered point below.
     * Coords are relative to the head centre (slightly above texture mid). */
    const float c = D / 2.0f;
    const float hy = -D * 0.08f;                /* head centre above mid */
    const float hr = D * 0.27f, hole = D * 0.105f;
    const float tip = D * 0.52f;                /* tip distance below head */
    for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float fx = x + (sx + 0.5f) / 4 - c;
                    const float fy = y + (sy + 0.5f) / 4 - c - hy;
                    const float r = sqrtf(fx * fx + fy * fy);
                    int hit = 0;
                    if (r <= hr) hit = 1;                      /* head disc */
                    else if (fy > 0.f && fy <= tip) {
                        /* Tapered point: width shrinks from head to tip. */
                        const float t = fy / tip;
                        const float half = hr * (1.f - t);
                        if (fabsf(fx) <= half) hit = 1;
                    }
                    if (hit && !(r < hole && fy < hr * 0.35f)) in++;
                }
            px[y * D + x] = ((Uint32)(in * 255 / 16) << 24) | 0xFFFFFFu;
        }
    g_ui_pin = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, D, D);
    g_ui_pin_ren = ren;
    if (g_ui_pin) {
        SDL_UpdateTexture(g_ui_pin, NULL, px, D * 4);
        SDL_SetTextureBlendMode(g_ui_pin, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_ui_pin, SDL_ScaleModeLinear);
    }
}

static void ui_rrect(SDL_Renderer *ren, float x, float y, float w, float h, float r, SDL_Color c)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    ui_disc_init(ren);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    if (!g_ui_disc || r < 1) { SDL_FRect f = { x, y, w, h }; SDL_RenderFillRectF(ren, &f); return; }
    const SDL_FRect mid = { x, y + r, w, h - 2 * r }, top = { x + r, y, w - 2 * r, r }, bot = { x + r, y + h - r, w - 2 * r, r };
    SDL_RenderFillRectF(ren, &mid);
    SDL_RenderFillRectF(ren, &top);
    SDL_RenderFillRectF(ren, &bot);
    SDL_SetTextureColorMod(g_ui_disc, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g_ui_disc, c.a);
    const int H = 48;   /* half the disc texture */
    const SDL_Rect q[4] = { { 0, 0, H, H }, { H, 0, H, H }, { 0, H, H, H }, { H, H, H, H } };
    const SDL_FRect d[4] = { { x, y, r, r }, { x + w - r, y, r, r }, { x, y + h - r, r, r }, { x + w - r, y + h - r, r, r } };
    for (int i = 0; i < 4; i++) SDL_RenderCopyF(ren, g_ui_disc, &q[i], &d[i]);
}


static const char *rec_dir_for_menu(void);
static const char *net_peer_for_menu(void);
static int status_for_menu(char *head, size_t hn, char *detail, size_t dn, uint32_t *reason);

static void menu_draw(SDL_Renderer *ren);   /* v0.7.12: ui.h - the home screen */
