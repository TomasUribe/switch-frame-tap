/* hud.h - v0.7.2: the stats overlay over the picture. O or F3 cycles
 *
 *   Off -> FPS (a counter in the corner) -> Graphs (fps, bitrate, latency,
 *   frame time and latency graphs) -> Full (every number the viewer keeps,
 *   and six graphs) -> Off
 *
 * and the choice is remembered in viewer.ini. The display loop feeds it one
 * sample per frame shown (hud_frame) and one summary a second (hud_second);
 * hud_draw paints it in the picture's own coordinates (the renderer's logical
 * size is the stream size), scaled so 720p and 1080p look the same. Needs
 * menu.h's text_draw and fill. */

enum { HUD_OFF = 0, HUD_FPS, HUD_GRAPHS, HUD_FULL, HUD_MODES };
static const char *const hud_mode_name[HUD_MODES] = { "off", "FPS", "graphs", "full" };
static int g_hud = HUD_OFF;
static Uint64 g_hud_toast = 0;          /* when the "Stats overlay: ..." note was set */

/* per-frame history: the last HUD_N frames shown, oldest first when read */
#define HUD_N 240
typedef struct { float v[HUD_N]; unsigned char key[HUD_N]; int n, at; } hud_ring;
static hud_ring h_gap, h_pc, h_con, h_kb, h_dec;
/* per-second history: the last 60 seconds */
#define HUD_S 60
typedef struct { float v[HUD_S]; int n, at; } hud_sring;
static hud_sring h_fps_s, h_mbps_s;

/* the last second, as the console log prints it */
typedef struct {
    double fps, dec_fps, mbps, pc, con, dec_ms, dec_max, draw_ms, gap_max, a_kbs, a_q;
    long skipped, lost, keyframes, underruns, resyncs, sessions, packets;
    int qmax, w, h, have;
} hud_sec;
static hud_sec g_hs;

static void hud_push(hud_ring *r, float x, int key)
{
    r->v[r->at] = x; r->key[r->at] = (unsigned char)key;
    r->at = (r->at + 1) % HUD_N;
    if (r->n < HUD_N) r->n++;
}

static float hud_at(const hud_ring *r, int i)            /* i = 0 the oldest */
{
    return r->v[(r->at - r->n + i + HUD_N) % HUD_N];
}

static void hud_spush(hud_sring *r, float x)
{
    r->v[r->at] = x;
    r->at = (r->at + 1) % HUD_S;
    if (r->n < HUD_S) r->n++;
}

static float hud_sat(const hud_sring *r, int i)
{
    return r->v[(r->at - r->n + i + HUD_S) % HUD_S];
}

/* one frame on screen: the gap since the last, both latencies, its size,
 * whether it was a keyframe, and how long it took to decode */
static void hud_frame(float gap_ms, float pc_ms, float con_ms, float kb, int key, float dec_ms)
{
    hud_push(&h_gap, gap_ms, 0);
    hud_push(&h_pc, pc_ms, 0);
    hud_push(&h_con, con_ms < 0 ? 0 : con_ms, 0);
    hud_push(&h_kb, kb, key);
    hud_push(&h_dec, dec_ms, 0);
}

static void hud_second(const hud_sec *s)
{
    g_hs = *s;
    g_hs.have = 1;
    hud_spush(&h_fps_s, (float)s->fps);
    hud_spush(&h_mbps_s, (float)s->mbps);
}

static void hud_cycle(void)
{
    g_hud = (g_hud + 1) % HUD_MODES;
    g_hud_toast = SDL_GetPerformanceCounter();
}

/* ---- drawing --------------------------------------------------------- */

static const SDL_Color hud_ink   = { 236, 239, 244, 255 };
static const SDL_Color hud_dim   = { 160, 168, 182, 255 };
static const SDL_Color hud_good  = { 120, 220, 140, 255 };
static const SDL_Color hud_warn  = { 245, 200, 90, 255 };
static const SDL_Color hud_bad   = { 240, 100, 90, 255 };
static const SDL_Color hud_blue  = { 110, 170, 250, 255 };
static const SDL_Color hud_panel = { 10, 12, 16, 170 };

