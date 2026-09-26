/*
 * applet-mitm - Track B / M1
 *
 * Standalone libstratosphere sysmodule that mitms "appletOE" and logs when a
 * game opens its application proxy. Forwards everything untouched.
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000C20/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000C20/flags/boot2.flag   (empty)
 * Log: sdmc:/applet-mitm.log
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_service.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_nvdrv.hpp"
#include "applet_mitm_nvjpg.hpp"
#include "applet_mitm_armfile.hpp"
#include "applet_mitm_clk.hpp"

/* Force libnx's nv layer to use "nvdrv:s" instead of picking a service via
 * appletGetAppletType() - that call is meaningless here and is what made a
 * plain sysmodule fatal on nvInitialize() during Phase 0. Also shrink the
 * nvdrv transfer memory: we only issue tiny nvmap ioctls, and libnx's 3 MB
 * default would not fit our allocator. */
extern "C" {
    NvServiceType __nx_nv_service_type    = NvServiceType_System;
    u32           __nx_nv_transfermem_size = 0x40000;
}

namespace ams {

    namespace {

        constexpr size_t MallocBufferSize = 1_MB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        enum PortIndex {
            PortIndex_AppletMitm,
            PortIndex_Count,
        };

        constexpr sm::ServiceName AppletMitmServiceName = sm::ServiceName::Encode("vi:u");

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        constexpr size_t MaxSessions = 8;

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_server_manager;

        /* ---- M72: the grc recorder's own server --------------------------
         * Separate from vi:u on purpose. Anything forwarded for grc can block
         * inside nvservices - a syncpoint wait, an event wait - and on a shared
         * LoopProcess thread that would stall the game's binder traffic, i.e.
         * freeze the picture. Here a blocked grc call ties up one of four
         * threads that serve grc and nobody else. */
        enum NvPortIndex {
            NvPortIndex_Nvdrv,
            NvPortIndex_Count,
        };

        constexpr sm::ServiceName NvdrvMitmServiceName = sm::ServiceName::Encode("nvdrv:s");
        constexpr size_t NvMaxSessions = 16;
        constexpr size_t NvThreads     = 4;

        /* M72b: a mitm session's pointer buffer must be at least as large as the
         * real service's (sf_hipc_server_session_manager.cpp:148 aborts
         * otherwise). M72 reused vi:u's 0x1000, and the first grc session
         * fataled the module the instant Mario Kart launched. nvservices'
         * size is reported through QueryPointerBufferSize as a u16, so
         * 0x10000 covers any value it can have. 16 sessions x 64 KB = 1 MB of
         * .bss, from the Applet pool that had 509 MB free at boot. */
        struct NvServerOptions {
            static constexpr size_t PointerBufferSize   = 0x10000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        class NvServerManager final : public sf::hipc::ServerManager<NvPortIndex_Count, NvServerOptions, NvMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        NvServerManager g_nv_server_manager;

        alignas(os::ThreadStackAlignment) constinit u8 g_nv_stacks[NvThreads][32_KB];
        constinit os::ThreadType g_nv_threads[NvThreads];

        void NvLoopThread(void *) { g_nv_server_manager.LoopProcess(); }

        /* Registered before anything else in main: mitm.lst has boot2 declare
         * this mitm in advance, so every nvdrv:s client in the system is waiting
         * on us until this returns. */
        void StartGrcRecorder() {
            /* M73: opt-in. M72 registered this unconditionally, which meant
             * every run carried the recorder's risk even when the run had
             * nothing to do with grc. Without "grc" in the arm file nothing
             * here is registered at all, so nvdrv:s is untouched - unless
             * mitm.lst is on the card, in which case see M76 below. */
            bool have_lst = false;
            {
                fs::FileHandle lf;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(lf), "sdmc:/atmosphere/contents/0100000000000C20/mitm.lst", fs::OpenMode_Read))) {
                    fs::CloseFile(lf);
                    have_lst = true;
                }
            }
            if (!mitm::applet::g_grc_armed) {
                if (!have_lst) {
                    mitm::applet::LogLine("grc IPC interceptor: off");
                    return;
                }
                /* M76: mitm.lst left on the card from M72-M75 and "grc" not
                 * armed. boot2 has already declared a future nvdrv:s mitm, so
                 * if nothing registers, every nvdrv:s client - vi among them -
                 * waits forever and the console never reaches the home menu.
                 * Register anyway: ShouldMitm is false for everyone while grc
                 * is off, so every client gets the real service and none of
                 * the recorder's handlers can run. */
                mitm::applet::LogLine("grc IPC interceptor: off, but mitm.lst is PRESENT - registering as a pass-through "
                                      "(accepts nobody) so nvdrv:s clients are not blocked. Delete mitm.lst.");
            } else if (!have_lst) {
                /* M75: refuse without mitm.lst. boot2 only declares a future mitm
                 * for services listed there; registering one late means clients
                 * that already have sessions bypass us, and the ones that do not
                 * can block. Checking is cheap and the failure mode is a console
                 * that will not launch a game. */
                mitm::applet::LogLine("grc IPC interceptor: ARMED but mitm.lst is MISSING - refusing to register");
                return;
            } else {
                mitm::applet::LogLine("grc IPC interceptor: ARMED (this handler has hung the console before)");
            }
            R_ABORT_UNLESS(g_nv_server_manager.RegisterMitmServer<mitm::applet::NvDrvMitm>(NvPortIndex_Nvdrv, NvdrvMitmServiceName));
            const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
            for (size_t i = 0; i < NvThreads; ++i) {
                R_ABORT_UNLESS(os::CreateThread(std::addressof(g_nv_threads[i]), NvLoopThread, nullptr,
                                                g_nv_stacks[i], sizeof(g_nv_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_nv_threads[i]), "applet-mitm.NvDrv");
                os::StartThread(std::addressof(g_nv_threads[i]));
            }
        }

