/*
 * Switch Frame Tap overlay (Tesla): turn the stream on and off, see what it
 * is doing, and switch the game between handheld and docked mode through
 * ReverseNX-RT (which needs SaltyNX).
 *
 *  - The stream: the applet-mitm sysmodule's "sftap" service
 *    (tier4/applet-mitm/source/applet_mitm_control.hpp).
 *  - The display mode: SaltyNX's shared memory, exactly as ReverseNX-RT's own
 *    overlay does it (github.com/masagrator/ReverseNX-RT, MIT): ask the
 *    "SaltySD" port for the handle (command 7), map 0x1000 bytes, find the
 *    block whose magic is "NXRT", and write its def/isDocked flags. SaltyNX
 *    applies them to the running game.
 */
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdio>
#include <cstring>

namespace {

    /* ---- sftap ---------------------------------------------------------- */

    struct StreamStatus {
        u32 version, state, enabled, width, height, fps_x10, game_fps_x10, sessions;
        u32 shot_enabled, shots, shot_fails, reserved;     /* version 2 (M99) */
        u64 app_tid; u32 app_excluded, reserved3;          /* version 3 (v0.1.1b) */
    };
    static_assert(sizeof(StreamStatus) == 64);

    enum : u32 {
        LiveState_Starting = 0, LiveState_Off, LiveState_WaitViewer, LiveState_WaitGame,
        LiveState_Streaming, LiveState_Wedged, LiveState_NotArmed,
    };

    Service g_sftap;
    bool g_sftap_open = false;

    /* sm defers GetService for a name nobody registered - the overlay would
     * hang without the sysmodule. Registering the name ourselves fails when
     * it is already registered, which is the answer. */
    bool IsServiceRunning(const char *name) {
        Handle h;
        const SmServiceName sn = smEncodeName(name);
        if (R_FAILED(smRegisterService(&h, sn, false, 1))) { return true; }
        svcCloseHandle(h);
        smUnregisterService(sn);
        return false;
    }

    /* libtesla keeps no sm session open after start-up (Run Y: every sm call
     * from the GUI failed, so the toggle never reached the sysmodule): open
     * one for the lookup. Once connected the session to sftap stays open. */
    bool SftapConnect() {
        if (g_sftap_open) { return true; }
        tsl::hlp::doWithSmSession([] {
            if (IsServiceRunning("sftap")) {
                g_sftap_open = R_SUCCEEDED(smGetService(&g_sftap, "sftap"));
            }
        });
        return g_sftap_open;
    }

    bool SftapStatus(StreamStatus *st) {
        if (!SftapConnect()) { return false; }
        if (R_FAILED(serviceDispatchOut(&g_sftap, 0, *st))) {
            serviceClose(&g_sftap);
            g_sftap_open = false;
            return false;
        }
        return true;
    }

    /* v0.1.1b: the never-attach list - for apps that refuse to run under a
     * debugger (TiCo's protected builds) */
    void SftapSetAppExcluded(u64 tid, bool excluded) {
        if (!SftapConnect()) { return; }
        const struct { u64 tid; u8 excluded; u8 pad[7]; } in = { tid, static_cast<u8>(excluded ? 1 : 0), {} };
        serviceDispatchIn(&g_sftap, 3, in);
    }

    void SftapSetEnabled(bool on) {
        if (!SftapConnect()) { return; }
        const u8 v = on ? 1 : 0;
        serviceDispatchIn(&g_sftap, 1, v);
    }

    /* ---- ReverseNX-RT, through SaltyNX ---------------------------------- */

    constexpr u32 NxrtMagic = 0x5452584E;   /* "NXRT" */

    struct RnxShared {
        u32 magic;
        bool isDocked;       /* forced mode, when def is false */
        bool def;            /* true: the game's own (system) mode */
        bool pluginActive;   /* the game asked for its mode through SaltyNX's hooks */
        u8 res;
        bool wasDDRused;
    } NX_PACKED;

    SharedMemory g_shmem;
    bool g_shmem_ok = false;
    u64 g_rnx_pid = 0;
    volatile RnxShared *g_rnx = nullptr;