static void hud_text(SDL_Renderer *ren, const char *s, int x, int y, int px, int bold, SDL_Color c, int align)
{
    const SDL_Color sh = { 0, 0, 0, 200 };
    text_draw(ren, s, x + 1, y + 1, px, bold, sh, align);
    text_draw(ren, s, x, y, px, bold, c, align);
}

/* the frame time's colour: steady at the stream's own rate, or not */
static SDL_Color hud_fps_colour(void)
{
    if (!g_hs.have) return hud_ink;
    if (g_hs.gap_max > 100.0) return hud_bad;
    if (g_hs.skipped > 0 || g_hs.gap_max > 50.0) return hud_warn;
    return hud_good;
}

/* a bar graph of a per-frame series; `stack` (may be NULL) is drawn on top
 * of `r` in another colour. Fixed guide lines at the values in `guides`. */
static void hud_graph(SDL_Renderer *ren, int x, int y, int w, int h, const char *label, const char *unit,
                      const hud_ring *r, SDL_Color c, const hud_ring *stack, SDL_Color c2,
                      float floor_max, const float *guides, int nguides, int key_red,
                      const char *leg1, SDL_Color lc1, const char *leg2, SDL_Color lc2)
{
    fill(ren, x, y, w, h, hud_panel);
    const int gx = x + 6, gw = w - 12, gy = y + 22, gh = h - 28;
    /* the scale: the largest sample (or the floor), rounded up */
    float mx = floor_max;
    for (int i = 0; i < r->n; i++) {
        const float v = hud_at(r, i) + (stack ? hud_at(stack, i) : 0.0f);
        if (v > mx) mx = v;
    }
    mx *= 1.1f;
    const SDL_Color guide = { 255, 255, 255, 40 };
    for (int k = 0; k < nguides; k++) {
        if (guides[k] >= mx) continue;
        const int gyl = gy + gh - (int)(guides[k] / mx * gh);
        fill(ren, gx, gyl, gw, 1, guide);
    }
    const int n = r->n;
    for (int i = 0; i < n; i++) {
        const int bx0 = gx + gw * (HUD_N - n + i) / HUD_N;
        const int bx1 = gx + gw * (HUD_N - n + i + 1) / HUD_N;
        const float a = hud_at(r, i);
        int ha = (int)(a / mx * gh); if (ha < 1 && a > 0) ha = 1;
        const int key = key_red && r->key[(r->at - r->n + i + HUD_N) % HUD_N];
        fill(ren, bx0, gy + gh - ha, bx1 - bx0 > 1 ? bx1 - bx0 - (gw >= 2 * HUD_N) : 1, ha, key ? hud_bad : c);
        if (stack) {
            const float b = hud_at(stack, i);
            int hb = (int)(b / mx * gh);
            fill(ren, bx0, gy + gh - ha - hb, bx1 - bx0 > 1 ? bx1 - bx0 - (gw >= 2 * HUD_N) : 1, hb, c2);
        }
    }
    char t[96];
    const float last = n ? hud_at(r, n - 1) + (stack ? hud_at(stack, n - 1) : 0.0f) : 0.0f;
    /* the label, then its legend in the bars' own colours */
    int lx = x + 8;
    text_draw(ren, label, lx, y + 3, 15, 1, hud_dim, 0);
    lx += text_width(label, 15, 1) + 10;
    if (leg1) { text_draw(ren, leg1, lx, y + 3, 15, 1, lc1, 0); lx += text_width(leg1, 15, 1) + 6; }
    if (leg2) { text_draw(ren, "+", lx, y + 3, 15, 1, hud_dim, 0); lx += text_width("+", 15, 1) + 6; text_draw(ren, leg2, lx, y + 3, 15, 1, lc2, 0); }
    snprintf(t, sizeof(t), "%.1f %s  (max %.0f)", last, unit, mx / 1.1f);
    text_draw(ren, t, x + w - 8, y + 3, 15, 0, hud_ink, 2);
}

