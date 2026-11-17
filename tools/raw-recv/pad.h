/* pad.h - v0.7.20: a controller, the keyboard and the mouse on this computer
 * play the Switch.
 *
 * The console turns it on (Switch app: Beta > "Controller from the PC
 * viewer", config viewer_input = 1) and then marks its stream headers with
 * SFT_FLAG_INPUT; only then does anything go out. There it is one more Pro
 * Controller (hid:dbg), attached at the first message and let go when the
 * viewer closes, sends "no controller" or goes quiet for 3 s.
 *
 * The message, 32 bytes little-endian (tier4/.../applet_mitm_viewermsg.hpp):
 *   "SFTI", u32 flags (1 = a controller is there), u64 buttons
 *   (HidNpadButton: A B X Y L3 R3 L R ZL ZR + - Left Up Right Down = bits
 *   0-15, HOME 18, Capture 19), s32 lx ly rx ry (-32767..32767, up +y).
 *
 * Which input is which Switch button is padcfg.h (the Controller menu, top
 * left, sets it). By default a controller is by place, not by label: the
 * bottom face button is the Switch's B on every controller (Xbox A,
 * PlayStation Cross, Switch Pro B), the right one A. The main thread reads
 * the devices (SDL wants that) and hands each change to a sender thread,
 * which also repeats the last state every 100 ms so the console knows the
 * viewer is still there. While the window is not focused, everything reads
 * as let go.
 *
 * The keyboard (off by default) takes Ryujinx's keys, by place on the
 * keyboard (scancodes - WASD stays WASD on AZERTY). While it plays (the
 * console asked, the window has focus, the menu is closed), the viewer's
 * letter shortcuts rest; F11, F10 and F3 still work.
 *
 * The mouse (off by default) is taken by a click on the picture and let go
 * with F10 (or Esc, the menu, another window): its buttons and wheel press
 * Switch buttons, and its movement leans a stick by how fast it moves. */

typedef struct { uint64_t b; int32_t lx, ly, rx, ry; } pad_state;

static SDL_GameController *g_pad = NULL;
static SDL_JoystickID g_pad_id = -1;
static char g_pad_name[64] = "";
static SDL_mutex *g_pad_m = NULL;
static SDL_cond *g_pad_cv = NULL;
static SDL_Thread *g_pad_th = NULL;
static volatile int g_pad_stop = 0;
static unsigned char g_pad_msg[32];        /* the latest state, for the sender */
static int g_pad_dirty = 0;
static int g_pad_inited = 0;
static SDL_atomic_t g_pad_live;            /* 1 while the console has our controller (for the menu) */
static int g_pad_test = 0;                 /* --pad-test: SDL's virtual controller, pressed by a script (tests) */
static Uint8 g_pad_test_keys[SDL_NUM_SCANCODES];   /* --pad-test: the keyboard, as the script holds it */

static int g_pad_redraw = 0;               /* the "pressing now" strip changed: repaint (home screen) */
static int g_pad_pv_on = 0;                /* the Controller page is open: keep its preview current */
static pad_state g_pad_pv;

static int g_mouse_cap = 0;                /* the mouse is taken */
static int g_mouse_down = 0;               /* bit per PMB_LEFT..PMB_X2 */
static Uint64 g_wheel_until[2];            /* wheel up / down: a press that lasts a moment */
static double g_mdx = 0, g_mdy = 0;        /* movement since the last stick update, pixels */
static double g_mox = 0, g_moy = 0;        /* the mouse stick now, -1..1 */
static Uint64 g_mlast = 0;

static int pad_open_index(int i)
{
    SDL_GameController *c = SDL_GameControllerOpen(i);
    if (!c) return 0;
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = c;
    g_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad));
    const char *n = SDL_GameControllerName(g_pad);
    snprintf(g_pad_name, sizeof(g_pad_name), "%s", n ? n : "Controller");
    fprintf(stderr, "controller: %s\n", g_pad_name);
    return 1;
}

static int pad_count(void)
{
    int n = 0;
    if (!g_pad_inited) return 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) n++;
    return n;
}

/* the controller to use: the one named in the menu if it is connected, else
 * the one in use, else the first */
