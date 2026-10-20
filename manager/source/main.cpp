/*
 * Switch Frame Tap - the manager app (hbmenu). Settings, status and a setup
 * check for the streaming sysmodule, in the spirit of SysDVR's settings app.
 *
 *  - Settings live in sdmc:/config/switch-frame-tap/config.ini (the format
 *    tier4/applet-mitm/source/applet_mitm_armfile.hpp BuildReleaseArm reads).
 *    Every change is written at once and, if the sysmodule is running, it is
 *    told to reload (sftap command 2): a stream in progress restarts with it.
 *  - Stream on/off goes through sftap (command 1) when the sysmodule runs,
 *    else the file it reads at boot (stream-off).
 *  - Start with the console: the launcher's flags/boot2.flag (v0.7.13: the
 *    donor, C21, starts the sysmodule once the system has booted).
 *
 * SDL2 for drawing, the console's own shared fonts through SDL2_ttf, libnx
 * pad/touch for input.
 */
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>
#include "../../common/sound_patch.hpp"
#include "../../common/capture_mode.hpp"
#include "../../tier4/applet-mitm/source/shot_buttons.hpp"
#include "extras.hpp"
#include <unordered_map>

namespace {

    constexpr const char *AppVersion  = "0.7.14-beta1";
    constexpr u64 ModuleTid           = 0x0100000000000C20ull;
    constexpr const char *ModuleDir   = "sdmc:/atmosphere/contents/0100000000000C20";
    constexpr const char *ModuleNsp   = "sdmc:/atmosphere/contents/0100000000000C20/exefs.nsp";
    /* v0.7.13: the launcher (the donor, C21) holds the boot flag and starts the
     * sysmodule once the system has booted; C20's own flag is an older install's */
    constexpr const char *BootFlag    = "sdmc:/atmosphere/contents/0100000000000C21/flags/boot2.flag";
    constexpr const char *OldBootFlag = "sdmc:/atmosphere/contents/0100000000000C20/flags/boot2.flag";
    constexpr const char *LauncherDir = "sdmc:/atmosphere/contents/0100000000000C21";
    constexpr const char *LauncherTxt = "sdmc:/config/switch-frame-tap/launcher.txt";
    constexpr const char *ConfigDir   = "sdmc:/config/switch-frame-tap";
    constexpr const char *ConfigIni   = "sdmc:/config/switch-frame-tap/config.ini";
    constexpr const char *StreamOff   = "sdmc:/config/switch-frame-tap/stream-off";
    constexpr const char *ExclFile    = "sdmc:/config/switch-frame-tap/excluded.txt";
    constexpr const char *CaptureIni  = sftcapture::IniPath;

    /* v0.1.1b: the never-attach list (one hex program id per line) */
    size_t CountExcluded() {
        size_t n = 0;
        if (FILE *f = std::fopen(ExclFile, "rb")) {
            char line[64];
            while (std::fgets(line, sizeof(line), f)) { if (std::strlen(line) >= 16) { ++n; } }
            std::fclose(f);
        }
        return n;
    }
    constexpr const char *TestArmFile = "sdmc:/applet-mitm.armed";
    constexpr const char *ShotDir     = "sdmc:/switch/switch-frame-tap/screenshots";

