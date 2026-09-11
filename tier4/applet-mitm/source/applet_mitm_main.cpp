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
            }
        }

        /* ---- opt-in arm file ----------------------------------------------
         * Nothing touches nvdrv/VIC unless sdmc:/applet-mitm.armed exists and
         * contains "vic". Default is a pure observer, i.e. M7d behaviour. */
        bool ArmFileContains(const char *keyword) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/applet-mitm.armed", fs::OpenMode_Read))) {
                return false;
            }
            char buf[256] = {};
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return false; }
            size_t n = static_cast<size_t>(fsz);
            if (n > sizeof(buf) - 1) { n = sizeof(buf) - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return false; }
            buf[n] = '\0';
            return std::strstr(buf, keyword) != nullptr;
        }

        /* Numeric option out of the arm file: "wait=180" delays the probe so
         * there is time to actually get into a race before it fires. M39 probed
         * at ~50 s, which is still the title screen. */
        u32 ArmFileNumber(const char *key, u32 def) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/applet-mitm.armed", fs::OpenMode_Read))) {
                return def;
            }
            char buf[256] = {};
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return def; }
            size_t n = static_cast<size_t>(fsz);
            if (n > sizeof(buf) - 1) { n = sizeof(buf) - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return def; }
            buf[n] = '\0';

            const char *p = std::strstr(buf, key);
            if (p == nullptr) { return def; }
            p += std::strlen(key);
            while (*p == '=' || *p == ' ') { ++p; }
            u32 v = 0;
            bool any = false;
            while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<u32>(*p - '0'); ++p; any = true; }
            return any ? v : def;
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
            mitm::applet::LogLine("   device descriptors (Full+High) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            UsbDsInterface *iface  = nullptr;
            UsbDsEndpoint  *ep_in  = nullptr;
            UsbDsEndpoint  *ep_out = nullptr;

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
            mitm::applet::LogLine("   configuration descriptors rc=0x%x", rc);
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
        mitm::applet::LogLine("applet-mitm M54: up. pool_partition 1 (Applet) - the heap must not come out of System.");

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::g_dbg_armed   = ArmFileContains("dbg");
        mitm::applet::g_dump_armed  = ArmFileContains("dump");
        g_usb_armed                 = ArmFileContains("usb");
        mitm::applet::g_probe_delay_s = ArmFileNumber("wait", 120);
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
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

        R_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");

        mitm::applet::LogMark("main:LoopProcess");
        g_server_manager.LoopProcess();
        mitm::applet::LogMark("main:LoopProcess_RETURNED");
    }

}
