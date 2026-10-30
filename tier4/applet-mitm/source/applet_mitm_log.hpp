/*
 * applet-mitm - Track B
 * SD-card line logger (libstratosphere fs) + a crash-durable breadcrumb.
 *
 *   sdmc:/applet-mitm.log   append, one open/write/close per line
 *   sdmc:/applet-mitm.last  single line: the step currently in flight
 *
 * A hard fatal can lose the tail of the .log before FAT commits; .last is
 * rewritten+closed on every mark, so it survives.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    /* Live counters. The heartbeat snapshots these into .last, so even a run
     * that ends in a forced power-off (which can lose the .log tail before FAT
     * commits) still tells us how far the module got: did it ever get a
     * session, did the wrapper chain run, were frames flowing. */
    struct Stats {
        std::atomic<u32> sessions;   /* OnNeedsToAccept calls   */
        std::atomic<u32> getdisp;    /* GetDisplayService calls */
        std::atomic<u32> relay;      /* GetRelayService calls   */
        std::atomic<u32> txns;       /* binder transactions     */
    };
    extern Stats g_stats;

    void LogInit();
    /* M98: true when there is no test arm file (sdmc:/applet-mitm.armed):
     * settings come from sdmc:/config/switch-frame-tap/config.ini and the log
     * is sdmc:/config/switch-frame-tap/log.txt (capped) */
    extern bool g_release_mode;
    extern u32 g_arm_probe_rc;    /* GetEntryType(applet-mitm.armed) at boot */
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    /* v0.7.10: true holds lines (and breadcrumbs) in memory, false writes them
     * in one go - for a teardown that must not wait on the SD card */
    void LogDefer(bool on);
    /* v0.7.15 (#33): true while a release install's stream runs - nothing is
     * written to the SD card then; LogFlushHeld writes what was held once it
     * has stopped (the heartbeat calls it) */
    bool LogQuiet();
    void LogFlushHeld();
    void LogFlushStale();   /* v0.7.20: held while a game runs - written once they are a minute old */
    /* whatever is held, written now - before a fatal */
    void LogWriteHeldNow();

    /* v0.7.7: an abort that says why first - the line reaches the SD card
     * (every LogLine is flushed) before the console shows the fatal screen.
     * Only once the log is open: sm::Initialize, before it, cannot use it. */
    #define SFT_ABORT_UNLESS(expr) do {                                                                   \
        const ::ams::Result sft_abort_rc_ = (expr);                                                       \
        if (R_FAILED(sft_abort_rc_)) {                                                                    \
            ::ams::mitm::applet::LogLine("FATAL: %s failed, rc=0x%x (%s:%d) - stopping the console here", \
                                         #expr, sft_abort_rc_.GetValue(), __FILE__, __LINE__);            \
            ::ams::mitm::applet::LogWriteHeldNow();   /* even mid-stream (v0.7.15) */                   \
            R_ABORT_UNLESS(sft_abort_rc_);                                                                \
        }                                                                                                 \
    } while (0)

    /* breadcrumb: overwrite sdmc:/applet-mitm.last, then LogLine("-> what") */
    void LogMark(const char *what);

    /* M57 - USB bulk transport. Implemented in applet_mitm_main.cpp, where the
     * usb:ds session and the endpoint handles live. Declared here because that
     * file keeps its USB state in an anonymous namespace, which has internal
     * linkage and cannot be reached by an extern declaration from another
     * translation unit - the lesson LogMemoryPools taught in M54.
     *
     * UsbReady() asks the kernel for the live UsbState rather than caching a
     * boot-time flag, so a cable seated after boot still counts.
     *
     * The buffer passed to UsbSendBuffer MUST be 0x1000-aligned and in normal
     * CACHED memory. Uncached nvmap memory is rejected (the 0xd401
     * InvalidCurrentMemory class of failure); g_ind_buf satisfies both. */
    bool UsbReady();
    bool UsbIsSuperSpeed();
    bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent);

    /* M59 - asynchronous single-URB post, so a frame's transfer overlaps the
     * next frame's capture. Serially, 480x270 costs 5.6 ms read + 12.4 ms send
     * = 43 fps; overlapped the bottleneck is the send alone and 60 fps fits.
     * Same buffer rules as UsbSendBuffer: 0x1000-aligned, cached memory.
     * The buffer must stay untouched until UsbWaitAsync returns. */
    bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb);
    bool UsbWaitAsync(u32 urb, size_t *out_sent);
    /* v0.7.12: the viewer says goodbye ("SFTQ" on the OUT endpoint) when it
     * closes, so a stream ends at once instead of after the 5 s timeout.
     * Called from the stream loop; UsbByeStop at the end of a session. */
    bool UsbViewerSaidBye();
    void SetScreenOffNow(bool on);   /* v0.7.12: sftap 4 */
    void UsbByeStop();

    /* M86: true if a host program is reading the stream endpoint right now.
     * Sends one empty SFTR header (length 0, which raw-view skips) and waits
     * `timeout_ms` for it to be taken. A cable with no viewer open stays
     * Configured, so UsbReady alone cannot tell. */
    bool UsbViewerPresent(u32 timeout_ms);
    /* v0.7.17: something every second while the stream waits between sessions */
    void ViewerKeepAlive();
    /* v0.7.10: why the viewer gets no picture, sent to it after each idle
     * probe that finds it (a status packet old viewers ignore) - 0 while
     * streaming */
    enum ViewerReason : u32 {
        ViewerReason_None        = 0,
        ViewerReason_NoGame      = 1,
        ViewerReason_GameHidden  = 2,
        ViewerReason_AppletOpen  = 3,
        ViewerReason_Off         = 4,
        ViewerReason_Excluded    = 5,
        ViewerReason_Unsupported = 6,
        ViewerReason_Wedged      = 7,
        ViewerReason_NoMemory    = 8,
        ViewerReason_NotFound    = 9,
        ViewerReason_Starting    = 10,
        /* v0.7.15: the keyboard beside System Settings (no room for a donor) */
        ViewerReason_Keyboard    = 11,
        /* v0.7.19: a picture above 1080p (Project Diva at 2560x1440, #48) */
        ViewerReason_TooBig      = 12,
        /* v0.7.20: a USB link at Full Speed, 12 Mbps (a Fire TV's OTG hub, #46) */
        ViewerReason_UsbSlow     = 13,
    };
    void SetViewerReason(ViewerReason r);
    /* v0.7.11: USB follows the stream switch (VIC worker thread only) */
    void UsbFollowStreamSwitch();
    void NoteUsbLeftAlone();


    /* v0.7.13: ServerManager::LoopProcess aborts the whole module when one
     * request cannot be handled - accepting one session more than a manager
     * has room for is enough - and the module going down takes every vi
     * client with it: a black screen until the power button. This loop logs
     * it and carries on: that one client is turned away, the console keeps
     * going (BigBro030, GBAtemp: apps launched one after another). */
    template<typename Manager>
    void LoopProcessSafely(Manager &m, const char *who) {
        u32 failures = 0;
        while (auto *h = m.WaitSignaled()) {
            if (const Result r = m.Process(h); R_FAILED(r)) {
                if (++failures <= 20) { LogLine("%s: a request could not be handled (rc=0x%x) - turned away, still running", who, r.GetValue()); }
            }
        }
    }


    /* v0.7.13: each thread's stack, to log how much of it was ever used
     * (LogStackUse) - the stacks are zeroed .bss, so the deepest byte not
     * zero is the high-water mark. For sizing them down (#15). */
    void NoteThread(const os::ThreadType *t);
    void LogStackUse();

    void UiProbeStep();   /* applet_mitm_uiprobe.cpp */

}
