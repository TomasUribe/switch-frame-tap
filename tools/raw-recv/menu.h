/* menu.h - v0.5: the viewer's text and drawing basics, and the home screen's
 * state (v0.7.12: the screen itself is drawn in ui.h). Drawn with SDL2 alone:
 * text comes from font_dejavu.h, DejaVu Sans built into the program
 * (tools/raw-recv/gen_font.py), rasterized by stb_truetype.
 *
 * Everything here is rasterized at the size it lands on the screen, in real
 * pixels: glyphs and icons into caches keyed by their pixel size, rounded
 * rectangles as anti-aliased geometry. Nothing is drawn from a bitmap made
 * at another size and scaled - that came out jagged. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "font_dejavu.h"
#define FONT_FIRST 32                  /* printable ASCII: what font_dejavu.h has */
#define FONT_LAST 126

enum { MENU_WAITING = 0, MENU_NODRIVER, MENU_WRONG_DEVICE, MENU_CONNECTED, MENU_PAUSED };

static int g_menu = 0;                 /* the menu is on screen (not a frame) */
static int g_menu_state = MENU_WAITING;
static char g_menu_err[64] = "";       /* the libusb error for MENU_NODRIVER */
static char g_menu_other[64] = "";     /* the other USB program (SysDVR) for MENU_WRONG_DEVICE */

/* ---- where the current units land on the screen ------------------------- */

/* how the current units (the overlays draw with a renderer scale / logical
 * size) map to device pixels: px = (unit * a + b) * hd, hd the pixels per
 * window point (more than 1 in a HiDPI window) */
typedef struct { float ax, ay, bx, by, hd; } xform_t;

static xform_t unit_xform(SDL_Renderer *ren)
{
    /* SDL2 hands window coordinates as ints: measure the map over a long span */
    float l0x = 0, l0y = 0, l1x = 0, l1y = 0;
    SDL_RenderWindowToLogical(ren, 0, 0, &l0x, &l0y);
    SDL_RenderWindowToLogical(ren, 10000, 10000, &l1x, &l1y);
    xform_t t = { 1, 1, 0, 0, 1 };
    if (l1x > l0x && l1y > l0y) {
        t.ax = 10000.f / (l1x - l0x); t.bx = -l0x * t.ax;
        t.ay = 10000.f / (l1y - l0y); t.by = -l0y * t.ay;
    }
    int ow = 0, oh = 0, ww = 0, wh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Window *win = SDL_RenderGetWindow(ren);
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    if (ww > 0 && ow > 0) t.hd = (float)ow / ww;
    return t;
}
static float xf_k(const xform_t *t) { return t->ax * t->hd; }   /* device pixels per unit */
static float xf_px(const xform_t *t, float x) { return (x * t->ax + t->bx) * t->hd; }
static float xf_py(const xform_t *t, float y) { return (y * t->ay + t->by) * t->hd; }
static float xf_ux(const xform_t *t, float px) { return (px / t->hd - t->bx) / t->ax; }
static float xf_uy(const xform_t *t, float py) { return (py / t->hd - t->by) / t->ay; }

/* ---- text: DejaVu Sans, each glyph rasterized at its pixel size --------- */

static stbtt_fontinfo g_ttf[2];        /* 0 regular, 1 bold */
static int g_ttf_ok = 0;
static SDL_Texture *g_font_tex = NULL; /* the glyph cache, filled shelf by shelf */
static SDL_Renderer *g_font_ren = NULL;
enum { GC_W = 1024, GC_H = 1024, GC_MAXPX = 128, GC_SUB = 4, GC_N = 4096 };
typedef struct { short x, y, w, h, ox, oy; } glyph_t;  /* in the cache; offset from the pen and baseline */
static glyph_t g_glyphs[GC_N];
static int g_nglyphs = 0, g_shelf_x = 0, g_shelf_y = 0, g_shelf_h = 0;
/* glyph index + 1, by style, pixel size, character and quarter-pixel shift */
static short g_glyph_at[2][GC_MAXPX][FONT_LAST - FONT_FIRST + 1][GC_SUB];

static void glyph_cache_reset(void)
{
    memset(g_glyph_at, 0, sizeof(g_glyph_at));
    g_nglyphs = g_shelf_x = g_shelf_y = g_shelf_h = 0;
}