    /* v0.7.11: the system's screenshot folder, as the sysmodule found it
     * (Nintendo/Album, or the redirected one under emuMMC) */
    std::string AlbumRoot() {
        std::string r = "sdmc:/Nintendo/Album";
        if (FILE *f = std::fopen("sdmc:/config/switch-frame-tap/album_path.txt", "rb")) {
            char b[0x110] = "";
            if (std::fgets(b, sizeof(b), f)) {
                std::string l = b;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) { l.pop_back(); }
                if (l.rfind("sdmc:/", 0) == 0) { r = l; }
            }
            std::fclose(f);
        }
        return r;
    }

    std::string BaseName(const std::string &p) { const size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }

    /* when it was taken, as YYYYMMDDHHMMSSnn, from either name:
     * "2026-09-29_17-27-10.png" or "2026092917271000-SwitchFrameTap.png" */
    std::string ShotKey(const std::string &path) {
        const std::string f = BaseName(path);
        if (f.size() >= 23 && f[10] == '_') {
            std::string k;
            for (char c : f.substr(0, 19)) { if (c >= '0' && c <= '9') { k += c; } }
            return k + (f.size() > 23 ? f.substr(19, f.size() - 23) : "00");
        }
        return f.substr(0, 16);
    }

    /* the screenshots, newest first: Switch Frame Tap's folder and (v0.7.11)
     * the ones saved next to the system's, Album/YYYY/MM/DD/*-SwitchFrameTap.png */
    std::vector<std::string> ListShots() {
        std::vector<std::string> v;
        auto scan = [&v](const std::string &dir, const char *suffix) {
            if (DIR *d = opendir(dir.c_str())) {
                const size_t sl = std::strlen(suffix);
                while (dirent *e = readdir(d)) {
                    const size_t n = std::strlen(e->d_name);
                    if (n > sl && std::strcmp(e->d_name + n - sl, suffix) == 0) { v.push_back(dir + "/" + e->d_name); }
                }
                closedir(d);
            }
        };
        auto subdirs = [](const std::string &dir) {
            std::vector<std::string> out;
            if (DIR *d = opendir(dir.c_str())) {
                while (dirent *e = readdir(d)) {
                    if (e->d_name[0] != '.' && std::strlen(e->d_name) <= 4 && std::isdigit(static_cast<unsigned char>(e->d_name[0]))) { out.push_back(dir + "/" + e->d_name); }
                }
                closedir(d);
            }
            return out;
        };
        scan(ShotDir, ".png");
        for (const auto &y : subdirs(AlbumRoot())) {
            for (const auto &m : subdirs(y)) {
                for (const auto &d : subdirs(m)) { scan(d, "-SwitchFrameTap.png"); }
            }
        }
        std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) { return ShotKey(a) > ShotKey(b); });
        return v;
    }

    /* "2026-09-29  17:27:10", from either name */
    std::string ShotLabel(const std::string &path) {
        const std::string k = ShotKey(path);
        if (k.size() >= 14) {
            std::string l = k.substr(0, 4) + "-" + k.substr(4, 2) + "-" + k.substr(6, 2) + "  " + k.substr(8, 2) + ":" + k.substr(10, 2) + ":" + k.substr(12, 2);
            if (k.size() >= 16 && k.substr(14, 2) != "00") { l += " (" + k.substr(14, 2) + ")"; }
            if (BaseName(path).find("-SwitchFrameTap") != std::string::npos) { l += "  Album"; }
            return l;
        }
        return BaseName(path);
    }

    bool FileExists(const char *p) { struct stat st; return stat(p, &st) == 0; }

    bool TouchFile(const char *p) {
        FILE *f = std::fopen(p, "wb");
        if (f == nullptr) { return false; }
        std::fclose(f);
        return true;
    }

    /* ---- the sysmodule's control service (applet_mitm_control.hpp) ------ */

    struct StreamStatus {
        u32 version, state, enabled, width, height, fps_x10, game_fps_x10, sessions;
        u32 shot_enabled, shots, shot_fails, reserved;     /* version 2 (M99) */
        u64 app_tid; u32 app_excluded, reserved3;          /* version 3 (v0.1.1b) */
    };

    /* v0.7.15: sftap 6 */
    struct CaptureInfo {
        u32 version, mode, requested, candidates, live, dead;
        u64 tid;
        u32 overrides, composite, reserved;
    };
    static_assert(sizeof(CaptureInfo) == 48);

    Service g_sftap;
    bool g_sftap_open = false;

    bool ModuleRunning() {
        u64 pid = 0;
        return R_SUCCEEDED(pmdmntGetProcessId(&pid, ModuleTid));
    }

    /* only once the module's process exists: sm would otherwise defer the
     * request until someone registers "sftap" - forever, without the module */
    bool SftapConnect() {
        if (g_sftap_open) { return true; }
        if (!ModuleRunning()) { return false; }
        g_sftap_open = R_SUCCEEDED(smGetService(&g_sftap, "sftap"));
        return g_sftap_open;
    }

    bool SftapStatus(StreamStatus *st) {
        if (!SftapConnect()) { return false; }
        if (R_FAILED(serviceDispatchOut(&g_sftap, 0, *st))) { serviceClose(&g_sftap); g_sftap_open = false; return false; }
        return true;
    }

    bool SftapSetEnabled(bool on) {
        if (!SftapConnect()) { return false; }
        const u8 v = on ? 1 : 0;
        return R_SUCCEEDED(serviceDispatchIn(&g_sftap, 1, v));
    }

    void SftapReload() {
        if (SftapConnect()) { serviceDispatch(&g_sftap, 2); }
    }

    bool SftapCaptureInfo(CaptureInfo *ci) {
        if (!SftapConnect()) { return false; }
        if (R_FAILED(serviceDispatchOut(&g_sftap, 6, *ci))) { return false; }
        return true;
    }

    std::string ReadTextFile(const char *path) {
        std::string s;
        if (FILE *f = std::fopen(path, "rb")) {
            char b[1024];
            size_t n;
            while ((n = std::fread(b, 1, sizeof(b), f)) > 0) { s.append(b, n); }
            std::fclose(f);
        }
        return s;
    }

    std::vector<sftcapture::Override> LoadCaptureOverridesFile() {
        std::vector<sftcapture::Override> out(sftcapture::MaxOverrides);
        const std::string text = ReadTextFile(CaptureIni);
        out.resize(sftcapture::ParseIni(text.data(), text.size(), out.data(), out.size()));
        return out;
    }

    bool SaveCaptureOverridesFile(const std::vector<sftcapture::Override> &ov) {
        mkdir("sdmc:/config", 0777);
        mkdir(ConfigDir, 0777);
        std::string text =
            "# Switch Frame Tap per-title capture modes - written by the manager app\n"
            "# <title id> = auto | standard | composite | layer\n";
        char line[64];
        for (const auto &o : ov) {
            const size_t n = sftcapture::FormatLine(line, sizeof(line), o.tid, o.mode);
            if (n != 0) { text.append(line, n); }
        }
        FILE *f = std::fopen(CaptureIni, "wb");
        if (f == nullptr) { return false; }
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        return ok;
    }

    /* ---- settings (config.ini) ------------------------------------------ */

    const char *const QualityNames[]  = { "High", "Medium", "Low" };
    const char *const ConnKeys[]      = { "usb", "webcam", "webcam-any", "network" };
    const char *const ConnNames[]     = { "USB - PC viewer", "USB - webcam (OBS)", "USB - webcam (any app)", "Network" };
    constexpr int ConnCount = 4, ConnNetwork = 3;
    const char *const QualityKeys[]   = { "high", "medium", "low" };
    const int KeyframeFrames[]        = { 30, 60, 120, 240 };
    const char *const KeyframeNames[] = { "Every 0.5 s", "Every second", "Every 2 s", "Every 4 s" };
    const int DelaySeconds[]          = { 5, 10, 20, 30, 60 };
    const char *const ComboNames[]    = { "L3 + R3 (click both sticks)", "L + R + D-pad Down", "ZL + ZR + D-pad Down", "Minus + D-pad Down", "Custom" };
    constexpr int ComboCustom = 4;   /* v0.7.11: screenshot_custom */

    struct Settings {
        int quality = 0;        /* index into QualityKeys */
        int keyframe = 1;       /* index into KeyframeFrames */
        bool cap720 = false;
        int delay = 2;          /* index into DelaySeconds */
        bool any_fw = false;
        bool shot = false;
        int shot_combo = 0;     /* index into ComboNames */
        uint32_t shot_mask = 0;   /* v0.7.11: the custom combo's buttons (HidNpadButton bits 0-15) */
        bool shot_capture = false;/* v0.7.11: ...and the Capture button */
        bool audio = true;      /* v0.3 */
        bool shot_album = false;  /* v0.7.11: screenshots next to the system's (Album) */
        bool detach_home = true;  /* v0.7.11 (PR #13): end the stream on HOME; kept when saving */
        bool screen_off = false;  /* v0.7.12: the console's screen dark while streaming */
        std::string early = "1";  /* v0.7.12: early_collect (1 / 0 / ab); kept when saving */
        bool home_menu = false;   /* v0.7.14: stream the HOME menu and system applets too (beta); from the next boot */
        int conn = 0;           /* v0.7: connection - 0 USB (PC viewer), 1 USB webcam, 2 (v0.7.7) USB webcam NV12, 3 network; from the next boot */
        u32 capture_mode = sftcapture::Auto;   /* v0.7.15: Auto/Standard/Compositor/Layer, overridable per title */
    };

    bool IniGet(const std::string &ini, const char *key, std::string *out) {
        size_t i = 0;
        const size_t klen = std::strlen(key);
        while (i < ini.size()) {
            size_t e = ini.find('\n', i);
            if (e == std::string::npos) { e = ini.size(); }
            std::string line = ini.substr(i, e - i);
            i = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#' || line[a] == ';' || line[a] == '[') { continue; }
            const size_t eq = line.find('=', a);
            if (eq == std::string::npos) { continue; }
            size_t kend = eq;
            while (kend > a && (line[kend - 1] == ' ' || line[kend - 1] == '\t')) { --kend; }
            if (kend - a != klen || line.compare(a, klen, key) != 0) { continue; }
            const size_t v = line.find_first_not_of(" \t", eq + 1);
            *out = v == std::string::npos ? "" : line.substr(v);
            return true;
        }
        return false;
    }

    Settings LoadSettings() {
        Settings s;
        std::string ini;
        if (FILE *f = std::fopen(ConfigIni, "rb")) {
            char buf[1024];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { ini.append(buf, n); }
            std::fclose(f);
        }
        std::string v;
        if (IniGet(ini, "quality", &v)) { for (int i = 0; i < 3; ++i) { if (v == QualityKeys[i]) { s.quality = i; } } }
        if (IniGet(ini, "keyframe_interval", &v)) { for (int i = 0; i < 4; ++i) { if (std::atoi(v.c_str()) == KeyframeFrames[i]) { s.keyframe = i; } } }
        if (IniGet(ini, "max_resolution", &v)) { s.cap720 = v == "720"; }
        if (IniGet(ini, "start_delay", &v)) { for (int i = 0; i < 5; ++i) { if (std::atoi(v.c_str()) == DelaySeconds[i]) { s.delay = i; } } }
        if (IniGet(ini, "allow_untested_firmware", &v)) { s.any_fw = v == "1"; }
        if (IniGet(ini, "screenshot", &v)) { s.shot = v == "1"; }
        if (IniGet(ini, "screenshot_location", &v)) { s.shot_album = v == "album"; }
        if (IniGet(ini, "detach_on_home", &v)) { s.detach_home = v != "0"; }
        if (IniGet(ini, "screen_off", &v)) { s.screen_off = v == "1"; }
        if (IniGet(ini, "early_collect", &v) && (v == "0" || v == "1" || v == "ab")) { s.early = v; }
        if (IniGet(ini, "audio", &v)) { s.audio = v != "0"; }
        if (IniGet(ini, "home_menu", &v)) { s.home_menu = v == "1"; }
        if (IniGet(ini, "capture_mode", &v)) { u32 m = sftcapture::Auto; if (sftcapture::Parse(v.data(), v.size(), &m)) { s.capture_mode = m; } }
        /* the v0.4-v0.7 keys first, then the one that replaced them */
        if (IniGet(ini, "usb_mode", &v) && v == "webcam") { s.conn = 1; }
        if (IniGet(ini, "network", &v) && v == "1") { s.conn = ConnNetwork; }
        if (IniGet(ini, "connection", &v)) { s.conn = v == "network" ? ConnNetwork : v == "webcam-any" ? 2 : v == "webcam" ? 1 : 0; }
        if (IniGet(ini, "screenshot_buttons", &v)) { const int k = std::atoi(v.c_str()); if (k >= 0 && k <= ComboCustom) { s.shot_combo = k; } }
        if (IniGet(ini, "screenshot_custom", &v)) { s.shot_mask = sft::shotbuttons::Parse(v.c_str(), &s.shot_capture); }
        return s;
    }

    bool SaveSettings(const Settings &s) {
        mkdir("sdmc:/config", 0777);
        mkdir(ConfigDir, 0777);
        FILE *f = std::fopen(ConfigIni, "wb");
        if (f == nullptr) { return false; }
        char custom[96];
        sft::shotbuttons::Format(s.shot_mask, s.shot_capture, custom, sizeof(custom));
        std::fprintf(f,
            "# Switch Frame Tap settings - written by the manager app; editing by hand is fine.\n"
            "# quality: high | medium | low  (high: the best picture and the most USB bandwidth)\n"
            "quality = %s\n"
            "# keyframe_interval: frames between keyframes (60 = once a second at 60 fps)\n"
            "keyframe_interval = %d\n"
            "# max_resolution: 1080 (native) | 720\n"
            "max_resolution = %s\n"
            "# start_delay: seconds after boot before streaming can start\n"
            "start_delay = %d\n"
            "# allow_untested_firmware: 1 streams on firmware this version was not tested on\n"
            "allow_untested_firmware = %d\n"
            "# screenshot: 1 = a button combo saves a native-resolution PNG to /switch/switch-frame-tap/screenshots\n"
            "screenshot = %d\n"
            "# screenshot_buttons: 0 = L3 + R3, 1 = L + R + D-pad Down, 2 = ZL + ZR + D-pad Down, 3 = Minus + D-pad Down,\n"
            "#                     4 = screenshot_custom (A B X Y L R ZL ZR L3 R3 Plus Minus Up Down Left Right Capture)\n"
            "screenshot_buttons = %d\n"
            "screenshot_custom = %s\n"
            "# screenshot_location: app (/switch/switch-frame-tap/screenshots) | album (next to the system's screenshots,\n"
            "#                      Nintendo/Album/YYYY/MM/DD - under emuMMC its own Nintendo folder)\n"
            "screenshot_location = %s\n"
            "# detach_on_home: 1 = pressing HOME ends the stream and frees its memory | 0 = keep showing the paused game\n"
            "detach_on_home = %d\n"
            "# screen_off: 1 = the console's screen goes dark while a stream runs (back on when it stops, or with HOME)\n"
            "screen_off = %d\n"
            "# early_collect: 1 = send a finished 1080p frame without waiting for the next one | 0 = the old way | ab = test both\n"
            "early_collect = %s\n"
            "# audio: 1 = send the game's sound to the PC viewer\n"
            "audio = %d\n"
            "# connection: usb (the PC viewer over USB) | webcam (a USB camera for OBS; no sound) |\n"
            "#             webcam-any (an uncompressed USB camera for any camera app; no sound) |\n"
            "#             network (the PC viewer over Wi-Fi or the dock's LAN port - play docked). Read at boot.\n"
            "connection = %s\n"
            "# home_menu: 1 = also stream the HOME menu and the system applets when no game is on screen (beta). Read at boot.\n"
            "home_menu = %d\n"
            "# capture_mode: auto | standard | composite | layer. Overrides per title live in capture.ini.\n"
            "capture_mode = %s\n",
            QualityKeys[s.quality], KeyframeFrames[s.keyframe], s.cap720 ? "720" : "1080",
            DelaySeconds[s.delay], s.any_fw ? 1 : 0, s.shot ? 1 : 0, s.shot_combo, custom, s.shot_album ? "album" : "app", s.detach_home ? 1 : 0, s.screen_off ? 1 : 0, s.early.c_str(),
            s.audio ? 1 : 0, ConnKeys[s.conn], s.home_menu ? 1 : 0, sftcapture::Key(s.capture_mode));
        std::fclose(f);
        return true;
    }

    /* ---- drawing --------------------------------------------------------- */

    constexpr int ScreenW = 1280, ScreenH = 720;
    const SDL_Color ColText   = { 0xE8, 0xE9, 0xEC, 0xFF };
    const SDL_Color ColDim    = { 0x9A, 0xA0, 0xAA, 0xFF };
    const SDL_Color ColAccent = { 0x00, 0xB8, 0xE6, 0xFF };
    const SDL_Color ColGood   = { 0x4C, 0xD9, 0x64, 0xFF };
    const SDL_Color ColWarn   = { 0xFF, 0xCC, 0x00, 0xFF };
    const SDL_Color ColBad    = { 0xFF, 0x4B, 0x3E, 0xFF };

    SDL_Renderer *g_ren = nullptr;
    TTF_Font *g_font[4] = {};      /* 20, 24, 28, 38 px */
    TTF_Font *g_ext = nullptr;     /* the Nintendo button glyphs, 26 px */
    const int FontPx[4] = { 20, 24, 28, 38 };

    struct CachedText { SDL_Texture *tex; int w, h; };
    std::unordered_map<std::string, CachedText> g_cache;

    const CachedText *Render(const std::string &s, int size, SDL_Color c, int wrap = 0, bool ext = false) {
        if (s.empty()) { return nullptr; }
        char key[48];
        std::snprintf(key, sizeof(key), "%d|%d|%02x%02x%02x|%d|", size, wrap, c.r, c.g, c.b, ext ? 1 : 0);
        const std::string k = key + s;
        auto it = g_cache.find(k);
        if (it != g_cache.end()) { return &it->second; }
        if (g_cache.size() > 400) {
            for (auto &e : g_cache) { SDL_DestroyTexture(e.second.tex); }
            g_cache.clear();
        }
        TTF_Font *f = ext ? g_ext : g_font[size];
        SDL_Surface *surf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), c, wrap) : TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if (surf == nullptr) { return nullptr; }
        CachedText t = { SDL_CreateTextureFromSurface(g_ren, surf), surf->w, surf->h };
        SDL_FreeSurface(surf);
        return &g_cache.emplace(k, t).first->second;
    }

    /* align: 0 left, 1 centre, 2 right; y is the top */
    int Text(const std::string &s, int x, int y, int size, SDL_Color c, int align = 0, int wrap = 0, bool ext = false) {
        const CachedText *t = Render(s, size, c, wrap, ext);
        if (t == nullptr) { return 0; }
        SDL_Rect r = { x - (align == 1 ? t->w / 2 : align == 2 ? t->w : 0), y, t->w, t->h };
        SDL_RenderCopy(g_ren, t->tex, nullptr, &r);
        return t->h;
    }

    void Fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
        SDL_Rect r = { x, y, w, h };
        SDL_RenderFillRect(g_ren, &r);
    }

    /* ---- the list ------------------------------------------------------- */

    enum class Kind { Header, Info, Setting };

    struct Row {
        Kind kind;
        std::string label;
        std::function<std::string()> value;      /* what is shown on the right of the row */
        std::function<SDL_Color()> color;        /* its colour (nullptr: text colour) */
        std::function<void(int)> change;         /* +1 / -1 (A and Right = +1); nullptr: not changeable */
        std::string help;
    };

    bool g_open_gallery = false;
    bool g_record_combo = false;   /* v0.7.11: the custom screenshot combo's recording screen */
    const extras::Tool *g_pending_install = nullptr;   /* v0.7.5: Get extras - run from the main loop */

    struct App {
        Settings set = LoadSettings();
        StreamStatus st = {};
        bool running = false, have_status = false, installed = false, boot = false, test_install = false;
        bool stream_file_on = true;
        size_t shot_files = 0, excluded = 0;
        bool confirm_clear = false;
        bool confirm_capture_clear = false;                 /* v0.7.15: two-step clear of capture.ini */
        std::vector<sftcapture::Override> capture_overrides = LoadCaptureOverridesFile();
        CaptureInfo ci = {};
        bool have_ci = false;
        sftsound::State sound = sftsound::State::Missing;   /* v0.7.4: the bundled dvr-patches */
        int confirm_extra = -1;                              /* v0.7.5: the Get extras row waiting for its second A */
        bool sound_changed = false;                          /* toggled since this app started: a restart applies it */
        u32 fw = 0;
        u64 ams = 0;
        std::string toast;
        u32 toast_frames = 0;
        std::vector<Row> rows;
        int sel = 0, scroll = 0;

        void Refresh() {
            installed = FileExists(ModuleNsp);
            boot = FileExists(BootFlag) || FileExists(OldBootFlag);
            test_install = FileExists(TestArmFile);
            stream_file_on = !FileExists(StreamOff);
            shot_files = ListShots().size();
            excluded = CountExcluded();
            sound = sftsound::Get();
            running = ModuleRunning();
            have_status = running && SftapStatus(&st);
            have_ci = running && SftapCaptureInfo(&ci);
        }

        void Toast(const std::string &s) { toast = s; toast_frames = 180; }

        std::string CustomComboText() const {
            char b[96];
            sft::shotbuttons::Format(set.shot_mask, set.shot_capture, b, sizeof(b));
            if (b[0] == 0) { return "Custom (not recorded yet)"; }
            std::string t = b;
            for (size_t i = 0; (i = t.find('+', i)) != std::string::npos; i += 3) { t.replace(i, 1, " + "); }
            return t;
        }

        void Saved() {
            if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
            if (test_install) { Toast("Saved - but a test arm file is installed, so the sysmodule ignores config.ini"); return; }
            if (have_status) { SftapReload(); Toast("Saved - the stream picks it up right away"); }
            else { Toast("Saved - used from the next boot"); }
        }

        void SaveCaptureOverrides() {
            if (!SaveCaptureOverridesFile(capture_overrides)) { Toast("Could not write capture.ini"); return; }
            if (test_install) { Toast("Saved - a test install reads capture.ini at boot"); return; }
            if (have_status) { SftapReload(); Toast("Saved - the next stream uses it"); }
            else { Toast("Saved - used from the next boot"); }
        }

        std::string StreamState() {
            if (!installed) { return "Not installed"; }
            if (!running) {
                if (!boot) { return "Off at boot (see below)"; }
                /* v0.7.13: the launcher's word on this boot */
                if (FILE *f = std::fopen(LauncherTxt, "r")) {
                    char b[1024] = {};
                    const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
                    std::fclose(f);
                    b[n] = 0;
                    if (std::strstr(b, "result=low-memory")) { return "Not started: other sysmodules use the memory it needs"; }
                }
                return "Not running yet - it starts ~30 s after boot";
            }
            if (!have_status) { return "Running (an older version?)"; }
            char b[96];
            switch (st.state) {
                case 0: return "Starting - waits for a game";
                case 1: return "Off";
                case 2: return RunningWebcam() ? "Waiting for a camera app on the PC" : "Waiting for the PC viewer";
                case 4: if (NetworkViewer()) { std::snprintf(b, sizeof(b), "Streaming %ux%u over the network, %u.%u fps", st.width, st.height, st.fps_x10 / 10, st.fps_x10 % 10); return b; }
                        std::snprintf(b, sizeof(b), "Streaming %ux%u, %u.%u fps", st.width, st.height, st.fps_x10 / 10, st.fps_x10 % 10); return b;
                case 3: return "Waiting for a game";
                case 5: return "Encoder stalled - reboot to stream again";
                case 6: return "Test install without live mode";
                case 7: return "Firmware not tested - streaming is off";
                case 8: return "Not streaming this app (excluded)";
                default: return "Unknown";
            }
        }

        SDL_Color StreamColor() {
            if (!installed || (running && have_status && (st.state == 5 || st.state == 7))) { return ColBad; }
            if (!running || !have_status) { return ColWarn; }
            return st.state == 4 ? ColGood : ColText;
        }

        /* v0.4: the mode this boot is in (the sysmodule's word), else config.ini */
        /* the connection this boot runs with (the sysmodule's word), else config.ini */
        int RunningConn() { return have_status ? ((st.reserved3 & 2) ? ConnNetwork : (st.reserved3 & 1) ? ((st.reserved3 & 8) ? 2 : 1) : 0) : set.conn; }
        bool RunningWebcam() { return RunningConn() == 1; }
        bool RunningHomeMenu() { return have_status ? (st.reserved3 & 16) != 0 : set.home_menu; }   /* v0.7.14 */
        /* v0.7: the network transport this boot, and whether a viewer is on it */
        bool RunningNetwork() { return RunningConn() == ConnNetwork; }
        bool NetworkViewer() { return have_status && (st.reserved3 & 4) != 0; }
        std::string IpString() {
            u32 ip = 0;
            if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) { return "offline"; }
            char b[20];
            std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
            return b;
        }

        bool StreamOn() { return have_status ? st.enabled != 0 : stream_file_on; }

        void SetStream(bool on) {
            if (have_status) {
                if (!SftapSetEnabled(on)) { Toast("The sysmodule did not answer"); }
                else { Toast(on ? "Streaming turned on" : "Streaming turned off"); }
            } else {
                mkdir("sdmc:/config", 0777);
                mkdir(ConfigDir, 0777);
                if (on) { std::remove(StreamOff); } else { TouchFile(StreamOff); }
                Toast(on ? "Streaming on from the next boot" : "Streaming off from the next boot");
            }
        }

        void SetBoot(bool on) {
            if (!installed) { Toast("The sysmodule is not installed"); return; }
            std::remove(OldBootFlag);
            if (on) {
                mkdir(LauncherDir, 0777);
                mkdir((std::string(LauncherDir) + "/flags").c_str(), 0777);
                if (!TouchFile(BootFlag)) { Toast("Could not write the boot flag"); return; }
            } else {
                std::remove(BootFlag);
            }
            Toast(on ? "Starts with the console from the next boot" : "Will not start at the next boot");
        }

        static std::string Check(bool ok) { return ok ? "Installed" : "Not found"; }

        void Build() {
            rows.clear();
            rows.push_back({ Kind::Header, "Status", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Info, "Stream", [this] { return StreamState(); }, [this] { return StreamColor(); }, nullptr,
                "What the sysmodule is doing right now. It streams whenever the PC viewer is open and a game is running. "
                "Open the viewer on the PC and connect the USB-C cable." });
            rows.push_back({ Kind::Info, "Firmware", [this] {
                    char b[64];
                    std::snprintf(b, sizeof(b), "%u.%u.%u%s", HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw), (HOSVER_MAJOR(fw) >= 21 && HOSVER_MAJOR(fw) <= 23) ? "" : " - untested");
                    return std::string(b);
                }, [this] { return (HOSVER_MAJOR(fw) >= 21 && HOSVER_MAJOR(fw) <= 23) ? ColText : ColWarn; }, nullptr,
                "This version was tested on firmware 22.x and 21.x (Atmosphere 1.11). On other firmware the sysmodule does not "
                "stream unless 'Allow untested firmware' is on." });
            rows.push_back({ Kind::Info, "Atmosphere", [this] {
                    char b[48];
                    if (ams == 0) { return std::string("Unknown"); }
                    std::snprintf(b, sizeof(b), "%u.%u.%u", static_cast<u32>((ams >> 56) & 0xFF), static_cast<u32>((ams >> 48) & 0xFF), static_cast<u32>((ams >> 40) & 0xFF));
                    return std::string(b);
                }, nullptr, nullptr, "The custom firmware this runs on." });

            rows.push_back({ Kind::Header, "Streaming", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Setting, "Stream to PC", [this] { return std::string(StreamOn() ? "On" : "Off"); },
                [this] { return StreamOn() ? ColGood : ColDim; }, [this](int) { SetStream(!StreamOn()); },
                "Off: the sysmodule leaves the game alone (no debugging, cheats work) and sends nothing. "
                "The same switch as in the overlay; it is remembered across reboots." });
            rows.push_back({ Kind::Setting, "Start with the console", [this] { return std::string(!installed ? "Not installed" : boot ? "On" : "Off"); },
                [this] { return boot ? ColGood : ColDim; }, [this](int) { SetBoot(!boot); },
                "Whether the sysmodule starts with the console - about 30 s after boot, once the HOME menu is up, and only if "
                "the other sysmodules leave it the memory it needs. Takes effect at the next boot. "
                "Off is the clean way to disable it completely." });
            rows.push_back({ Kind::Setting, "Connection", [this] {
                    std::string v = ConnNames[set.conn];
                    if (have_status && RunningConn() != set.conn) { v += " - after a reboot"; }
                    else if (set.conn == ConnNetwork) { v += " - " + IpString(); }
                    return v;
                }, [this] { return (have_status && RunningConn() != set.conn) ? ColWarn : ColText; },
                [this](int d) {
                    set.conn = (set.conn + (d > 0 ? 1 : ConnCount - 1)) % ConnCount;
                    if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
                    Toast(test_install ? "Saved - but a test arm file is installed, so the sysmodule ignores config.ini"
                                       : "Saved - restart the console to switch the connection");
                },
                "How the stream reaches the PC - one at a time. USB - PC viewer: the viewer app over the USB-C cable "
                "(handheld; sound, the lowest latency). USB - webcam (OBS): the Switch shows up as a USB camera, "
                "H.264 up to 1080p60, for OBS - picture only. USB - webcam (any app): an uncompressed camera the "
                "Windows Camera app, browsers, Discord or Zoom can open - 720p at 25 fps or 432p at 60. Network: the viewer app over Wi-Fi or the dock's LAN port, so you can play docked; set "
                "the viewer to Network too (Tab in the viewer). A LAN adapter or strong 5 GHz Wi-Fi at Medium quality "
                "works best. Changes need a reboot." });

            rows.push_back({ Kind::Setting, "Apps never streamed", [this] {
                    char b[32];
                    std::snprintf(b, sizeof(b), confirm_clear ? "Press A to clear" : "%zu", excluded);
                    return std::string(b);
                }, [this] { return confirm_clear ? ColWarn : ColText; }, [this](int) {
                    if (excluded == 0) { Toast("No app is excluded"); return; }
                    if (!confirm_clear) { confirm_clear = true; return; }
                    confirm_clear = false;
                    std::remove(ExclFile);
                    if (have_status) { SftapReload(); }
                    Toast("Every app is streamed again");
                },
                "Apps that refuse to run under a debugger - such as TiCo's protected builds - need the sysmodule to leave "
                "them alone. Turn \"Stream this app\" off in the overlay while one runs, and it is never attached again "
                "(no stream, no screenshots). A here clears the list." });

            rows.push_back({ Kind::Setting, "Game audio", [this] {
                    if (!set.audio) { return std::string("Off"); }
                    if (have_status && st.version >= 4) {
                        switch (st.reserved & 0xFF) {
                            case 1: return std::string("On - playing");
                            case 2: return std::string("On - recorder unavailable");
                            case 3: return std::string("On - could not start");
                        }
                    }
                    return std::string("On");
                }, [this] { return !set.audio ? ColDim : (have_status && st.version >= 4 && (st.reserved & 0xFF) >= 2) ? ColWarn : ColGood; },
                [this](int) { set.audio = !set.audio; Saved(); },
                "Sends the game's sound to the PC viewer (M in the viewer mutes it). It comes from the console's own video "
                "recorder, as with SysDVR - games that turn video capture off have no sound in the stream unless "
                "\"Sound in no-record games\" below is on. "
                "Webcam mode has no sound: a USB camera cannot carry it from the Switch." });

            rows.push_back({ Kind::Setting, "Sound in no-record games", [this] {
                    const char *v = sound == sftsound::State::On ? "On" : sound == sftsound::State::Off ? "Off" : "Not installed";
                    return std::string(v) + (sound_changed ? " - restart to apply" : "");
                }, [this] { return sound_changed ? ColWarn : sound == sftsound::State::On ? ColGood : ColDim; },
                [this](int) {
                    if (sound == sftsound::State::Missing) { Toast("The patches are not on the SD card - reinstall the Switch Frame Tap zip"); return; }
                    const bool on = sound != sftsound::State::On;
                    if (sftsound::Set(on)) { sound_changed = !sound_changed; Toast(on ? "On - restart the console to apply" : "Off - restart the console to apply"); }
                    else { Toast("Could not change it - check the SD card"); }
                },
                "Some games turn the console's video recorder off (Super Smash Bros. Ultimate, for one), and with it the "
                "sound the stream gets. This turns on dvr-patches (by exelix11, bundled): they let the recorder run for "
                "every game, so their sound comes through. Off by default; a restart applies it. Turn it off again if a "
                "game crashes with it on (a crash report for 0100000000000023)." });

            rows.push_back({ Kind::Setting, "Screen off while streaming", [this] { return std::string(set.screen_off ? "On" : "Off"); },
                [this] { return set.screen_off ? ColGood : ColDim; }, [this](int) { set.screen_off = !set.screen_off; Saved(); },
                "Turns the Switch's own screen dark while the stream runs, to save battery when you play on the PC. "
                "It comes back on the moment the stream stops: close the viewer, press HOME, or switch the stream "
                "off. Handheld only - a TV is not affected." });

            rows.push_back({ Kind::Setting, "HOME menu (beta)", [this] {
                    std::string v = set.home_menu ? "On" : "Off";
                    if (have_status && RunningHomeMenu() != set.home_menu) { v += " - after a reboot"; }
                    return v;
                }, [this] { return (have_status && RunningHomeMenu() != set.home_menu) ? ColWarn : set.home_menu ? ColGood : ColDim; },
                [this](int) {
                    set.home_menu = !set.home_menu;
                    if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
                    Toast(test_install ? "Saved - but a test arm file is installed, so the sysmodule ignores config.ini"
                                       : "Saved - restart the console to apply");
                },
                "Also streams the HOME menu and the system screens when no game is on screen: the HOME menu itself, "
                "System Settings, the user picker, Controllers, My Page, the Album and the Homebrew Menu. "
                "No sound outside games yet. Beta: off by default, and a reboot applies it." });

            rows.push_back({ Kind::Header, "Capture", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Setting, "Capture mode", [this] { return std::string(sftcapture::Label(set.capture_mode)); }, nullptr,
                [this](int d) { set.capture_mode = (set.capture_mode + (d > 0 ? 1u : 3u)) % 4u; Saved(); },
                "How the sysmodule gets the picture. Auto (default) starts with the game's swapchain and, if it sees a game "
                "that draws in layers, switches to Layer (or Compositor when that route is available). Standard always reads "
                "the game's swapchain. Compositor reads the final screen; not available in this build yet, so it falls back "
                "to Layer. Layer sends only the part of the picture that changes - stable, but it can be slower. "
                "Per-title overrides are listed below and win over this setting." });
            if (have_ci && ci.tid != 0) {
                rows.push_back({ Kind::Info, "This game", [this] {
                        std::string v = "Selected: " + std::string(sftcapture::Label(ci.requested));
                        if (ci.reserved & 1) { v += " / Active: " + std::string(sftcapture::Label(ci.mode)); }
                        else { v += " (next stream)"; }
                        if (!ci.live && !ci.dead) { v += " - waiting for frames"; }
                        return v;
                    }, [this] { return (ci.reserved & 1) && ci.live != 0 && ci.dead != 0 ? ColWarn : ColText; }, nullptr,
                    "Selected is the per-title mode from capture.ini; Active is what the running stream is using. "
                    "A Layer fallback is expected when the stream restarts after a multi-surface detection." });
            }
            if (!capture_overrides.empty()) {
                rows.push_back({ Kind::Header, "Capture overrides", nullptr, nullptr, nullptr, "" });
                for (size_t i = 0; i < capture_overrides.size(); ++i) {
                    const size_t oi = i;
                    char label[48];
                    std::snprintf(label, sizeof(label), "%016llx", static_cast<unsigned long long>(capture_overrides[oi].tid));
                    rows.push_back({ Kind::Setting, label, [this, oi] { return std::string(sftcapture::Label(capture_overrides[oi].mode)); }, nullptr,
                        [this, oi](int d) {
                            capture_overrides[oi].mode = (capture_overrides[oi].mode + (d > 0 ? 1u : 3u)) % 4u;
                            SaveCaptureOverrides();
                        },
                        "Per-title mode from sdmc:/config/switch-frame-tap/capture.ini. Auto follows the global setting and "
                        "detects multi-surface games; Standard, Compositor and Layer force that route for this title." });
                }
                rows.push_back({ Kind::Setting, "Clear capture overrides", [this] { return std::string(confirm_capture_clear ? "Press A to clear" : "A"); },
                    [this] { return confirm_capture_clear ? ColWarn : ColText; }, [this](int) {
                        if (!confirm_capture_clear) { confirm_capture_clear = true; return; }
                        confirm_capture_clear = false;
                        capture_overrides.clear();
                        SaveCaptureOverrides();
                        Build();
                        Toast("Capture overrides cleared");
                    },
                    "Removes every per-title mode; every game follows Capture mode above." });
            }

            rows.push_back({ Kind::Header, "Picture", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Setting, "Quality", [this] { return std::string(QualityNames[set.quality]); }, nullptr,
                [this](int d) { set.quality = (set.quality + (d > 0 ? 1 : 2)) % 3; Saved(); },
                "High is the sharpest picture: about 80 Mbps in a fast 1080p race, so a recording from the PC viewer "
                "takes about 600 MB a minute. Medium is roughly half that and Low about a third, still clean. "
                "Calm games and 720p need much less. Lower it if the stream stutters on a slow cable or PC." });
            rows.push_back({ Kind::Setting, "Keyframes", [this] { return std::string(KeyframeNames[set.keyframe]); }, nullptr,
                [this](int d) { set.keyframe = (set.keyframe + (d > 0 ? 1 : 3)) % 4; Saved(); },
                "A keyframe is a complete picture; the frames between only carry changes. More keyframes recover faster "
                "from a glitch, fewer save bandwidth." });
            rows.push_back({ Kind::Setting, "Maximum resolution", [this] { return std::string(set.cap720 ? "720p" : "1080p (native)"); }, nullptr,
                [this](int) { set.cap720 = !set.cap720; Saved(); },
                "1080p streams what the game renders in docked mode (with ReverseNX-RT in handheld). 720p scales it down: "
                "less bandwidth, and it runs at stock clocks." });

            rows.push_back({ Kind::Header, "Screenshots", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Setting, "Screenshot button", [this] { return std::string(set.shot ? "On" : "Off"); },
                [this] { return set.shot ? ColGood : ColDim; }, [this](int) { set.shot = !set.shot; Saved(); },
                "Press the button combo below in any game to save the picture at the game's own resolution (1920x1080 "
                "docked or with ReverseNX-RT, 1280x720 handheld) as a PNG in /switch/switch-frame-tap/screenshots. "
                "Works with or without the PC viewer; the PC stream pauses briefly while it saves. With the Ultrahand "
                "overlay menu installed, a notification confirms each one." });
            rows.push_back({ Kind::Setting, "Buttons", [this] { return set.shot_combo == ComboCustom ? CustomComboText() : std::string(ComboNames[set.shot_combo]); }, nullptr,
                [this](int d) { set.shot_combo = (set.shot_combo + (d > 0 ? 1 : ComboCustom)) % (ComboCustom + 1); Saved(); },
                "Hold these together to take a screenshot. Pick a combo your game does not use - L3 + R3 (pressing both "
                "sticks in) is rarely used by games. Custom: your own buttons (below)." });
            rows.push_back({ Kind::Setting, "Custom: record buttons", [this] {
                    return set.shot_combo == ComboCustom ? std::string("Press A, then hold them") : std::string("-");
                }, nullptr, [](int) { g_record_combo = true; },
                "Record your own screenshot combo: press A here, then hold the buttons you want together for two "
                "seconds. Add the Capture button with the next setting - pressing it here would take a picture of this app." });
            rows.push_back({ Kind::Setting, "Custom: with the Capture button", [this] {
                    return set.shot_combo == ComboCustom ? std::string(set.shot_capture ? "On" : "Off") : std::string("-");
                }, [this] { return (set.shot_combo == ComboCustom && set.shot_capture) ? ColGood : ColDim; },
                [this](int) { set.shot_combo = ComboCustom; set.shot_capture = !set.shot_capture; Saved(); },
                "The Capture button (the square one) as part of the combo - for example ZR + Capture. The console still "
                "takes its own screenshot (a JPEG in the Album) when you press it, so you get both: the system's and this "
                "native-resolution PNG. Capture on its own also works: every system screenshot then gets a PNG too." });
            rows.push_back({ Kind::Setting, "Save to", [this] { return std::string(set.shot_album ? "With the system's screenshots" : "Switch Frame Tap folder"); }, nullptr,
                [this](int) { set.shot_album = !set.shot_album; Saved(); },
                "Where the screenshots go. With the system's screenshots: the same Album folder on the SD card as the "
                "console's own (Nintendo/Album/year/month/day - under emuMMC, its own Nintendo folder), named by date so a "
                "PC lists them together. The Switch's Album app does not show them (it only shows Nintendo's own JPEGs); "
                "this app's screenshot viewer does. Switch Frame Tap folder: /switch/switch-frame-tap/screenshots." });
            rows.push_back({ Kind::Setting, "View screenshots", [this] {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%zu saved", shot_files);
                    return std::string(b);
                }, nullptr, [](int) { g_open_gallery = true; },
                "Browse the screenshots on the SD card: A shows one full screen (Left/Right for the others), Y deletes. "
                "They are in /switch/switch-frame-tap/screenshots, or with the system's screenshots (Save to) - copy them "
                "to a PC from there." });
            rows.push_back({ Kind::Info, "Taken this boot", [this] {
                    if (!have_status || st.version < 2) { return std::string("-"); }
                    char b[48];
                    std::snprintf(b, sizeof(b), st.shot_fails ? "%u (%u failed)" : "%u", st.shots, st.shot_fails);
                    return std::string(b);
                }, [this] { return (have_status && st.version >= 2 && st.shot_fails) ? ColWarn : ColText; }, nullptr,
                "Screenshots saved since the console started. Each 1080p PNG is about 6 MB (stored uncompressed, "
                "exactly the pixels the game drew). Open them from the SD card on a PC." });

            rows.push_back({ Kind::Header, "Advanced", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Setting, "Start delay after boot", [this] { char b[32]; std::snprintf(b, sizeof(b), "%d s", DelaySeconds[set.delay]); return std::string(b); }, nullptr,
                [this](int d) { set.delay = (set.delay + (d > 0 ? 1 : 4)) % 5; Saved(); },
                "Seconds after boot before the sysmodule starts looking for a game. Applies from the next boot." });
            rows.push_back({ Kind::Setting, "Allow untested firmware", [this] { return std::string(set.any_fw ? "Yes" : "No"); },
                [this] { return set.any_fw ? ColWarn : ColText; }, [this](int) { set.any_fw = !set.any_fw; Saved(); },
                "Stream on firmware this version was not tested on. It may work; it may also freeze the console, "
                "which needs a hard power-off. Applies from the next boot." });

            /* v0.7.5: the optional tools, from their authors' GitHub releases */
            rows.push_back({ Kind::Header, "Get extras (from GitHub)", nullptr, nullptr, nullptr, "" });
            auto extra = [this](const extras::Tool *t, bool (*has)(), std::function<std::string()> note, std::function<std::string()> blocked, const char *help) {
                const int idx = static_cast<int>(rows.size());
                rows.push_back({ Kind::Setting, t->name, [this, idx, has, note, blocked] {
                        if (confirm_extra == idx) { return std::string(has() ? "A again: update to the latest" : "A again: download and install"); }
                        const std::string b = blocked();
                        if (!b.empty()) { return b; }
                        if (!has()) { return std::string("Not installed"); }
                        const std::string n = note();
                        return n.empty() ? std::string("Installed") : n;
                    }, [this, idx, has, note, blocked] {
                        if (confirm_extra == idx) { return ColWarn; }
                        if (!blocked().empty()) { return ColDim; }
                        if (!has()) { return ColText; }
                        return note().empty() ? ColGood : ColWarn;
                    }, [this, idx, t, blocked](int) {
                        const std::string b = blocked();
                        if (!b.empty()) { Toast(b + " - one overclock tool at a time: remove it first"); return; }
                        if (confirm_extra != idx) { confirm_extra = idx; return; }
                        confirm_extra = -1;
                        g_pending_install = t;
                    }, help });
            };
            auto none = [] { return std::string(); };
            extra(&extras::Ultrahand, extras::HasUltrahand, none, none,
                  "The overlay menu (L + D-pad Down + right stick): Switch Frame Tap's overlay and the \"Screenshot saved\" "
                  "notes need it. Includes nx-ovlloader; replaces Tesla Menu if you have it. Your Ultrahand settings are kept "
                  "on an update. From github.com/ppkantorski/Ultrahand-Overlay.");
            extra(&extras::SaltyNX, extras::HasSaltyNX, none, none,
                  "Needed by ReverseNX-RT: it loads plugins into games. It can clash with some game mods (modded Smash with "
                  "Skyline crashed in testing) - if a game crashes, turn SaltyNX off. From github.com/masagrator/SaltyNX.");
            extra(&extras::ReverseNX, extras::HasReverseNX, none, none,
                  "Makes a game render its docked 1080p picture in handheld, for 1080p over USB (overlay menu, then "
                  "ReverseNX-RT). Needs SaltyNX; not every game supports it. From github.com/masagrator/ReverseNX-RT.");
            extra(&extras::SysClk, extras::HasSysClk, none,
                  [] { return extras::HasHorizonOC() ? std::string("Horizon OC is installed") : std::string(); },
                  "Raises the clocks so 1080p streams at a full 60 fps (CPU 1785 MHz, set it in its overlay). Pick sys-clk or "
                  "Horizon OC, not both. From github.com/retronx-team/sys-clk.");
            extra(&extras::HorizonOC, extras::HasHorizonOC,
                  [] { return extras::HekateMissesHocKip() ? std::string("Installed - Hekate needs its kip") : std::string(); },
                  [] { return extras::HasSysClk() ? std::string("sys-clk is installed") : std::string(); },
                  "An overclocking tool with more headroom than sys-clk (CPU, GPU and RAM). Its authors warn that RAM "
                  "overclocking can corrupt the NAND or the SD card: back both up first. It replaces atmosphere/exosphere.bin; "
                  "booting with Hekate, the boot entry needs kip1=atmosphere/kips/hoc.kip and secmon=atmosphere/exosphere.bin. "
                  "An update replaces hoc.kip, which holds its settings: save them again in its overlay afterwards. "
                  "Pick sys-clk or Horizon OC, not both. From github.com/Horizon-OC/Horizon-OC.");

            rows.push_back({ Kind::Header, "Setup check", nullptr, nullptr, nullptr, "" });
            auto check = [this](const char *label, const char *path, bool needed, const char *help) {
                rows.push_back({ Kind::Info, label, [path] { return Check(FileExists(path)); },
                    [path, needed] { return FileExists(path) ? ColGood : (needed ? ColBad : ColDim); }, nullptr, help });
            };
            check("Sysmodule", ModuleNsp, true, "atmosphere/contents/0100000000000C20/exefs.nsp - the part that streams.");
            check("Memory helper", "sdmc:/atmosphere/contents/0100000000000C21/exefs.nsp", true,
                  "atmosphere/contents/0100000000000C21/exefs.nsp - starts the sysmodule once the console has booted, and while a viewer "
                  "is connected lends it the streaming memory, so that game manuals, news and the eShop work whenever you are not streaming.");
            check("Overlay", "sdmc:/switch/.overlays/switch-frame-tap.ovl", false,
                  "switch/.overlays/switch-frame-tap.ovl - stream on/off and handheld/docked from inside a game (needs the Tesla overlay loader).");
            /* v0.7.13: the launcher's view of this boot - the memory sysmodules
             * share, and which sysmodules use it (launcher.txt) */
            {
                std::string free_s = "Not measured this boot", others;
                if (FILE *f = std::fopen(LauncherTxt, "r")) {
                    char b[2048] = {};
                    const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
                    std::fclose(f);
                    b[n] = 0;
                    if (const char *m = std::strstr(b, "system memory ")) {
                        const long kb = std::strtol(m + 14, nullptr, 10);
                        char t[64];
                        std::snprintf(t, sizeof(t), "%.1f MB free before it started", kb / 1024.0);
                        free_s = t;
                    }
                    if (const char *o = std::strstr(b, "others=")) {
                        others = o + 7;
                        if (const size_t nl = others.find('\n'); nl != std::string::npos) { others.resize(nl); }
                        for (size_t i = 0; (i = others.find("; ", i)) != std::string::npos; ) { others.replace(i, 2, "\n"); }
                    }
                }
                rows.push_back({ Kind::Info, "System memory", [free_s] { return free_s; }, nullptr, nullptr,
                    "All sysmodules share a few MB of the console's system memory with the system itself. When too little is left, "
                    "launching a game can end on a black screen, and Switch Frame Tap does not start at all (the console still boots). "
                    "Turning off a sysmodule you do not use frees some." +
                    (others.empty() ? std::string() : std::string(" Sysmodules running at boot, biggest first:\n") + others) });
            }
            rows.push_back({ Kind::Info, "Install type", [this] { return std::string(test_install ? "Test (applet-mitm.armed)" : "Release"); },
                [this] { return test_install ? ColWarn : ColText; }, nullptr,
                "A test install is driven by sdmc:/applet-mitm.armed and ignores config.ini. Delete that file for normal use." });

            rows.push_back({ Kind::Header, "About", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Info, "Version", [] { return std::string(AppVersion); }, nullptr, nullptr,
                "Switch Frame Tap - native 1080p60 game streaming to a PC over USB. github.com/TomasUribe/switch-frame-tap (GPL-2.0)." });
            rows.push_back({ Kind::Info, "If something goes wrong", [] { return std::string("Hold Volume Up at boot"); }, nullptr, nullptr,
                "Holding Volume Up while Atmosphere boots skips all sysmodules. Or turn off 'Start with the console' here, "
                "or delete atmosphere/contents/0100000000000C20 and 0100000000000C21 on a PC." });

            sel = 1;
        }

        bool Selectable(int i) const { return rows[i].kind != Kind::Header; }

        void Move(int d) {
            confirm_clear = false;
            confirm_capture_clear = false;
            confirm_extra = -1;
            int i = sel;
            do { i += d; } while (i >= 0 && i < static_cast<int>(rows.size()) && !Selectable(i));
            if (i >= 0 && i < static_cast<int>(rows.size())) { sel = i; }
        }

        void Activate(int d) {
            Row &r = rows[sel];
            if (r.change) { r.change(d); Refresh(); }
        }
    };

    constexpr int ListX = 40, ListW = 780, ListTop = 104, ListBottom = 640;
    constexpr int RowH = 62, HeaderH = 52;

    int RowHeight(const Row &r) { return r.kind == Kind::Header ? HeaderH : RowH; }

    void Draw(App &a) {
        SDL_SetRenderDrawColor(g_ren, 0x16, 0x17, 0x1B, 0xFF);
        SDL_RenderClear(g_ren);

        /* top bar */
        Fill(0, 0, ScreenW, 88, { 0x1E, 0x20, 0x26, 0xFF });
        Fill(0, 88, ScreenW, 2, ColAccent);
        Text("Switch Frame Tap", 60, 22, 3, ColText);
        Text(std::string("v") + AppVersion, ScreenW - 60, 34, 1, ColDim, 2);

        /* keep the selection in view */
        int y_sel = 0;
        for (int i = 0; i < a.sel; ++i) { y_sel += RowHeight(a.rows[i]); }
        const int view = ListBottom - ListTop;
        if (y_sel - a.scroll < HeaderH) { a.scroll = y_sel - HeaderH; }
        if (y_sel + RowH - a.scroll > view) { a.scroll = y_sel + RowH - view; }
        if (a.scroll < 0) { a.scroll = 0; }

        SDL_Rect clip = { ListX - 10, ListTop, ListW + 20, view };
        SDL_RenderSetClipRect(g_ren, &clip);
        int y = ListTop - a.scroll;
        for (size_t i = 0; i < a.rows.size(); ++i) {
            const Row &r = a.rows[i];
            const int h = RowHeight(r);
            if (y + h > ListTop && y < ListBottom) {
                if (r.kind == Kind::Header) {
                    Text(r.label, ListX + 4, y + 18, 1, ColAccent);
                    Fill(ListX, y + h - 4, ListW, 1, { 0x33, 0x36, 0x3E, 0xFF });
                } else {
                    const bool s = static_cast<int>(i) == a.sel;
                    if (s) {
                        Fill(ListX, y + 3, ListW, h - 6, { 0x25, 0x28, 0x30, 0xFF });
                        Fill(ListX, y + 3, 5, h - 6, ColAccent);
                    }
                    Text(r.label, ListX + 24, y + 16, 2, r.change ? ColText : ColDim);
                    if (r.value) {
                        const SDL_Color c = r.color ? r.color() : ColText;
                        Text(r.value(), ListX + ListW - 20, y + 18, 1, c, 2);
                    }
                }
            }
            y += h;
        }
        SDL_RenderSetClipRect(g_ren, nullptr);

        /* the help panel */
        Fill(860, 104, 380, 536, { 0x1E, 0x20, 0x26, 0xFF });
        const Row &cur = a.rows[a.sel];
        int hy = 126;
        hy += Text(cur.label, 884, hy, 2, ColText, 0, 332) + 14;
        Text(cur.help, 884, hy, 0, ColDim, 0, 332);

        /* bottom bar: button glyphs from the Nintendo extension font */
        Fill(0, 656, ScreenW, 64, { 0x1E, 0x20, 0x26, 0xFF });
        int bx = ScreenW - 60;
        auto hint = [&bx](const char *glyph, const char *label) {
            const CachedText *t = Render(label, 1, ColText);
            if (t) { bx -= t->w; Text(label, bx, 673, 1, ColText); }
            bx -= 8;
            const CachedText *g = Render(glyph, 0, ColText, 0, true);
            if (g) { bx -= g->w; Text(glyph, bx, 670, 0, ColText, 0, 0, true); }
            bx -= 36;
        };
        hint("", "Exit");
        if (cur.change) { hint("", "Change"); }
        if (a.toast_frames > 0) {
            --a.toast_frames;
            Text(a.toast, 60, 673, 1, ColAccent);
        }

        SDL_RenderPresent(g_ren);
    }

    /* ---- the screenshot gallery --------------------------------------------- */

    struct Gallery {
        std::vector<std::string> files;
        int sel = 0, scroll = 0;
        bool full = false;
        SDL_Texture *tex = nullptr;
        int tex_idx = -1, tw = 0, th = 0;
        long long bytes = 0;
        u32 settle = 0;             /* frames since the selection moved: load when it rests */
        int confirm = -1;           /* Y pressed once on this index */
        std::string note;

        void Open() { files = ListShots(); sel = 0; scroll = 0; full = false; confirm = -1; note.clear(); Drop(); settle = 6; }
        void Drop() { if (tex) { SDL_DestroyTexture(tex); tex = nullptr; } tex_idx = -1; }
        void Select(int i) {
            if (files.empty()) { return; }
            i = std::clamp(i, 0, static_cast<int>(files.size()) - 1);
            if (i != sel) { sel = i; settle = 0; confirm = -1; note.clear(); }
        }
        void Load() {
            if (files.empty() || tex_idx == sel || (settle < 6 && !full)) { return; }
            Drop();
            const std::string &path = files[sel];
            struct stat st;
            bytes = stat(path.c_str(), &st) == 0 ? st.st_size : 0;
            if (SDL_Surface *surf = IMG_Load(path.c_str())) {
                tex = SDL_CreateTextureFromSurface(g_ren, surf);
                tw = surf->w; th = surf->h;
                SDL_FreeSurface(surf);
            } else {
                tw = th = 0;
            }
            tex_idx = sel;
        }
        void Delete() {
            if (files.empty()) { return; }
            if (confirm != sel) { confirm = sel; note = "Press Y again to delete this screenshot"; return; }
            const std::string &path = files[sel];
            note = std::remove(path.c_str()) == 0 ? "Deleted" : "Could not delete it";
            files.erase(files.begin() + sel);
            if (sel >= static_cast<int>(files.size()) && sel > 0) { --sel; }
            confirm = -1;
            full = false;
            Drop();
            settle = 0;
        }
    };

    Gallery g_gal;

    constexpr int GalRowH = 54, GalListX = 40, GalListW = 440;

    void DrawImageFit(SDL_Texture *tex, int tw, int th, int x, int y, int w, int h) {
        if (tex == nullptr || tw == 0 || th == 0) { return; }
        int dw = w, dh = w * th / tw;
        if (dh > h) { dh = h; dw = h * tw / th; }
        SDL_Rect r = { x + (w - dw) / 2, y + (h - dh) / 2, dw, dh };
        SDL_RenderCopy(g_ren, tex, nullptr, &r);
    }

    /* button hints, right to left, in the bottom bar */
    void Hints(std::initializer_list<std::pair<const char *, const char *>> list) {
        int bx = ScreenW - 60;
        for (const auto &h : list) {
            const CachedText *t = Render(h.second, 1, ColText);
            if (t) { bx -= t->w; Text(h.second, bx, 673, 1, ColText); }
            bx -= 8;
            const CachedText *gl = Render(h.first, 0, ColText, 0, true);
            if (gl) { bx -= gl->w; Text(h.first, bx, 670, 0, ColText, 0, 0, true); }
            bx -= 36;
        }
    }

    void DrawGallery(Gallery &g) {
        g.Load();
        ++g.settle;
        SDL_SetRenderDrawColor(g_ren, 0x16, 0x17, 0x1B, 0xFF);
        SDL_RenderClear(g_ren);
        if (g.full && g.tex) {
            DrawImageFit(g.tex, g.tw, g.th, 0, 0, ScreenW, ScreenH);
            Fill(0, 656, ScreenW, 64, { 0x10, 0x11, 0x14, 0xC0 });
            char b[96];
            std::snprintf(b, sizeof(b), "%s   (%d of %zu)", ShotLabel(g.files[g.sel]).c_str(), g.sel + 1, g.files.size());
            Text(b, 60, 673, 1, ColText);
            Hints({ { "", "Back" }, { "", "Delete" }, { " ", "Previous / next" } });
            if (!g.note.empty()) { Text(g.note, ScreenW / 2, 610, 1, ColWarn, 1); }
            SDL_RenderPresent(g_ren);
            return;
        }
        Fill(0, 0, ScreenW, 88, { 0x1E, 0x20, 0x26, 0xFF });
        Fill(0, 88, ScreenW, 2, ColAccent);
        Text("Screenshots", 60, 22, 3, ColText);
        char cnt[32];
        std::snprintf(cnt, sizeof(cnt), "%zu", g.files.size());
        Text(cnt, ScreenW - 60, 34, 1, ColDim, 2);
        Fill(0, 656, ScreenW, 64, { 0x1E, 0x20, 0x26, 0xFF });

        if (g.files.empty()) {
            Text("No screenshots yet.", ScreenW / 2, 300, 2, ColText, 1);
            Text("Turn on the screenshot button in the main menu, then press the combo in a game.", ScreenW / 2, 350, 1, ColDim, 1);
            Hints({ { "", "Back" } });
            if (!g.note.empty()) { Text(g.note, 60, 673, 1, ColWarn); }
            SDL_RenderPresent(g_ren);
            return;
        }
        /* the list */
        const int view = ListBottom - ListTop;
        if (g.sel * GalRowH - g.scroll < 0) { g.scroll = g.sel * GalRowH; }
        if (g.sel * GalRowH + GalRowH - g.scroll > view) { g.scroll = g.sel * GalRowH + GalRowH - view; }
        SDL_Rect clip = { GalListX, ListTop, GalListW, view };
        SDL_RenderSetClipRect(g_ren, &clip);
        for (int i = 0; i < static_cast<int>(g.files.size()); ++i) {
            const int y = ListTop + i * GalRowH - g.scroll;
            if (y + GalRowH < ListTop || y > ListBottom) { continue; }
            if (i == g.sel) {
                Fill(GalListX, y + 3, GalListW, GalRowH - 6, { 0x25, 0x28, 0x30, 0xFF });
                Fill(GalListX, y + 3, 5, GalRowH - 6, ColAccent);
            }
            Text(ShotLabel(g.files[i]), GalListX + 22, y + 13, 1, i == g.sel ? ColText : ColDim);
        }
        SDL_RenderSetClipRect(g_ren, nullptr);
        /* the preview */
        Fill(510, 104, 730, 536, { 0x1E, 0x20, 0x26, 0xFF });
        if (g.tex_idx == g.sel && g.tex) {
            DrawImageFit(g.tex, g.tw, g.th, 525, 119, 700, 394);
            char info[96];
            std::snprintf(info, sizeof(info), "%d x %d   %.1f MB", g.tw, g.th, g.bytes / 1048576.0);
            Text(info, 525, 530, 1, ColText);
            Text(g.files[g.sel].substr(g.files[g.sel].rfind("sdmc:", 0) == 0 ? 5 : 0), 525, 570, 0, ColDim);
        } else if (g.tex_idx == g.sel) {
            Text("This file could not be opened.", 875, 300, 1, ColWarn, 1);
        } else {
            Text("Loading...", 875, 300, 1, ColDim, 1);
        }
        Hints({ { "", "Back" }, { "", "Delete" }, { "", "View" } });
        if (!g.note.empty()) { Text(g.note, 60, 673, 1, ColWarn); }
        SDL_RenderPresent(g_ren);
    }

    /* false: leave the gallery */
    bool GalleryInput(Gallery &g, u64 down, u64 held, u32 &hold) {
        if (down & HidNpadButton_B) {
            if (g.full) { g.full = false; return true; }
            g.Drop();
            return false;
        }
        if (down & HidNpadButton_Y) { g.Delete(); return true; }
        if (g.full) {
            if (down & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_L)) { g.Select(g.sel - 1); }
            if (down & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_R)) { g.Select(g.sel + 1); }
            return true;
        }
        if (down & HidNpadButton_A) { if (!g.files.empty()) { g.full = true; } return true; }
        const u64 up_mask = HidNpadButton_Up | HidNpadButton_StickLUp;
        const u64 dn_mask = HidNpadButton_Down | HidNpadButton_StickLDown;
        if (down & up_mask) { g.Select(g.sel - 1); hold = 0; }
        else if (down & dn_mask) { g.Select(g.sel + 1); hold = 0; }
        else if (held & (up_mask | dn_mask)) {
            if (++hold > 24 && hold % 6 == 0) { g.Select(g.sel + ((held & up_mask) ? -1 : 1)); }
        } else { hold = 0; }
        return true;
    }

    /* touch in the gallery: tap a row to select it, tap it again (or the
     * preview) to view it full screen, tap a full-screen picture to go back */
    void GalleryTouch(Gallery &g, int tx, int ty) {
        if (g.full) { g.full = false; return; }
        if (tx >= GalListX && tx < GalListX + GalListW && ty >= ListTop && ty < ListBottom) {
            const int i = (ty - ListTop + g.scroll) / GalRowH;
            if (i >= 0 && i < static_cast<int>(g.files.size())) {
                if (i == g.sel) { g.full = true; } else { g.Select(i); }
            }
        } else if (tx >= 510 && ty >= 104 && ty < 640 && !g.files.empty()) {
            g.full = true;
        }
    }

}