static void pad_pick(void)
{
    if (!g_pad_inited) return;
    int first = -1, pref = -1;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        if (g_pad_test && !SDL_JoystickIsVirtual(i)) continue;      /* a real pad on the test machine is not the script's */
        if (first < 0) first = i;
        const char *n = SDL_GameControllerNameForIndex(i);
        if (pref < 0 && g_pad_pref[0] && n && !strcmp(n, g_pad_pref)) pref = i;
    }
    if (pref >= 0) {
        if (!g_pad || SDL_JoystickGetDeviceInstanceID(pref) != g_pad_id) pad_open_index(pref);
    } else if (!g_pad && first >= 0) pad_open_index(first);
}

/* the menu's "Change": the next connected controller, and keep it */
static void pad_cycle(void)
{
    int idx[16], n = 0, cur = -1;
    if (!g_pad_inited) return;
    for (int i = 0; i < SDL_NumJoysticks() && n < 16; i++) {
        if (!SDL_IsGameController(i)) continue;
        if (g_pad && SDL_JoystickGetDeviceInstanceID(i) == g_pad_id) cur = n;
        idx[n++] = i;
    }
    if (n < 2) return;
    const int nx = idx[(cur + 1) % n];
    const char *nm = SDL_GameControllerNameForIndex(nx);
    if (pad_open_index(nx) && nm) snprintf(g_pad_pref, sizeof(g_pad_pref), "%s", nm);
}

static void pad_init(void)
{
    if (g_pad_inited) return;
    g_pad_inited = 1;
    /* SDL_CONTROLLER_BUTTON_A is the bottom button on Nintendo controllers too */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    /* SDL drops presses while no window has focus - a test window never has it */
    if (g_pad_test) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "controller: SDL could not start controller support (%s)\n", SDL_GetError());
        return;
    }
    g_pad_m = SDL_CreateMutex();
    g_pad_cv = SDL_CreateCond();
    if (g_pad_test && SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                SDL_CONTROLLER_BUTTON_MAX, 0) < 0)
        fprintf(stderr, "controller: no virtual controller (%s)\n", SDL_GetError());
    pad_pick();
    if (g_rd) pad_start();                 /* the connection came first (raw-view --net, no --app) */
}

/* the mouse, from events (app_events) or the test script */
static void pad_mouse_motion(double dx, double dy) { g_mdx += dx; g_mdy += dy; }

static void pad_mouse_button(int sdl_button, int down)
{
    int m = -1;
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: m = PMB_LEFT; break;
    case SDL_BUTTON_RIGHT: m = PMB_RIGHT; break;
    case SDL_BUTTON_MIDDLE: m = PMB_MIDDLE; break;
    case SDL_BUTTON_X1: m = PMB_X1; break;
    case SDL_BUTTON_X2: m = PMB_X2; break;
    }
    if (m < 0) return;
    if (down) g_mouse_down |= 1 << m; else g_mouse_down &= ~(1 << m);
}

static void pad_mouse_wheel(int y)
{
    if (y) g_wheel_until[y > 0 ? 0 : 1] = SDL_GetTicks64() + 90;
}

static int pad_mouse_ready(void)
{
    return g_pad_inited && g_pad_mouse && SDL_AtomicGet(&g_console_takes_pad);
}

static void pad_mouse_grab(int on, const char *why)
{
    if (on == g_mouse_cap) return;
    if (on) {
        if (!g_pad_test && SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
            hud_toast("Could not take the mouse");
            return;
        }
    } else if (!g_pad_test) SDL_SetRelativeMouseMode(SDL_FALSE);
    g_mouse_cap = on;
    g_mouse_down = 0;
    g_wheel_until[0] = g_wheel_until[1] = 0;
    g_mdx = g_mdy = g_mox = g_moy = 0;
    g_mlast = 0;
    if (why) hud_toast(why);
    fprintf(stderr, "\nmouse %s the Switch\n", on ? "plays" : "let go of");
}

/* --pad-test: B (the bottom button) for 0.3 s, then the left stick right and
 * the right stick down, then HOME, then ZR, then the keyboard's W + D + Z
 * (up-right, A), then nothing, then the mouse (left button, moving right) -
 * every 2 s */
