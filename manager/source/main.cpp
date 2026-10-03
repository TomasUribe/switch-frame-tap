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
 *  - Start with the console: the sysmodule's flags/boot2.flag.
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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace {

    constexpr const char *AppVersion  = "0.7.2";
    constexpr u64 ModuleTid           = 0x0100000000000C20ull;
    constexpr const char *ModuleDir   = "sdmc:/atmosphere/contents/0100000000000C20";
    constexpr const char *ModuleNsp   = "sdmc:/atmosphere/contents/0100000000000C20/exefs.nsp";
    constexpr const char *BootFlag    = "sdmc:/atmosphere/contents/0100000000000C20/flags/boot2.flag";
    constexpr const char *ConfigDir   = "sdmc:/config/switch-frame-tap";
    constexpr const char *ConfigIni   = "sdmc:/config/switch-frame-tap/config.ini";
    constexpr const char *StreamOff   = "sdmc:/config/switch-frame-tap/stream-off";
    constexpr const char *ExclFile    = "sdmc:/config/switch-frame-tap/excluded.txt";

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

    /* the screenshots, newest first (the names are dates, so by name) */
    std::vector<std::string> ListShots() {
        std::vector<std::string> v;
        if (DIR *d = opendir(ShotDir)) {
            while (dirent *e = readdir(d)) {
                const size_t n = std::strlen(e->d_name);
                if (n > 4 && std::strcmp(e->d_name + n - 4, ".png") == 0) { v.emplace_back(e->d_name); }
            }
            closedir(d);
        }
        std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) { return a > b; });
        return v;
    }

    /* "2026-09-29_17-27-10.png" -> "2026-09-29  17:27:10" */
    std::string ShotLabel(const std::string &f) {
        if (f.size() >= 23 && f[10] == '_') {
            std::string t = f.substr(11, 8);
            std::replace(t.begin(), t.end(), '-', ':');
            return f.substr(0, 10) + "  " + t + f.substr(19, f.size() - 23);
        }
        return f.substr(0, f.size() - 4);
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

    /* ---- settings (config.ini) ------------------------------------------ */

    const char *const QualityNames[]  = { "High", "Medium", "Low" };
    const char *const ConnKeys[]      = { "usb", "webcam", "network" };
    const char *const ConnNames[]     = { "USB - PC viewer", "USB - webcam", "Network" };
    const char *const QualityKeys[]   = { "high", "medium", "low" };
    const int KeyframeFrames[]        = { 30, 60, 120, 240 };
    const char *const KeyframeNames[] = { "Every 0.5 s", "Every second", "Every 2 s", "Every 4 s" };
    const int DelaySeconds[]          = { 5, 10, 20, 30, 60 };
    const char *const ComboNames[]    = { "L3 + R3 (click both sticks)", "L + R + D-pad Down", "ZL + ZR + D-pad Down", "Minus + D-pad Down" };

    struct Settings {
        int quality = 0;        /* index into QualityKeys */
        int keyframe = 1;       /* index into KeyframeFrames */
        bool cap720 = false;
        int delay = 2;          /* index into DelaySeconds */
        bool any_fw = false;
        bool shot = false;
        int shot_combo = 0;     /* index into ComboNames */
        bool audio = true;      /* v0.3 */
        int conn = 0;           /* v0.7: connection - 0 USB (PC viewer), 1 USB webcam, 2 network; from the next boot */
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
        if (IniGet(ini, "audio", &v)) { s.audio = v != "0"; }
        /* the v0.4-v0.7 keys first, then the one that replaced them */
        if (IniGet(ini, "usb_mode", &v) && v == "webcam") { s.conn = 1; }
        if (IniGet(ini, "network", &v) && v == "1") { s.conn = 2; }
        if (IniGet(ini, "connection", &v)) { s.conn = v == "network" ? 2 : v == "webcam" ? 1 : 0; }
        if (IniGet(ini, "screenshot_buttons", &v)) { const int k = std::atoi(v.c_str()); if (k >= 0 && k <= 3) { s.shot_combo = k; } }
        return s;
    }

    bool SaveSettings(const Settings &s) {
        mkdir("sdmc:/config", 0777);
        mkdir(ConfigDir, 0777);
        FILE *f = std::fopen(ConfigIni, "wb");
        if (f == nullptr) { return false; }
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
            "# screenshot_buttons: 0 = L3 + R3, 1 = L + R + D-pad Down, 2 = ZL + ZR + D-pad Down, 3 = Minus + D-pad Down\n"
            "screenshot_buttons = %d\n"
            "# audio: 1 = send the game's sound to the PC viewer\n"
            "audio = %d\n"
            "# connection: usb (the PC viewer over USB) | webcam (a USB camera for OBS; no sound) |\n"
            "#             network (the PC viewer over Wi-Fi or the dock's LAN port - play docked). Read at boot.\n"
            "connection = %s\n",
            QualityKeys[s.quality], KeyframeFrames[s.keyframe], s.cap720 ? "720" : "1080",
            DelaySeconds[s.delay], s.any_fw ? 1 : 0, s.shot ? 1 : 0, s.shot_combo, s.audio ? 1 : 0, ConnKeys[s.conn]);
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

    struct App {
        Settings set = LoadSettings();
        StreamStatus st = {};
        bool running = false, have_status = false, installed = false, boot = false, test_install = false;
        bool stream_file_on = true;
        size_t shot_files = 0, excluded = 0;
        bool confirm_clear = false;
        u32 fw = 0;
        u64 ams = 0;
        std::string toast;
        u32 toast_frames = 0;
        std::vector<Row> rows;
        int sel = 0, scroll = 0;

        void Refresh() {
            installed = FileExists(ModuleNsp);
            boot = FileExists(BootFlag);
            test_install = FileExists(TestArmFile);
            stream_file_on = !FileExists(StreamOff);
            shot_files = ListShots().size();
            excluded = CountExcluded();
            running = ModuleRunning();
            have_status = running && SftapStatus(&st);
        }

        void Toast(const std::string &s) { toast = s; toast_frames = 180; }

        void Saved() {
            if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
            if (test_install) { Toast("Saved - but a test arm file is installed, so the sysmodule ignores config.ini"); return; }
            if (have_status) { SftapReload(); Toast("Saved - the stream picks it up right away"); }
            else { Toast("Saved - used from the next boot"); }
        }

        std::string StreamState() {
            if (!installed) { return "Not installed"; }
            if (!running) { return boot ? "Not running - reboot to start it" : "Off at boot (see below)"; }
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
        int RunningConn() { return have_status ? ((st.reserved3 & 2) ? 2 : (st.reserved3 & 1) ? 1 : 0) : set.conn; }
        bool RunningWebcam() { return RunningConn() == 1; }
        /* v0.7: the network transport this boot, and whether a viewer is on it */
        bool RunningNetwork() { return RunningConn() == 2; }
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
            if (on) {
                mkdir((std::string(ModuleDir) + "/flags").c_str(), 0777);
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
                    std::snprintf(b, sizeof(b), "%u.%u.%u%s", HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw), HOSVER_MAJOR(fw) == 22 ? "" : " - untested");
                    return std::string(b);
                }, [this] { return HOSVER_MAJOR(fw) == 22 ? ColText : ColWarn; }, nullptr,
                "This version was tested on firmware 22.x (Atmosphere 1.11, Mariko). On other firmware the sysmodule does not "
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
                "Whether Atmosphere starts the sysmodule at boot. Takes effect at the next boot. "
                "Off is the clean way to disable it completely." });
            rows.push_back({ Kind::Setting, "Connection", [this] {
                    std::string v = ConnNames[set.conn];
                    if (have_status && RunningConn() != set.conn) { v += " - after a reboot"; }
                    else if (set.conn == 2) { v += " - " + IpString(); }
                    return v;
                }, [this] { return (have_status && RunningConn() != set.conn) ? ColWarn : ColText; },
                [this](int d) {
                    set.conn = (set.conn + (d > 0 ? 1 : 2)) % 3;
                    if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
                    Toast(test_install ? "Saved - but a test arm file is installed, so the sysmodule ignores config.ini"
                                       : "Saved - restart the console to switch the connection");
                },
                "How the stream reaches the PC - one at a time. USB - PC viewer: the viewer app over the USB-C cable "
                "(handheld; sound, the lowest latency). USB - webcam: the Switch shows up as a USB camera for OBS - "
                "picture only. Network: the viewer app over Wi-Fi or the dock's LAN port, so you can play docked; set "
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
                "recorder, as with SysDVR - games that turn video capture off have no sound in the stream. "
                "Webcam mode has no sound: a USB camera cannot carry it from the Switch." });

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
            rows.push_back({ Kind::Setting, "Buttons", [this] { return std::string(ComboNames[set.shot_combo]); }, nullptr,
                [this](int d) { set.shot_combo = (set.shot_combo + (d > 0 ? 1 : 3)) % 4; Saved(); },
                "Hold these together to take a screenshot. Pick a combo your game does not use - L3 + R3 (pressing both "
                "sticks in) is rarely used by games." });
            rows.push_back({ Kind::Setting, "View screenshots", [this] {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%zu saved", shot_files);
                    return std::string(b);
                }, nullptr, [](int) { g_open_gallery = true; },
                "Browse the screenshots on the SD card: A shows one full screen (Left/Right for the others), Y deletes. "
                "They are in /switch/switch-frame-tap/screenshots - copy them to a PC from there." });
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

            rows.push_back({ Kind::Header, "Setup check", nullptr, nullptr, nullptr, "" });
            auto check = [this](const char *label, const char *path, bool needed, const char *help) {
                rows.push_back({ Kind::Info, label, [path] { return Check(FileExists(path)); },
                    [path, needed] { return FileExists(path) ? ColGood : (needed ? ColBad : ColDim); }, nullptr, help });
            };
            check("Sysmodule", ModuleNsp, true, "atmosphere/contents/0100000000000C20/exefs.nsp - the part that streams.");
            check("Overlay", "sdmc:/switch/.overlays/switch-frame-tap.ovl", false,
                  "switch/.overlays/switch-frame-tap.ovl - stream on/off and handheld/docked from inside a game (needs the Tesla overlay loader).");
            check("Overlay loader", "sdmc:/atmosphere/contents/420000000007E51A/exefs.nsp", false,
                  "nx-ovlloader, which runs Tesla overlays (L + D-pad Down + right stick by default).");
            check("SaltyNX", "sdmc:/atmosphere/contents/0000000000534C56/exefs.nsp", false,
                  "Needed by ReverseNX-RT. Only for 1080p over USB: it makes the game render its docked picture in handheld.");
            check("ReverseNX-RT", "sdmc:/switch/.overlays/ReverseNX-RT-ovl.ovl", false,
                  "The docked/handheld switch the overlay uses. From github.com/masagrator/ReverseNX-RT.");
            rows.push_back({ Kind::Info, "Install type", [this] { return std::string(test_install ? "Test (applet-mitm.armed)" : "Release"); },
                [this] { return test_install ? ColWarn : ColText; }, nullptr,
                "A test install is driven by sdmc:/applet-mitm.armed and ignores config.ini. Delete that file for normal use." });

            rows.push_back({ Kind::Header, "About", nullptr, nullptr, nullptr, "" });
            rows.push_back({ Kind::Info, "Version", [] { return std::string(AppVersion); }, nullptr, nullptr,
                "Switch Frame Tap - native 1080p60 game streaming to a PC over USB. github.com/TomasUribe/switch-frame-tap (GPL-2.0)." });
            rows.push_back({ Kind::Info, "If something goes wrong", [] { return std::string("Hold Volume Up at boot"); }, nullptr, nullptr,
                "Holding Volume Up while Atmosphere boots skips all sysmodules. Or turn off 'Start with the console' here, "
                "or delete atmosphere/contents/0100000000000C20 on a PC." });

            sel = 1;
        }

        bool Selectable(int i) const { return rows[i].kind != Kind::Header; }

        void Move(int d) {
            confirm_clear = false;
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
            const std::string path = std::string(ShotDir) + "/" + files[sel];
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
            const std::string path = std::string(ShotDir) + "/" + files[sel];
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
            Text(g.files[g.sel], 525, 570, 0, ColDim);
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