static int text_init(SDL_Renderer *ren)
{
    if (g_font_tex && g_font_ren == ren) return 0;
    if (!g_ttf_ok) {
        if (!stbtt_InitFont(&g_ttf[0], font_regular, 0) || !stbtt_InitFont(&g_ttf[1], font_bold, 0)) return -1;
        g_ttf_ok = 1;
    }
    if (g_font_tex && g_font_ren != ren) g_font_tex = NULL;     /* went with its renderer */
    if (g_font_tex) SDL_DestroyTexture(g_font_tex);
    g_font_ren = ren;
    glyph_cache_reset();
    g_font_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, GC_W, GC_H);
    if (g_font_tex) {
        SDL_SetTextureBlendMode(g_font_tex, SDL_BLENDMODE_BLEND);
        /* drawn texel for pixel */
        SDL_SetTextureScaleMode(g_font_tex, SDL_ScaleModeNearest);
    }
    return g_font_tex ? 0 : -1;
}

/* the glyph at pixel size px, shifted right by sub/GC_SUB of a pixel; NULL: none */
static const glyph_t *glyph_get(int bold, int px, int c, int sub)
{
    short *slot = &g_glyph_at[bold][px][c - FONT_FIRST][sub];
    if (*slot) return &g_glyphs[*slot - 1];
    const stbtt_fontinfo *f = &g_ttf[bold];
    const float s = stbtt_ScaleForMappingEmToPixels(f, (float)px), sx = (float)sub / GC_SUB;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBoxSubpixel(f, c, s, s, sx, 0, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w >= GC_W || h >= GC_H) return NULL;
    if (g_shelf_x + w + 1 > GC_W) { g_shelf_x = 0; g_shelf_y += g_shelf_h + 1; g_shelf_h = 0; }
    if (g_nglyphs == GC_N || g_shelf_y + h > GC_H) {
        /* full: start again (SDL draws what is queued before the update) */
        glyph_cache_reset();
        slot = &g_glyph_at[bold][px][c - FONT_FIRST][sub];
    }
    unsigned char *a = malloc((size_t)w * h);
    Uint32 *argb = malloc((size_t)w * h * 4);
    if (!a || !argb) { free(a); free(argb); return NULL; }
    stbtt_MakeCodepointBitmapSubpixel(f, a, w, h, w, s, s, sx, 0, c);
    for (int i = 0; i < w * h; i++) argb[i] = ((Uint32)a[i] << 24) | 0xFFFFFFu;  /* white, the colour comes from the mod */
    glyph_t *g = &g_glyphs[g_nglyphs];
    *g = (glyph_t){ (short)g_shelf_x, (short)g_shelf_y, (short)w, (short)h, (short)x0, (short)y0 };
    const SDL_Rect r = { g->x, g->y, w, h };
    SDL_UpdateTexture(g_font_tex, &r, argb, w * 4);
    free(a);
    free(argb);
    g_shelf_x += w + 1;
    if (h > g_shelf_h) g_shelf_h = h;
    *slot = (short)++g_nglyphs;
    return g;
}

/* width in units of text at px units: advances and kerning at that size */
static float text_widthf(const char *s, int px, int bold)
{
    if (!g_ttf_ok) return 0;
    const stbtt_fontinfo *f = &g_ttf[bold];
    const float sc = stbtt_ScaleForMappingEmToPixels(f, (float)px);
    float w = 0;
    int prev = 0;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(f, prev, c) * sc;
        w += adv * sc;
        prev = c;
    }
    return w;
}

static int text_width(const char *s, int px, int bold)
{
    return (int)(text_widthf(s, px, bold) + 0.5f);
}

