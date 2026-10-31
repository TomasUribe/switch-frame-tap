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
 *  14  (reserved / unused)
 *  15  AckHeadroom(u32 free_kb) - SwitchU finished a hard reclaim so the donor
 *                            may remount; clears NeedHeadroom (reserved3 bit 6)
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
                                v0.7.7: bit 3 - webcam-any; v0.7.14: bit 4 - this boot streams the HOME menu,
                                bit 5 - and the system overlays;
                                SwitchU coexistence: bit 6 - NeedHeadroom (donor about to remount;
                                SwitchU should reclaim GPU and AckHeadroom).
                                Requested MB is GetDonorCap (cmd 13), not shot_fails. */
    };
    static_assert(sizeof(StreamStatus) == 64);

    static constexpr u32 kReserved3NeedHeadroom = 64u;

    /* read the persisted switch; called once at boot, before live mode can start */
    void LoadStreamEnabled();
    /* M98: implemented in applet_mitm_main.cpp, next to the config reader */
    void ReloadReleaseConfig();
    void StartControlService();
    /* v0.1.1b: the never-attach list */
    void LoadExcluded();
    /* v0.7.19: put an app on the never-attach list (or take it off), as the
     * overlay's "Stream this app" does - the stream's auto-skip uses it */
    void SetExcluded(u64 program_id, bool excluded);
    bool IsExcluded(u64 program_id);

    /* Ask SwitchU (GetStatus NeedHeadroom) to free applet headroom; wait up to
     * timeout_ms for AckHeadroom or the ready file. Does not launch the donor -
     * caller decides. Clear with HeadroomClear if not remounting. */
    void HeadroomAskAndWait(u32 need_mb, u32 timeout_ms);
    void HeadroomClear();
    bool HeadroomNeeded();
    u32 HeadroomNeedMb();
    void HeadroomAck(u32 free_kb);
    /* True after SwitchU AckHeadroom at least once this boot - gate for the
     * 2 s remount wait when 100D is not currently open (stock users never wait). */
    bool HeadroomEverAcked();

}

#define AMS_SFTAP_CONTROL_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStatus,  (sf::Out<ams::mitm::applet::StreamStatus> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetEnabled, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, ReloadConfig, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, SetAppExcluded, (u64 program_id, u8 excluded), (program_id, excluded)) \
    AMS_SF_METHOD_INFO(C, H, 4, Result, SetScreenOff, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 10, Result, Donate, (u64 size, sf::CopyHandle &&tmem), (size, std::move(tmem))) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, DonateFailed, (u32 rc), (rc)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Donate2, (u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem), (size, nv_size, std::move(tmem), std::move(nv_tmem))) \
    AMS_SF_METHOD_INFO(C, H, 13, Result, GetDonorCap, (sf::Out<u32> out_mb), (out_mb)) \
    AMS_SF_METHOD_INFO(C, H, 15, Result, AckHeadroom, (u32 free_kb), (free_kb))

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IStreamControl, AMS_SFTAP_CONTROL_INTERFACE_INFO, 0x5F7A4001)