namespace {

    /* v0.7.5: a Get extras download, with its progress on screen; B cancels */
    /* v0.7.11: hold the buttons for the custom screenshot combo: the same set
     * held for two seconds is recorded; touch cancels, 20 s gives up */
    void RecordCombo(App &app, PadState &pad) {
        auto draw = [](const char *line1, const std::string &names, double frac) {
            SDL_SetRenderDrawColor(g_ren, 0x16, 0x17, 0x1B, 0xFF);
            SDL_RenderClear(g_ren);
            Fill(0, 0, ScreenW, 88, { 0x1E, 0x20, 0x26, 0xFF });
            Fill(0, 88, ScreenW, 2, ColAccent);
            Text("Screenshot combo", 60, 22, 3, ColText);
            Text(line1, ScreenW / 2, 230, 2, ColText, 1);
            Text(names.empty() ? std::string("-") : names, ScreenW / 2, 310, 2, ColAccent, 1);
            const int bx = 340, bw = 600, by = 380, bh = 16;
            Fill(bx, by, bw, bh, { 0x2A, 0x2D, 0x35, 0xFF });
            if (frac > 0.0) { Fill(bx, by, static_cast<int>(bw * (frac > 1.0 ? 1.0 : frac)), bh, ColAccent); }
            Text("The Capture button is a separate setting (pressing it here would take a picture of this app).", ScreenW / 2, 440, 1, ColDim, 1);
            Fill(0, 656, ScreenW, 64, { 0x1E, 0x20, 0x26, 0xFF });
            Text("Touch the screen to cancel", ScreenW - 60, 673, 1, ColText, 2);
            SDL_RenderPresent(g_ren);
        };
        auto names_of = [](uint32_t m) {
            char b[96];
            sft::shotbuttons::Format(m, false, b, sizeof(b));
            std::string t = b;
            for (size_t i = 0; (i = t.find('+', i)) != std::string::npos; i += 3) { t.replace(i, 1, " + "); }
            return t;
        };
        uint32_t last = 0, steady = 0;
        bool released = false, touch_free = false;   /* the tap that opened this must lift first */
        for (u32 frame = 0; frame < 60 * 20 && appletMainLoop(); ++frame) {
            padUpdate(&pad);
            HidTouchScreenState ts = {};
            const bool touched = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
            if (!touched) { touch_free = true; } else if (touch_free) { app.Toast("Not changed"); return; }
            const uint32_t held = static_cast<uint32_t>(padGetButtons(&pad)) & sft::shotbuttons::AllButtons;
            if (!released) {                      /* the A that opened this first */
                if (held == 0) { released = true; }
                draw("Let go of all buttons...", "", 0.0);
                continue;
            }
            if (held != last) { last = held; steady = 0; } else if (held != 0) { ++steady; }
            if (held != 0 && steady >= 120) {
                app.set.shot_mask = held;
                app.set.shot_combo = ComboCustom;
                app.Saved();
                app.Toast("Screenshot combo: " + app.CustomComboText());
                return;
            }
            draw(held ? "Keep holding them..." : "Hold the buttons you want, together", names_of(held), held ? steady / 120.0 : 0.0);
        }
        app.Toast("Not changed");
    }