static void pad_test_script(void)
{
    const Uint64 t = SDL_GetTicks64() % 2000;
    if (g_pad) {
        SDL_Joystick *j = SDL_GameControllerGetJoystick(g_pad);
        SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_A, t < 300);
        SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, t >= 300 && t < 600 ? 32767 : 0);
        SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_RIGHTY, t >= 300 && t < 600 ? 32767 : 0);
        SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_GUIDE, t >= 600 && t < 900);
        /* a virtual trigger's raw axis rests mid-way (half pressed); real ones rest at the bottom */
        SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, t >= 900 && t < 1200 ? 32767 : -32768);
        SDL_JoystickUpdate();
    }
    const Uint8 k = t >= 1200 && t < 1400;
    g_pad_test_keys[SDL_SCANCODE_W] = g_pad_test_keys[SDL_SCANCODE_D] = g_pad_test_keys[SDL_SCANCODE_Z] = k;
    if (g_pad_mouse && pad_mouse_ready() && !g_mouse_cap) pad_mouse_grab(1, NULL);
    if (g_mouse_cap) {
        const int m = t >= 1600 && t < 1900;
        pad_mouse_button(SDL_BUTTON_LEFT, m);
        if (m) pad_mouse_motion(10, 0);
    }
}

static void pad_apply_keyboard(const Uint8 *ks, pad_state *s)
{
    int dir[8] = { 0 };
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < PS_N; i++) {
            const int sc = g_bind_kb[c][i];
            if (sc <= 0 || sc >= SDL_NUM_SCANCODES || !ks[sc]) continue;
            if (i < PS_NBTN) s->b |= 1ull << ps_hidbit[i]; else dir[i - PS_NBTN] = 1;
        }
    /* a stick held by keys: full tilt, diagonals on the circle; dir = up, left, down, right (left stick, then right) */
    for (int k = 0; k < 2; k++) {
        const int *d = dir + 4 * k;
        const int x = d[3] - d[1], y = d[0] - d[2];
        if (!x && !y) continue;              /* the controller's stick, if any */
        const int32_t full = x && y ? 23170 : 32767;
        (k ? &s->rx : &s->lx)[0] = x * full;
        (k ? &s->ry : &s->ly)[0] = y * full;
    }
}

/* the keyboard plays now: on, the console asked, the window has focus, and
 * nothing else wants the keys (the menu, an address being typed) */
static int pad_keyboard_playing(SDL_Window *win, int keys_free)
{
    return g_pad_kbd && keys_free && SDL_AtomicGet(&g_console_takes_pad) &&
           (g_pad_test || !win || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS));
}

static void pad_put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

/* a stick with a small round dead zone (a resting stick drifts by a few
 * percent), the rest stretched to the full range; y up */
static void pad_stick(SDL_GameController *c, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, int32_t *ox, int32_t *oy)
{
    const double dz = 327.67 * g_pad_dz, x = SDL_GameControllerGetAxis(c, ax), y = -(double)SDL_GameControllerGetAxis(c, ay);
    const double m = SDL_sqrt(x * x + y * y);
    if (m <= dz || m <= 0) { *ox = *oy = 0; return; }
    double k = (m - dz) / (32767.0 - dz) / m * 32767.0;
    if (m * k > 32767.0) k = 32767.0 / m;
    const double vx = SDL_clamp(x * k, -32767.0, 32767.0), vy = SDL_clamp(y * k, -32767.0, 32767.0);
    *ox = (int32_t)vx;
    *oy = (int32_t)vy;
}

static void pad_apply_controller(SDL_GameController *c, pad_state *s)
{
    const int tp = (int)(327.67 * g_pad_trig);
    for (int i = 0; i < PS_NBTN; i++) {
        const int code = g_bind_ctl[i];
        if (code < 0) continue;
        int down;
        if (code == CT_TRIG_L) down = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > tp;
        else if (code == CT_TRIG_R) down = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > tp;
        else down = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)code) != 0;
        if (down) s->b |= 1ull << ps_hidbit[i];
    }
    pad_stick(c, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &s->lx, &s->ly);
    pad_stick(c, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &s->rx, &s->ry);
}

/* the mouse's part: buttons and wheel as bound; moving leans a stick by how
 * fast the mouse goes, smoothed a little, with the first notch skipped (a
 * game's own dead zone would swallow slow movement) */
