#include "applet_mitm_control.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_shot.hpp"

namespace ams::mitm::applet {

    namespace {

        constexpr const char OffDir[]  = "sdmc:/config/switch-frame-tap";
        constexpr const char OffFile[] = "sdmc:/config/switch-frame-tap/stream-off";

        class StreamControl {
            public:
                Result GetStatus(sf::Out<StreamStatus> out) {
                    StreamStatus st = {};
                    st.version      = 2;
                    st.state        = g_live_state.load(std::memory_order_relaxed);
                    st.enabled      = g_stream_enabled.load(std::memory_order_relaxed) ? 1 : 0;
                    st.width        = g_live_w.load(std::memory_order_relaxed);
                    st.height       = g_live_h.load(std::memory_order_relaxed);
                    st.fps_x10      = g_live_fps_x10.load(std::memory_order_relaxed);
                    st.game_fps_x10 = g_live_game_fps_x10.load(std::memory_order_relaxed);
                    st.sessions     = g_live_sessions.load(std::memory_order_relaxed);
                    st.shot_enabled = g_shot_enabled.load(std::memory_order_relaxed) ? 1 : 0;
                    st.shots        = g_shot_count.load(std::memory_order_relaxed);
                    st.shot_fails   = g_shot_fail.load(std::memory_order_relaxed);
                    out.SetValue(st);
                    R_SUCCEED();
                }

                Result SetEnabled(u8 on) {
                    const bool en = on != 0;
                    if (g_stream_enabled.exchange(en, std::memory_order_relaxed) == en) { R_SUCCEED(); }
                    /* remembered across boots: the file's presence means off */
                    /* M98: the rcs are logged - Run Z left stream-off on the card
                     * after an ON, and this boot had said "on at boot" */
                    Result r1 = ResultSuccess(), r2 = ResultSuccess();
                    if (en) {
                        r1 = fs::DeleteFile(OffFile);
                    } else {
                        r1 = fs::CreateDirectory(OffDir);
                        r2 = fs::CreateFile(OffFile, 0);
                    }
                    LogLine("sftap: stream turned %s (%s rc=0x%x%s)", en ? "ON" : "OFF", en ? "delete stream-off" : "mkdir",
                            r1.GetValue(), en ? "" : (R_SUCCEEDED(r2) ? ", create ok" : ", create FAILED"));
                    if (!en && R_FAILED(r2)) { LogLine("sftap: create stream-off rc=0x%x", r2.GetValue()); }
                    R_SUCCEED();
                }

                Result ReloadConfig() {
                    ReloadReleaseConfig();
                    R_SUCCEED();
                }
        };
        static_assert(IsIStreamControl<StreamControl>);

        using ControlServerOptions = sf::hipc::DefaultServerManagerOptions;
        constexpr sm::ServiceName ControlServiceName = sm::ServiceName::Encode("sftap");
        constexpr size_t ControlMaxSessions = 2;

        sf::hipc::ServerManager<1, ControlServerOptions, ControlMaxSessions> g_control_manager;
        constinit sf::UnmanagedServiceObject<IStreamControl, StreamControl> g_control_object;

        alignas(os::ThreadStackAlignment) constinit u8 g_control_stack[16_KB];
        constinit os::ThreadType g_control_thread;

        void ControlThread(void *) {
            g_control_manager.LoopProcess();
        }

    }

    void LoadStreamEnabled() {
        fs::DirectoryEntryType t;
        const Result r = fs::GetEntryType(std::addressof(t), OffFile);
        const bool off = R_SUCCEEDED(r);
        g_stream_enabled.store(!off, std::memory_order_relaxed);
        LogLine("sftap: stream %s at boot (stream-off: rc=0x%x)%s", off ? "OFF" : "on", r.GetValue(),
                off ? " - turn it on from the overlay or the manager" : "");
    }

    void StartControlService() {
        const Result r = g_control_manager.RegisterObjectForServer(g_control_object.GetShared(), ControlServiceName, ControlMaxSessions);
        LogLine("sftap: control service rc=0x%x", r.GetValue());
        if (R_FAILED(r)) { return; }
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_control_thread), ControlThread, nullptr,
                                        g_control_stack, sizeof(g_control_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_control_thread), "applet-mitm.Control");
        os::StartThread(std::addressof(g_control_thread));
    }

}