/* align: 0 left, 1 centre, 2 right; y is the top of the line */
static void text_draw(SDL_Renderer *ren, const char *s, int x, int y, int px, int bold, SDL_Color col, int align)
{
    if (!g_font_tex || !g_ttf_ok) return;
    float fx = (float)x;
    if (align) fx -= text_widthf(s, px, bold) / (align == 1 ? 2 : 1);
    SDL_SetTextureColorMod(g_font_tex, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(g_font_tex, col.a);
    const stbtt_fontinfo *f = &g_ttf[bold];
    const xform_t t = unit_xform(ren);
    const float k = xf_k(&t);
    int pxd = (int)(px * k + 0.5f);            /* the size on the screen, in pixels */
    if (pxd < 1) pxd = 1;
    if (pxd >= GC_MAXPX) pxd = GC_MAXPX - 1;
    const float sc = stbtt_ScaleForMappingEmToPixels(f, (float)px) * k;   /* font units -> pixels, for the layout */
    int asc, desc, gap;
    stbtt_GetFontVMetrics(f, &asc, &desc, &gap);
    float pen = xf_px(&t, fx);
    const float top = xf_py(&t, (float)y);
    const float base = floorf(top + asc * stbtt_ScaleForMappingEmToPixels(f, (float)pxd) + 0.5f);
    int prev = 0;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        if (prev) pen += stbtt_GetCodepointKernAdvance(f, prev, c) * sc;
        const float whole = floorf(pen);
        int sub = (int)((pen - whole) * GC_SUB);
        if (sub >= GC_SUB) sub = GC_SUB - 1;
        const glyph_t *g = c == ' ' ? NULL : glyph_get(bold, pxd, c, sub);
        if (g) {
            /* texel for pixel: place it on whole pixels, in units */
            const float ux0 = xf_ux(&t, whole + g->ox), uy0 = xf_uy(&t, base + g->oy);
            const SDL_Rect src = { g->x, g->y, g->w, g->h };
            const SDL_FRect dst = { ux0, uy0, xf_ux(&t, whole + g->ox + g->w) - ux0, xf_uy(&t, base + g->oy + g->h) - uy0 };
            SDL_RenderCopyF(ren, g_font_tex, &src, &dst);
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        pen += adv * sc;
        prev = c;
    }
}

static void fill(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(ren, &r);
}

/* ---- v0.7.12: rounded panels, pills and knobs ---------------------------
 * Anti-aliased geometry at any size: the inside as a fan, then a band one
 * pixel wide along the outline that fades from the colour to nothing.
 * Coordinates in the current units (float: the overlay draws scaled). */
static void ui_rrect(SDL_Renderer *ren, float x, float y, float w, float h, float r, SDL_Color c)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    const xform_t xf = unit_xform(ren);
    const float k = xf_k(&xf), e = 0.5f / k;  /* half a pixel, in units */
    if (r < e) r = e;                          /* a square corner: the band still goes round it */
    /* points a quarter: enough that the arc is off by under a tenth of a pixel */
    int n = (int)(sqrtf(r * k) * 1.6f) + 1;
    if (n > 32) n = 32;
    enum { MAXP = 4 * 33 };
    SDL_Vertex v[1 + 2 * MAXP];
    int idx[3 * MAXP + 6 * MAXP], ni = 0, np = 0;
    const int O = 1 + 4 * (n + 1);             /* the outer ring follows the inner one */
    const SDL_Color in = c, out = { c.r, c.g, c.b, 0 };
    const float cx[4] = { x + w - r, x + r, x + r, x + w - r }, cy[4] = { y + r, y + r, y + h - r, y + h - r };
    v[0] = (SDL_Vertex){ { x + w / 2, y + h / 2 }, in, { 0, 0 } };
    for (int q = 0; q < 4; q++)                /* corners: top right, top left, bottom left, bottom right */
        for (int i = 0; i <= n; i++) {
            const float a = (q + (float)i / n) * 1.5707963f;
            const float nx = cosf(a), ny = -sinf(a);
            v[1 + np] = (SDL_Vertex){ { cx[q] + nx * (r - e), cy[q] + ny * (r - e) }, in, { 0, 0 } };
            v[O + np] = (SDL_Vertex){ { cx[q] + nx * (r + e), cy[q] + ny * (r + e) }, out, { 0, 0 } };
            np++;
        }
    for (int i = 0; i < np; i++) {
        const int j = (i + 1) % np;
        idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + j;
        idx[ni++] = 1 + i; idx[ni++] = O + i; idx[ni++] = O + j;
        idx[ni++] = 1 + i; idx[ni++] = O + j; idx[ni++] = 1 + j;
    }
    SDL_RenderGeometry(ren, NULL, v, O + np, idx, ni);
}

/* ---- icons: the settings gear and the always-on-top pin, rasterized at the
 * pixel size they are drawn at (cached by that size) ---------------------- */
enum { ICON_GEAR, ICON_PIN, ICON_N };
enum { ICON_SIZES = 8 };
static struct { SDL_Texture *tex; int px; } g_icons[ICON_N][ICON_SIZES];
static SDL_Renderer *g_icons_ren = NULL;

