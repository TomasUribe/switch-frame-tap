/* menu.h - v0.5: the viewer's text and drawing basics, and the home screen's
 * state (v0.7.12: the screen itself is drawn in ui.h). Drawn with SDL2 alone:
 * text comes from font_dejavu.h, a DejaVu Sans atlas built into the program
 * (tools/raw-recv/gen_font.py). */
#include "font_dejavu.h"

enum { MENU_WAITING = 0, MENU_NODRIVER, MENU_WRONG_DEVICE, MENU_CONNECTED, MENU_PAUSED };

static int g_menu = 0;                 /* the menu is on screen (not a frame) */
static int g_menu_state = MENU_WAITING;
static char g_menu_err[64] = "";       /* the libusb error for MENU_NODRIVER */
static char g_menu_other[64] = "";     /* the other USB program (SysDVR) for MENU_WRONG_DEVICE */
static SDL_Texture *g_font_tex[FONT_ROWS];
static SDL_Renderer *g_font_ren = NULL;
static float g_text_scale = 1.0f;       /* output pixels per 720-unit text size */

static void text_free(void)
{
    for (int r = 0; r < FONT_ROWS; r++) {
        if (g_font_tex[r]) SDL_DestroyTexture(g_font_tex[r]);
        g_font_tex[r] = NULL;
    }
    g_font_ren = NULL;
}

static int text_init(SDL_Renderer *ren)
{
    if (g_font_tex[0] && g_font_ren == ren) return 0;
    text_free();
    g_font_ren = ren;
    for (int r = 0; r < FONT_ROWS; r++) {
        const int w = font_row_w[r], h = font_row_h[r];
        if (font_row_len[r] < (unsigned)((w * h + 1) / 2)) { text_free(); return -1; }
        Uint32 *px = malloc((size_t)w * h * 4);
        if (!px) { text_free(); return -1; }
        const unsigned char *src = font_atlas + font_row_off[r];
        const int n = w * h;
        for (int i = 0; i < n; i++) {
            const unsigned b = src[i >> 1];
            const unsigned a = ((i & 1) ? (b & 15) : (b >> 4)) * 17;
            px[i] = (a << 24) | 0xFFFFFFu;         /* white, the colour comes from the mod */
        }
        g_font_tex[r] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (g_font_tex[r]) {
            SDL_UpdateTexture(g_font_tex[r], NULL, px, w * 4);
            SDL_SetTextureBlendMode(g_font_tex[r], SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_font_tex[r], SDL_ScaleModeNearest);
        }
        free(px);
        if (!g_font_tex[r]) { text_free(); return -1; }
    }
    return 0;
}

/* the row whose native size is closest to the physical pixels this text
 * needs; exact when g_text_scale is 1 or 2 */
static int text_row(int bold, int px)
{
    const int want = (int)(px * g_text_scale + 0.5f);
    int best = -1, bestd = 0x7fffffff;
    for (int r = 0; r < FONT_ROWS; r++) {
        if (font_row_bold[r] != (bold ? 1 : 0)) continue;
        int d = font_row_px[r] - want;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = r; }
    }
    return best < 0 ? 0 : best;
}

static int text_width(const char *s, int px, int bold)
{
    const int row = text_row(bold, px);
    const int fp = font_row_px[row];
    int w = 0;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        w += font_glyphs[row][c - FONT_FIRST][2];
    }
    return w * px / fp;
}

/* align: 0 left, 1 centre, 2 right; y is the top of the line */
static void text_draw(SDL_Renderer *ren, const char *s, int x, int y, int px, int bold, SDL_Color col, int align)
{
    const int row = text_row(bold, px);
    SDL_Texture *tex = g_font_tex[row];
    if (!tex) return;
    if (align) x -= text_width(s, px, bold) / (align == 1 ? 2 : 1);
    SDL_SetTextureColorMod(tex, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(tex, col.a);
    const int fp = font_row_px[row];
    int pen = x * fp;                               /* in font pixels * px, to keep fractions */
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        const unsigned short *g = font_glyphs[row][c - FONT_FIRST];
        if (c != ' ') {
            SDL_Rect src = { g[0], 0, g[1], font_row_h[row] };
            SDL_Rect dst = { pen / fp, y, g[1] * px / fp + 1, font_row_h[row] * px / fp };
            SDL_RenderCopy(ren, tex, &src, &dst);
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

/* v0.7.12: the settings icon - a gear (8 teeth, a hole), smooth like the disc.
 * v0.7.14: rendered at the two sizes it is shown at, so it is not a 96 px
 * texture squeezed into 22 px. */
static const int ui_gear_sizes[2] = { 22, 44 };
static SDL_Texture *g_ui_gear[2] = { NULL, NULL };
static SDL_Renderer *g_ui_gear_ren = NULL;

static void ui_gear_free(void)
{
    for (int i = 0; i < 2; i++) {
        if (g_ui_gear[i]) SDL_DestroyTexture(g_ui_gear[i]);
        g_ui_gear[i] = NULL;
    }
    g_ui_gear_ren = NULL;
}

static int ui_gear_make(SDL_Renderer *ren, int idx)
{
    const int D = ui_gear_sizes[idx];
    Uint32 *px = malloc((size_t)D * D * 4);
    if (!px) return -1;
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
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, D, D);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, px, D * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    free(px);
    g_ui_gear[idx] = tex;
    return tex ? 0 : -1;
}

static void ui_gear_init(SDL_Renderer *ren)
{
    if (g_ui_gear[0] && g_ui_gear_ren == ren) return;
    ui_gear_free();
    g_ui_gear_ren = ren;
    for (int i = 0; i < 2; i++) if (ui_gear_make(ren, i) != 0) { ui_gear_free(); return; }
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