    /* one old-style request on the SaltySD port (SaltyNX.h: SFCI magic,
     * u64 command id, zero payload, the pid sent along) */
    Result SaltyCall(Handle port, u32 cmd, u32 payload, Handle *out) {
        void *tls = armGetTls();
        CmifRequest req = cmifMakeRequest(tls, CmifRequestFormat{ .request_id = cmd, .data_size = payload, .send_pid = 1 });
        std::memset(req.data, 0, payload);
        Result rc = svcSendSyncRequest(port);
        if (R_FAILED(rc)) { return rc; }
        HipcResponse resp = hipcParseResponse(tls);
        const auto *hdr = reinterpret_cast<const CmifOutHeader *>((reinterpret_cast<uintptr_t>(resp.data_words) + 15) & ~uintptr_t(15));
        if (hdr->magic != CMIF_OUT_HEADER_MAGIC) { return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader); }
        if (hdr->result != 0) { return hdr->result; }
        if (out != nullptr) {
            if (resp.num_copy_handles > 0)      { *out = resp.copy_handles[0]; }
            else if (resp.num_move_handles > 0) { *out = resp.move_handles[0]; }
            else { return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
        }
        return 0;
    }

    bool MapSaltyShared() {
        if (g_shmem_ok) { return true; }
        Handle port = INVALID_HANDLE;
        if (R_FAILED(svcConnectToNamedPort(&port, "SaltySD"))) { return false; }
        Handle sh = INVALID_HANDLE;
        const Result rc = SaltyCall(port, 7, 16, &sh);
        SaltyCall(port, 0, 24, nullptr);    /* Term: SaltySD answers 0xf601 and closes */
        svcCloseHandle(port);
        if (R_FAILED(rc)) { return false; }
        shmemLoadRemote(&g_shmem, sh, 0x1000, Perm_Rw);
        g_shmem_ok = R_SUCCEEDED(shmemMap(&g_shmem));
        return g_shmem_ok;
    }

    /* the game's block, found again whenever the game changes */
    volatile RnxShared *FindRnx(u64 pid) {
        if (!g_shmem_ok) { return nullptr; }
        if (pid == g_rnx_pid && g_rnx != nullptr && g_rnx->magic == NxrtMagic) { return g_rnx; }
        g_rnx = nullptr;
        g_rnx_pid = pid;
        const uintptr_t base = reinterpret_cast<uintptr_t>(shmemGetAddr(&g_shmem));
        for (uintptr_t off = 0; off + sizeof(RnxShared) <= 0x1000; off += 4) {
            if (*reinterpret_cast<volatile u32 *>(base + off) == NxrtMagic) {
                g_rnx = reinterpret_cast<volatile RnxShared *>(base + off);
                break;
            }
        }
        return g_rnx;
    }

    enum class Mode { Unavailable, Default, Handheld, Docked };

    std::string g_rnx_note;