        /* ---- liveness heartbeat -------------------------------------------
         * Two runs in a row went silent after "registered mitm server", with
         * the system wedging on vi. That is ambiguous: the process may have
         * died, or it may be alive but never reaching our handlers (sm blocks
         * every vi:u open on the mitm query port, so a dead LoopProcess wedges
         * the system exactly the same way).
         *
         * This thread is independent of the dispatch path. LogMark rewrites
         * sdmc:/applet-mitm.last whole, so even after a forced power-off the
         * last committed beat tells us how long the process lived. */
        alignas(os::ThreadStackAlignment) constinit u8 g_hb_stack[16_KB];
        constinit os::ThreadType g_hb_thread;

        void HeartbeatThread(void *) {
            for (u32 i = 1; ; i++) {
                os::SleepThread(TimeSpan::FromSeconds(3));
                char b[96];
                const auto &st = mitm::applet::g_stats;
                std::snprintf(b, sizeof(b), "hb:%u sess=%u getdisp=%u relay=%u txn=%u vic=%s",
                              i, st.sessions.load(), st.getdisp.load(),
                              st.relay.load(), st.txns.load(),
                              mitm::applet::g_vic_stage.load(std::memory_order_relaxed));
                mitm::applet::LogMark(b);
                mitm::applet::FlushNvdrvRecords();
            }
        }

