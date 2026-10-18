/*
 * M99: native-resolution screenshots on a button combo.
 *
 * An input thread watches the controllers the way Tesla's overlay loader
 * does (hid with aruid 0, next to the game). It touches hid only once
 * screenshots are turned on, so a user who never enables them runs exactly
 * what M98 ran. A press sets a request; the capture thread takes it - from
 * the frame it just read while streaming, or by attaching to the game for one
 * read when not (applet_mitm_nv.cpp) - and WriteShot turns the block-linear
 * slot into a PNG (applet_mitm_png.hpp) in
 * sdmc:/switch/switch-frame-tap/screenshots/.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern std::atomic<bool> g_shot_enabled;
    extern std::atomic<u32>  g_shot_combo;     /* 0 L3+R3, 1 L+R+Down, 2 ZL+ZR+Down, 3 Minus+Down, 4 custom */
    extern std::atomic<u32>  g_shot_mask;      /* v0.7.11: custom - the buttons (HidNpadButton bits) */
    extern std::atomic<bool> g_shot_capture;   /* v0.7.11: custom - and the Capture button */
    /* v0.7.11: save next to the system's screenshots (Album/YYYY/MM/DD) */
    extern std::atomic<bool> g_shot_album;
    /* writes the album folder in use to config/switch-frame-tap/album_path.txt */
    void NoteAlbumPath();
    extern std::atomic<u32>  g_shot_count, g_shot_fail;

    void StartShotInput();
    /* a toast through Ultrahand, if it is installed */
    void NotifyShot(const char *text);

    /* HOME button edge while streaming (detach_on_home confirm). within_ms
     * from the last press; false if never / too old. Read-only hid shm (+ optional
     * hidsysActivateHomeButton once). */
    bool HomeButtonRecent(u32 within_ms);

    /* a fresh (< 3 s old) request, consumed; stale ones are dropped */
    bool TakeShotRequest();
    bool ShotRequestPending();   /* without taking it */

    /* the visible w x h of a block-linear A8B8G8R8 surface -> PNG */
    /* flip: bit 0 mirror horizontally, bit 1 vertically (v0.4.1: the present's transform) */
    /* v0.7.13: out (ShotOutBufSize) is the PNG's write buffer - the end of the
     * capture region, not 128 KB of the System group held for the boot */
    constexpr size_t ShotOutBufSize = 128 * 1024;
    bool WriteShot(u8 *out, const u8 *src, size_t src_size, u32 w, u32 h, u32 stride_bytes, u32 bh_log2, bool bgra = false, u32 flip = 0);

}
