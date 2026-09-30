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
    extern std::atomic<u32>  g_shot_combo;     /* 0 L3+R3, 1 L+R+Down, 2 ZL+ZR+Down, 3 Minus+Down */
    extern std::atomic<u32>  g_shot_count, g_shot_fail;

    void StartShotInput();
    /* a toast through Ultrahand, if it is installed */
    void NotifyShot(const char *text);

    /* a fresh (< 3 s old) request, consumed; stale ones are dropped */
    bool TakeShotRequest();

    /* the visible w x h of a block-linear A8B8G8R8 surface -> PNG */
    /* flip: bit 0 mirror horizontally, bit 1 vertically (v0.4.1: the present's transform) */
    bool WriteShot(const u8 *src, size_t src_size, u32 w, u32 h, u32 stride_bytes, u32 bh_log2, bool bgra = false, u32 flip = 0);

}