        /* ---- opt-in arm file ----------------------------------------------
         * Nothing touches nvdrv/VIC unless sdmc:/applet-mitm.armed exists and
         * contains "vic". Default is a pure observer, i.e. M7d behaviour. */
        /* Called once per flag at boot; the file is a single short line. */
        size_t ReadArmFile(char *buf, size_t cap) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/applet-mitm.armed", fs::OpenMode_Read))) {
                return 0;
            }
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return 0; }
            size_t n = static_cast<size_t>(fsz);
            if (n > cap - 1) { n = cap - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return 0; }
            buf[n] = '\0';
            return n;
        }

        /* M75: WHOLE-TOKEN match, not substring.
         *
         * This was `strstr`, and it cost a hardware cycle and a forced
         * power-off. The arm file "vic grcscan wait=90" made
         * ArmFileContains("grc") true, because "grcscan" contains "grc" -
         * so the grc IPC interceptor registered when I believed it was off,
         * re-running a handler already known to be broken, and the game
         * could not launch. The run I thought was a read-only observer was
         * nothing of the kind.
         *
         * M76: ArmFileNumber still used `strstr` and carried the same bug:
         * "sweep stream sw=768" read `sw` out of "sweep" and fell back to 0.
         * Both now share applet_mitm_armfile.hpp, which has a host test. */
        bool ArmFileContains(const char *keyword) {
            char buf[256] = {};
            const size_t n = ReadArmFile(buf, sizeof(buf));
            return mitm::applet::armfile::Contains(buf, n, keyword);
        }

        /* Numeric option out of the arm file: "wait=180" delays the probe so
         * there is time to actually get into a race before it fires. M39 probed
         * at ~50 s, which is still the title screen. */
        u32 ArmFileNumber(const char *key, u32 def) {
            char buf[256] = {};
            const size_t n = ReadArmFile(buf, sizeof(buf));
            return mitm::applet::armfile::Number(buf, n, key, def);
        }

        /* Ask the kernel how big each physical memory pool is, rather than
         * inferring our budget from a failed allocation.
         *
         * M47 measured this process at total=4720 KB, used=3744 KB - the whole
         * allocation is smaller than ONE 1080p frame (7913 KB). masagrator's
         * objection on GBAtemp was right, and by a wider margin than he argued.
         * The open question is whether pool_partition is the lever: we are on 2
         * (System), and if Applet or Application has meaningful headroom then a
         * one-line NPDM change might buy the room NVENC needs.
         *
         * Deliberately logged at BOOT. Every memory question then costs a ~20 s
         * boot instead of a three-minute race to reach the probe. */
        void LogMemoryPools() {
            static const char *const names[4] = { "Application", "Applet", "System", "SystemUnsafe" };
            mitm::applet::LogLine("---- physical memory pools (svcGetSystemInfo) ----");
            for (u64 pool = 0; pool < 4; ++pool) {
                u64 tot = 0, used = 0;
                const auto r1 = svc::GetSystemInfo(std::addressof(tot),  svc::SystemInfoType_TotalPhysicalMemorySize, svc::InvalidHandle, pool);
                const auto r2 = svc::GetSystemInfo(std::addressof(used), svc::SystemInfoType_UsedPhysicalMemorySize,  svc::InvalidHandle, pool);
                mitm::applet::LogLine("   pool %llu %-12s total=%8llu KB  used=%8llu KB  free=%9lld KB   rc=%x/%x",
                                      static_cast<unsigned long long>(pool), names[pool],
                                      static_cast<unsigned long long>(tot / 1024),
                                      static_cast<unsigned long long>(used / 1024),
                                      static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024),
                                      r1.GetValue(), r2.GetValue());
            }
            u64 ptot = 0, pused = 0;
            svc::GetInfo(std::addressof(ptot),  svc::InfoType_TotalMemorySize, svc::PseudoHandle::CurrentProcess, 0);
            svc::GetInfo(std::addressof(pused), svc::InfoType_UsedMemorySize,  svc::PseudoHandle::CurrentProcess, 0);
            mitm::applet::LogLine("   THIS PROCESS (pool_partition 1 = Applet, at boot): total=%llu KB used=%llu KB free=%lld KB",
                                  static_cast<unsigned long long>(ptot / 1024),
                                  static_cast<unsigned long long>(pused / 1024),
                                  static_cast<long long>((static_cast<s64>(ptot) - static_cast<s64>(pused)) / 1024));
            mitm::applet::LogLine("   for scale: one 1080p frame = 7913 KB, one block-row strip = 960 KB");
        }

        /* M50 FATALED THE CONSOLE: 2001-0132 (0x10801, kernel LimitReached) in
         * program 0100000000000023 - am, the applet manager. Not us: a DIFFERENT
         * sysmodule, killed because we took the memory it needed.
         *
         *     [boot] System pool now: used=231424 KB free=6304 KB (we took 8192 KB)
         *
         * 8 MB out of a pool with 14,496 KB free left 6,304 KB, and am could not
         * start. The allocation itself always succeeded - holding it is what
         * broke the console.
         *
         * So M49's reading was wrong, and so was mine. 8 MB was grantable at boot
         * precisely BECAUSE am had not allocated yet; that was a transient, not
         * headroom. The 2 MB seen at probe time is the System pool's honest
         * steady state once every sysmodule has claimed its share. masagrator's
         * original objection stands: on 22.5.0 a sysmodule cannot hold a 1080p
         * buffer.
         *
         * This is the second time this pool has been over-drawn - M27 did it with
         * 4 MB of .bss and fataled a sysmodule the same way. No heap is taken at
         * boot. The probe allocates 2 MB, which has been safe across ~20 runs,
         * and capture stays strip-wise - which costs nothing structurally, since
         * the 983,040 B block-row is the natural unit of the block-linear layout
         * anyway. */

        /* ---- USB device enumeration -------------------------------------
         * The transport half, and the reason it is worth a run before NVENC:
         * NVENC has a class ID now (0x21, cross-checked against VIC's 0x5D which
         * we proved on hardware in M16) but no method table anywhere local, so a
         * SETCL for it would be writing blind at an engine. USB is safe - no
         * engine, no memory grab - and it is on the critical path.
         *
         * The arithmetic that makes this the interesting direction: the VIC
         * already downscales correctly, and 480x270 raw at 30 fps is 15.5 MB/s,
         * inside USB 2.0's ~30 MB/s with no encoder at all. A working prototype
         * without NVENC.
         *
         * Sequence and descriptor values follow SysDVR's UsbComms.c, which is a
         * known-good implementation on this firmware. Endpoint buffers are
         * static 0x1000 pairs - 8 KB of .bss, not heap, so this costs nothing
         * against the 976 KB. */
        alignas(0x1000) constinit u8 g_usb_ep_in_buf[0x1000]  = {};
        alignas(0x1000) constinit u8 g_usb_ep_out_buf[0x1000] = {};
        constinit bool g_usb_armed = false;

        /* M57: the endpoint handles have to outlive TryUsbEnumerate. Until now
         * they were locals inside it and were discarded the moment it returned,
         * which is why the transport could enumerate but never transmit a byte. */
        constinit UsbDsInterface *g_usb_iface  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_in  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_out = nullptr;

        /* One bulk post per iteration. 256 KB is a multiple of 0x1000, so every
         * chunk boundary stays aligned for the next PostBufferAsync. */
        constexpr size_t UsbChunk     = 0x40000;
        constexpr u64    UsbTimeoutNs = UINT64_C(5000000000);

        void TryUsbEnumerate() {
            if (!g_usb_armed) {
                mitm::applet::LogLine("   usb: not armed (add \"usb\" to the arm file)");
                return;
            }
            mitm::applet::LogLine("---- USB device enumeration (usb:ds) ----");

            /* ::Result - libnx's u32 - NOT ams::Result, which is a class and
             * would not survive being passed through printf varargs. */
            ::Result rc = usbDsInitialize();
            mitm::applet::LogLine("   usbDsInitialize rc=0x%x %s", rc,
                                  R_SUCCEEDED(rc) ? "" : "<- could not acquire usb:ds");
            if (R_FAILED(rc)) { return; }

            u8 iMan = 0, iProd = 0, iSer = 0;
            static const u16 langs[1] = { 0x0409 };
            rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iMan),  "switch-frame-tap"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iProd), "Switch Frame Tap"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iSer),  "0001"); }
            mitm::applet::LogLine("   string descriptors rc=0x%x", rc);

            struct usb_device_descriptor dd = {
                .bLength            = USB_DT_DEVICE_SIZE,
                .bDescriptorType    = USB_DT_DEVICE,
                .bcdUSB             = 0x0110,
                .bDeviceClass       = 0x00,
                .bDeviceSubClass    = 0x00,
                .bDeviceProtocol    = 0x00,
                .bMaxPacketSize0    = 0x40,
                .idVendor           = 0x1209,   /* pid.codes open range */
                .idProduct          = 0x5F1E,
                .bcdDevice          = 0x0100,
                .iManufacturer      = iMan,
                .iProduct           = iProd,
                .iSerialNumber      = iSer,
                .bNumConfigurations = 0x01,
            };
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, std::addressof(dd));
            dd.bcdUSB = 0x0200;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, std::addressof(dd)); }
            /* M58: SuperSpeed. M52-M57 declared only Full and High, so the host
             * negotiated 480 Mbps because that is all we ever offered - the
             * ceiling was ours, not the platform's. Atmosphere's own haze
             * declares Super (usb_session.cpp:144), which proves the console
             * supports USB 3.0 device mode.
             *
             * USB 3.0 requires bcdUSB 0x0300 and bMaxPacketSize0 encoded as a
             * POWER OF TWO: 9 means 2^9 = 512, not 512 itself. */
            dd.bcdUSB          = 0x0300;
            dd.bMaxPacketSize0 = 9;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, std::addressof(dd)); }
            mitm::applet::LogLine("   device descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            /* M71: the Binary Object Store - the piece M58 was missing.
             *
             * M58 declared SuperSpeed device and endpoint descriptors and the
             * link still trained to High every time. I read that as a cable or
             * a console limit. It was neither: USB 3.0 enumeration requires a
             * BOS containing a SuperSpeed USB Device Capability descriptor, and
             * we never called usbDsSetBinaryObjectStore at all. Atmosphere's
             * haze does (usb_session.cpp:220) immediately after its Super
             * device descriptor, and haze negotiates SuperSpeed.
             *
             * wSpeedSupported 0x000c = bit2 (High) | bit3 (Super).
             * bFunctionalitySupport 3 = full functionality from High upward. */
            {
                const u8 bos[0x16] = {
                    0x05, USB_DT_BOS, 0x16, 0x00, 0x02,

                    /* USB 2.0 extension */
                    0x07, USB_DT_DEVICE_CAPABILITY, 0x02,
                    0x02, 0x00, 0x00, 0x00,

                    /* SuperSpeed USB device capability */
                    0x0a, USB_DT_DEVICE_CAPABILITY, 0x03,
                    0x00,
                    0x0c, 0x00,
                    0x03,
                    0x00,
                    0x00, 0x00,
                };
                rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
                mitm::applet::LogLine("   usbDsSetBinaryObjectStore rc=0x%x%s", rc,
                                      R_SUCCEEDED(rc) ? "" : "  <- SuperSpeed will not be offered");
                if (R_FAILED(rc)) { return; }
            }

            /* bound to the file-scope handles so the transport can post to
             * ep_in long after boot; the rest of this function is unchanged */
            UsbDsInterface *&iface  = g_usb_iface;
            UsbDsEndpoint  *&ep_in  = g_usb_ep_in;
            UsbDsEndpoint  *&ep_out = g_usb_ep_out;
            iface = nullptr; ep_in = nullptr; ep_out = nullptr;

            struct usb_interface_descriptor id = {
                .bLength            = USB_DT_INTERFACE_SIZE,
                .bDescriptorType    = USB_DT_INTERFACE,
                .bInterfaceNumber   = 4,
                .bAlternateSetting  = 0,
                .bNumEndpoints      = 2,
                .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
                .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
                .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
                .iInterface         = 0,
            };
            struct usb_endpoint_descriptor ep_i = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_IN,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x200,
                .bInterval        = 0,
            };
            struct usb_endpoint_descriptor ep_o = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_OUT,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x40,
                .bInterval        = 0,
            };

            std::memset(g_usb_ep_in_buf,  0, sizeof(g_usb_ep_in_buf));
            std::memset(g_usb_ep_out_buf, 0, sizeof(g_usb_ep_out_buf));

            rc = usbDsRegisterInterface(std::addressof(iface));
            mitm::applet::LogLine("   usbDsRegisterInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            id.bInterfaceNumber   = iface->interface_index;
            ep_i.bEndpointAddress = static_cast<u8>(ep_i.bEndpointAddress + id.bInterfaceNumber + 1);
            ep_o.bEndpointAddress = static_cast<u8>(ep_o.bEndpointAddress + id.bInterfaceNumber + 1);

            /* Full speed */
            rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(id),   USB_DT_INTERFACE_SIZE);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            /* High speed - 512 B bulk, the speed we actually expect */
            ep_i.wMaxPacketSize = 0x200;
            ep_o.wMaxPacketSize = 0x200;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(id),   USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            /* M58: SuperSpeed config - 1024 B bulk, and every SS endpoint needs a
             * companion descriptor immediately after it or the host rejects the
             * configuration. bMaxBurst 0x0f = 16 packets per burst, copied from
             * haze (usb_session.cpp:108). Order matters: iface, ep, companion,
             * ep, companion. */
            struct usb_ss_endpoint_companion_descriptor ss_comp = {
                .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
                .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
                .bMaxBurst         = 0x0f,
                .bmAttributes      = 0x00,
                .wBytesPerInterval = 0x00,
            };
            ep_i.wMaxPacketSize = 0x400;
            ep_o.wMaxPacketSize = 0x400;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(id),      USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_i),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_o),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            mitm::applet::LogLine("   configuration descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_in), ep_i.bEndpointAddress);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_out), ep_o.bEndpointAddress); }
            mitm::applet::LogLine("   endpoints IN=0x%02x OUT=0x%02x rc=0x%x", ep_i.bEndpointAddress, ep_o.bEndpointAddress, rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_EnableInterface(iface);
            mitm::applet::LogLine("   EnableInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsEnable();
            mitm::applet::LogLine("   usbDsEnable rc=0x%x", rc);
            if (R_FAILED(rc)) {
                mitm::applet::LogLine("   usbDsEnable failed - device will not appear on the bus");
                return;
            }

            /* M53: usbDsEnable returning 0x0 only means OUR side configured itself.
             * It says nothing about whether the HOST enumerated us - M52 logged
             * "USB DEVICE ENUMERATED" off this rc alone and that claim was wrong.
             * usbDsGetState reaching UsbState_Configured is the real signal: the
             * host has read our descriptors and selected a configuration.
             * Atmosphere's own haze uses exactly this test (usb_session.cpp:242).
             * Poll 10 s so a cable seated slightly late still counts. */
            static const char *const state_names[] = {
                "Detached", "Attached", "Powered", "Default", "Address", "Configured", "Suspended",
            };
            UsbState st = UsbState_Detached, best = UsbState_Detached;
            for (int i = 0; i < 100; ++i) {
                if (R_FAILED(usbDsGetState(std::addressof(st)))) { break; }
                if (static_cast<int>(st) > static_cast<int>(best)) { best = st; }
                if (st == UsbState_Configured) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
            const unsigned b = static_cast<unsigned>(best);
            mitm::applet::LogLine("   usb state high-water = %u (%s)", b,
                                  b < (sizeof(state_names) / sizeof(state_names[0])) ? state_names[b] : "?");
            /* M58: the number this whole milestone exists to produce. */
            {
                UsbDeviceSpeed neg = UsbDeviceSpeed_None;
                const ::Result srrc = usbDsGetSpeed(std::addressof(neg));
                static const char *const speed_names[] = {
                    "None", "Low(1.5Mbps)", "Full(12Mbps)", "High(480Mbps)", "SUPER(5Gbps)",
                };
                const unsigned sn = static_cast<unsigned>(neg);
                mitm::applet::LogLine("   *** NEGOTIATED SPEED = %u (%s) *** rc=0x%x", sn,
                                      sn < (sizeof(speed_names) / sizeof(speed_names[0])) ? speed_names[sn] : "?",
                                      srrc);
                mitm::applet::LogLine("   %s", neg == UsbDeviceSpeed_Super
                    ? "SuperSpeed: NV12 1080p60 (178 MiB/s) is within reach"
                    : "not SuperSpeed - check the CABLE is USB 3.0, and that the PC port is blue/SS");
            }
            mitm::applet::LogLine("   %s", best == UsbState_Configured
                ? "*** HOST CONFIGURED US - we own the bus, expect 1209:5f1e in lsusb ***"
                : "host did NOT configure us - bus owned by someone else, or no cable attached");
        }

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            mitm::applet::g_stats.sessions.fetch_add(1);

            mitm::applet::LogLine("OnNeedsToAccept port=%d program=%016llx", port_index,
                                  static_cast<unsigned long long>(client_info.program_id.value));

            switch (port_index) {
                case PortIndex_AppletMitm:
                    {
                        const Result r = this->AcceptMitmImpl(server,
                            sf::CreateSharedObjectEmplaced<mitm::applet::IViRootMitm, mitm::applet::ViRootMitm>(decltype(fsrv)(fsrv), client_info),
                            fsrv);
                        mitm::applet::LogLine("  AcceptMitmImpl rc=0x%x", r.GetValue());
                        R_RETURN(r);
                    }
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        Result NvServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            AMS_ABORT_UNLESS(port_index == NvPortIndex_Nvdrv);
            /* logged BEFORE accepting, so a failure inside the accept still
             * leaves the number that caused it on the SD card */
            mitm::applet::LogLine("nvdrv:s accept program=%016llx forward pointer_buffer_size=%#x (ours %#zx)",
                                  static_cast<unsigned long long>(client_info.program_id.value),
                                  static_cast<unsigned>(fsrv->pointer_buffer_size), NvServerOptions::PointerBufferSize);
            R_RETURN(this->AcceptMitmImpl(server,
                sf::CreateSharedObjectEmplaced<mitm::applet::INvDrvMitm, mitm::applet::NvDrvMitm>(decltype(fsrv)(fsrv), client_info),
                fsrv));
        }

    }

    namespace init {

        void InitializeSystemModule() {
            R_ABORT_UNLESS(sm::Initialize());
            fs::InitializeForSystem();
            fs::SetEnabledAutoAbort(false);

            /* pm:dmnt does NOT self-initialise: libstratosphere's
             * pm::dmnt::GetApplicationProcessId calls straight through to libnx's
             * pmdmntGetApplicationProcessId, and dmnt / dmnt.gen2 / ams_mitm all
             * call pmdmntInitialize() here. Deliberately NOT R_ABORT_UNLESS as
             * they do - a boot-time fatal is the one failure mode this module
             * must never have. Record it and let the debug route skip instead. */
            mitm::applet::g_pmdmnt_rc = ::pmdmntInitialize();

            ams::CheckApiVersion();
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void Main() {
        os::SetThreadNamePointer(os::GetCurrentThread(), "applet-mitm.Main");

        mitm::applet::LogInit();

        /* M72: first thing, before any other work. With mitm.lst present every
         * nvdrv:s client - vi among them - is blocked until this registers. The
         * arm file is read here only for the one flag ShouldMitm needs; without
         * "grc" we still register (mitm.lst obliges us to) but accept nobody. */
        mitm::applet::g_grc_armed = ArmFileContains("grc");
        StartGrcRecorder();

        /* M76: this line used to print jpg=off before jpg was parsed, and
         * called every build a "read-only observer". The flag dump below is
         * the record of what this boot armed. */
        mitm::applet::LogLine("applet-mitm M89: up (grc IPC interceptor %s)",
                              mitm::applet::g_grc_armed ? "ARMED" : "off");

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::g_dbg_armed   = ArmFileContains("dbg");
        mitm::applet::g_dump_armed  = ArmFileContains("dump");
        g_usb_armed                 = ArmFileContains("usb");
        mitm::applet::g_bench_armed   = ArmFileContains("bench");
        mitm::applet::g_nvenc_armed   = ArmFileContains("nvenc");
        mitm::applet::g_nvjpg_armed   = ArmFileContains("jpg");
        mitm::applet::g_nvjpg_quality = ArmFileNumber("q", 85);
        mitm::applet::g_nvjpg_attempts = ArmFileNumber("jat", 90);
        mitm::applet::g_sweep_armed   = ArmFileContains("sweep");
        mitm::applet::g_matrix_armed  = ArmFileContains("mtx");
        mitm::applet::g_grcscan_armed = ArmFileContains("grcscan");
        mitm::applet::g_clk_armed     = ArmFileContains("clk");
        mitm::applet::g_jpgdec_armed  = ArmFileContains("jpgdec");
        mitm::applet::g_nvgrc_armed   = ArmFileContains("nvgrc");
        mitm::applet::g_csc_armed     = ArmFileContains("csc");
        mitm::applet::g_nvframe_armed = ArmFileContains("nvframe");
        mitm::applet::g_nvframe_n     = ArmFileNumber("nvframe", 120);
        mitm::applet::g_nvstream_armed = ArmFileContains("nvstream");
        mitm::applet::g_nvstream_n    = ArmFileNumber("nvstream", 3600);
        mitm::applet::g_nvstream_qp   = ArmFileNumber("nvqp", 20);
        mitm::applet::g_nvstream_gop  = ArmFileNumber("nvgop", 0);
        mitm::applet::g_live_armed    = ArmFileContains("live");
        /* live is a mode of the H.264 stream: everything nvstream sets up, it needs */
        if (mitm::applet::g_live_armed) { mitm::applet::g_nvstream_armed = true; }
        mitm::applet::g_nvp_armed     = ArmFileContains("nvp");
        mitm::applet::g_nvp_n         = ArmFileNumber("nvp", 30);
        mitm::applet::g_matrix_mode   = ArmFileNumber("mtx", 1);
        mitm::applet::g_stream_armed  = ArmFileContains("stream");
        mitm::applet::g_stream_w      = ArmFileNumber("sw", 0);
        mitm::applet::g_stream_h      = ArmFileNumber("sh", 0);
        /* an explicit sw=/sh= overrides the link-speed picker; without one the
         * stream sizes itself to whatever the negotiated link can carry */
        mitm::applet::g_stream_auto   = (mitm::applet::g_stream_w == 0 ||
                                         mitm::applet::g_stream_h == 0);
        mitm::applet::g_stream_frames = ArmFileNumber("sframes", 600);
        mitm::applet::g_probe_delay_s = ArmFileNumber("wait", 120);
        mitm::applet::LogLine("ARMED FLAGS: vic=%d exec=%d dbg=%d dump=%d usb=%d bench=%d nvenc=%d "
                              "jpg=%d sweep=%d mtx=%d stream=%d grc=%d grcscan=%d clk=%d jpgdec=%d nvgrc=%d csc=%d nvframe=%d(%u) nvstream=%d(%u, qp %u, gop %u) nvp=%d(%u) live=%d wait=%u",
                              mitm::applet::g_vic_armed, mitm::applet::g_vic_execute,
                              mitm::applet::g_dbg_armed, mitm::applet::g_dump_armed,
                              g_usb_armed, mitm::applet::g_bench_armed,
                              mitm::applet::g_nvenc_armed, mitm::applet::g_nvjpg_armed,
                              mitm::applet::g_sweep_armed, mitm::applet::g_matrix_armed,
                              mitm::applet::g_stream_armed, mitm::applet::g_grc_armed,
                              mitm::applet::g_grcscan_armed, mitm::applet::g_clk_armed,
                              mitm::applet::g_jpgdec_armed, mitm::applet::g_nvgrc_armed,
                              mitm::applet::g_csc_armed, mitm::applet::g_nvframe_armed, mitm::applet::g_nvframe_n,
                              mitm::applet::g_nvstream_armed, mitm::applet::g_nvstream_n, mitm::applet::g_nvstream_qp,
                              mitm::applet::g_nvstream_gop,
                              mitm::applet::g_nvp_armed, mitm::applet::g_nvp_n,
                              mitm::applet::g_live_armed,
                              mitm::applet::g_probe_delay_s);
        if ((mitm::applet::g_jpgdec_armed || mitm::applet::g_nvgrc_armed ||
             mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_armed) {
            mitm::applet::LogLine("jpgdec/nvgrc/csc/nvframe/nvstream armed without vic: they run inside the VIC worker, so they will NOT run");
        }
        if ((mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed || mitm::applet::g_nvp_armed) && !mitm::applet::g_dbg_armed) {
            mitm::applet::LogLine("nvframe/nvstream/nvp armed without dbg: they run inside the debug capture, so they will NOT run");
        }
        if ((mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_execute) {
            mitm::applet::LogLine("csc/nvframe/nvstream/nvp armed without exec: the VIC buffers are never mapped, so they will NOT run");
        }
        if (mitm::applet::g_nvstream_armed && !g_usb_armed) {
            mitm::applet::LogLine("nvstream armed without usb: the USB device is never brought up, so it will NOT stream");
        }
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
        if (mitm::applet::g_stream_armed) {
            if (mitm::applet::g_stream_auto) {
                mitm::applet::LogLine("stream ARMED: size AUTO (from the negotiated link speed), %u frames",
                                      mitm::applet::g_stream_frames);
            } else {
                mitm::applet::LogLine("stream ARMED: %ux%u packed-420, %u frames (%u B/frame); 60 fps needs <=16667 us/frame",
                                      mitm::applet::g_stream_w, mitm::applet::g_stream_h,
                                      mitm::applet::g_stream_frames,
                                      mitm::applet::g_stream_w * mitm::applet::g_stream_h * 3 / 2);
            }
        }
        LogMemoryPools();
        TryUsbEnumerate();

        mitm::applet::LogLine("probe fires at t=%u s; frame dump to SD: %s",
                              mitm::applet::g_probe_delay_s,
                              mitm::applet::g_dump_armed ? "ARMED (freezes the game ~0.3-2.5 s)" : "off");
        mitm::applet::LogLine("debug-capture route: %s   pmdmntInitialize rc=0x%x",
                              mitm::applet::g_dbg_armed ? "ARMED" : "off (add \"dbg\" to the arm file)",
                              mitm::applet::g_pmdmnt_rc);

        /* start the heartbeat before anything that can block */
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_hb_thread), HeartbeatThread, nullptr,
                                        g_hb_stack, sizeof(g_hb_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_hb_thread), "applet-mitm.HB");
        os::StartThread(std::addressof(g_hb_thread));
        mitm::applet::LogMark("main:heartbeat_started");

        mitm::applet::StartVicWorker();
        /* M76: its own thread, so an mm:u or clkrst call that blocks can never
         * hold up vi:u registration below. Holds the clocks past the survey
         * only when an engine probe in this run needs them. */
        mitm::applet::StartClockProbe(mitm::applet::g_jpgdec_armed || mitm::applet::g_nvenc_armed ||
                                      mitm::applet::g_nvjpg_armed || mitm::applet::g_nvgrc_armed ||
                                      mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
                                      mitm::applet::g_nvp_armed);

        R_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");

        mitm::applet::LogMark("main:LoopProcess");
        g_server_manager.LoopProcess();
        mitm::applet::LogMark("main:LoopProcess_RETURNED");
    }

}

/* ---- M57: USB bulk transport ------------------------------------------------
 * Defined out here, not in the anonymous namespace above, because
 * applet_mitm_nv.cpp calls these and internal linkage would not reach it.
 * The anonymous namespace's members are still visible from this block: an
 * unnamed namespace inside `namespace ams` injects its names into `ams`, so
 * ::ams::g_usb_ep_in resolves within this translation unit.
 *
 * Every usbDs and event call returns libnx's ::Result - a bare u32, NOT
 * ams::Result. Calling .GetValue() on one does not compile, which has already
 * cost this project a build cycle once. They are printed with %x directly.
 *
 * The sequence follows Atmosphere's own haze (troposphere/haze/source/
 * usb_session.cpp:250,256-258): PostBufferAsync -> wait CompletionEvent ->
 * eventClear -> GetReportData -> ParseReportData. */
namespace ams::mitm::applet {

    /* M71: the negotiated link speed, asked of the kernel rather than
     * assumed. The stream picks its resolution from this: SuperSpeed lifts
     * the ~37 MB/s wall M70 measured, and there is no point aiming a
     * 129 MB/s frame rate at a High-Speed link. */
    bool UsbIsSuperSpeed() {
        UsbDeviceSpeed sp = UsbDeviceSpeed_None;
        if (R_FAILED(usbDsGetSpeed(std::addressof(sp)))) { return false; }
        return sp == UsbDeviceSpeed_Super;
    }

    void UsbCancelIn();

    bool UsbViewerPresent(u32 timeout_ms) {
        if (!UsbReady()) { return false; }
        alignas(0x1000) static u8 hello[0x1000];
        const u32 h[8] = { 0x52544653u, 2u, 0, 0, 0, 0, 0, 0 };   /* "SFTR", v2, all else 0: length 0 */
        std::memcpy(hello, h, sizeof(h));
        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in, hello, sizeof(h), std::addressof(urb)))) { return false; }
        if (R_FAILED(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), static_cast<u64>(timeout_ms) * 1000000))) {
            UsbCancelIn();
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
        UsbDsReportData report = {};
        u32 transferred = 0;
        if (R_FAILED(usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report)))) { return false; }
        if (R_FAILED(usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred)))) { return false; }
        return transferred == sizeof(h);
    }

    bool UsbReady() {
        if (::ams::g_usb_ep_in == nullptr) { return false; }
        UsbState st = UsbState_Detached;
        if (R_FAILED(usbDsGetState(std::addressof(st)))) { return false; }
        return st == UsbState_Configured;
    }

    bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb) {
        if (out_urb != nullptr) { *out_urb = 0; }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        u32 urb_id = 0;
        const ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                                const_cast<void *>(buf), static_cast<u32>(len),
                                std::addressof(urb_id));
        if (R_FAILED(rc)) {
            LogLine("   usb: PostBufferAsync(%zu B) rc=0x%x", len, rc);
            return false;
        }
        if (out_urb != nullptr) { *out_urb = urb_id; }
        return true;
    }

    /* M86: a transfer the host never takes stays posted. Left there, its
     * bytes go to whichever program reads the endpoint next - a new viewer
     * would start mid-frame. Cancel it and let the cancellation complete
     * (SysDVR's UsbComms.c does the same). */
    void UsbCancelIn() {
        if (::ams::g_usb_ep_in == nullptr) { return; }
        static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_in));
        static_cast<void>(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), UINT64_C(1000000000)));
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
    }

    bool UsbWaitAsync(u32 urb, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        ::Result rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
        if (R_FAILED(rc)) {
            LogLine("   usb: async completion timed out rc=0x%x (host not draining?) - cancelled", rc);
            UsbCancelIn();
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

        UsbDsReportData report = {};
        rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
        if (R_FAILED(rc)) { LogLine("   usb: async GetReportData rc=0x%x", rc); return false; }

        u32 transferred = 0;
        rc = usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred));
        if (R_FAILED(rc)) { LogLine("   usb: async ParseReportData rc=0x%x", rc); return false; }

        if (out_sent != nullptr) { *out_sent = transferred; }
        return transferred > 0;
    }

    bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        const u8 *p = static_cast<const u8 *>(buf);
        size_t done = 0;

        while (done < len) {
            const size_t chunk = ((len - done) < ::ams::UsbChunk) ? (len - done) : ::ams::UsbChunk;

            u32 urb_id = 0;
            ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                              const_cast<u8 *>(p + done), static_cast<u32>(chunk),
                              std::addressof(urb_id));
            if (R_FAILED(rc)) {
                LogLine("   usb: PostBufferAsync(+%zu, %zu B) rc=0x%x", done, chunk, rc);
                return false;
            }

            rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
            if (R_FAILED(rc)) {
                LogLine("   usb: completion wait timed out at +%zu rc=0x%x (host not reading?) - cancelled", done, rc);
                UsbCancelIn();
                return false;
            }
            eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

            UsbDsReportData report = {};
            rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
            if (R_FAILED(rc)) { LogLine("   usb: GetReportData rc=0x%x", rc); return false; }

            u32 transferred = 0;
            rc = usbDsParseReportData(std::addressof(report), urb_id, nullptr, std::addressof(transferred));
            if (R_FAILED(rc)) { LogLine("   usb: ParseReportData rc=0x%x", rc); return false; }

            if (transferred == 0) {
                LogLine("   usb: zero-length completion at +%zu - host is not draining", done);
                return false;
            }
            if (transferred != chunk) {
                /* A short completion would push the next post off 0x1000
                 * alignment, which usbDs rejects. Report it rather than send
                 * a silently corrupt frame. */
                done += transferred;
                if (out_sent != nullptr) { *out_sent = done; }
                LogLine("   usb: short completion %u/%zu at +%zu - stopping (would break alignment)",
                        transferred, chunk, done - transferred);
                return false;
            }

            done += transferred;
            if (out_sent != nullptr) { *out_sent = done; }
        }
        return true;
    }

}