static void pad_apply_mouse(pad_state *s)
{
    const Uint64 now = SDL_GetTicks64();
    for (int m = PMB_LEFT; m <= PMB_X2; m++)
        if (((g_mouse_down >> m) & 1) && g_bind_mouse[m] >= 0) s->b |= 1ull << ps_hidbit[g_bind_mouse[m]];
    for (int w = 0; w < 2; w++)
        if (now < g_wheel_until[w] && g_bind_mouse[PMB_WHEEL_UP + w] >= 0) s->b |= 1ull << ps_hidbit[g_bind_mouse[PMB_WHEEL_UP + w]];
    if (!g_mlast) g_mlast = now;
    const double dt = (double)(now - g_mlast);
    if (dt >= 8) {
        const double full = 900.0 * 50.0 / g_pad_msens;            /* pixels per second for a full tilt */
        double tx = g_mdx / dt * 1000.0 / full, ty = -g_mdy / dt * 1000.0 / full;
        if (g_pad_minvert) ty = -ty;
        const double m = SDL_sqrt(tx * tx + ty * ty);
        if (m > 0.005) {
            const double k = (m > 1.0 ? 1.0 : 0.18 + 0.82 * m) / m;
            tx *= k; ty *= k;
        }
        const double a = 1.0 - SDL_exp(-dt / 35.0);
        g_mox += (tx - g_mox) * a;
        g_moy += (ty - g_moy) * a;
        if (SDL_fabs(g_mox) < 0.003) g_mox = 0;
        if (SDL_fabs(g_moy) < 0.003) g_moy = 0;
        g_mdx = g_mdy = 0;
        g_mlast = now;
    }
    if (g_mox != 0 || g_moy != 0) {
        int32_t *px = g_pad_mstick ? &s->lx : &s->rx, *py = g_pad_mstick ? &s->ly : &s->ry;
        *px = (int32_t)(SDL_clamp(g_mox, -1.0, 1.0) * 32767.0);
        *py = (int32_t)(SDL_clamp(g_moy, -1.0, 1.0) * 32767.0);
    }
}

/* the keys that are not for binding: Esc backs out, F10 lets go of the
 * mouse, F11 is fullscreen, F3 the stats; Delete and Backspace clear */
static int pad_key_reserved(int sc)
{
    return sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F3 ||
           sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE;
}

static void pad_bound(int dev, int slot, int cell, int code)
{
    char b[96];
    const int was = dev == 1 ? pcfg_set_key(slot, cell, code) : pcfg_set_ctl(slot, code);
    if (was >= 0) snprintf(b, sizeof(b), "%s now %s (was %s)", ps_name[slot], dev == 1 ? pcfg_key_name(code) : pcfg_ct_name(code), ps_name[was]);
    else snprintf(b, sizeof(b), "%s now %s", ps_name[slot], dev == 1 ? pcfg_key_name(code) : pcfg_ct_name(code));
    hud_toast(b);
    prefs_save();
    g_pcap_dev = 0;
    g_pad_redraw = 1;
}

static void pad_bind_clear(int dev, int slot, int cell)
{
    if (dev == 1) g_bind_kb[cell][slot] = 0; else g_bind_ctl[slot] = -1;
    prefs_save();
    g_pcap_dev = 0;
    g_pad_redraw = 1;
}

/* hot-plug, binding by pressing, and the mouse while it is taken. Returns 2
 * when the event is used up, 1 when seen, 0 when not ours. */