    volatile RnxShared *CurrentRnx() {
        u64 pid = 0;
        if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) { g_rnx_note = "No game running"; return nullptr; }
        if (!MapSaltyShared()) { g_rnx_note = "SaltyNX not found"; return nullptr; }
        volatile RnxShared *r = FindRnx(pid);
        if (r == nullptr) { g_rnx_note = "ReverseNX-RT not loaded in this game"; return nullptr; }
        if (!r->pluginActive) { g_rnx_note = "This game does not use ReverseNX-RT"; return nullptr; }
        g_rnx_note.clear();
        return r;
    }

    Mode CurrentMode() {
        volatile RnxShared *r = CurrentRnx();
        if (r == nullptr) { return Mode::Unavailable; }
        if (r->def) { return Mode::Default; }
        return r->isDocked ? Mode::Docked : Mode::Handheld;
    }

    void SetMode(Mode m) {
        volatile RnxShared *r = CurrentRnx();
        if (r == nullptr || m == Mode::Unavailable) { return; }
        if (m == Mode::Default) { r->def = true; return; }
        r->isDocked = (m == Mode::Docked);
        r->def = false;
    }

    /* ---- the GUI -------------------------------------------------------- */

    std::string StatusText(bool have, const StreamStatus &st) {
        char b[96];
        if (!have) { return "Sysmodule not running"; }
        switch (st.state) {
            case LiveState_Starting:   return "Starting (waits for a game)";
            case LiveState_Off:        return "Off";
            case LiveState_WaitViewer: return (st.reserved3 & 1) ? "Waiting for a camera app" : (st.reserved3 & 2) ? "Waiting for the viewer (USB/network)" : "Waiting for the PC viewer";   /* v0.4 webcam, v0.7 network */
            case LiveState_WaitGame:   return "Waiting for a game";
            case LiveState_Streaming:
                std::snprintf(b, sizeof(b), "Streaming %ux%u, %u.%u fps", st.width, st.height, st.fps_x10 / 10, st.fps_x10 % 10);
                return b;
            case LiveState_Wedged:     return "Encoder stalled - reboot to stream again";
            case LiveState_NotArmed:   return "Live mode not armed (arm file)";
            case 7:                    return "Firmware not tested - see the manager app";
            case 8:                    return "Not streaming this app (excluded)";
            default:                   return "Unknown";
        }
    }

    class MainGui : public tsl::Gui {
        public:
            tsl::elm::Element *createUI() override {
                auto *frame = new tsl::elm::OverlayFrame("Switch Frame Tap", "v0.7.1");
                auto *list = new tsl::elm::List();

                list->addItem(new tsl::elm::CategoryHeader("Stream to PC"));
                m_have = SftapStatus(&m_st);
                m_toggle = new tsl::elm::ToggleListItem("Stream", m_have && m_st.enabled != 0);
                m_toggle->setStateChangedListener([this](bool on) {
                    SftapSetEnabled(on);
                    m_tick = 0;
                });
                list->addItem(m_toggle);
                m_app = new tsl::elm::ListItem("Stream this app");
                m_app->setClickListener([this](u64 keys) {
                    if ((keys & HidNpadButton_A) && m_have && m_st.version >= 3 && m_st.app_tid != 0) {
                        SftapSetAppExcluded(m_st.app_tid, m_st.app_excluded == 0);
                        m_tick = 0;
                        return true;
                    }
                    return false;
                });
                list->addItem(m_app);
                list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                    r->drawString(m_status.c_str(), false, x + 15, y + 22, 18, r->a(tsl::style::color::ColorText));
                    r->drawString(m_game.c_str(), false, x + 15, y + 46, 16, r->a(tsl::style::color::ColorDescription));
                }), 60);

                list->addItem(new tsl::elm::CategoryHeader("Display mode (ReverseNX-RT)"));
                static const char *const names[] = { "", "Game default", "Handheld (720p)", "Docked (1080p)" };
                for (int i = 1; i <= 3; ++i) {
                    auto *item = new tsl::elm::ListItem(names[i]);
                    const Mode m = static_cast<Mode>(i);
                    item->setClickListener([this, m](u64 keys) {
                        if (keys & HidNpadButton_A) { SetMode(m); m_tick = 0; return true; }
                        return false;
                    });
                    m_modes[i] = item;
                    list->addItem(item);
                }
                list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                    const char *note = !g_rnx_note.empty() ? g_rnx_note.c_str() : "1080p60 wants CPU 1785 MHz";
                    r->drawString(note, false, x + 15, y + 22, 16, r->a(tsl::style::color::ColorDescription));
                }), 36);

                frame->setContent(list);
                this->Refresh();
                return frame;
            }

            void update() override {
                if (++m_tick % 30 == 1) { this->Refresh(); }
            }

        private:
            void Refresh() {
                m_have = SftapStatus(&m_st);
                m_status = StatusText(m_have, m_st);
                if (m_have && m_st.state == LiveState_Streaming && m_st.game_fps_x10 != 0) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "game %u.%u fps", m_st.game_fps_x10 / 10, m_st.game_fps_x10 % 10);
                    m_game = b;
                } else {
                    m_game.clear();
                }
                if (m_app != nullptr) {
                    if (!m_have || m_st.version < 3 || m_st.app_tid == 0) { m_app->setValue("No app", true); }
                    else { m_app->setValue(m_st.app_excluded ? "Off" : "On", m_st.app_excluded != 0); }
                }
                if (m_toggle != nullptr && m_have && m_toggle->getState() != (m_st.enabled != 0)) {
                    m_toggle->setState(m_st.enabled != 0);
                }
                const Mode cur = CurrentMode();
                for (int i = 1; i <= 3; ++i) {
                    if (m_modes[i] == nullptr) { continue; }
                    if (cur == Mode::Unavailable) { m_modes[i]->setValue("-", true); }
                    else { m_modes[i]->setValue(static_cast<int>(cur) == i ? "Active" : "", false); }
                }
            }

            StreamStatus m_st = {};
            bool m_have = false;
            u32 m_tick = 0;
            std::string m_status, m_game;
            tsl::elm::ToggleListItem *m_toggle = nullptr;
            tsl::elm::ListItem *m_app = nullptr;
            tsl::elm::ListItem *m_modes[4] = {};
    };

    class FrameTapOverlay : public tsl::Overlay {
        public:
            /* libtesla has already initialized fs, hid, pl, pmdmnt, hid:sys and set:sys */
            void initServices() override {}
            void exitServices() override {
                if (g_sftap_open) { serviceClose(&g_sftap); g_sftap_open = false; }
                if (g_shmem_ok) { shmemClose(&g_shmem); g_shmem_ok = false; }
            }
            std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
    };

}

int main(int argc, char **argv) {
    return tsl::loop<FrameTapOverlay>(argc, argv);
}