/* a line graph of a per-second series (the last minute) */
static void hud_sgraph(SDL_Renderer *ren, int x, int y, int w, int h, const char *label, const char *unit,
                       const hud_sring *r, SDL_Color c, float floor_max)
{
    fill(ren, x, y, w, h, hud_panel);
    const int gx = x + 6, gw = w - 12, gy = y + 22, gh = h - 28;
    float mx = floor_max;
    for (int i = 0; i < r->n; i++) if (hud_sat(r, i) > mx) mx = hud_sat(r, i);
    mx *= 1.1f;
    SDL_Point pts[HUD_S];
    for (int i = 0; i < r->n; i++) {
        pts[i].x = gx + gw * (HUD_S - r->n + i) / (HUD_S - 1);
        pts[i].y = gy + gh - (int)(hud_sat(r, i) / mx * gh);
    }
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    if (r->n > 1) {
        SDL_RenderDrawLines(ren, pts, r->n);
        for (int i = 0; i < r->n; i++) pts[i].y -= 1;      /* 2 px thick */
        SDL_RenderDrawLines(ren, pts, r->n);
    }
    char t[96];
    text_draw(ren, label, x + 8, y + 3, 15, 1, hud_dim, 0);
    snprintf(t, sizeof(t), "%.1f %s  (60 s, max %.0f)", r->n ? hud_sat(r, r->n - 1) : 0.0f, unit, mx / 1.1f);
    text_draw(ren, t, x + w - 8, y + 3, 15, 0, hud_ink, 2);
}

