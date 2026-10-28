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
 * SDL2 for drawing (ui.hpp: v0.7.14's look, the PC viewer's theme - a sidebar
 * of pages, rounded cards, switches), the console's own shared fonts through
 * SDL2_ttf, libnx pad/touch for input. tools/manager-preview draws the same
 * screens on a PC.
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
#include "../../tier4/applet-mitm/source/shot_buttons.hpp"
#ifdef SFT_PREVIEW
#include "preview_extras.hpp"   /* tools/manager-preview: the PC preview's stand-ins */
#else
#include "extras.hpp"
#endif
#include "ui.hpp"

namespace {

    constexpr const char *AppVersion  = "0.7.19";
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
        int usb_gop = 240;        /* usb_keyframe_interval (0..250); kept when saving */
        bool home_menu = false;   /* v0.7.14: stream the HOME menu and system applets too (beta); from the next boot */
        bool overlays = false;    /* v0.7.14: the system overlays in the stream (beta); from the next boot */
        bool hb_docked = false;   /* v0.7.18: homebrew is told the console is docked (1080p); the next app started */
        bool update_betas = false;/* v0.7.14: "Check for updates" offers pre-releases too (this app's own key) */
        int conn = 0;           /* v0.7: connection - 0 USB (PC viewer), 1 USB webcam, 2 (v0.7.7) USB webcam NV12, 3 network; from the next boot */
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
        if (IniGet(ini, "usb_keyframe_interval", &v) && !v.empty() && v.find_first_not_of("0123456789") == std::string::npos && v.size() <= 3 && std::atoi(v.c_str()) <= 250) { s.usb_gop = std::atoi(v.c_str()); }
        if (IniGet(ini, "audio", &v)) { s.audio = v != "0"; }
        if (IniGet(ini, "home_menu", &v)) { s.home_menu = v == "1"; }
        if (IniGet(ini, "overlays", &v)) { s.overlays = v == "1"; }
        if (IniGet(ini, "homebrew_docked", &v)) { s.hb_docked = v == "1"; }
        if (IniGet(ini, "update_betas", &v)) { s.update_betas = v == "1"; }
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
            "# usb_keyframe_interval: over USB, frames between keyframes when longer than keyframe_interval (at most 250;\n"
            "#                        the viewer asks for one when it needs it) | 0 = keyframe_interval over USB too\n"
            "usb_keyframe_interval = %d\n"
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
            "# overlays: 1 = the volume bar, quick settings, power menu and Tesla/Ultrahand menus in the stream (beta). Read at boot.\n"
            "overlays = %d\n"
            "# homebrew_docked: 1 = homebrew started from the Homebrew Menu is told the console is docked, so apps that\n"
            "#                  choose their resolution from that draw at 1080p (beta). From the next app started.\n"
            "homebrew_docked = %d\n"
            "# update_betas: 1 = Check for updates in the app also offers betas (pre-releases)\n"
            "update_betas = %d\n",
            QualityKeys[s.quality], KeyframeFrames[s.keyframe], s.usb_gop, s.cap720 ? "720" : "1080",
            DelaySeconds[s.delay], s.any_fw ? 1 : 0, s.shot ? 1 : 0, s.shot_combo, custom, s.shot_album ? "album" : "app", s.detach_home ? 1 : 0, s.screen_off ? 1 : 0, s.early.c_str(),
            s.audio ? 1 : 0, ConnKeys[s.conn], s.home_menu ? 1 : 0, s.overlays ? 1 : 0, s.hb_docked ? 1 : 0, s.update_betas ? 1 : 0);
        std::fclose(f);
        return true;
    }


    /* ---- the pages ------------------------------------------------------- */

    /* Section: a group's title. Hero and Tiles: the home page's status card
     * and its small facts. The rest are cards: Info (a fact), Toggle (a
     * switch), Choice (< value >), Action (opens or does something). A card
     * with focus shows its help under it. */
    enum class Kind { Section, Hero, Tiles, Info, Toggle, Choice, Action };

    struct Tile {
        std::string label;
        std::function<std::string()> value;
        std::function<SDL_Color()> color;
    };

    struct Row {
        Kind kind;
        std::string label;
        std::function<std::string()> value;   /* Info, Choice, Action: the text on the right; Toggle: a note beside the switch */
        std::function<SDL_Color()> color;     /* that text's colour (nullptr: the default) */
        std::function<void(int)> change;      /* +1 / -1 (a Toggle flips); nullptr: nothing to change */
        std::string help;
        std::function<bool()> on;             /* Toggle */
        std::vector<Tile> tiles;              /* Tiles */
    };

    bool Selectable(const Row &r) { return r.kind != Kind::Section && r.kind != Kind::Hero && r.kind != Kind::Tiles; }

    Row Section(std::string label) { return { Kind::Section, std::move(label), nullptr, nullptr, nullptr, "", nullptr, {} }; }
    Row Info(std::string label, std::function<std::string()> value, std::function<SDL_Color()> color, std::string help) {
        return { Kind::Info, std::move(label), std::move(value), std::move(color), nullptr, std::move(help), nullptr, {} };
    }
    Row Toggle(std::string label, std::function<bool()> on, std::function<void(int)> change, std::string help,
               std::function<std::string()> note = nullptr, std::function<SDL_Color()> note_color = nullptr) {
        return { Kind::Toggle, std::move(label), std::move(note), std::move(note_color), std::move(change), std::move(help), std::move(on), {} };
    }
    Row Choice(std::string label, std::function<std::string()> value, std::function<void(int)> change, std::string help,
               std::function<SDL_Color()> color = nullptr) {
        return { Kind::Choice, std::move(label), std::move(value), std::move(color), std::move(change), std::move(help), nullptr, {} };
    }
    Row Action(std::string label, std::function<std::string()> value, std::function<void(int)> change, std::string help,
               std::function<SDL_Color()> color = nullptr) {
        return { Kind::Action, std::move(label), std::move(value), std::move(color), std::move(change), std::move(help), nullptr, {} };
    }

    struct Page {
        const char *name;
        std::vector<Row> rows;
        int sel = -1;
        float scroll = 0, target = 0;
        int seen_sel = -2;          /* the selection the scroll last followed */
    };

    bool g_open_gallery = false;
    bool g_record_combo = false;   /* v0.7.11: the custom screenshot combo's recording screen */
    const extras::Tool *g_pending_install = nullptr;   /* v0.7.5: Get extras - run from the main loop */
    int g_pending_update = 0;      /* v0.7.14: 1 check for an update, 2 install it - run from the main loop */

    struct App {
        Settings set = LoadSettings();
        StreamStatus st = {};
        bool running = false, have_status = false, installed = false, boot = false, test_install = false;
        bool stream_file_on = true;
        size_t shot_files = 0, excluded = 0;
        bool confirm_clear = false;
        sftsound::State sound = sftsound::State::Missing;   /* v0.7.4: the bundled dvr-patches */
        int confirm_extra = -1;                              /* v0.7.5: the Get extras row waiting for its second A */
        bool sound_changed = false;                          /* toggled since this app started: a restart applies it */
        u32 fw = 0;
        u64 ams = 0;
        std::string free_mem, others;                        /* v0.7.13: launcher.txt */
        std::string toast;
        u32 toast_frames = 0;
        /* v0.7.14: Switch Frame Tap's own updates (extras::CheckUpdate) */
        enum class Upd { Unknown, Latest, Found, Installed, Failed } upd = Upd::Unknown;
        extras::Release upd_rel;
        std::string upd_msg;
        bool confirm_update = false;
        std::vector<Page> pages;
        int page = 0;
        bool in_content = false;    /* the keys move in the page (else in the sidebar) */

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
        }

        /* v0.7.13: the launcher's view of this boot - the memory sysmodules
         * share, and which sysmodules use it */
        void ReadLauncher() {
            free_mem.clear();
            others.clear();
            if (FILE *f = std::fopen(LauncherTxt, "r")) {
                char b[2048] = {};
                const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
                std::fclose(f);
                b[n] = 0;
                if (const char *m = std::strstr(b, "system memory ")) {
                    const long kb = std::strtol(m + 14, nullptr, 10);
                    char t[32];
                    std::snprintf(t, sizeof(t), "%.1f MB", kb / 1024.0);
                    free_mem = t;
                }
                if (const char *o = std::strstr(b, "others=")) {
                    others = o + 7;
                    if (const size_t nl = others.find('\n'); nl != std::string::npos) { others.resize(nl); }
                    for (size_t i = 0; (i = others.find("; ", i)) != std::string::npos; ) { others.replace(i, 2, "\n"); }
                }
            }
        }

        bool LowMemoryBoot() {
            if (FILE *f = std::fopen(LauncherTxt, "r")) {
                char b[1024] = {};
                const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
                std::fclose(f);
                b[n] = 0;
                return std::strstr(b, "result=low-memory") != nullptr;
            }
            return false;
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

        /* the settings the sysmodule reads at boot only */
        void SavedForBoot(const char *what) {
            if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
            Toast(test_install ? std::string("Saved - but a test arm file is installed, so the sysmodule ignores config.ini")
                               : std::string("Saved - restart the console to ") + what);
        }

        /* v0.4: the mode this boot is in (the sysmodule's word), else config.ini */
        int RunningConn() { return have_status ? ((st.reserved3 & 2) ? ConnNetwork : (st.reserved3 & 1) ? ((st.reserved3 & 8) ? 2 : 1) : 0) : set.conn; }
        bool RunningWebcam() { return RunningConn() == 1 || RunningConn() == 2; }
        bool RunningHomeMenu() { return have_status ? (st.reserved3 & 16) != 0 : set.home_menu; }   /* v0.7.14 */
        bool RunningOverlays() { return have_status ? (st.reserved3 & 32) != 0 : set.overlays; }     /* v0.7.14 */
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

        /* the status card: what the sysmodule is doing, and what to do next */
        std::string StateTitle() {
            if (!installed) { return "Not installed"; }
            if (!running) {
                if (!boot) { return "Off at boot"; }
                return LowMemoryBoot() ? "Not started" : "Starting soon";
            }
            if (!have_status) { return "Running"; }
            switch (st.state) {
                case 0: return "Starting";
                case 1: return "Off";
                case 2: return RunningWebcam() ? "Waiting for a camera app" : "Waiting for the viewer";
                case 3: return "Waiting for a game";
                case 4: return "Streaming";
                case 5: return "Encoder stalled";
                case 6: return "Test install";
                case 7: return "Firmware not tested";
                case 8: return "Not streaming this app";
                default: return "Unknown";
            }
        }

        std::string StateDetail() {
            if (!installed) { return "Copy the Switch Frame Tap zip to the SD card and restart the console."; }
            if (!running) {
                if (!boot) { return "Turn on \"Start with the console\" below, then restart."; }
                return LowMemoryBoot() ? "Other sysmodules use the memory it needs - see Setup."
                                       : "It starts about 30 seconds after boot, once the HOME menu is up.";
            }
            if (!have_status) { return "An older version answered - update the sysmodule."; }
            char b[160];
            switch (st.state) {
                case 0: return "It waits for a game.";
                case 1: return "Turn on \"Stream to PC\" below to stream again.";
                case 2:
                    if (RunningWebcam()) { return "Open a camera app on the PC (OBS, Camera, a browser) and pick Switch Frame Tap."; }
                    if (RunningNetwork()) {
                        std::snprintf(b, sizeof(b), "Open the viewer on the PC, set it to Network and connect to %s.", IpString().c_str());
                        return b;
                    }
                    return "Open the viewer on the PC and connect the USB-C cable.";
                case 3: return "Start a game to stream it.";
                case 4:
                    std::snprintf(b, sizeof(b), "%u x %u%s", st.width, st.height,
                                  NetworkViewer() ? " over the network" : RunningWebcam() ? " to the camera app" : " over USB");
                    return b;
                case 5: return "Restart the console to stream again.";
                case 6: return "Live mode is not on in the test arm file.";
                case 7: return "Streaming is off on this firmware - see Setup, Allow untested firmware.";
                case 8: return "It is on the never-streamed list (Stream, Apps never streamed).";
                default: return "";
            }
        }

        SDL_Color StreamColor() {
            if (!installed || (running && have_status && (st.state == 5 || st.state == 7))) { return ui::Bad; }
            if (!running || !have_status) { return ui::Warn; }
            if (st.state == 4) { return ui::Good; }
            return st.state == 1 ? ui::Faint : ui::Acc;
        }

        std::string ConnShort() {
            switch (RunningConn()) {
                case 1: return "Webcam (OBS)";
                case 2: return "Webcam (any app)";
                case ConnNetwork: return "Network  " + IpString();
                default: return "USB";
            }
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

        std::string FwText() {
            char b[64];
            std::snprintf(b, sizeof(b), "%u.%u.%u%s", HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw), FwTested() ? "" : " - untested");
            return b;
        }
        bool FwTested() { return HOSVER_MAJOR(fw) >= 21 && HOSVER_MAJOR(fw) <= 23; }
        std::string AmsText() {
            if (ams == 0) { return "Unknown"; }
            char b[48];
            std::snprintf(b, sizeof(b), "%u.%u.%u", static_cast<u32>((ams >> 56) & 0xFF), static_cast<u32>((ams >> 48) & 0xFF), static_cast<u32>((ams >> 40) & 0xFF));
            return b;
        }

        static std::string Check(bool ok) { return ok ? "Installed" : "Not found"; }

        void Build() {
            pages.clear();
            ReadLauncher();
            const std::string boot_help =
                "Whether the sysmodule starts with the console - about 30 s after boot, once the HOME menu is up, and only if "
                "the other sysmodules leave it the memory it needs. Takes effect at the next boot. "
                "Off is the clean way to disable it completely.";

            /* ---- Home ---- */
            {
                Page p{ "Home", {} };
                p.rows.push_back({ Kind::Hero, "", nullptr, nullptr, nullptr, "", nullptr, {} });
                p.rows.push_back(Toggle("Stream to PC", [this] { return StreamOn(); }, [this](int) { SetStream(!StreamOn()); },
                    "Off: the sysmodule leaves the game alone (no debugging, cheats work) and sends nothing. "
                    "The same switch as in the overlay; it is remembered across reboots."));
                p.rows.push_back(Toggle("Start with the console", [this] { return installed && boot; }, [this](int) { SetBoot(!boot); }, boot_help,
                    [this] { return installed ? std::string() : std::string("Not installed"); }, [] { return ui::Bad; }));
                p.rows.push_back({ Kind::Tiles, "", nullptr, nullptr, nullptr, "", nullptr, {
                    { "Firmware", [this] { return FwText(); }, [this] { return FwTested() ? ui::Ink : ui::Warn; } },
                    { "Atmosphere", [this] { return AmsText(); }, nullptr },
                    { "Free memory at boot", [this] { return free_mem.empty() ? std::string("-") : free_mem; }, nullptr },
                    { "Screenshots", [this] {
                          char b[32];
                          std::snprintf(b, sizeof(b), "%zu saved", shot_files);
                          return std::string(b);
                      }, nullptr },
                } });
                pages.push_back(std::move(p));
            }

            /* ---- Stream ---- */
            {
                Page p{ "Stream", {} };
                p.rows.push_back(Choice("Connection", [this] {
                        std::string v = ConnNames[set.conn];
                        if (have_status && RunningConn() != set.conn) { v += " - after a restart"; }
                        else if (set.conn == ConnNetwork) { v += "  " + IpString(); }
                        return v;
                    },
                    [this](int d) {
                        set.conn = (set.conn + (d > 0 ? 1 : ConnCount - 1)) % ConnCount;
                        SavedForBoot("switch the connection");
                    },
                    "How the stream reaches the PC - one at a time. USB - PC viewer: the viewer app over the USB-C cable "
                    "(handheld; sound, the lowest latency). USB - webcam (OBS): the Switch shows up as a USB camera, "
                    "H.264 up to 1080p60, for OBS - picture only. USB - webcam (any app): an uncompressed camera the "
                    "Windows Camera app, browsers, Discord or Zoom can open - 720p at 25 fps or 432p at 60. Network: the viewer "
                    "app over Wi-Fi or the dock's LAN port, so you can play docked; set the viewer to Network too. A LAN adapter "
                    "or strong 5 GHz Wi-Fi at Medium quality works best. Changes need a restart.",
                    [this] { return (have_status && RunningConn() != set.conn) ? ui::Warn : ui::Ink; }));
                p.rows.push_back(Toggle("Game audio", [this] { return set.audio; }, [this](int) { set.audio = !set.audio; Saved(); },
                    "Sends the game's sound to the PC viewer (M in the viewer mutes it). It comes from the console's own video "
                    "recorder, as with SysDVR - games that turn video capture off have no sound in the stream unless "
                    "\"Sound in no-record games\" is on. Webcam mode has no sound: a USB camera cannot carry it from the Switch.",
                    [this] {
                        if (!set.audio || !have_status || st.version < 4) { return std::string(); }
                        switch (st.reserved & 0xFF) {
                            case 1: return std::string("Playing");
                            case 2: return std::string("Recorder unavailable");
                            case 3: return std::string("Could not start");
                            default: return std::string();
                        }
                    },
                    [this] { return (st.reserved & 0xFF) >= 2 ? ui::Warn : ui::Good; }));
                p.rows.push_back(Toggle("Sound in no-record games", [this] { return sound == sftsound::State::On; },
                    [this](int) {
                        if (sound == sftsound::State::Missing) { Toast("The patches are not on the SD card - reinstall the Switch Frame Tap zip"); return; }
                        const bool on = sound != sftsound::State::On;
                        if (sftsound::Set(on)) { sound_changed = !sound_changed; Toast(on ? "On - restart the console to apply" : "Off - restart the console to apply"); }
                        else { Toast("Could not change it - check the SD card"); }
                    },
                    "Some games turn the console's video recorder off (Super Smash Bros. Ultimate, for one), and with it the "
                    "sound the stream gets. This turns on dvr-patches (by exelix11, bundled): they let the recorder run for "
                    "every game, so their sound comes through. Off by default; a restart applies it. Turn it off again if a "
                    "game crashes with it on (a crash report for 0100000000000023).",
                    [this] { return sound == sftsound::State::Missing ? std::string("Not installed") : sound_changed ? std::string("Restart to apply") : std::string(); },
                    [] { return ui::Warn; }));
                p.rows.push_back(Toggle("Screen off while streaming", [this] { return set.screen_off; }, [this](int) { set.screen_off = !set.screen_off; Saved(); },
                    "Turns the Switch's own screen dark while the stream runs, to save battery when you play on the PC. "
                    "It comes back on the moment the stream stops: close the viewer, press HOME, or switch the stream "
                    "off. Handheld only - a TV is not affected."));
                p.rows.push_back(Action("Apps never streamed", [this] {
                        char b[32];
                        std::snprintf(b, sizeof(b), confirm_clear ? "Press A to clear" : excluded == 1 ? "1 app" : "%zu apps", excluded);
                        return std::string(b);
                    },
                    [this](int) {
                        if (excluded == 0) { Toast("No app is excluded"); return; }
                        if (!confirm_clear) { confirm_clear = true; return; }
                        confirm_clear = false;
                        std::remove(ExclFile);
                        if (have_status) { SftapReload(); }
                        Toast("Every app is streamed again");
                    },
                    "Apps that refuse to run under a debugger - such as TiCo's protected builds - need the sysmodule to leave "
                    "them alone. Turn \"Stream this app\" off in the overlay while one runs, and it is never attached again "
                    "(no stream, no screenshots). A here clears the list.",
                    [this] { return confirm_clear ? ui::Warn : ui::Dim; }));
                pages.push_back(std::move(p));
            }

            /* ---- Picture ---- */
            {
                Page p{ "Picture", {} };
                p.rows.push_back(Choice("Quality", [this] { return std::string(QualityNames[set.quality]); },
                    [this](int d) { set.quality = (set.quality + (d > 0 ? 1 : 2)) % 3; Saved(); },
                    "High is the sharpest picture: about 80 Mbps in a fast 1080p race, so a recording from the PC viewer "
                    "takes about 600 MB a minute. Medium is roughly half that and Low about a third, still clean. "
                    "Calm games and 720p need much less. Lower it if the stream stutters on a slow cable or PC."));
                p.rows.push_back(Choice("Keyframes", [this] { return std::string(KeyframeNames[set.keyframe]); },
                    [this](int d) { set.keyframe = (set.keyframe + (d > 0 ? 1 : 3)) % 4; Saved(); },
                    "A keyframe is a complete picture; the frames between only carry changes. More keyframes recover faster "
                    "from a glitch, fewer save bandwidth."));
                p.rows.push_back(Choice("Maximum resolution", [this] { return std::string(set.cap720 ? "720p" : "1080p (native)"); },
                    [this](int) { set.cap720 = !set.cap720; Saved(); },
                    "1080p streams what the game renders in docked mode (with ReverseNX-RT in handheld). 720p scales it down: "
                    "less bandwidth, and it runs at stock clocks."));
                pages.push_back(std::move(p));
            }

            /* ---- Screenshots ---- */
            {
                Page p{ "Screenshots", {} };
                p.rows.push_back(Toggle("Screenshot button", [this] { return set.shot; }, [this](int) { set.shot = !set.shot; Saved(); },
                    "Press the button combo in any game to save the picture at the game's own resolution (1920x1080 "
                    "docked or with ReverseNX-RT, 1280x720 handheld) as a PNG. Works with or without the PC viewer; the PC "
                    "stream pauses briefly while it saves. With the Ultrahand overlay menu installed, a notification confirms each one."));
                p.rows.push_back(Choice("Buttons", [this] { return set.shot_combo == ComboCustom ? CustomComboText() : std::string(ComboNames[set.shot_combo]); },
                    [this](int d) { set.shot_combo = (set.shot_combo + (d > 0 ? 1 : ComboCustom)) % (ComboCustom + 1); Saved(); },
                    "Hold these together to take a screenshot. Pick a combo your game does not use - L3 + R3 (pressing both "
                    "sticks in) is rarely used by games. Custom: your own buttons (below)."));
                p.rows.push_back(Action("Record your own buttons", [this] {
                        return set.shot_combo == ComboCustom ? std::string("Press A, then hold them") : std::string("For Custom");
                    }, [](int) { g_record_combo = true; },
                    "Record your own screenshot combo: press A here, then hold the buttons you want together for two "
                    "seconds. Add the Capture button with the next setting - pressing it here would take a picture of this app."));
                p.rows.push_back(Toggle("Custom combo with Capture", [this] { return set.shot_combo == ComboCustom && set.shot_capture; },
                    [this](int) { set.shot_combo = ComboCustom; set.shot_capture = !set.shot_capture; Saved(); },
                    "The Capture button (the square one) as part of the combo - for example ZR + Capture. The console still "
                    "takes its own screenshot (a JPEG in the Album) when you press it, so you get both: the system's and this "
                    "native-resolution PNG. Capture on its own also works: every system screenshot then gets a PNG too."));
                p.rows.push_back(Choice("Save to", [this] { return std::string(set.shot_album ? "With the system's screenshots" : "Switch Frame Tap folder"); },
                    [this](int) { set.shot_album = !set.shot_album; Saved(); },
                    "Where the screenshots go. With the system's screenshots: the same Album folder on the SD card as the "
                    "console's own (Nintendo/Album/year/month/day - under emuMMC, its own Nintendo folder), named by date so a "
                    "PC lists them together. The Switch's Album app does not show them (it only shows Nintendo's own JPEGs); "
                    "this app's screenshot viewer does. Switch Frame Tap folder: /switch/switch-frame-tap/screenshots."));
                p.rows.push_back(Action("View screenshots", [this] {
                        char b[32];
                        std::snprintf(b, sizeof(b), "%zu saved", shot_files);
                        return std::string(b);
                    }, [](int) { g_open_gallery = true; },
                    "Browse the screenshots on the SD card: A shows one full screen (Left/Right for the others), Y deletes."));
                p.rows.push_back(Info("Taken this boot", [this] {
                        if (!have_status || st.version < 2) { return std::string("-"); }
                        char b[48];
                        std::snprintf(b, sizeof(b), st.shot_fails ? "%u (%u failed)" : "%u", st.shots, st.shot_fails);
                        return std::string(b);
                    }, [this] { return (have_status && st.version >= 2 && st.shot_fails) ? ui::Warn : ui::Dim; },
                    "Screenshots saved since the console started. Each 1080p PNG is about 6 MB (stored uncompressed, "
                    "exactly the pixels the game drew)."));
                pages.push_back(std::move(p));
            }

            /* ---- Beta ---- */
            {
                Page p{ "Beta", {} };
                p.rows.push_back(Section("Read at boot - restart the console after a change"));
                p.rows.push_back(Toggle("Stream the HOME menu", [this] { return set.home_menu; },
                    [this](int) { set.home_menu = !set.home_menu; SavedForBoot("apply it"); },
                    "Also streams the HOME menu and the system screens when no game is on screen: the HOME menu itself, "
                    "System Settings, the user picker, Controllers, My Page, the Album and the Homebrew Menu. "
                    "No sound outside games yet.",
                    [this] { return (have_status && RunningHomeMenu() != set.home_menu) ? std::string("Restart to apply") : std::string(); },
                    [] { return ui::Warn; }));
                p.rows.push_back(Toggle("Stream the overlays", [this] { return set.overlays; },
                    [this](int) { set.overlays = !set.overlays; SavedForBoot("apply it"); },
                    "Puts the console's overlays in the stream, on top of the game: the volume bar, the quick settings "
                    "(hold HOME), the power menu (hold POWER) and Tesla / Ultrahand menus. They are copied 10-20 times a "
                    "second, so they move less smoothly than the game, and the power menu is a little soft.",
                    [this] { return (have_status && RunningOverlays() != set.overlays) ? std::string("Restart to apply") : std::string(); },
                    [] { return ui::Warn; }));
                /* v0.7.18 */
                p.rows.push_back(Section("From the next app you start"));
                p.rows.push_back(Toggle("Homebrew in docked mode (1080p)", [this] { return set.hb_docked; },
                    [this](int) { set.hb_docked = !set.hb_docked; Saved(); },
                    "Tells homebrew started by holding R over a game, or from a forwarder on the HOME menu, that the console is docked, so "
                    "apps that choose their resolution from that - SDL2 ports, emulators - draw at 1080p and stream at "
                    "1080p. The console's screen still shows them. Apps with a fixed 1280x720 picture stay 720p. "
                    "Not for homebrew opened from the Album: there it has too little memory for 1080p."));
                pages.push_back(std::move(p));
            }

            /* ---- Extras (v0.7.5: the optional tools, from their authors' GitHub releases) ---- */
            {
                Page p{ "Extras", {} };
                p.rows.push_back(Section("Downloaded from each tool's GitHub release"));
                auto extra = [this, &p](const extras::Tool *t, bool (*has)(), std::function<std::string()> note, std::function<std::string()> blocked, const char *help) {
                    const int idx = static_cast<int>(p.rows.size());
                    p.rows.push_back(Action(t->name, [this, idx, has, note, blocked] {
                            if (confirm_extra == idx) { return std::string(has() ? "A again: update" : "A again: install"); }
                            const std::string b = blocked();
                            if (!b.empty()) { return b; }
                            if (!has()) { return std::string("Not installed"); }
                            const std::string n = note();
                            return n.empty() ? std::string("Installed") : n;
                        },
                        [this, idx, t, blocked](int) {
                            const std::string b = blocked();
                            if (!b.empty()) { Toast(b + " - one overclock tool at a time: remove it first"); return; }
                            if (confirm_extra != idx) { confirm_extra = idx; return; }
                            confirm_extra = -1;
                            g_pending_install = t;
                        }, help,
                        [this, idx, has, note, blocked] {
                            if (confirm_extra == idx) { return ui::Warn; }
                            if (!blocked().empty() || !has()) { return ui::Dim; }
                            return note().empty() ? ui::Good : ui::Warn;
                        }));
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
                pages.push_back(std::move(p));
            }

            /* ---- Setup ---- */
            {
                Page p{ "Setup", {} };
                p.rows.push_back(Section("What is on the SD card"));
                auto check = [&p](const char *label, const char *path, bool needed, const char *help) {
                    p.rows.push_back(Info(label, [path] { return Check(FileExists(path)); },
                        [path, needed] { return FileExists(path) ? ui::Good : (needed ? ui::Bad : ui::Dim); }, help));
                };
                check("Sysmodule", ModuleNsp, true, "atmosphere/contents/0100000000000C20/exefs.nsp - the part that streams.");
                check("Memory helper", "sdmc:/atmosphere/contents/0100000000000C21/exefs.nsp", true,
                      "atmosphere/contents/0100000000000C21/exefs.nsp - starts the sysmodule once the console has booted, and while a viewer "
                      "is connected lends it the streaming memory, so that game manuals, news and the eShop work whenever you are not streaming.");
                check("Overlay", "sdmc:/switch/.overlays/switch-frame-tap.ovl", false,
                      "switch/.overlays/switch-frame-tap.ovl - stream on/off, the beta features and handheld/docked from inside a game "
                      "(needs the Tesla overlay loader - Ultrahand in Extras).");
                p.rows.push_back(Info("System memory", [this] { return free_mem.empty() ? std::string("Not measured this boot") : free_mem + " free before it started"; }, nullptr,
                    "All sysmodules share a few MB of the console's system memory with the system itself. When too little is left, "
                    "launching a game can end on a black screen, and Switch Frame Tap does not start at all (the console still boots). "
                    "Turning off a sysmodule you do not use frees some." +
                    (others.empty() ? std::string() : std::string(" Sysmodules running at boot, biggest first:\n") + others)));
                p.rows.push_back(Info("Firmware", [this] { return FwText(); }, [this] { return FwTested() ? ui::Dim : ui::Warn; },
                    "This version was tested on firmware 21.x to 23.x. On other firmware the sysmodule does not "
                    "stream unless \"Allow untested firmware\" is on."));
                p.rows.push_back(Info("Atmosphere", [this] { return AmsText(); }, nullptr, "The custom firmware this runs on."));
                p.rows.push_back(Info("Install type", [this] { return std::string(test_install ? "Test (applet-mitm.armed)" : "Release"); },
                    [this] { return test_install ? ui::Warn : ui::Dim; },
                    "A test install is driven by sdmc:/applet-mitm.armed and ignores config.ini. Delete that file for normal use."));
                p.rows.push_back(Section("Advanced"));
                p.rows.push_back(Choice("Start delay after boot", [this] { char b[32]; std::snprintf(b, sizeof(b), "%d s", DelaySeconds[set.delay]); return std::string(b); },
                    [this](int d) { set.delay = (set.delay + (d > 0 ? 1 : 4)) % 5; Saved(); },
                    "Seconds after boot before the sysmodule starts looking for a game. Applies from the next boot."));
                p.rows.push_back(Toggle("Allow untested firmware", [this] { return set.any_fw; }, [this](int) { set.any_fw = !set.any_fw; Saved(); },
                    "Stream on firmware this version was not tested on. It may work; it may also freeze the console, "
                    "which needs a hard power-off. Applies from the next boot.", nullptr, nullptr));
                pages.push_back(std::move(p));
            }

            /* ---- Updates (v0.7.14) ---- */
            {
                Page p{ "Updates", {} };
                p.rows.push_back(Section("From this project's GitHub releases"));
                p.rows.push_back(Action("Check for updates", [this] {
                        switch (upd) {
                            case Upd::Latest:    return std::string("Up to date");
                            case Upd::Found:     return upd_rel.tag + " is available";
                            case Upd::Installed: return upd_rel.tag + " installed";
                            case Upd::Failed:    return upd_msg;
                            default:             return std::string("Not checked yet");
                        }
                    }, [](int) { g_pending_update = 1; },
                    "Asks GitHub for the newest Switch Frame Tap - only full releases, unless \"Include betas\" below is on. "
                    "The console must be online.",
                    [this] { return upd == Upd::Found ? ui::Good : upd == Upd::Failed ? ui::Warn : upd == Upd::Installed ? ui::Acc : ui::Dim; }));
                p.rows.push_back(Action("Install the update", [this] {
                        if (upd == Upd::Installed) { return std::string("Restart the console to finish"); }
                        if (upd != Upd::Found) { return std::string("-"); }
                        return confirm_update ? "A again: install " + upd_rel.tag : upd_rel.tag;
                    }, [this](int) {
                        if (upd == Upd::Installed) { Toast("Restart the console: hold POWER, Power Options, Restart"); return; }
                        if (upd != Upd::Found) { Toast("Check for updates first"); return; }
                        if (!confirm_update) { confirm_update = true; return; }
                        confirm_update = false;
                        g_pending_update = 2;
                    },
                    "Downloads the new version's Switch zip and puts it on the SD card, as a manual update would: the "
                    "sysmodule, the memory helper, the overlay and this app. Your settings are kept. The console runs the new "
                    "version after a restart.",
                    [this] { return confirm_update ? ui::Warn : upd == Upd::Found ? ui::Good : upd == Upd::Installed ? ui::Acc : ui::Dim; }));
                p.rows.push_back(Toggle("Include betas", [this] { return set.update_betas; },
                    [this](int) {
                        set.update_betas = !set.update_betas;
                        if (!SaveSettings(set)) { Toast("Could not write config.ini"); return; }
                        if (upd == Upd::Latest || upd == Upd::Found) { upd = Upd::Unknown; }
                        Toast(set.update_betas ? "Betas are offered too - check again" : "Only full releases are offered");
                    },
                    "Betas (pre-releases) try new things before a full release. They work for most people, but have had "
                    "less testing - turn this off to stay on full releases."));
                p.rows.push_back(Info("This version", [] { return std::string(AppVersion); }, nullptr,
                    "The version of this app. The sysmodule and the overlay come in the same zip, so they match it."));
                pages.push_back(std::move(p));
            }

            /* ---- About ---- */
            {
                Page p{ "About", {} };
                p.rows.push_back(Info("Version", [] { return std::string(AppVersion); }, nullptr,
                    "Switch Frame Tap - native 1080p60 game streaming to a PC, over USB or the network."));
                p.rows.push_back(Info("Source code", [] { return std::string("github.com/TomasUribe/switch-frame-tap"); }, nullptr,
                    "Free and open source (GPL-2.0). Bug reports and ideas: the GitHub issues, or the GBAtemp thread."));
                p.rows.push_back(Info("If something goes wrong", [] { return std::string("Hold Volume Up at boot"); }, nullptr,
                    "Holding Volume Up while Atmosphere boots skips all sysmodules. Or turn off \"Start with the console\" on the "
                    "Home page, or delete atmosphere/contents/0100000000000C20 and 0100000000000C21 on a PC."));
                p.rows.push_back(Info("Thanks", [] { return std::string("Bundled and optional tools"); }, nullptr,
                    "dvr-patches by exelix11 (sound in no-record games), SaltyNX and ReverseNX-RT by masagrator, Ultrahand by "
                    "ppkantorski, sys-clk by the RetroNX team, Horizon OC by its team, and Atmosphere by SciresM and contributors."));
                pages.push_back(std::move(p));
            }

            for (auto &pg : pages) { pg.sel = FirstSelectable(pg); }
            page = 0;
        }

        static int FirstSelectable(const Page &p) {
            for (size_t i = 0; i < p.rows.size(); ++i) { if (Selectable(p.rows[i])) { return static_cast<int>(i); } }
            return -1;
        }

        Page &Cur() { return pages[page]; }
        const Row *CurRow() { Page &p = Cur(); return (in_content && p.sel >= 0) ? &p.rows[p.sel] : nullptr; }

        void Unconfirm() { confirm_clear = false; confirm_extra = -1; confirm_update = false; }

        void GoPage(int i) {
            Unconfirm();
            page = std::clamp(i, 0, static_cast<int>(pages.size()) - 1);
            if (Cur().sel < 0) { in_content = false; }
        }

        void Move(int d) {
            Unconfirm();
            if (!in_content) { GoPage(page + d); return; }
            Page &p = Cur();
            int i = p.sel;
            do { i += d; } while (i >= 0 && i < static_cast<int>(p.rows.size()) && !Selectable(p.rows[i]));
            if (i >= 0 && i < static_cast<int>(p.rows.size())) { p.sel = i; }
        }

        void Activate(int d) {
            Page &p = Cur();
            if (p.sel < 0) { return; }
            Row &r = p.rows[p.sel];
            if (r.change) { r.change(d); Refresh(); }
        }
    };

    /* ---- drawing --------------------------------------------------------- */

    constexpr int SideW = 304;                 /* the sidebar */
    constexpr int CX = 336, CW = 912;          /* the page's column */
    constexpr int ViewTop = 92, ViewBottom = 650, BarY = 662;
    constexpr int CardH = 72, Gap = 10, Pad = 24;
    constexpr int NavY = 116, NavH = 50, NavStep = 56;

    /* where a tap lands: drawn this frame */
    enum class HitKind { Nav, Row, Left, Right, Switch };
    struct Hit { SDL_Rect r; HitKind what; int index; };
    std::vector<Hit> g_hits;
    void AddHit(int x, int y, int w, int h, HitKind k, int i) { g_hits.push_back({ { x, y, w, h }, k, i }); }

    int RowHeight(App &a, const Page &p, int i) {
        const Row &r = p.rows[i];
        switch (r.kind) {
            case Kind::Section: return 46;
            case Kind::Hero:    return 168 + Gap;
            case Kind::Tiles:   return 88 + Gap;
            default: break;
        }
        int h = CardH;
        if (a.in_content && p.sel == i && !r.help.empty()) { h = 62 + ui::TextH(r.help, ui::Body, CW - 2 * Pad) + 20; }
        return h + Gap;
    }

    void DrawHero(App &a, int x, int y, int w) {
        const int h = 168;
        ui::RRect(x, y, w, h, 20, ui::Surf);
        const SDL_Color c = a.StreamColor();
        ui::Disc(x + 40, y + 54, 16, ui::Alpha(c, 46));
        ui::Disc(x + 40, y + 54, 8, c);
        ui::Text(a.StateTitle(), x + 70, y + 30, ui::Big, ui::Ink);
        const bool streaming = a.have_status && a.st.state == 4;
        ui::Text(a.StateDetail(), x + 28, y + 88, ui::Body, ui::Dim, 0, w - 56 - (streaming ? 170 : 0));
        int cx = x + 28;
        const int cy = y + h - 46;
        cx += ui::Chip(a.ConnShort(), cx, cy, ui::Acc) + 8;
        if (a.RunningHomeMenu()) { cx += ui::Chip("HOME menu", cx, cy, ui::Good) + 8; }
        if (a.RunningOverlays()) { cx += ui::Chip("Overlays", cx, cy, ui::Good) + 8; }
        if (!a.StreamOn() && a.installed) { cx += ui::Chip("Stream off", cx, cy, ui::Faint) + 8; }
        if (a.upd == App::Upd::Found) { ui::Chip("Update " + a.upd_rel.tag + " available", cx, cy, ui::Warn); }
        if (streaming) {
            char b[32];
            std::snprintf(b, sizeof(b), "%u.%u", a.st.fps_x10 / 10, a.st.fps_x10 % 10);
            ui::Text(b, x + w - 30, y + 26, ui::Big, ui::Ink, 2);
            ui::Text("fps", x + w - 30, y + 76, ui::Small, ui::Dim, 2);
            if (a.st.game_fps_x10 != 0) {
                std::snprintf(b, sizeof(b), "game %u.%u", a.st.game_fps_x10 / 10, a.st.game_fps_x10 % 10);
                ui::Text(b, x + w - 30, y + 100, ui::Small, ui::Faint, 2);
            }
        }
    }

    void DrawTiles(const Row &r, int x, int y, int w) {
        const int n = static_cast<int>(r.tiles.size());
        if (n == 0) { return; }
        const int tw = (w - (n - 1) * 12) / n;
        for (int i = 0; i < n; ++i) {
            const Tile &t = r.tiles[i];
            const int tx = x + i * (tw + 12);
            ui::RRect(tx, y, tw, 88, 16, ui::Surf);
            ui::Text(t.label, tx + 20, y + 16, ui::Small, ui::Faint);
            ui::Text(t.value ? t.value() : std::string(), tx + 20, y + 44, ui::Label, t.color ? t.color() : ui::Ink);
        }
    }

    void DrawCard(App &a, Page &p, int i, int x, int y, int w, int h) {
        const Row &r = p.rows[i];
        const bool focus = a.in_content && p.sel == i;
        AddHit(x, y, w, h, HitKind::Row, i);
        if (focus) { ui::RRectRing(x - 3, y - 3, w + 6, h + 6, 19, 3, ui::Acc, ui::Surf2); }
        else { ui::RRect(x, y, w, h, 16, ui::Surf); }
        ui::Text(r.label, x + Pad, y + 22, ui::Label, ui::Ink);
        const int xr = x + w - Pad, cy = y + CardH / 2;
        const std::string v = r.value ? r.value() : std::string();
        switch (r.kind) {
            case Kind::Toggle: {
                ui::Toggle(xr, cy, r.on && r.on());
                AddHit(xr - 76, y, 76 + Pad, CardH, HitKind::Switch, i);
                if (!v.empty()) { ui::Text(v, xr - 58 - 18, y + 25, ui::Body, r.color ? r.color() : ui::Dim, 2); }
                break;
            }
            case Kind::Choice: {
                const SDL_Color cc = focus ? ui::Acc : ui::Faint;
                const int vw = ui::TextW(v, ui::Body);
                ui::Chevron(xr - 6, cy, 16, +1, cc);
                ui::Text(v, xr - 28, y + 25, ui::Body, r.color ? r.color() : ui::Ink, 2);
                ui::Chevron(xr - 28 - vw - 22, cy, 16, -1, cc);
                AddHit(xr - 28 - vw - 44, y, 44, CardH, HitKind::Left, i);
                AddHit(xr - 26, y, 26 + Pad, CardH, HitKind::Right, i);
                break;
            }
            case Kind::Action:
                ui::Chevron(xr - 6, cy, 16, +1, focus ? ui::Acc : ui::Faint);
                ui::Text(v, xr - 28, y + 25, ui::Body, r.color ? r.color() : ui::Dim, 2);
                break;
            default:
                ui::Text(v, xr, y + 25, ui::Body, r.color ? r.color() : ui::Dim, 2);
                break;
        }
        if (focus && !r.help.empty()) { ui::Text(r.help, x + Pad, y + 62, ui::Body, ui::Dim, 0, w - 2 * Pad); }
    }

    void DrawSidebar(App &a) {
        ui::Fill(0, 0, SideW, ui::H, ui::Surf);
        ui::Logo(26, 30, 40);
        ui::Text("Switch Frame Tap", 100, 26, ui::Label, ui::Ink);
        ui::Text(std::string("v") + AppVersion, 100, 54, ui::Small, ui::Faint);
        for (int i = 0; i < static_cast<int>(a.pages.size()); ++i) {
            const int y = NavY + i * NavStep;
            const bool cur = i == a.page;
            if (cur) {
                if (!a.in_content) { ui::RRectRing(14, y, SideW - 28, NavH, 12, 3, ui::Acc, ui::Surf3); }
                else { ui::RRect(14, y, SideW - 28, NavH, 12, ui::Surf3); }
            }
            ui::Text(a.pages[i].name, 36, y + 12, ui::Label, cur ? ui::Ink : ui::Dim);
            AddHit(14, y, SideW - 28, NavH, HitKind::Nav, i);
        }
        /* the stream, at a glance */
        const SDL_Color c = a.StreamColor();
        ui::Disc(36, ui::H - 38, 6, c);
        ui::Text(a.StateTitle(), 52, ui::H - 50, ui::Small, ui::Dim);
    }

    /* button hints, right to left: glyph, the letter the PC preview draws, label */
    struct Hint { const char *glyph; char letter; const char *label; };
    void DrawHints(std::initializer_list<Hint> list, int x0) {
        int x = ui::W - 32;
        for (const Hint &h : list) {
            x -= ui::TextW(h.label, ui::Body);
            ui::Text(h.label, x, BarY + 17, ui::Body, ui::Dim);
            x -= 8;
            int gw = 26;
            if (ui::g_glyphs != nullptr) { if (const ui::CachedText *t = ui::Render(h.glyph, 0, ui::Ink, 0, true)) { gw = t->w; } }
            x -= gw;
            ui::Button(h.glyph, h.letter, x, BarY + 29);
            x -= 30;
        }
        ui::Fill(x0, BarY, ui::W - x0, 1, ui::Line);
    }

    void DrawToast(App &a, int cx) {
        if (a.toast_frames == 0) { return; }
        --a.toast_frames;
        const Uint8 al = a.toast_frames < 20 ? static_cast<Uint8>(a.toast_frames * 255 / 20) : 255;
        const int tw = ui::TextW(a.toast, ui::Body) + 44;
        ui::RRect(cx - tw / 2, 592, tw, 46, 23, ui::Alpha(ui::Surf3, al));
        ui::Text(a.toast, cx, 603, ui::Body, ui::Alpha(ui::Ink, al), 1);
    }

    void Draw(App &a) {
        g_hits.clear();
        SDL_SetRenderDrawColor(ui::g_ren, ui::Bg.r, ui::Bg.g, ui::Bg.b, 255);
        SDL_RenderClear(ui::g_ren);
        DrawSidebar(a);

        Page &p = a.Cur();
        ui::Text(p.name, CX, 26, ui::Title, ui::Ink);

        /* the page's rows, scrolled to keep the selection in view */
        std::vector<int> ys(p.rows.size());
        int total = 0;
        for (size_t i = 0; i < p.rows.size(); ++i) { ys[i] = total; total += RowHeight(a, p, static_cast<int>(i)); }
        const int view = ViewBottom - ViewTop;
        const int max_scroll = std::max(0, total - view);
        const int follow = a.in_content ? p.sel : -1;
        if (follow != p.seen_sel) {
            p.seen_sel = follow;
            if (follow >= 0) {
                int top = ys[follow];
                if (follow > 0 && p.rows[follow - 1].kind == Kind::Section) { top = ys[follow - 1]; }
                const int bot = ys[follow] + RowHeight(a, p, follow);
                if (top < p.target) { p.target = static_cast<float>(top); }
                if (bot - p.target > view) { p.target = static_cast<float>(bot - view); }
            } else {
                p.target = 0;
            }
        }
        p.target = std::clamp(p.target, 0.0f, static_cast<float>(max_scroll));
        p.scroll += (p.target - p.scroll) * 0.35f;
        if (std::fabs(p.target - p.scroll) < 0.5f) { p.scroll = p.target; }

        SDL_Rect clip = { CX - 8, ViewTop - 6, CW + 16, view + 12 };
        SDL_RenderSetClipRect(ui::g_ren, &clip);
        for (size_t i = 0; i < p.rows.size(); ++i) {
            const int idx = static_cast<int>(i);
            const int y = ViewTop + ys[i] - static_cast<int>(p.scroll);
            const int h = RowHeight(a, p, idx) - (p.rows[i].kind == Kind::Section ? 0 : Gap);
            if (y + h < ViewTop - 6 || y > ViewBottom + 6) { continue; }
            const Row &r = p.rows[i];
            switch (r.kind) {
                case Kind::Section: ui::Text(r.label, CX + 4, y + 14, ui::Small, ui::Faint); break;
                case Kind::Hero:    DrawHero(a, CX, y, CW); break;
                case Kind::Tiles:   DrawTiles(r, CX, y, CW); break;
                default:            DrawCard(a, p, idx, CX, y, CW, h); break;
            }
        }
        SDL_RenderSetClipRect(ui::g_ren, nullptr);
        if (total > view) {   /* a thin scroll bar */
            const int th = std::max(40, view * view / total);
            const int ty = ViewTop + static_cast<int>((view - th) * (p.scroll / std::max(1, max_scroll)));
            ui::RRect(ui::W - 18, ViewTop, 4, view, 2, ui::Line);
            ui::RRect(ui::W - 18, ty, 4, th, 2, ui::Surf3);
        }

        const Row *cur = a.CurRow();
        if (a.in_content) {
            const char *act = cur == nullptr || !cur->change ? nullptr : cur->kind == Kind::Action ? "Open" : "Change";
            if (act != nullptr) { DrawHints({ { "", '+', "Exit" }, { "", 'B', "Back" }, { "", 'A', act } }, SideW); }
            else { DrawHints({ { "", '+', "Exit" }, { "", 'B', "Back" } }, SideW); }
        } else {
            DrawHints({ { "", '+', "Exit" }, { "", 'A', "Open" } }, SideW);
        }
        DrawToast(a, CX + CW / 2);
        SDL_RenderPresent(ui::g_ren);
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
                tex = SDL_CreateTextureFromSurface(ui::g_ren, surf);
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

    constexpr int GalRowH = 58, GalListX = 32, GalListW = 440, GalTop = 92, GalBottom = 650;
    constexpr int PrevX = 500, PrevW = 748;

    void DrawImageFit(SDL_Texture *tex, int tw, int th, int x, int y, int w, int h) {
        if (tex == nullptr || tw == 0 || th == 0) { return; }
        int dw = w, dh = w * th / tw;
        if (dh > h) { dh = h; dw = h * tw / th; }
        SDL_Rect r = { x + (w - dw) / 2, y + (h - dh) / 2, dw, dh };
        SDL_RenderCopy(ui::g_ren, tex, nullptr, &r);
    }

    /* a screen of its own: the page title, the bar */
    void ScreenTop(const char *title, const std::string &count = "") {
        SDL_SetRenderDrawColor(ui::g_ren, ui::Bg.r, ui::Bg.g, ui::Bg.b, 255);
        SDL_RenderClear(ui::g_ren);
        ui::Logo(32, 28, 36);
        ui::Text(title, 108, 24, ui::Title, ui::Ink);
        if (!count.empty()) { ui::Chip(count, ui::W - 32, 30, ui::Dim, 2); }
    }

    void DrawGallery(Gallery &g) {
        g.Load();
        ++g.settle;
        if (g.full && g.tex) {
            SDL_SetRenderDrawColor(ui::g_ren, 0, 0, 0, 255);
            SDL_RenderClear(ui::g_ren);
            DrawImageFit(g.tex, g.tw, g.th, 0, 0, ui::W, ui::H);
            ui::Fill(0, BarY, ui::W, ui::H - BarY, ui::Alpha(ui::Bg, 200));
            char b[96];
            std::snprintf(b, sizeof(b), "%s   (%d of %zu)", ShotLabel(g.files[g.sel]).c_str(), g.sel + 1, g.files.size());
            ui::Text(b, 32, BarY + 17, ui::Body, ui::Ink);
            DrawHints({ { "", 'B', "Back" }, { "", 'Y', "Delete" }, { "", 'R', "Next" }, { "", 'L', "Previous" } }, 0);
            if (!g.note.empty()) { ui::Text(g.note, ui::W / 2, 600, ui::Body, ui::Warn, 1); }
            SDL_RenderPresent(ui::g_ren);
            return;
        }
        char cnt[48];
        std::snprintf(cnt, sizeof(cnt), g.files.size() == 1 ? "%zu screenshot" : "%zu screenshots", g.files.size());
        ScreenTop("Screenshots", cnt);
        if (g.files.empty()) {
            ui::RRect(240, 200, 800, 220, 24, ui::Surf);
            ui::Text("No screenshots yet", ui::W / 2, 250, ui::Title, ui::Ink, 1);
            ui::Text("Turn on the screenshot button, then press the combo in a game.", ui::W / 2, 310, ui::Body, ui::Dim, 1);
            DrawHints({ { "", 'B', "Back" } }, 0);
            if (!g.note.empty()) { ui::Text(g.note, 32, BarY + 17, ui::Body, ui::Warn); }
            SDL_RenderPresent(ui::g_ren);
            return;
        }
        /* the list */
        const int view = GalBottom - GalTop;
        if (g.sel * GalRowH - g.scroll < 0) { g.scroll = g.sel * GalRowH; }
        if (g.sel * GalRowH + GalRowH - g.scroll > view) { g.scroll = g.sel * GalRowH + GalRowH - view; }
        SDL_Rect clip = { GalListX - 4, GalTop - 4, GalListW + 8, view + 8 };
        SDL_RenderSetClipRect(ui::g_ren, &clip);
        for (int i = 0; i < static_cast<int>(g.files.size()); ++i) {
            const int y = GalTop + i * GalRowH - g.scroll;
            if (y + GalRowH < GalTop || y > GalBottom) { continue; }
            if (i == g.sel) { ui::RRectRing(GalListX, y, GalListW, GalRowH - 8, 14, 3, ui::Acc, ui::Surf2); }
            else { ui::RRect(GalListX, y, GalListW, GalRowH - 8, 14, ui::Surf); }
            ui::Text(ShotLabel(g.files[i]), GalListX + 20, y + 12, ui::Body, i == g.sel ? ui::Ink : ui::Dim);
        }
        SDL_RenderSetClipRect(ui::g_ren, nullptr);
        /* the preview */
        ui::RRect(PrevX, GalTop, PrevW, view - 8, 20, ui::Surf);
        if (g.tex_idx == g.sel && g.tex) {
            DrawImageFit(g.tex, g.tw, g.th, PrevX + 20, GalTop + 20, PrevW - 40, 396);
            char info[96];
            std::snprintf(info, sizeof(info), "%d x %d    %.1f MB", g.tw, g.th, g.bytes / 1048576.0);
            ui::Text(info, PrevX + 24, GalTop + 432, ui::Label, ui::Ink);
            ui::Text(g.files[g.sel].substr(g.files[g.sel].rfind("sdmc:", 0) == 0 ? 5 : 0), PrevX + 24, GalTop + 468, ui::Small, ui::Faint, 0, PrevW - 48);
        } else if (g.tex_idx == g.sel) {
            ui::Text("This file could not be opened.", PrevX + PrevW / 2, GalTop + 220, ui::Body, ui::Warn, 1);
        } else {
            ui::Text("Loading...", PrevX + PrevW / 2, GalTop + 220, ui::Body, ui::Dim, 1);
        }
        DrawHints({ { "", 'B', "Back" }, { "", 'Y', "Delete" }, { "", 'A', "View" } }, 0);
        if (!g.note.empty()) { ui::Text(g.note, 32, BarY + 17, ui::Body, ui::Warn); }
        SDL_RenderPresent(ui::g_ren);
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
        if (tx >= GalListX && tx < GalListX + GalListW && ty >= GalTop && ty < GalBottom) {
            const int i = (ty - GalTop + g.scroll) / GalRowH;
            if (i >= 0 && i < static_cast<int>(g.files.size())) {
                if (i == g.sel) { g.full = true; } else { g.Select(i); }
            }
        } else if (tx >= PrevX && ty >= GalTop && ty < GalBottom && !g.files.empty()) {
            g.full = true;
        }
    }

    /* a progress screen: a card in the middle with a bar */
    void DrawProgressCard(const char *title, const std::string &line1, const std::string &line2, double frac, const char *foot) {
        ScreenTop(title);
        ui::RRect(240, 170, 800, 320, 24, ui::Surf);
        ui::Text(line1, ui::W / 2, 218, ui::Title, ui::Ink, 1);
        ui::Text(line2.empty() ? std::string(" ") : line2, ui::W / 2, 280, ui::Label, ui::Acc, 1);
        const int bx = 320, bw = 640, by = 352, bh = 14;
        ui::RRect(bx, by, bw, bh, bh / 2, ui::Surf3);
        if (frac > 0.0) { ui::RRect(bx, by, std::max(bh, static_cast<int>(bw * (frac > 1.0 ? 1.0 : frac))), bh, bh / 2, ui::Acc); }
        ui::Text(foot, ui::W / 2, 400, ui::Body, ui::Dim, 1, 700);
    }

}

namespace {

    /* v0.7.11: hold the buttons for the custom screenshot combo: the same set
     * held for two seconds is recorded; touch cancels, 20 s gives up */
    void RecordCombo(App &app, PadState &pad) {
        auto draw = [](const char *line1, const std::string &names, double frac) {
            DrawProgressCard("Screenshot combo", line1, names.empty() ? std::string("-") : names, frac,
                             "The Capture button is a separate setting (pressing it here would take a picture of this app).");
            ui::Fill(0, BarY, ui::W, 1, ui::Line);
            ui::Text("Touch the screen to cancel", ui::W - 32, BarY + 17, ui::Body, ui::Dim, 2);
            SDL_RenderPresent(ui::g_ren);
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

    /* v0.7.5: a Get extras download, with its progress on screen; B cancels */
    void RunInstall(App &app, PadState &pad) {
        const extras::Tool &t = *g_pending_install;
        g_pending_install = nullptr;
        auto draw = [&t](const char *what, double frac) {
            DrawProgressCard("Get extras", t.name, what, frac, "From the official GitHub release");
            DrawHints({ { "", 'B', "Cancel" } }, 0);
            SDL_RenderPresent(ui::g_ren);
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

    /* v0.7.14: Check for updates / Install the update, with its progress on
     * screen; B cancels */
    void RunUpdate(App &app, PadState &pad, int mode) {
        const char *title = mode == 1 ? "Check for updates" : "Update";
        auto draw = [title](const char *what, double frac) {
            DrawProgressCard(title, "Switch Frame Tap", what, frac, "From this project's GitHub releases");
            DrawHints({ { "\uE0A1", 'B', "Cancel" } }, 0);
            SDL_RenderPresent(ui::g_ren);
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
        std::string err, done;
        if (mode == 1) {
            const int r = extras::CheckUpdate(AppVersion, app.set.update_betas, &app.upd_rel, progress, &err);
            if (r < 0) { app.upd = App::Upd::Failed; app.upd_msg = err; app.Toast("Could not check: " + err); }
            else if (r == 0) { app.upd = App::Upd::Latest; app.Toast(std::string("Switch Frame Tap ") + AppVersion + " is the newest"); }
            else { app.upd = App::Upd::Found; app.Toast(app.upd_rel.tag + " is available - Install the update below"); }
        } else if (extras::InstallUpdate(app.upd_rel, progress, &done, &err)) {
            app.upd = App::Upd::Installed;
            app.Toast(done);
        } else {
            app.upd_msg = err;
            app.Toast("The update did not finish: " + err);
        }
        app.Refresh();
    }

    /* touch on the main screen: a tap (lifted where it went down) acts; a
     * drag scrolls the page */
    struct Touch {
        bool down = false, dragging = false;
        int x0 = 0, y0 = 0, y_last = 0;
        float scroll0 = 0;
    };

    void TouchTap(App &app, int tx, int ty) {
        for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
            const SDL_Rect &r = it->r;
            if (tx < r.x || tx >= r.x + r.w || ty < r.y || ty >= r.y + r.h) { continue; }
            if (it->what == HitKind::Nav) { app.GoPage(it->index); app.in_content = false; return; }
            Page &p = app.Cur();
            const bool was = app.in_content && p.sel == it->index;
            app.in_content = true;
            p.sel = it->index;
            switch (it->what) {
                case HitKind::Switch: app.Activate(+1); break;
                case HitKind::Left:   app.Activate(-1); break;
                case HitKind::Right:  app.Activate(+1); break;
                default:              if (was) { app.Activate(+1); } break;
            }
            return;
        }
    }

    bool InContentColumn(int tx, int ty) { return tx >= CX - 8 && tx < CX + CW + 8 && ty >= ViewTop && ty < ViewBottom; }

}

#ifndef SFT_PREVIEW
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
    SDL_Window *win = SDL_CreateWindow("Switch Frame Tap", 0, 0, ui::W, ui::H, 0);
    ui::g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ui::g_ren, SDL_BLENDMODE_BLEND);

    PlFontData std_font = {}, ext_font = {};
    plGetSharedFontByType(&std_font, PlSharedFontType_Standard);
    plGetSharedFontByType(&ext_font, PlSharedFontType_NintendoExt);
    ui::LoadFonts(std_font.address, std_font.size, ext_font.address, ext_font.size);

    App app;
    app.fw = hosversionGet();
    if (R_FAILED(splGetConfig(static_cast<SplConfigItem>(65000), &app.ams))) { app.ams = 0; }
    app.Refresh();
    app.Build();

    u32 frame = 0, hold = 0;
    Touch touch;
    bool gallery = false, gal_touching = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);

        if (gallery) {
            if (down & HidNpadButton_Plus) { break; }
            HidTouchScreenState ts = {};
            if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
                if (!gal_touching) { gal_touching = true; GalleryTouch(g_gal, ts.touches[0].x, ts.touches[0].y); }
            } else {
                gal_touching = false;
            }
            if (!GalleryInput(g_gal, down, held, hold)) { gallery = false; app.Refresh(); continue; }
            DrawGallery(g_gal);
            continue;
        }
        if (down & HidNpadButton_Plus) { break; }
        if (down & HidNpadButton_B) {
            if (app.in_content) { app.in_content = false; app.Unconfirm(); } else { break; }
        }

        /* the pages: L / R from anywhere */
        if (down & (HidNpadButton_L | HidNpadButton_ZL)) { app.GoPage(app.page - 1); }
        if (down & (HidNpadButton_R | HidNpadButton_ZR)) { app.GoPage(app.page + 1); }

        /* D-pad / stick with auto-repeat */
        const u64 up_mask = HidNpadButton_Up | HidNpadButton_StickLUp;
        const u64 dn_mask = HidNpadButton_Down | HidNpadButton_StickLDown;
        if (down & up_mask) { app.Move(-1); hold = 0; }
        else if (down & dn_mask) { app.Move(1); hold = 0; }
        else if (held & (up_mask | dn_mask)) {
            if (++hold > 24 && hold % 6 == 0) { app.Move((held & up_mask) ? -1 : 1); }
        } else { hold = 0; }

        const u64 left = HidNpadButton_Left | HidNpadButton_StickLLeft, right = HidNpadButton_Right | HidNpadButton_StickLRight;
        if (!app.in_content) {
            if ((down & (HidNpadButton_A | right)) && app.Cur().sel >= 0) { app.in_content = true; }
        } else {
            const Row *r = app.CurRow();
            const bool choice = r != nullptr && r->kind == Kind::Choice;
            if (down & HidNpadButton_A) { app.Activate(+1); }
            else if (down & right) { if (choice) { app.Activate(+1); } }
            else if (down & left) { if (choice) { app.Activate(-1); } else { app.in_content = false; app.Unconfirm(); } }
        }

        /* touch: a tap acts on what it lands on; a drag in the page scrolls it */
        HidTouchScreenState ts = {};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            const int tx = ts.touches[0].x, ty = ts.touches[0].y;
            if (!touch.down) {
                touch = { true, false, tx, ty, ty, app.Cur().scroll };
            } else if (!touch.dragging && std::abs(ty - touch.y0) > 14 && InContentColumn(touch.x0, touch.y0)) {
                touch.dragging = true;
            }
            if (touch.dragging) {
                Page &p = app.Cur();
                p.target = touch.scroll0 - (ty - touch.y0);
                p.scroll = p.target;
            }
            touch.y_last = ty;
        } else if (touch.down) {
            if (!touch.dragging) { TouchTap(app, touch.x0, touch.y0); }
            touch = {};
        }

        if (g_pending_install != nullptr) { RunInstall(app, pad); touch = {}; continue; }
        if (g_pending_update != 0) { const int m = g_pending_update; g_pending_update = 0; RunUpdate(app, pad, m); touch = {}; continue; }
        if (g_record_combo) { g_record_combo = false; RecordCombo(app, pad); touch = {}; continue; }
        if (g_open_gallery) {
            g_open_gallery = false;
            g_gal.Open();
            gallery = true;
            gal_touching = true;      /* the tap that opened it must not also select in it */
            continue;
        }
        if (++frame % 30 == 0) { app.Refresh(); }
        Draw(app);
    }

    g_gal.Drop();
    extras::NetDown();
    IMG_Quit();
    ui::FreeAll();
    TTF_Quit();
    SDL_DestroyRenderer(ui::g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_sftap_open) { serviceClose(&g_sftap); }
    splExit();
    nifmExit();
    pmdmntExit();
    plExit();
    return 0;
}
#endif
