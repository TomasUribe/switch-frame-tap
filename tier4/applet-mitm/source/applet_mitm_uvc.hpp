/*
 * v0.4: webcam mode - the Switch as a standard USB Video Class camera.
 *
 * With `usb_mode = webcam` in config.ini the sysmodule describes itself as a
 * UVC 1.5 camera (1209:5F1F, a different id from the viewer mode's 5F1E, so a
 * WinUSB binding made for the viewer never captures the camera) and sends the
 * same H.264 stream as UVC frame-based payloads: OBS, browsers or any camera
 * app see it without our viewer and without a driver. Video only - UVC audio
 * needs an isochronous endpoint, which usb:ds cannot configure (see the
 * research notes below). The mode is fixed at boot: usb:ds cannot change its
 * descriptors once enabled.
 *
 * The USB rules here come from Insektaure's SysDVR-UVC-Capture (GPL-2.0,
 * https://github.com/Insektaure/SysDVR-UVC-Capture, docs/RESEARCH.md), which
 * paid for them in console crashes:
 *  - EP0 is touched only inside the SetupEvent -> GetSetupPacket window, and
 *    the status stage is manual: a GET ends with a zero-length OUT, a SET
 *    with a zero-length IN; StallCtrl on anything else. Anything else asserts
 *    inside Nintendo's usb sysmodule.
 *  - The camera input terminal is the full 18 bytes (Linux rejects the whole
 *    device otherwise); probe/commit answers are the 48-byte UVC 1.5 struct.
 *  - Payloads stay under 16 KB with a 2-byte header (EOH, FID, EOF - no
 *    PTS/SCR, which makes Windows queue frames); a payload that is a
 *    multiple of 512 bytes is followed by a zero-length packet; the frame ID
 *    toggles after every frame, sent or aborted.
 *  - A bulk stop (CLEAR_FEATURE HALT) never reaches us: a transfer that times
 *    out is cancelled AND reaped, the frame dropped, and streaming goes on.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern bool g_uvc_mode;                        /* "uvc" token: this boot is a webcam */
    extern std::atomic<bool> g_uvc_streaming;      /* a host committed a stream and is attached */
    extern std::atomic<bool> g_uvc_new_consumer;   /* a (new) commit since the stream thread looked */

    /* describes the camera, registers both interfaces and enables usb:ds
     * (instead of the viewer mode's descriptors); starts the EP0 and sender
     * threads */
    bool UvcSetupDevice();

    /* the committed frame size: 1920x1080 or 1280x720 */
    void UvcFrameSize(u32 *w, u32 *h);

    /* hand one H.264 access unit to the sender thread (buf page-aligned, it
     * must stay untouched until UvcWaitSent returns) */
    void UvcQueueFrame(const u8 *buf, size_t len);

    /* wait until the frame queued last is on the wire (or dropped) */
    void UvcWaitSent();

    /* the host stopped reading: 3 frames in a row timed out (a camera app
     * closed without the cable going - usb:ds never tells us) */
    bool UvcHostStalled();

    /* after a stall: does the host read again? Posts one empty payload (a
     * bare 2-byte header, which a host discards) and waits timeout_ms. A
     * fresh commit also counts. Only while no frame is queued. */
    bool UvcHostReading(u32 timeout_ms);

    /* frames the sender has dropped so far (a P frame after a drop would
     * decode against a picture the host never got: the stream IDRs) */
    u32 UvcDroppedCount();

    /* statistics for the log */
    void UvcLogStats(const char *who);

}