/* is (fx, fy), from the centre of a D-pixel square, inside the icon? */
static int icon_gear_at(float fx, float fy, float D)
{
    /* 8 teeth, a hole */
    const float rb = D * 0.34f, rt = D * 0.47f, rh = D * 0.15f;
    const float r = sqrtf(fx * fx + fy * fy);
    if (r < rh) return 0;                      /* the hole */
    if (r <= rb) return 1;                     /* the body */
    if (r > rt) return 0;
    /* a tooth, a little narrower at the tip */
    float a = atan2f(fy, fx) / (2 * 3.14159265f) * 8.0f;
    a -= floorf(a);                            /* 0..1 within one tooth period */
    const float half = 0.25f - 0.07f * (r - rb) / (rt - rb);
    return fabsf(a - 0.5f) < half;
}
static int icon_pin_at(float fx, float fy, float D)
{
    /* an upright map pin: round head with a hole, tapered point below;
     * relative to the head centre (slightly above the middle) */
    const float hy = -D * 0.08f, hr = D * 0.27f, hole = D * 0.105f, tip = D * 0.52f;
    fy -= hy;
    const float r = sqrtf(fx * fx + fy * fy);
    int hit = r <= hr;                         /* head disc */
    if (!hit && fy > 0.f && fy <= tip && fabsf(fx) <= hr * (1.f - fy / tip)) hit = 1;   /* the point */
    return hit && !(r < hole && fy < hr * 0.35f);
}

static void ui_icons_free(void)
{
    for (int i = 0; i < ICON_N; i++)
        for (int j = 0; j < ICON_SIZES; j++) {
            if (g_icons[i][j].tex && g_icons_ren) SDL_DestroyTexture(g_icons[i][j].tex);
            g_icons[i][j].tex = NULL;
            g_icons[i][j].px = 0;
        }
    g_icons_ren = NULL;
}

/* the icon in the square x, y, size (units), on whole pixels */
static void ui_icon(SDL_Renderer *ren, int which, float x, float y, float size, SDL_Color c)
{
    if (g_icons_ren != ren) {
        for (int i = 0; i < ICON_N; i++)
            for (int j = 0; j < ICON_SIZES; j++) g_icons[i][j].tex = NULL, g_icons[i][j].px = 0;   /* went with their renderer */
        g_icons_ren = ren;
    }
    const xform_t xf = unit_xform(ren);
    const float px0 = xf_px(&xf, x), py0 = xf_py(&xf, y);
    const int D = (int)(size * xf_k(&xf) + 0.5f);
    if (D < 2 || D > 512) return;
    int j = 0;
    while (j < ICON_SIZES && g_icons[which][j].tex && g_icons[which][j].px != D) j++;
    if (j == ICON_SIZES) {                     /* full: forget them (window sizes come and go) */
        for (j = 0; j < ICON_SIZES; j++) { SDL_DestroyTexture(g_icons[which][j].tex); g_icons[which][j].tex = NULL; }
        j = 0;
    }
    if (!g_icons[which][j].tex) {
        Uint32 *pix = malloc((size_t)D * D * 4);
        if (!pix) return;
        int (*at)(float, float, float) = which == ICON_GEAR ? icon_gear_at : icon_pin_at;
        for (int yy = 0; yy < D; yy++)
            for (int xx = 0; xx < D; xx++) {
                /* 4x4 samples a pixel: the coverage of the icon */
                int in = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        in += at(xx + (sx + 0.5f) / 4 - D / 2.0f, yy + (sy + 0.5f) / 4 - D / 2.0f, (float)D);
                pix[yy * D + xx] = ((Uint32)(in * 255 / 16) << 24) | 0xFFFFFFu;
            }
        SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, D, D);
        if (t) {
            SDL_UpdateTexture(t, NULL, pix, D * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        }
        free(pix);
        if (!t) return;
        g_icons[which][j].tex = t;
        g_icons[which][j].px = D;
    }
    SDL_Texture *t = g_icons[which][j].tex;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    const float X = floorf(px0 + 0.5f), Y = floorf(py0 + 0.5f);   /* on whole pixels */
    const float ux0 = xf_ux(&xf, X), uy0 = xf_uy(&xf, Y);
    const SDL_FRect dst = { ux0, uy0, xf_ux(&xf, X + D) - ux0, xf_uy(&xf, Y + D) - uy0 };
    SDL_RenderCopyF(ren, t, NULL, &dst);
}


static const char *rec_dir_for_menu(void);
static const char *net_peer_for_menu(void);
static int status_for_menu(char *head, size_t hn, char *detail, size_t dn, uint32_t *reason);

static void menu_draw(SDL_Renderer *ren);   /* v0.7.12: ui.h - the home screen */