static int pad_event(const SDL_Event *e)
{
    if (!g_pad_inited) return 0;
    if (e->type == SDL_CONTROLLERDEVICEADDED) { pad_pick(); return 1; }
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        if (g_pad && e->cdevice.which == g_pad_id) {
            fprintf(stderr, "controller: %s disconnected\n", g_pad_name);
            SDL_GameControllerClose(g_pad);
            g_pad = NULL; g_pad_id = -1; g_pad_name[0] = 0;
            pad_pick();
        }
        return 1;
    }
    if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        if (g_mouse_cap) pad_mouse_grab(0, NULL);
        g_pcap_dev = 0;
        return 0;
    }
    if (g_pcap_dev) {
        if (e->type == SDL_MOUSEBUTTONDOWN) { g_pcap_dev = 0; return 0; }      /* the click does its own thing */
        if (e->type == SDL_KEYDOWN) {
            if (e->key.repeat) return 2;
            const int sc = (int)e->key.keysym.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) { g_pcap_dev = 0; g_pad_redraw = 1; return 2; }
            if (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) { pad_bind_clear(g_pcap_dev, g_pcap_slot, g_pcap_cell); return 2; }
            if (g_pcap_dev == 1) {
                if (pad_key_reserved(sc)) hud_toast("That key is the viewer's - pick another");
                else pad_bound(1, g_pcap_slot, g_pcap_cell, sc);
            }
            return 2;
        }
        if (g_pcap_dev == 2) {
            if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.which == g_pad_id) {
                pad_bound(2, g_pcap_slot, 0, e->cbutton.button);
                return 2;
            }
            if (e->type == SDL_CONTROLLERAXISMOTION && e->caxis.which == g_pad_id && e->caxis.value > 20000 &&
                (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
                pad_bound(2, g_pcap_slot, 0, e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? CT_TRIG_L : CT_TRIG_R);
                return 2;
            }
        }
        return 0;
    }
    if (g_mouse_cap) {
        switch (e->type) {
        case SDL_MOUSEMOTION: pad_mouse_motion(e->motion.xrel, e->motion.yrel); return 2;
        case SDL_MOUSEBUTTONDOWN: pad_mouse_button(e->button.button, 1); return 2;
        case SDL_MOUSEBUTTONUP: pad_mouse_button(e->button.button, 0); return 2;
        case SDL_MOUSEWHEEL: pad_mouse_wheel(e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y); return 2;
        case SDL_KEYDOWN:
            if (e->key.keysym.scancode == SDL_SCANCODE_F10) { pad_mouse_grab(0, "Mouse is yours again"); return 2; }
            break;
        }
    }
    return 0;
}

/* Map viewer-relative stick / D-pad to console axes so motion matches a
 * rotated picture (g_orient). Inverse of the CW display rotation. */
static void pad_orient_vec(int32_t *x, int32_t *y)
{
    if (g_orient == 0 || (!*x && !*y)) return;
    const int32_t ox = *x, oy = *y;
    if (g_orient == 90) { *x = -oy; *y = ox; }
    else if (g_orient == 180) { *x = -ox; *y = -oy; }
    else if (g_orient == 270) { *x = oy; *y = -ox; }
}

static void pad_orient_dpad(uint64_t *b)
{
    if (g_orient == 0) return;
    const uint64_t L = 1ull << 12, U = 1ull << 13, R = 1ull << 14, D = 1ull << 15;
    const uint64_t m = (*b) & (L | U | R | D);
    if (!m) return;
    *b &= ~(L | U | R | D);
    const int x = !!(m & R) - !!(m & L), y = !!(m & U) - !!(m & D);
    int nx = x, ny = y;
    if (g_orient == 90) { nx = -y; ny = x; }
    else if (g_orient == 180) { nx = -x; ny = -y; }
    else if (g_orient == 270) { nx = y; ny = -x; }
    if (nx < 0) *b |= L; else if (nx > 0) *b |= R;
    if (ny > 0) *b |= U; else if (ny < 0) *b |= D;
}

static void pad_orient_apply(pad_state *s)
{
    if (g_orient == 0) return;
    pad_orient_vec(&s->lx, &s->ly);
    pad_orient_vec(&s->rx, &s->ry);
    pad_orient_dpad(&s->b);
}

