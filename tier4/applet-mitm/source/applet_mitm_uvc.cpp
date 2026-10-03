/*
 * v0.4 webcam mode - see applet_mitm_uvc.hpp. The descriptor layout, the EP0
 * state machine and the payload rules follow Insektaure's SysDVR-UVC-Capture
 * (GPL-2.0, source/uvc/uvc_device.c), adapted to this sysmodule: two frame
 * sizes at 60 fps instead of one at 30, and a sender thread fed by the NVENC
 * stream instead of grc:d.
 */
#include "applet_mitm_uvc.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    constinit bool g_uvc_mode = false;
    constinit bool g_uvc_raw = false;
    constinit std::atomic<bool> g_uvc_streaming{false};
    constinit std::atomic<bool> g_uvc_new_consumer{false};

    namespace {

        /* ---- UVC constants ----------------------------------------------- */
        constexpr u8 CS_INTERFACE = 0x24;
        constexpr u8 VC_HEADER = 0x01, VC_INPUT_TERMINAL = 0x02, VC_OUTPUT_TERMINAL = 0x03;
        constexpr u8 VS_INPUT_HEADER = 0x01, VS_FORMAT_FRAME_BASED = 0x10, VS_FRAME_FRAME_BASED = 0x11;
        constexpr u8 VS_FORMAT_UNCOMPRESSED = 0x04, VS_FRAME_UNCOMPRESSED = 0x05;
        constexpr u8 CLASS_VIDEO = 0x0E, SC_VIDEOCONTROL = 0x01, SC_VIDEOSTREAMING = 0x02, SC_COLLECTION = 0x03, PC_PROTOCOL_15 = 0x01;
        constexpr u8 ENTITY_IT = 1, ENTITY_OT = 2;
        constexpr u8 SET_CUR = 0x01, GET_CUR = 0x81, GET_MIN = 0x82, GET_MAX = 0x83, GET_LEN = 0x85, GET_INFO = 0x86, GET_DEF = 0x87;
        constexpr u8 VS_PROBE = 0x01, VS_COMMIT = 0x02;
        constexpr u8 VC_POWER_MODE = 0x01, VC_ERROR_CODE = 0x02;
        constexpr u8 ERR_NONE = 0, ERR_INVALID_UNIT = 5, ERR_INVALID_CONTROL = 6, ERR_INVALID_REQUEST = 7;
        constexpr u8 HDR_FID = 0x01, HDR_EOF = 0x02, HDR_EOH = 0x80;
        constexpr u32 ClockFrequency = 90000;
        constexpr u32 Interval60 = 166666;              /* 100 ns units */
        constexpr u32 Interval25 = 400000;
        constexpr u32 MaxFrameSize = 0x200000;          /* what a stage buffer holds */
        constexpr u32 MaxPayload = 0x4000;              /* per bulk transfer, header included */

        struct __attribute__((packed)) Iad { u8 bLength, bDescriptorType, bFirstInterface, bInterfaceCount, bFunctionClass, bFunctionSubClass, bFunctionProtocol, iFunction; };
        struct __attribute__((packed)) VcHeader { u8 bLength, bDescriptorType, bDescriptorSubType; u16 bcdUVC, wTotalLength; u32 dwClockFrequency; u8 bInCollection, baInterfaceNr1; };
        struct __attribute__((packed)) InputTerminal { u8 bLength, bDescriptorType, bDescriptorSubType, bTerminalID; u16 wTerminalType; u8 bAssocTerminal, iTerminal; u16 wObjectiveFocalLengthMin, wObjectiveFocalLengthMax, wOcularFocalLength; u8 bControlSize; u8 bmControls[3]; };
        struct __attribute__((packed)) OutputTerminal { u8 bLength, bDescriptorType, bDescriptorSubType, bTerminalID; u16 wTerminalType; u8 bAssocTerminal, bSourceID, iTerminal; };
        struct __attribute__((packed)) VsInputHeader { u8 bLength, bDescriptorType, bDescriptorSubType, bNumFormats; u16 wTotalLength; u8 bEndpointAddress, bmInfo, bTerminalLink, bStillCaptureMethod, bTriggerSupport, bTriggerUsage, bControlSize, bmaControls1; };
        struct __attribute__((packed)) FormatFrameBased { u8 bLength, bDescriptorType, bDescriptorSubType, bFormatIndex, bNumFrameDescriptors; u8 guidFormat[16]; u8 bBitsPerPixel, bDefaultFrameIndex, bAspectRatioX, bAspectRatioY, bmInterlaceFlags, bCopyProtect, bVariableSize; };
        struct __attribute__((packed)) FrameFrameBased { u8 bLength, bDescriptorType, bDescriptorSubType, bFrameIndex, bmCapabilities; u16 wWidth, wHeight; u32 dwMinBitRate, dwMaxBitRate, dwDefaultFrameInterval; u8 bFrameIntervalType; u32 dwBytesPerLine, dwFrameInterval1; };
        struct __attribute__((packed)) ProbeCommit {
            u16 bmHint; u8 bFormatIndex, bFrameIndex; u32 dwFrameInterval; u16 wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
            u32 dwMaxVideoFrameSize, dwMaxPayloadTransferSize, dwClockFrequency; u8 bmFramingInfo, bPreferedVersion, bMinVersion, bMaxVersion;
            u8 bUsage, bBitDepthLuma, bmSettings, bMaxNumberOfRefFramesPlus1; u16 bmRateControlModes; u64 bmLayoutPerStream;
        };
        /* v0.7.7: uncompressed NV12 (webcam-any) */
        struct __attribute__((packed)) FormatUncompressed { u8 bLength, bDescriptorType, bDescriptorSubType, bFormatIndex, bNumFrameDescriptors; u8 guidFormat[16]; u8 bBitsPerPixel, bDefaultFrameIndex, bAspectRatioX, bAspectRatioY, bmInterlaceFlags, bCopyProtect; };
        struct __attribute__((packed)) FrameUncompressed { u8 bLength, bDescriptorType, bDescriptorSubType, bFrameIndex, bmCapabilities; u16 wWidth, wHeight; u32 dwMinBitRate, dwMaxBitRate, dwMaxVideoFrameBufferSize, dwDefaultFrameInterval; u8 bFrameIntervalType; u32 dwFrameInterval1; };
        static_assert(sizeof(InputTerminal) == 18 && sizeof(ProbeCommit) == 48 && sizeof(FormatFrameBased) == 28 && sizeof(FrameFrameBased) == 30);
        static_assert(sizeof(FormatUncompressed) == 27 && sizeof(FrameUncompressed) == 30);

        struct __attribute__((packed)) VcBlock { VcHeader header; InputTerminal it; OutputTerminal ot; };
        /* no colour-matching descriptor: Run AH's usb:ds refused the VS
         * interface's SuperSpeed configuration at 130 bytes (0xcc8c,
         * InvalidParameter) where the reference's 99 pass - it looks like a
         * 128-byte cap per interface. The colour information is in the H.264
         * VUI, and a host without the descriptor assumes BT.709 anyway. */
        struct __attribute__((packed)) VsBlock { VsInputHeader header; FormatFrameBased format; FrameFrameBased f1080; FrameFrameBased f720; };
        struct __attribute__((packed)) VsRawBlock { VsInputHeader header; FormatUncompressed format; FrameUncompressed f720; FrameUncompressed f432; };

        constinit Iad g_iad = { 8, 0x0B, 0, 2, CLASS_VIDEO, SC_COLLECTION, 0, 0 };
        constinit usb_interface_descriptor g_vc_if = { USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 0, 0, 0, CLASS_VIDEO, SC_VIDEOCONTROL, PC_PROTOCOL_15, 0 };
        constinit VcBlock g_vc = {
            { sizeof(VcHeader), CS_INTERFACE, VC_HEADER, 0x0150, sizeof(VcBlock), ClockFrequency, 1, 1 },
            { sizeof(InputTerminal), CS_INTERFACE, VC_INPUT_TERMINAL, ENTITY_IT, 0x0201 /* ITT_CAMERA */, 0, 0, 0, 0, 0, 3, { 0, 0, 0 } },
            { sizeof(OutputTerminal), CS_INTERFACE, VC_OUTPUT_TERMINAL, ENTITY_OT, 0x0101 /* TT_STREAMING */, 0, ENTITY_IT, 0 },
        };
        /* bulk streams keep their endpoint in alternate setting 0 */
        constinit usb_interface_descriptor g_vs_if = { USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 1, 0, 1, CLASS_VIDEO, SC_VIDEOSTREAMING, 0, 0 };
        constinit VsBlock g_vs = {
            { sizeof(VsInputHeader), CS_INTERFACE, VS_INPUT_HEADER, 1, sizeof(VsBlock), 0x81, 0, ENTITY_OT, 0, 0, 0, 1, 0 },
            { sizeof(FormatFrameBased), CS_INTERFACE, VS_FORMAT_FRAME_BASED, 1, 2,
              { 'H', '2', '6', '4', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 },
              0, 1, 16, 9, 0, 0, 1 },
            { sizeof(FrameFrameBased), CS_INTERFACE, VS_FRAME_FRAME_BASED, 1, 0, 1920, 1080, 20000000, 150000000, Interval60, 1, 0, Interval60 },
            { sizeof(FrameFrameBased), CS_INTERFACE, VS_FRAME_FRAME_BASED, 2, 0, 1280, 720, 10000000, 100000000, Interval60, 1, 0, Interval60 },
        };
        static_assert(USB_DT_INTERFACE_SIZE + sizeof(VsBlock) + USB_DT_ENDPOINT_SIZE + 6 <= 128);
        /* v0.7.7: webcam-any - uncompressed NV12 straight from the VIC, for
         * camera apps that take no H.264 (the Windows Camera app, browsers,
         * Discord, Zoom). USB 2.0 carries ~37 MB/s (M70): 1280x720 NV12 is
         * 1.38 MB a frame - 25 fps; 768x432 (the M70 60 fps size, a 256-byte
         * pitch for the VIC) 0.5 MB - 60 fps. No colour-matching descriptor
         * (the 128-byte cap): hosts assume BT.709 primaries, and the VIC
         * writes BT.709 limited range as for NVENC. */
        constexpr u32 Raw720W = 1280, Raw720H = 720, Raw432W = 768, Raw432H = 432;
        constexpr u32 Raw720Bytes = Raw720W * Raw720H * 3 / 2, Raw432Bytes = Raw432W * Raw432H * 3 / 2;
        constinit VsRawBlock g_vs_raw = {
            { sizeof(VsInputHeader), CS_INTERFACE, VS_INPUT_HEADER, 1, sizeof(VsRawBlock), 0x81, 0, ENTITY_OT, 0, 0, 0, 1, 0 },
            { sizeof(FormatUncompressed), CS_INTERFACE, VS_FORMAT_UNCOMPRESSED, 1, 2,
              { 'N', 'V', '1', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 },
              12, 1, 16, 9, 0, 0 },
            { sizeof(FrameUncompressed), CS_INTERFACE, VS_FRAME_UNCOMPRESSED, 1, 0, Raw720W, Raw720H,
              Raw720Bytes * 8 * 25, Raw720Bytes * 8 * 25, Raw720Bytes, Interval25, 1, Interval25 },
            { sizeof(FrameUncompressed), CS_INTERFACE, VS_FRAME_UNCOMPRESSED, 2, 0, Raw432W, Raw432H,
              Raw432Bytes * 8 * 60, Raw432Bytes * 8 * 60, Raw432Bytes, Interval60, 1, Interval60 },
        };
        static_assert(USB_DT_INTERFACE_SIZE + sizeof(VsRawBlock) + USB_DT_ENDPOINT_SIZE + 6 <= 128);
        constinit usb_endpoint_descriptor g_ep = { USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x81, USB_TRANSFER_TYPE_BULK, 512, 0 };
        constinit usb_ss_endpoint_companion_descriptor g_ss = { sizeof(usb_ss_endpoint_companion_descriptor), USB_DT_SS_ENDPOINT_COMPANION, 15, 0, 0 };

        UsbDsInterface *g_vc_intf = nullptr, *g_vs_intf = nullptr;
        UsbDsEndpoint *g_video_ep = nullptr;

        constinit ProbeCommit g_probe = {}, g_commit = {};
        constinit std::atomic<u32> g_frame_index{1};
        constinit u8 g_last_error = ERR_NONE;

        alignas(0x1000) constinit u8 g_vc_ctrl[0x1000] = {};
        alignas(0x1000) constinit u8 g_vs_ctrl[0x1000] = {};
        alignas(0x1000) constinit u8 g_bounce[MaxPayload] = {};

        /* statistics */
        constinit std::atomic<u32> s_frames{0}, s_dropped{0}, s_commits{0}, s_drop_run{0};

        u32 FrameInterval(u8 frame) { return g_uvc_raw && frame == 1 ? Interval25 : Interval60; }
        u32 FrameBytes(u8 frame) { return g_uvc_raw ? (frame == 2 ? Raw432Bytes : Raw720Bytes) : MaxFrameSize; }

        void DefaultProbe(ProbeCommit *c, u8 frame) {
            std::memset(c, 0, sizeof(*c));
            c->bmHint = 1;
            c->bFormatIndex = 1;
            c->bFrameIndex = frame;
            c->dwFrameInterval = FrameInterval(frame);
            c->dwMaxVideoFrameSize = FrameBytes(frame);
            c->dwMaxPayloadTransferSize = MaxPayload;
            c->dwClockFrequency = ClockFrequency;
            c->bmFramingInfo = 0x03;
            c->bPreferedVersion = c->bMinVersion = c->bMaxVersion = 1;
        }

        /* ---- EP0 (control) -------------------------------------------------- */
        struct __attribute__((packed)) Setup { u8 bmRequestType, bRequest; u16 wValue, wIndex, wLength; };

        u8 *CtrlBuf(UsbDsInterface *i) { return i == g_vc_intf ? g_vc_ctrl : g_vs_ctrl; }

        ::Result CtrlIn(UsbDsInterface *i, u8 *buf, size_t len) {
            eventClear(&i->CtrlInCompletionEvent);
            u32 urb = 0;
            ::Result rc = usbDsInterface_CtrlInPostBufferAsync(i, buf, len, &urb);
            if (R_FAILED(rc)) { return rc; }
            if (R_FAILED(rc = eventWait(&i->CtrlInCompletionEvent, 1000000000ull))) { return rc; }
            eventClear(&i->CtrlInCompletionEvent);
            UsbDsReportData r;
            if (R_FAILED(rc = usbDsInterface_GetCtrlInReportData(i, &r))) { return rc; }
            u32 t = 0;
            return usbDsParseReportData(&r, urb, nullptr, &t);
        }

        ::Result CtrlOut(UsbDsInterface *i, u8 *buf, size_t len, u32 *got) {
            eventClear(&i->CtrlOutCompletionEvent);
            u32 urb = 0;
            ::Result rc = usbDsInterface_CtrlOutPostBufferAsync(i, buf, len, &urb);
            if (R_FAILED(rc)) { return rc; }
            if (R_FAILED(rc = eventWait(&i->CtrlOutCompletionEvent, 1000000000ull))) { return rc; }
            eventClear(&i->CtrlOutCompletionEvent);
            UsbDsReportData r;
            if (R_FAILED(rc = usbDsInterface_GetCtrlOutReportData(i, &r))) { return rc; }
            u32 t = 0;
            rc = usbDsParseReportData(&r, urb, nullptr, &t);
            if (got) { *got = t; }
            return rc;
        }

        /* GET: data IN, then a zero-length OUT status stage */
        void Reply(UsbDsInterface *i, const void *data, size_t len) {
            u8 *buf = CtrlBuf(i);
            std::memcpy(buf, data, len);
            ::Result rc = CtrlIn(i, buf, len);
            if (R_SUCCEEDED(rc)) { rc = CtrlOut(i, buf, 0, nullptr); }
            if (R_FAILED(rc)) { usbDsInterface_StallCtrl(i); }
        }

        /* SET: data OUT, then a zero-length IN status stage */
        bool Receive(UsbDsInterface *i, void *data, size_t len) {
            u8 *buf = CtrlBuf(i);
            u32 got = 0;
            ::Result rc = CtrlOut(i, buf, len, &got);
            if (R_SUCCEEDED(rc)) { rc = CtrlIn(i, buf, 0); }
            if (R_FAILED(rc)) { usbDsInterface_StallCtrl(i); return false; }
            std::memcpy(data, buf, got < len ? got : len);
            return true;
        }

        void Stall(UsbDsInterface *i, u8 err) { g_last_error = err; usbDsInterface_StallCtrl(i); }

        void HandleVs(UsbDsInterface *i, const Setup &s) {
            const u8 cs = s.wValue >> 8;
            if (cs != VS_PROBE && cs != VS_COMMIT) { Stall(i, ERR_INVALID_CONTROL); return; }
            ProbeCommit *ctrl = cs == VS_PROBE ? &g_probe : &g_commit;
            const size_t len = s.wLength < sizeof(ProbeCommit) ? s.wLength : sizeof(ProbeCommit);
            switch (s.bRequest) {
                case GET_CUR: case GET_MIN: case GET_MAX: Reply(i, ctrl, len); break;
                case GET_DEF: { ProbeCommit d; DefaultProbe(&d, 1); Reply(i, &d, len); break; }
                case GET_INFO: { const u8 info = 0x03; Reply(i, &info, 1); break; }
                case GET_LEN: { const u16 l = sizeof(ProbeCommit); Reply(i, &l, s.wLength < 2 ? s.wLength : 2); break; }
                case SET_CUR: {
                    ProbeCommit h = {};
                    if (!Receive(i, &h, len)) { return; }
                    /* one format; the host picks 1080p (1) or 720p (2) */
                    const u8 frame = (h.bFrameIndex == 2) ? 2 : 1;
                    ProbeCommit n = h;
                    n.bFormatIndex = 1;
                    n.bFrameIndex = frame;
                    n.dwFrameInterval = FrameInterval(frame);
                    n.dwMaxVideoFrameSize = FrameBytes(frame);
                    n.dwMaxPayloadTransferSize = MaxPayload;
                    n.dwClockFrequency = ClockFrequency;
                    n.bmFramingInfo = 0x03;
                    *ctrl = n;
                    if (cs == VS_COMMIT) {
                        g_frame_index.store(frame, std::memory_order_relaxed);
                        g_uvc_streaming.store(true, std::memory_order_relaxed);
                        g_uvc_new_consumer.store(true, std::memory_order_relaxed);
                        ++s_commits;
                        if (g_uvc_raw) { LogLine("uvc: commit - NV12 %s", frame == 2 ? "768x432 at 60 fps" : "1280x720 at 25 fps"); }
                        else { LogLine("uvc: commit - %s at 60 fps", frame == 2 ? "1280x720" : "1920x1080"); }
                    }
                    break;
                }
                default: Stall(i, ERR_INVALID_REQUEST); return;
            }
            g_last_error = ERR_NONE;
        }

        void HandleVc(UsbDsInterface *i, const Setup &s) {
            const u8 entity = s.wIndex >> 8, cs = s.wValue >> 8;
            if (entity == 0 && cs == VC_ERROR_CODE) {
                if (s.bRequest == GET_CUR) { Reply(i, &g_last_error, 1); return; }
                if (s.bRequest == GET_INFO) { const u8 info = 0x01; Reply(i, &info, 1); return; }
            }
            if (entity == 0 && cs == VC_POWER_MODE) {
                if (s.bRequest == GET_CUR) { const u8 p = 0; Reply(i, &p, 1); g_last_error = ERR_NONE; return; }
                if (s.bRequest == SET_CUR) { u8 p = 0; if (Receive(i, &p, 1)) { g_last_error = ERR_NONE; } return; }
                if (s.bRequest == GET_INFO) { const u8 info = 0x03; Reply(i, &info, 1); g_last_error = ERR_NONE; return; }
            }
            Stall(i, entity == 0 || entity == ENTITY_IT || entity == ENTITY_OT ? ERR_INVALID_CONTROL : ERR_INVALID_UNIT);
        }

        void HandleSetup(UsbDsInterface *i) {
            Setup s = {};
            if (R_FAILED(usbDsInterface_GetSetupPacket(i, &s, sizeof(s)))) { return; }
            const u8 type = s.bmRequestType & 0x60, recipient = s.bmRequestType & 0x1F;
            if (recipient == 0x01 && (s.wIndex & 0xFF) != i->interface_index) { return; }   /* the other interface's */
            if (type == 0x20 && recipient == 0x01) {
                if (i == g_vs_intf) { HandleVs(i, s); } else { HandleVc(i, s); }
                return;
            }
            if (type == 0x00) {
                if (s.bRequest == 0x0B) {                /* SET_INTERFACE: alt 0 only */
                    if (s.wValue == 0) { if (R_FAILED(CtrlIn(i, CtrlBuf(i), 0))) { usbDsInterface_StallCtrl(i); } }
                    else { usbDsInterface_StallCtrl(i); }
                    return;
                }
                if (s.bRequest == 0x0A && (s.bmRequestType & 0x80)) { const u8 alt = 0; Reply(i, &alt, 1); return; }
            }
            usbDsInterface_StallCtrl(i);
        }

        bool ServiceSetup(UsbDsInterface *i) {
            if (i == nullptr || R_FAILED(eventWait(&i->SetupEvent, 0))) { return false; }
            eventClear(&i->SetupEvent);
            HandleSetup(i);
            return true;
        }

        alignas(os::ThreadStackAlignment) constinit u8 g_ep0_stack[16_KB];
        constinit os::ThreadType g_ep0_thread;

        void Ep0Thread(void *) {
            bool configured = false;
            for (;;) {
                UsbState st = UsbState_Detached;
                if (R_FAILED(usbDsGetState(&st)) || st != UsbState_Configured) {
                    if (configured) {
                        configured = false;
                        g_uvc_streaming.store(false, std::memory_order_relaxed);
                        g_last_error = ERR_NONE;
                        LogLine("uvc: host disconnected");
                    }
                    os::SleepThread(TimeSpan::FromMilliSeconds(50));
                    continue;
                }
                if (!configured) { configured = true; LogLine("uvc: host connected (configured)"); }
                s32 idx = -1;
                if (R_FAILED(waitMulti(&idx, 100000000ull, waiterForEvent(&g_vc_intf->SetupEvent), waiterForEvent(&g_vs_intf->SetupEvent)))) { continue; }
                /* a negotiation is a burst: serve until 20 ms pass without a request */
                for (;;) {
                    bool any = ServiceSetup(g_vc_intf);
                    any |= ServiceSetup(g_vs_intf);
                    if (!any && R_FAILED(waitMulti(&idx, 20000000ull, waiterForEvent(&g_vc_intf->SetupEvent), waiterForEvent(&g_vs_intf->SetupEvent)))) { break; }
                }
            }
        }

        /* ---- the sender ------------------------------------------------------- */
        constinit os::SdkMutex g_send_lock;
        constinit os::SdkConditionVariable g_send_cv;
        constinit const u8 *g_send_buf = nullptr;
        constinit size_t g_send_len = 0;
        constinit bool g_send_busy = false;
        constinit u8 g_fid = 0;

        alignas(os::ThreadStackAlignment) constinit u8 g_send_stack[16_KB];
        constinit os::ThreadType g_send_thread;

        /* one transfer: post, wait, reap; false on a timeout (cancelled + reaped) */
        bool PostBuf(const u8 *buf, size_t n, u64 timeout_ns = 1000000000ull) {
            UsbDsReportData r;
            eventClear(&g_video_ep->CompletionEvent);
            u32 urb = 0;
            if (R_FAILED(usbDsEndpoint_PostBufferAsync(g_video_ep, const_cast<u8 *>(buf), n, &urb))) { return false; }
            if (R_FAILED(eventWait(&g_video_ep->CompletionEvent, timeout_ns))) {
                usbDsEndpoint_Cancel(g_video_ep);
                if (R_SUCCEEDED(eventWait(&g_video_ep->CompletionEvent, 100000000ull))) {
                    eventClear(&g_video_ep->CompletionEvent);
                    usbDsEndpoint_GetReportData(g_video_ep, &r);
                }
                return false;
            }
            eventClear(&g_video_ep->CompletionEvent);
            usbDsEndpoint_GetReportData(g_video_ep, &r);
            return true;
        }

        /* one payload from the bounce buffer */
        bool PostOne(size_t n, u64 timeout_ns = 1000000000ull) { return PostBuf(g_bounce, n, timeout_ns); }

        /* v0.7.7: a frame UvcRawPack already cut into payloads (every one
         * MaxPayload bytes but the last): 16 payloads per transfer. A bulk
         * payload ends at a short packet or at dwMaxPayloadTransferSize, so
         * full payloads back to back split on the host exactly as if each
         * were its own transfer - and a 1.4 MB frame costs 6 round trips to
         * usb:ds instead of 85. */
        constexpr size_t RawChunk = 16 * MaxPayload;
        void SendPacked(const u8 *data, size_t size) {
            bool ok = true;
            for (size_t off = 0; off < size && ok; off += RawChunk) {
                const size_t n = size - off < RawChunk ? size - off : RawChunk;
                ok = PostBuf(data + off, n);
            }
            /* the last payload short of the maximum but a multiple of 512 ends with no short packet */
            const size_t last = size % MaxPayload;
            if (ok && last != 0 && (last % 512) == 0) { ok = PostOne(0); }
            if (ok) { ++s_frames; s_drop_run = 0; } else { ++s_dropped; ++s_drop_run; }
        }

        void SendFrame(const u8 *data, size_t size) {
            const size_t max_chunk = MaxPayload - 12;   /* keeps every payload short of the maximum */
            bool ok = true;
            while (size > 0 && ok) {
                const size_t n = size > max_chunk ? max_chunk : size;
                const bool last = n == size;
                g_bounce[0] = 2;
                g_bounce[1] = HDR_EOH | (last ? HDR_EOF : 0) | (g_fid & 1 ? HDR_FID : 0);
                std::memcpy(g_bounce + 2, data, n);
                const size_t total = n + 2;
                ok = PostOne(total);
                /* no short packet would end this payload: add a zero-length one */
                if (ok && (total % 512) == 0) { ok = PostOne(0); }
                data += n; size -= n;
            }
            if (ok) { ++s_frames; s_drop_run = 0; } else { ++s_dropped; ++s_drop_run; }
            g_fid ^= 1;   /* sent or aborted: an aborted frame must not merge into the next */
        }

        void SendThread(void *) {
            for (;;) {
                const u8 *buf = nullptr;
                size_t len = 0;
                {
                    std::scoped_lock lk(g_send_lock);
                    while (g_send_buf == nullptr) { g_send_cv.Wait(g_send_lock); }
                    buf = g_send_buf; len = g_send_len;
                }
                if (!g_uvc_streaming.load(std::memory_order_relaxed)) { ++s_dropped; }
                else if (g_uvc_raw) { SendPacked(buf, len); }
                else { SendFrame(buf, len); }
                {
                    std::scoped_lock lk(g_send_lock);
                    g_send_buf = nullptr;
                    g_send_busy = false;
                    g_send_cv.Broadcast();
                }
            }
        }

    }

    bool UvcSetupDevice() {
        LogLine("---- USB: webcam mode (UVC 1.5, %s, 1209:5F1F) ----", g_uvc_raw ? "uncompressed NV12 - any camera app" : "H.264");
        ::Result rc = usbDsInitialize();
        if (R_FAILED(rc)) { LogLine("   usbDsInitialize rc=0x%x", rc); return false; }
        u8 iMan = 0, iProd = 0, iSer = 0;
        static const u16 langs[1] = { 0x0409 };
        rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
        if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(&iMan, "switch-frame-tap"); }
        if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(&iProd, "Switch Frame Tap Camera"); }
        /* v0.7.7: the NV12 camera has its own serial and bcdDevice, so a host
         * that remembers the H.264 camera's formats sees a new device */
        if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(&iSer, g_uvc_raw ? "0002" : "0001"); }
        usb_device_descriptor dd = {
            USB_DT_DEVICE_SIZE, USB_DT_DEVICE, 0x0200,
            0xEF, 0x02, 0x01,          /* multi-interface: interface association */
            64, 0x1209, 0x5F1F, static_cast<u16>(g_uvc_raw ? 0x0200 : 0x0100), iMan, iProd, iSer, 1,
        };
        if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dd); }
        if (R_SUCCEEDED(rc)) { dd.bcdUSB = 0x0300; dd.bMaxPacketSize0 = 9; rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dd); }
        static const u8 bos[] = {
            0x05, 0x0F, 0x16, 0x00, 0x02,
            0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
            0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
        };
        if (R_SUCCEEDED(rc)) { rc = usbDsSetBinaryObjectStore(bos, sizeof(bos)); }
        LogLine("   device descriptors rc=0x%x", rc);
        if (R_FAILED(rc)) { return false; }

        /* VC first (the IAD goes on the first registered interface), then VS */
        if (R_FAILED(rc = usbDsRegisterInterface(&g_vc_intf)) || R_FAILED(rc = usbDsRegisterInterface(&g_vs_intf))) {
            LogLine("   RegisterInterface rc=0x%x", rc); return false;
        }
        const u8 vc_n = g_vc_intf->interface_index, vs_n = g_vs_intf->interface_index;
        g_iad.bFirstInterface = vc_n;
        g_vc_if.bInterfaceNumber = vc_n;
        g_vc.header.baInterfaceNr1 = vs_n;
        g_vs_if.bInterfaceNumber = vs_n;
        g_ep.bEndpointAddress = static_cast<u8>(USB_ENDPOINT_IN | (vs_n + 1));
        g_vs.header.bEndpointAddress = g_ep.bEndpointAddress;
        g_vs_raw.header.bEndpointAddress = g_ep.bEndpointAddress;

        /* one descriptor per append, each checked by name: a refusal here
         * cannot be undone this boot, so the log must say which one */
        const char *step = "";
        auto add = [&](UsbDsInterface *i, UsbDeviceSpeed sp, const void *d, size_t n, const char *what) {
            if (R_FAILED(rc)) { return; }
            rc = usbDsInterface_AppendConfigurationData(i, sp, d, n);
            if (R_FAILED(rc)) { step = what; LogLine("   append %s (%s, %zu B) rc=0x%x", what, sp == UsbDeviceSpeed_Super ? "Super" : "High", n, rc); }
        };
        for (const UsbDeviceSpeed sp : { UsbDeviceSpeed_High, UsbDeviceSpeed_Super }) {
            add(g_vc_intf, sp, &g_iad, sizeof(g_iad), "IAD");
            add(g_vc_intf, sp, &g_vc_if, USB_DT_INTERFACE_SIZE, "VC interface");
            add(g_vc_intf, sp, &g_vc, sizeof(g_vc), "VC class block");
        }
        if (R_SUCCEEDED(rc) && R_FAILED(rc = usbDsInterface_EnableInterface(g_vc_intf))) { step = "enable VC"; }
        for (const UsbDeviceSpeed sp : { UsbDeviceSpeed_High, UsbDeviceSpeed_Super }) {
            g_ep.wMaxPacketSize = sp == UsbDeviceSpeed_Super ? 1024 : 512;
            add(g_vs_intf, sp, &g_vs_if, USB_DT_INTERFACE_SIZE, "VS interface");
            if (g_uvc_raw) {
                add(g_vs_intf, sp, &g_vs_raw.header, sizeof(g_vs_raw.header), "VS input header");
                add(g_vs_intf, sp, &g_vs_raw.format, sizeof(g_vs_raw.format), "NV12 format");
                add(g_vs_intf, sp, &g_vs_raw.f720, sizeof(g_vs_raw.f720), "720p NV12 frame");
                add(g_vs_intf, sp, &g_vs_raw.f432, sizeof(g_vs_raw.f432), "432p NV12 frame");
            } else {
                add(g_vs_intf, sp, &g_vs.header, sizeof(g_vs.header), "VS input header");
                add(g_vs_intf, sp, &g_vs.format, sizeof(g_vs.format), "H.264 format");
                add(g_vs_intf, sp, &g_vs.f1080, sizeof(g_vs.f1080), "1080p frame");
                add(g_vs_intf, sp, &g_vs.f720, sizeof(g_vs.f720), "720p frame");
            }
            add(g_vs_intf, sp, &g_ep, USB_DT_ENDPOINT_SIZE, "bulk endpoint");
            if (sp == UsbDeviceSpeed_Super) { add(g_vs_intf, sp, &g_ss, sizeof(g_ss), "SS companion"); }
        }
        if (R_SUCCEEDED(rc) && R_FAILED(rc = usbDsInterface_RegisterEndpoint(g_vs_intf, &g_video_ep, g_ep.bEndpointAddress))) { step = "register endpoint"; }
        if (R_SUCCEEDED(rc) && R_FAILED(rc = usbDsInterface_EnableInterface(g_vs_intf))) { step = "enable VS"; }
        if (R_FAILED(rc)) { LogLine("   failed at: %s", step); }
        LogLine("   interfaces: VC %u, VS %u, video endpoint 0x%02x rc=0x%x", vc_n, vs_n, g_ep.bEndpointAddress, rc);
        if (R_FAILED(rc)) { return false; }
        rc = usbDsEnable();
        LogLine("   usbDsEnable rc=0x%x", rc);
        if (R_FAILED(rc)) { return false; }

        DefaultProbe(&g_probe, 1);
        g_commit = g_probe;

        SFT_ABORT_UNLESS(os::CreateThread(&g_ep0_thread, Ep0Thread, nullptr, g_ep0_stack, sizeof(g_ep0_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(&g_ep0_thread, "applet-mitm.UvcEp0");
        os::StartThread(&g_ep0_thread);
        SFT_ABORT_UNLESS(os::CreateThread(&g_send_thread, SendThread, nullptr, g_send_stack, sizeof(g_send_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(&g_send_thread, "applet-mitm.UvcSend");
        os::StartThread(&g_send_thread);
        return true;
    }

    void UvcFrameSize(u32 *w, u32 *h) {
        const bool f2 = g_frame_index.load(std::memory_order_relaxed) == 2;
        if (g_uvc_raw) { *w = f2 ? Raw432W : Raw720W; *h = f2 ? Raw432H : Raw720H; return; }
        *w = f2 ? 1280 : 1920;
        *h = f2 ? 720 : 1080;
    }

    size_t UvcRawPack(u8 *dst, const u8 *src, size_t len, u32 frame_no) {
        constexpr size_t Data = MaxPayload - 2;
        const u8 fid = (frame_no & 1) ? HDR_FID : 0;
        u8 *d = dst;
        while (len > 0) {
            const size_t n = len > Data ? Data : len;
            d[0] = 2;
            d[1] = HDR_EOH | fid | (n == len ? HDR_EOF : 0);
            std::memcpy(d + 2, src, n);
            d += n + 2; src += n; len -= n;
        }
        return static_cast<size_t>(d - dst);
    }

    void UvcQueueFrame(const u8 *buf, size_t len) {
        std::scoped_lock lk(g_send_lock);
        while (g_send_busy) { g_send_cv.Wait(g_send_lock); }
        g_send_buf = buf;
        g_send_len = len;
        g_send_busy = true;
        g_send_cv.Broadcast();
    }

    void UvcWaitSent() {
        std::scoped_lock lk(g_send_lock);
        while (g_send_busy) { g_send_cv.Wait(g_send_lock); }
    }

    bool UvcHostStalled() { return s_drop_run.load(std::memory_order_relaxed) >= 3; }

    bool UvcHostReading(u32 timeout_ms) {
        if (g_uvc_new_consumer.load(std::memory_order_relaxed)) { s_drop_run = 0; return true; }
        std::scoped_lock lk(g_send_lock);
        if (g_send_busy || !g_uvc_streaming.load(std::memory_order_relaxed)) { return false; }
        g_bounce[0] = 2;
        g_bounce[1] = HDR_EOH | (g_fid & 1 ? HDR_FID : 0);
        if (!PostOne(2, static_cast<u64>(timeout_ms) * 1000000ull)) { return false; }
        s_drop_run = 0;
        LogLine("uvc: the host reads again");
        return true;
    }

    u32 UvcDroppedCount() { return s_dropped.load(std::memory_order_relaxed); }

    void UvcLogStats(const char *who) {
        LogLine("   %s: webcam - %u frames sent, %u dropped (host not reading), %u commits", who,
                s_frames.load(), s_dropped.load(), s_commits.load());
    }

}