    void RunInstall(App &app, PadState &pad) {
        const extras::Tool &t = *g_pending_install;
        g_pending_install = nullptr;
        auto draw = [&t](const char *what, double frac) {
            SDL_SetRenderDrawColor(g_ren, 0x16, 0x17, 0x1B, 0xFF);
            SDL_RenderClear(g_ren);
            Fill(0, 0, ScreenW, 88, { 0x1E, 0x20, 0x26, 0xFF });
            Fill(0, 88, ScreenW, 2, ColAccent);
            Text("Get extras", 60, 22, 3, ColText);
            Text(t.name, ScreenW / 2, 250, 2, ColText, 1);
            Text(what, ScreenW / 2, 320, 1, ColDim, 1);
            const int bx = 340, bw = 600, by = 380, bh = 16;
            Fill(bx, by, bw, bh, { 0x2A, 0x2D, 0x35, 0xFF });
            if (frac >= 0.0) { Fill(bx, by, static_cast<int>(bw * (frac > 1.0 ? 1.0 : frac)), bh, ColAccent); }
            Text("From the official GitHub release", ScreenW / 2, 430, 1, ColDim, 1);
            Fill(0, 656, ScreenW, 64, { 0x1E, 0x20, 0x26, 0xFF });
            Hints({ { "\uE0E1", "Cancel" } });
            SDL_RenderPresent(g_ren);
        };
        u32 tick = 0;
        extras::Progress progress = [&](const char *what, double frac) {
            if (!appletMainLoop()) { return false; }
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_B) { return false; }
            if ((++tick % 4) == 0 || frac < 0.0) { draw(what, frac); }
            return true;
        };
        draw("Connecting...", -1.0);
        std::string done, err;
        if (extras::Install(t, progress, &done, &err)) { app.Toast(done); }
        else { app.Toast(std::string(t.name) + ": " + err); }
        app.Refresh();
    }

}