/* the controller, keyboard and mouse now, as the message; 0 in flags = nothing to send */
static void pad_build(unsigned char *msg, SDL_Window *win, int keys_free)
{
    memset(msg, 0, 32);
    memcpy(msg, "SFTI", 4);
    const int pad = g_pad && g_pad_send;
    if (!pad && !g_pad_kbd && !g_pad_mouse) return;
    pad_put32(msg + 4, 1);
    if (win && !g_pad_test && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;   /* attached, nothing pressed */
    pad_state s;
    memset(&s, 0, sizeof(s));
    if (pad && g_pcap_dev != 2) pad_apply_controller(g_pad, &s);       /* not while it is being bound */
    if (pad_keyboard_playing(win, keys_free))
        pad_apply_keyboard(g_pad_test ? g_pad_test_keys : SDL_GetKeyboardState(NULL), &s);
    if (g_mouse_cap) pad_apply_mouse(&s);
    pad_orient_apply(&s);
    pad_put32(msg + 8, (uint32_t)s.b);
    pad_put32(msg + 12, (uint32_t)(s.b >> 32));
    pad_put32(msg + 16, (uint32_t)s.lx);
    pad_put32(msg + 20, (uint32_t)s.ly);
    pad_put32(msg + 24, (uint32_t)s.rx);
    pad_put32(msg + 28, (uint32_t)s.ry);
}

/* the Controller page's "pressing now": the same devices, keyboard included
 * even though the menu has the keys */
static void pad_preview(SDL_Window *win)
{
    pad_state s;
    memset(&s, 0, sizeof(s));
    if (!win || g_pad_test || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
        if (g_pad && g_pad_send && g_pcap_dev != 2) pad_apply_controller(g_pad, &s);
        if (g_pad_kbd && !g_pcap_dev) pad_apply_keyboard(g_pad_test ? g_pad_test_keys : SDL_GetKeyboardState(NULL), &s);
    }
    if (memcmp(&s, &g_pad_pv, sizeof(s)) != 0) { g_pad_pv = s; g_pad_redraw = 1; }
}

/* every pass of the main loop: a change goes to the sender at once.
 * keys_free: the menu is closed and no address is being typed */
static void pad_poll(SDL_Window *win, int keys_free)
{
    if (!g_pad_m) return;
    if (g_pad_test) pad_test_script();
    if (g_mouse_cap && !pad_mouse_ready()) pad_mouse_grab(0, NULL);
    unsigned char msg[32];
    pad_build(msg, win, keys_free);
    SDL_LockMutex(g_pad_m);
    if (memcmp(msg, g_pad_msg, sizeof(msg)) != 0) {
        memcpy(g_pad_msg, msg, sizeof(msg));
        g_pad_dirty = 1;
        SDL_CondSignal(g_pad_cv);
    }
    SDL_UnlockMutex(g_pad_m);
    if (g_pad_pv_on) pad_preview(win);
}

static int pad_send_now(const unsigned char *msg)
{
    int ok = 0;
    SDL_LockMutex(g_tx_m);
    if (g_sock != SOCK_BAD) ok = send(g_sock, (const char *)msg, 32, 0) == 32;
    else if (g_usb) {
        int n = 0;
        ok = libusb_bulk_transfer(g_usb, SFT_EP_OUT, (unsigned char *)msg, 32, &n, 50) == 0 && n == 32;
    }
    SDL_UnlockMutex(g_tx_m);
    return ok;
}

static int pad_main(void *arg)
{
    (void)arg;
    int attached = 0;                      /* the console has had a message with a controller */
    unsigned char msg[32];
    while (!g_pad_stop) {
        SDL_LockMutex(g_pad_m);
        if (!g_pad_dirty && !g_pad_stop) SDL_CondWaitTimeout(g_pad_cv, g_pad_m, 100);
        memcpy(msg, g_pad_msg, sizeof(msg));
        g_pad_dirty = 0;
        SDL_UnlockMutex(g_pad_m);
        if (g_pad_stop) break;
        if (!SDL_AtomicGet(&g_console_takes_pad) || (!g_usb && g_sock == SOCK_BAD)) {
            attached = 0;
            SDL_AtomicSet(&g_pad_live, 0);
            continue;
        }
        const int on = msg[4] & 1;
        if (!on && !attached) continue;     /* no controller, and the console has none of ours */
        if (pad_send_now(msg)) attached = on;
        SDL_AtomicSet(&g_pad_live, attached);
    }
    SDL_AtomicSet(&g_pad_live, 0);
    return 0;
}

static void pad_start(void)
{
    if (!g_pad_m || g_pad_th) return;
    g_pad_stop = 0;
    SDL_LockMutex(g_pad_m);
    g_pad_dirty = 1;                       /* the current state, first thing */
    SDL_UnlockMutex(g_pad_m);
    g_pad_th = SDL_CreateThread(pad_main, "pad-sender", NULL);
}

static void pad_stop(void)
{
    if (!g_pad_th) return;
    SDL_LockMutex(g_pad_m);
    g_pad_stop = 1;
    SDL_CondSignal(g_pad_cv);
    SDL_UnlockMutex(g_pad_m);
    SDL_WaitThread(g_pad_th, NULL);
    g_pad_th = NULL;
}

/* the page's lines under the three switches */
static const char *pad_status(char *b, size_t n)
{
    if (!g_pad_send) snprintf(b, n, "Off");
    else if (!g_pad_inited || !g_pad_m) snprintf(b, n, "Controllers are not available");
    else if (!g_pad) snprintf(b, n, "No controller connected");
    else if (SDL_AtomicGet(&g_pad_live)) snprintf(b, n, "%s - playing the Switch", g_pad_name);
    else if (g_usb || g_sock != SOCK_BAD) snprintf(b, n, "%s - turn it on in the Switch app (Beta page)", g_pad_name);
    else snprintf(b, n, "%s - ready", g_pad_name);
    return b;
}

static const char *pad_kbd_status(char *b, size_t n)
{
    if (!g_pad_kbd) snprintf(b, n, "Off - the letter keys are the viewer's shortcuts");
    else if (SDL_AtomicGet(&g_pad_live)) snprintf(b, n, "Playing the Switch - Esc opens this menu");
    else if (g_usb || g_sock != SOCK_BAD) snprintf(b, n, "Turn it on in the Switch app (Beta page)");
    else snprintf(b, n, "Ready");
    return b;
}

static const char *pad_mouse_status(char *b, size_t n)
{
    if (!g_pad_mouse) snprintf(b, n, "Off - the mouse works the viewer");
    else if (g_mouse_cap) snprintf(b, n, "The Switch has the mouse - F10 lets go");
    else if (SDL_AtomicGet(&g_pad_live)) snprintf(b, n, "Click the picture to use it - F10 lets go");
    else if (g_usb || g_sock != SOCK_BAD) snprintf(b, n, "Turn it on in the Switch app (Beta page)");
    else snprintf(b, n, "Ready");
    return b;
}

/* "A  ZR  Left stick right" - what the preview holds */
static void pad_pressed_text(char *b, size_t n)
{
    size_t o = 0;
    b[0] = 0;
    for (int i = 0; i < PS_NBTN && o + 24 < n; i++)
        if ((g_pad_pv.b >> ps_hidbit[i]) & 1) o += (size_t)snprintf(b + o, n - o, "%s%s", o ? "   " : "", ps_name[i]);
    const struct { const char *nm; int32_t x, y; } st[2] = { { "Left stick", g_pad_pv.lx, g_pad_pv.ly }, { "Right stick", g_pad_pv.rx, g_pad_pv.ry } };
    for (int i = 0; i < 2 && o + 40 < n; i++) {
        const int x = st[i].x > 9000 ? 1 : st[i].x < -9000 ? -1 : 0, y = st[i].y > 9000 ? 1 : st[i].y < -9000 ? -1 : 0;
        if (!x && !y) continue;
        o += (size_t)snprintf(b + o, n - o, "%s%s %s%s%s", o ? "   " : "", st[i].nm, y > 0 ? "up" : y < 0 ? "down" : "",
                              x && y ? " " : "", x > 0 ? "right" : x < 0 ? "left" : "");
    }
}

static void act_pad_kbd(int on)
{
    g_pad_kbd = on != 0;
    prefs_save();
    hud_toast(g_pad_kbd ? "Keyboard plays the Switch - Esc opens Settings" : "Keyboard shortcuts are back");
    fprintf(stderr, "\nkeyboard to the Switch %s\n", g_pad_kbd ? "on" : "off");
}

static void act_pad_send(int on)
{
    g_pad_send = on != 0;
    prefs_save();
    hud_toast(g_pad_send ? "Controller plays the Switch" : "Controller stays on this computer");
    fprintf(stderr, "\ncontroller to the Switch %s\n", g_pad_send ? "on" : "off");
}

static void act_pad_mouse(int on)
{
    g_pad_mouse = on != 0;
    if (!g_pad_mouse) pad_mouse_grab(0, NULL);
    prefs_save();
    hud_toast(g_pad_mouse ? "Mouse plays the Switch - click the picture" : "The mouse works the viewer");
    fprintf(stderr, "\nmouse to the Switch %s\n", g_pad_mouse ? "on" : "off");
}

static void act_pad_defaults(int what)     /* 0 keyboard, 1 mouse, 2 controller */
{
    if (what == 0) pcfg_defaults_kb();
    else if (what == 1) pcfg_defaults_mouse();
    else { pcfg_defaults_ctl(); pcfg_defaults_ctl_options(); }
    g_pcap_dev = 0;
    prefs_save();
    hud_toast("Back to the defaults");
}
