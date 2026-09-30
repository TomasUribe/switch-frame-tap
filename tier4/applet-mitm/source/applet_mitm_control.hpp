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
        u32 reserved3;       /* v0.4: bit 0 - this boot is in webcam (UVC) mode */
    };
    static_assert(sizeof(StreamStatus) == 64);

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
    AMS_SF_METHOD_INFO(C, H, 3, Result, SetAppExcluded, (u64 program_id, u8 excluded), (program_id, excluded))

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IStreamControl, AMS_SFTAP_CONTROL_INTERFACE_INFO, 0x5F7A4001)
