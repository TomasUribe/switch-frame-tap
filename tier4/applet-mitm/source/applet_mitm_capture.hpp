/*
 * v0.7.15: capture mode resolution and per-title overrides.
 *
 * capture.ini is separate from config.ini on purpose: the manager app rewrites
 * config.ini as a whole and would drop unknown lines. The sysmodule owns the
 * override file (sftap command 5 persists it), while the manager/overlay only
 * send a mode or edit the global default.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "capture_mode.hpp"

namespace ams::mitm::applet {

    /* the global default from config.ini / the arm file ("capture=...") */
    void SetCaptureDefault(u32 mode);
    u32  CaptureDefault();

    /* read sdmc:/config/switch-frame-tap/capture.ini into the override table */
    void LoadCaptureConfig();

    /* global -> title override -> the auto hint. Auto is returned as Auto:
     * the session starts in Standard and probes for a multi-surface game. */
    u32 ResolveCaptureMode(u64 tid);

    /* sftap 5: remember and persist this title's mode, then restart the
     * session so it applies without a reboot */
    void SetCaptureMode(u64 tid, u32 mode);
    size_t CaptureOverrides(sftcapture::Override *out, size_t cap);

    /* an Auto probe saw a multi-surface game: remember the fallback for the
     * next session and ask the stream to restart */
    void CaptureAutoDetected(u64 tid, u32 mode, const char *why);

    /* true when the compositor route is present in this build and was armed */
    bool CompositeCaptureAvailable();

    /* filled by the streaming session for GetCaptureInfo / the log */
    extern std::atomic<u32> g_capture_mode_active;     /* what the session is using */
    extern std::atomic<u32> g_capture_mode_requested;  /* what ResolveCaptureMode picked */
    extern std::atomic<u32> g_capture_ncand;
    extern std::atomic<u32> g_capture_live_cands;
    extern std::atomic<u32> g_capture_dead_cands;
    extern std::atomic<u64> g_capture_tid;
    extern std::atomic<bool> g_capture_reconfig;        /* session should restart for a mode change */
    extern std::atomic<bool> g_capture_auto_probe;      /* auto: observe the candidates this session */
    extern std::atomic<u32> g_capture_layer_cand;       /* last live candidate, 0xFF = none */
    extern std::atomic<u64> g_capture_layer_addr;       /* its buffer address (survives a re-index) */
    extern std::atomic<u64> g_capture_layer_tid;        /* the title that candidate belongs to */

}