int main(int, char **) {
    plInitialize(PlServiceType_User);
    pmdmntInitialize();
    nifmInitialize(NifmServiceType_User);   /* v0.7: the console's IP, shown with Network streaming */
    splInitialize();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);
    SDL_Window *win = SDL_CreateWindow("Switch Frame Tap", 0, 0, ScreenW, ScreenH, 0);
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    PlFontData std_font = {}, ext_font = {};
    plGetSharedFontByType(&std_font, PlSharedFontType_Standard);
    plGetSharedFontByType(&ext_font, PlSharedFontType_NintendoExt);
    for (int i = 0; i < 4; ++i) { g_font[i] = TTF_OpenFontRW(SDL_RWFromMem(std_font.address, std_font.size), 1, FontPx[i]); }
    g_ext = TTF_OpenFontRW(SDL_RWFromMem(ext_font.address, ext_font.size), 1, 26);

    App app;
    app.fw = hosversionGet();
    if (R_FAILED(splGetConfig(static_cast<SplConfigItem>(65000), &app.ams))) { app.ams = 0; }
    app.Refresh();
    app.Build();

    u32 frame = 0, hold = 0;
    int touch_row = -1;
    bool touching = false, gallery = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);

        if (gallery) {
            if (down & HidNpadButton_Plus) { break; }
            HidTouchScreenState ts = {};
            if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
                if (!touching) { touching = true; GalleryTouch(g_gal, ts.touches[0].x, ts.touches[0].y); }
            } else {
                touching = false;
            }
            if (!GalleryInput(g_gal, down, held, hold)) { gallery = false; app.Refresh(); continue; }
            DrawGallery(g_gal);
            continue;
        }
        if (down & (HidNpadButton_B | HidNpadButton_Plus)) { break; }

        /* D-pad / stick with auto-repeat */
        const u64 up_mask = HidNpadButton_Up | HidNpadButton_StickLUp;
        const u64 dn_mask = HidNpadButton_Down | HidNpadButton_StickLDown;
        if (down & up_mask) { app.Move(-1); hold = 0; }
        else if (down & dn_mask) { app.Move(1); hold = 0; }
        else if (held & (up_mask | dn_mask)) {
            if (++hold > 24 && hold % 6 == 0) { app.Move((held & up_mask) ? -1 : 1); }
        } else { hold = 0; }
        if (down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_StickLRight)) { app.Activate(1); }
        if (down & (HidNpadButton_Left | HidNpadButton_StickLLeft)) { app.Activate(-1); }

        /* touch: tap a row to select it, tap it again to change it */
        HidTouchScreenState ts = {};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            const int tx = ts.touches[0].x, ty = ts.touches[0].y;
            if (!touching && tx >= ListX && tx < ListX + ListW && ty >= ListTop && ty < ListBottom) {
                int y = ListTop - app.scroll;
                touch_row = -1;
                for (size_t i = 0; i < app.rows.size(); ++i) {
                    const int h = RowHeight(app.rows[i]);
                    if (ty >= y && ty < y + h && app.Selectable(static_cast<int>(i))) { touch_row = static_cast<int>(i); }
                    y += h;
                }
            }
            touching = true;
        } else if (touching) {
            touching = false;
            if (touch_row >= 0) {
                if (touch_row == app.sel) { app.Activate(1); } else { app.sel = touch_row; }
            }
            touch_row = -1;
        }

        if (g_pending_install != nullptr) { RunInstall(app, pad); touching = true; touch_row = -1; continue; }
        if (g_record_combo) { g_record_combo = false; RecordCombo(app, pad); touching = true; touch_row = -1; continue; }
        if (g_open_gallery) {
            g_open_gallery = false;
            g_gal.Open();
            gallery = true;
            touching = true;      /* the tap that opened it must not also select in it */
            touch_row = -1;
            continue;
        }
        if (++frame % 30 == 0) { app.Refresh(); }
        Draw(app);
    }

    g_gal.Drop();
    extras::NetDown();
    IMG_Quit();
    for (auto &e : g_cache) { SDL_DestroyTexture(e.second.tex); }
    for (auto *f : g_font) { if (f) { TTF_CloseFont(f); } }
    if (g_ext) { TTF_CloseFont(g_ext); }
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_sftap_open) { serviceClose(&g_sftap); }
    splExit();
    nifmExit();
    pmdmntExit();
    plExit();
    return 0;
}
