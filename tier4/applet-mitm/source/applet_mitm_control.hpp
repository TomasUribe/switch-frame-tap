/*
 * M97: "sftap", the control service the Switch Frame Tap overlay talks to.
 *
 *   0  GetStatus() -> StreamStatus (48 bytes; version 1 was the first 32)
 *   1  SetEnabled(u8 on)   - persisted: sdmc:/config/switch-frame-tap/stream-off
 *   2  ReloadConfig()      - M98: re-read config.ini (release installs); a
 *                            running stream restarts once with the new settings
 *                            (v0.1.1b: and excluded.txt, on any install)
 *   3  SetAppExcluded(u64 program_id, u8 excluded) - v0.1.1b: the never-attach
 *                            list, sdmc:/config/switch-frame-tap/excluded.txt
 *   4  SetScreenOff(u8 on) - v0.7.12: "screen off while streaming", now (the
 *                            overlay keeps config.ini's screen_off in step)
 *   5  SetCaptureMode(u64 title_id, u32 mode) - v0.7.15: Auto/Standard/
 *                            Compositor/Layer, persisted in capture.ini
 *   6  GetCaptureInfo() -> CaptureInfo - v0.7.15: the active mode and the
 *                            conservative Auto probe's counts
 *
 * It runs on its own server thread, never on vi:u's: nothing a client does
 * here can delay the game's binder traffic. Both commands only touch atomics
 * (and, for SetEnabled, one small file).
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct StreamStatus {
        u32 version;         /* 1 */
        u32 state;           /* LiveState */
        u32 enabled;
        u32 width, height;   /* of the current (or last) stream */
        u32 fps_x10;         /* frames sent over the last second, x10 */
        u32 game_fps_x10;    /* the game's presents over the same second, x10 */
        u32 sessions;        /* live sessions started this boot */
        /* version 2 (M99) */
        u32 shot_enabled;
        u32 shots;           /* screenshots written this boot */
        u32 shot_fails;
        u32 reserved;        /* version 4 (v0.3): audio - bit 8 enabled, low byte 0 idle / 1 playing / 2 grc:d unavailable / 3 start failed */
        /* version 3 (v0.1.1b) */
        u64 app_tid;         /* the running application, 0 = none */
        u32 app_excluded;
        u32 reserved3;       /* v0.4: bit 0 - this boot is in webcam (UVC) mode; v0.7: bit 1 - network
                                transport on, bit 2 - a viewer is connected over the network;
                                v0.7.7: bit 3 - webcam-any; v0.7.14: bit 4 - this boot streams the HOME menu */
    };
    static_assert(sizeof(StreamStatus) == 64);

    /* v0.7.15: sftap 6. Kept separate from StreamStatus so older manager and
     * overlay builds keep reading the same 64 bytes. */
    struct CaptureInfo {
        u32 version;       /* 1 */
        u32 mode;          /* what the session is using */
        u32 requested;     /* global/override/Auto resolution */
        u32 candidates;    /* buffers found for the game's swapchain */
        u32 live;          /* candidates whose contents change */
        u32 dead;          /* candidates that stayed static/empty */
        u64 tid;           /* 0: no game override applies */
        u32 overrides;     /* lines in capture.ini */
        u32 composite;     /* 1 when the compositor route is available */
        u32 reserved;      /* bit 0: the active session belongs to tid */
    };
    static_assert(sizeof(CaptureInfo) == 48);

    /* read the persisted switch; called once at boot, before live mode can start */
    void LoadStreamEnabled();
    /* M98: implemented in applet_mitm_main.cpp, next to the config reader */
    void ReloadReleaseConfig();
    void StartControlService();
    /* v0.1.1b: the never-attach list */
    void LoadExcluded();
    bool IsExcluded(u64 program_id);

}

#define AMS_SFTAP_CONTROL_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStatus,  (sf::Out<ams::mitm::applet::StreamStatus> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetEnabled, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, ReloadConfig, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, SetAppExcluded, (u64 program_id, u8 excluded), (program_id, excluded)) \
    AMS_SF_METHOD_INFO(C, H, 4, Result, SetScreenOff, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 5, Result, SetCaptureMode, (u64 title_id, u32 mode), (title_id, mode)) \
    AMS_SF_METHOD_INFO(C, H, 6, Result, GetCaptureInfo, (sf::Out<ams::mitm::applet::CaptureInfo> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 10, Result, Donate, (u64 size, sf::CopyHandle &&tmem), (size, std::move(tmem))) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, DonateFailed, (u32 rc), (rc)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Donate2, (u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem), (size, nv_size, std::move(tmem), std::move(nv_tmem))) \
    AMS_SF_METHOD_INFO(C, H, 13, Result, GetDonorCap, (sf::Out<u32> out_mb), (out_mb))

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IStreamControl, AMS_SFTAP_CONTROL_INTERFACE_INFO, 0x5F7A4001)