/* W x H: the picture (the renderer's logical size); conn: "USB" / "Network ..." */
static void hud_draw(SDL_Renderer *ren, int W, int H, const char *conn, const char *rec)
{
    if (H <= 0 || text_init(ren) != 0) return;    /* the font, if the menu has not loaded it yet */
    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(ren, &old);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    /* draw in 720p units: everything scales with the picture */
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren, &sx, &sy);
    const float k = (float)H / 720.0f;
    SDL_RenderSetScale(ren, sx * k, sy * k);
    const int w720 = (int)(W / k);

    const Uint64 now = SDL_GetPerformanceCounter();
    const double since_toast = (double)(now - g_hud_toast) / (double)SDL_GetPerformanceFrequency();
    if (g_hud_toast && since_toast >= 1.5) g_hud_toast = 0;
    if (g_hud_toast) {
        char t[64];
        snprintf(t, sizeof(t), "Stats overlay: %s  (O / F3)", hud_mode_name[g_hud]);
        const int tw = text_width(t, 20, 1) + 30;
        fill(ren, (w720 - tw) / 2, 640, tw, 36, hud_panel);
        hud_text(ren, t, w720 / 2, 646, 20, 1, hud_ink, 1);
    }

    char t[256];
    const SDL_Color fc = hud_fps_colour();
    if (g_hud == HUD_FPS) {
        snprintf(t, sizeof(t), "%.0f fps", g_hs.have ? g_hs.fps : 0.0);
        fill(ren, 8, 8, text_width(t, 26, 1) + 20, 38, hud_panel);
        hud_text(ren, t, 18, 12, 26, 1, fc, 0);
    } else if (g_hud == HUD_GRAPHS) {
        fill(ren, 10, 10, 470, 34, hud_panel);
        snprintf(t, sizeof(t), "%.1f fps   %.1f Mbps   latency %.0f ms", g_hs.fps, g_hs.mbps, g_hs.pc + g_hs.con);
        text_draw(ren, t, 20, 16, 19, 1, fc, 0);
        const float ft[] = { 16.7f, 33.3f };
        hud_graph(ren, 10, 50, 470, 96, "FRAME TIME", "ms", &h_gap, hud_good, NULL, hud_good, 20.0f, ft, 2, 0, NULL, hud_ink, NULL, hud_ink);
        const float lt[] = { 50.0f, 100.0f };
        hud_graph(ren, 10, 152, 470, 96, "LATENCY", "ms", &h_con, hud_blue, &h_pc, hud_warn, 40.0f, lt, 2, 0, "Switch", hud_blue, "PC", hud_warn);
    } else if (g_hud == HUD_FULL) {
        /* numbers on the left, graphs on the right */
        const int lx = 10, ly = 10, lw = 340;
        fill(ren, lx, ly, lw, 470, hud_panel);
        int y = ly + 10;
        #define HUD_ROW(name, ...) do { text_draw(ren, name, lx + 12, y, 16, 0, hud_dim, 0); \
            snprintf(t, sizeof(t), __VA_ARGS__); text_draw(ren, t, lx + lw - 12, y, 16, 1, hud_ink, 2); y += 22; } while (0)
        snprintf(t, sizeof(t), "%.1f fps", g_hs.fps);
        text_draw(ren, t, lx + 12, y - 2, 30, 1, fc, 0);
        y += 40;
        HUD_ROW("Picture", "%dx%d H.264", g_hs.w, g_hs.h);
        HUD_ROW("Connection", "%s", conn);
        HUD_ROW("Decoded / shown", "%.1f / %.1f fps", g_hs.dec_fps, g_hs.fps);
        HUD_ROW("Bitrate", "%.1f Mbps", g_hs.mbps);
        HUD_ROW("Latency, total", "%.1f ms", g_hs.pc + g_hs.con);
        HUD_ROW("  on the Switch", "%.1f ms", g_hs.con);
        HUD_ROW("  on this PC", "%.1f ms", g_hs.pc);
        HUD_ROW("Decode", "%.2f ms (max %.1f)", g_hs.dec_ms, g_hs.dec_max);
        HUD_ROW("Draw + vsync", "%.1f ms", g_hs.draw_ms);
        HUD_ROW("Worst frame gap", "%.1f ms", g_hs.gap_max);
        HUD_ROW("Frames skipped", "%ld (this second)", g_hs.skipped);
        HUD_ROW("Packets lost", "%ld", g_hs.lost);
        HUD_ROW("Keyframes", "%ld", g_hs.keyframes);
        HUD_ROW("Transfer queue max", "%d", g_hs.qmax);
        HUD_ROW("Audio", "%.0f KB/s, %.0f ms queued", g_hs.a_kbs, g_hs.a_q);
        HUD_ROW("Audio underruns", "%ld (resyncs %ld)", g_hs.underruns, g_hs.resyncs);
        HUD_ROW("Recording", "%s", rec[0] ? rec : "off");
        #undef HUD_ROW
        const int gx = lx + lw + 8, gw = 400, gh = 74;
        int gy = 10;
        const float ft[] = { 16.7f, 33.3f };
        hud_graph(ren, gx, gy, gw, gh, "FRAME TIME", "ms", &h_gap, hud_good, NULL, hud_good, 20.0f, ft, 2, 0, NULL, hud_ink, NULL, hud_ink); gy += gh + 6;
        const float lt[] = { 50.0f, 100.0f };
        hud_graph(ren, gx, gy, gw, gh, "LATENCY", "ms", &h_con, hud_blue, &h_pc, hud_warn, 40.0f, lt, 2, 0, "Switch", hud_blue, "PC", hud_warn); gy += gh + 6;
        hud_graph(ren, gx, gy, gw, gh, "FRAME SIZE", "KB", &h_kb, hud_blue, NULL, hud_blue, 20.0f, NULL, 0, 1, "keyframe", hud_bad, NULL, hud_ink); gy += gh + 6;
        const float dt[] = { 8.0f, 16.7f };
        hud_graph(ren, gx, gy, gw, gh, "DECODE TIME", "ms", &h_dec, hud_warn, NULL, hud_warn, 4.0f, dt, 2, 0, NULL, hud_ink, NULL, hud_ink); gy += gh + 6;
        hud_sgraph(ren, gx, gy, gw, gh, "FPS", "fps", &h_fps_s, hud_good, 30.0f); gy += gh + 6;
        hud_sgraph(ren, gx, gy, gw, gh, "BITRATE", "Mbps", &h_mbps_s, hud_blue, 5.0f);
    }

    SDL_RenderSetScale(ren, sx, sy);
    SDL_SetRenderDrawBlendMode(ren, old);
}
