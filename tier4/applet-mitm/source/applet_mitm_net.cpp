/*
 * v0.7 network transport - see applet_mitm_net.hpp. Socket setup after
 * SysDVR's sysmodule (GPL-2.0, sysmodule/source/net/sockets.c).
 */
#include "applet_mitm_net.hpp"
#include "applet_mitm_log.hpp"
/* the umbrella <switch.h> wraps libnx in extern "C"; the single headers do not */
extern "C" {
#include <switch/services/bsd.h>
#include <switch/services/nifm.h>
}
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>

namespace ams::mitm::applet {

    constinit bool g_net_armed = false;
    constinit std::atomic<bool> g_tx_net{false};

    namespace {

        constexpr u16 NetPort = 9950;            /* the stream (TCP) */
        constexpr u16 BeaconPort = 9951;         /* "here I am" (UDP broadcast) */

        /* SysDVR's sizes: a 16 KB TCP send buffer that may grow to 200 KB.
         * transfer memory = (tx max + rx + udp tx + udp rx) x efficiency */
        constexpr u32 TcpTx = 16 * 1024, TcpTxMax = 200 * 1024, TcpRx = 8 * 1024;
        constexpr u32 UdpTx = 8 * 1024, UdpRx = 4 * 1024, Efficiency = 2;
        constexpr size_t TmemSize = ((TcpTxMax + TcpRx + UdpTx + UdpRx + 0xFFF) & ~size_t(0xFFF)) * Efficiency;
        alignas(0x1000) constinit u8 g_tmem[TmemSize] = {};

        constinit std::atomic<int> g_client{-1};
        constinit std::atomic<u32> g_ip{0};
        constinit u32 g_client_ip = 0;

        /* the sender: one outstanding payload, as on USB */
        constinit os::SdkMutex g_tx_lock;
        constinit os::SdkConditionVariable g_tx_cv;
        constinit const u8 *g_tx_buf = nullptr;
        constinit size_t g_tx_len = 0, g_tx_done = 0;
        constinit bool g_tx_busy = false, g_tx_ok = true;

        constinit std::atomic<u64> s_bytes{0};
        constinit std::atomic<u64> s_busy_ns{0};     /* time spent inside sends */
        constinit u64 s_busy_seen = 0, s_busy_t0 = 0;
        constinit std::atomic<u32> s_clients{0}, s_send_fail{0};

        alignas(os::ThreadStackAlignment) constinit u8 g_net_stack[16_KB];
        alignas(os::ThreadStackAlignment) constinit u8 g_send_stack[16_KB];
        constinit os::ThreadType g_net_thread, g_send_thread;

        void IpString(u32 ip, char *out, size_t cap) {
            const u8 *b = reinterpret_cast<const u8 *>(&ip);   /* network order */
            std::snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }

        /* all of len, or false (the client is gone) */
        bool SendAll(int s, const u8 *p, size_t len) {
            while (len > 0) {
                const size_t chunk = len > 0x40000 ? 0x40000 : len;
                const ssize_t n = bsdSend(s, p, chunk, 0);
                if (n <= 0) { return false; }
                p += n; len -= static_cast<size_t>(n);
                s_bytes += static_cast<u64>(n);
            }
            return true;
        }

        void SendThread(void *) {
            for (;;) {
                const u8 *buf; size_t len;
                {
                    std::scoped_lock lk(g_tx_lock);
                    while (g_tx_buf == nullptr) { g_tx_cv.Wait(g_tx_lock); }
                    buf = g_tx_buf; len = g_tx_len;
                }
                const int s = g_client.load();
                const u64 t0 = armTicksToNs(armGetSystemTick());
                const bool ok = s >= 0 && SendAll(s, buf, len);
                s_busy_ns += armTicksToNs(armGetSystemTick()) - t0;
                {
                    std::scoped_lock lk(g_tx_lock);
                    g_tx_ok = ok;
                    g_tx_done = ok ? len : 0;
                    g_tx_buf = nullptr;
                    g_tx_busy = false;
                    g_tx_cv.Broadcast();
                }
            }
        }

        int Listen() {
            const int s = bsdSocket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { return -1; }
            const int one = 1;
            bsdSetSockOpt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = INADDR_ANY;
            a.sin_port = htons(NetPort);
            if (bsdBind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || bsdListen(s, 1) < 0) {
                LogLine("net: cannot listen on %u (errno %d)", NetPort, g_bsdErrno);
                bsdClose(s);
                return -1;
            }
            return s;
        }

        /* Run AN: the viewer never heard the 255.255.255.255 beacon on a
         * Wi-Fi LAN (many access points filter broadcasts between wireless
         * clients). Also the subnet's own broadcast address; the viewer
         * additionally scans the subnet and remembers the last address. */
        void Beacon(int u, u32 ip, u32 mask) {
            static u32 logged = 0;
            char ips[20], msg[96];
            IpString(ip, ips, sizeof(ips));
            const int n = std::snprintf(msg, sizeof(msg), "SFTAP1 %s %u Switch Frame Tap", ips, NetPort);
            const u32 dests[2] = { 0xFFFFFFFFu, mask ? ((ip & mask) | ~mask) : 0 };
            for (u32 k = 0; k < 2; ++k) {
                if (dests[k] == 0) { continue; }
                sockaddr_in a = {};
                a.sin_family = AF_INET;
                a.sin_port = htons(BeaconPort);
                a.sin_addr.s_addr = dests[k];
                const ssize_t r = bsdSendTo(u, msg, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
                if (logged < 4) {
                    char ds[20];
                    IpString(dests[k], ds, sizeof(ds));
                    LogLine("net: beacon to %s -> %d (errno %d)", ds, static_cast<int>(r), r < 0 ? g_bsdErrno : 0);
                    ++logged;
                }
            }
        }

        void NetThread(void *) {
            /* wait for a network: nifm reports an IP once the console is online */
            ::Result rc = nifmInitialize(NifmServiceType_User);
            if (R_FAILED(rc)) { LogLine("net: nifmInitialize rc=0x%x - no network transport", rc); return; }
            const BsdInitConfig cfg = {
                .version = 1,
                .tmem_buffer = g_tmem, .tmem_buffer_size = TmemSize,
                .tcp_tx_buf_size = TcpTx, .tcp_rx_buf_size = TcpRx,
                .tcp_tx_buf_max_size = TcpTxMax, .tcp_rx_buf_max_size = 0,
                .udp_tx_buf_size = UdpTx, .udp_rx_buf_size = UdpRx,
                .sb_efficiency = Efficiency,
            };
            rc = bsdInitialize(&cfg, 3, BsdServiceType_User);
            LogLine("net: bsd:u with %zu KB of transfer memory rc=0x%x", TmemSize / 1024, rc);
            if (R_FAILED(rc)) { return; }

            int ls = -1, us = -1;
            u32 last_ip = 0;
            u32 tick = 0;
            for (;; ++tick) {
                u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
                if (R_FAILED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2))) {
                    mask = 0;
                    if (R_FAILED(nifmGetCurrentIpAddress(&ip))) { ip = 0; }
                }
                g_ip = ip;
                if (ip != last_ip) {
                    char s[20];
                    IpString(ip, s, sizeof(s));
                    LogLine("net: %s%s", ip ? "online at " : "offline", ip ? s : "");
                    /* a new address (or a wake from sleep): new sockets */
                    if (ls >= 0) { bsdClose(ls); ls = -1; }
                    if (us >= 0) { bsdClose(us); us = -1; }
                    last_ip = ip;
                }
                if (ip == 0) { os::SleepThread(TimeSpan::FromMilliSeconds(1000)); continue; }
                if (ls < 0) {
                    ls = Listen();
                    if (ls < 0) { os::SleepThread(TimeSpan::FromMilliSeconds(1000)); continue; }
                    LogLine("net: listening on TCP %u, beacon on UDP %u", NetPort, BeaconPort);
                }
                if (us < 0) {
                    us = bsdSocket(AF_INET, SOCK_DGRAM, 0);
                    const int one = 1;
                    if (us >= 0) { bsdSetSockOpt(us, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)); }
                }
                if (const int c = g_client.load(); c >= 0) {
                    /* one viewer at a time. The viewer never sends anything,
                     * so its socket turning readable means it closed. */
                    pollfd cp = { c, POLLIN, 0 };
                    if (bsdPoll(&cp, 1, 250) > 0 && (cp.revents & (POLLIN | POLLHUP | POLLERR))) {
                        u8 junk[64];
                        if (bsdRecv(c, junk, sizeof(junk), 0) <= 0) { NetDropClient("the viewer closed the connection"); }
                    }
                    continue;
                }
                if (us >= 0 && tick % 4 == 0) { Beacon(us, ip, mask); }
                pollfd pf = { ls, POLLIN, 0 };
                const int pr = bsdPoll(&pf, 1, 250);
                if (pr < 0) { bsdClose(ls); ls = -1; continue; }
                if (pr == 0) { continue; }
                sockaddr_in from = {};
                socklen_t fl = sizeof(from);
                const int c = bsdAccept(ls, reinterpret_cast<sockaddr *>(&from), &fl);
                if (c < 0) {
                    /* after sleep, accept fails while poll says ready: start over */
                    LogLine("net: accept failed (errno %d) - new listener", g_bsdErrno);
                    bsdClose(ls); ls = -1;
                    continue;
                }
                const int one = 1, sndbuf = static_cast<int>(TcpTxMax);
                bsdSetSockOpt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                bsdSetSockOpt(c, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                g_client_ip = from.sin_addr.s_addr;
                char s[20];
                IpString(g_client_ip, s, sizeof(s));
                ++s_clients;
                LogLine("net: viewer connected from %s", s);
                g_client = c;
            }
        }

    }

    void StartNet() {
        if (!g_net_armed) { return; }
        R_ABORT_UNLESS(os::CreateThread(&g_net_thread, NetThread, nullptr, g_net_stack, sizeof(g_net_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(&g_net_thread, "applet-mitm.Net");
        os::StartThread(&g_net_thread);
        R_ABORT_UNLESS(os::CreateThread(&g_send_thread, SendThread, nullptr, g_send_stack, sizeof(g_send_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(&g_send_thread, "applet-mitm.NetSend");
        os::StartThread(&g_send_thread);
    }

    bool NetClientPresent() { return g_net_armed && g_client.load() >= 0; }

    u32 NetIp() { return g_ip.load(); }

    bool NetSendBuffer(const void *buf, size_t len) {
        const int s = g_client.load();
        if (s < 0) { return false; }
        /* never interleave with a payload the sender is still writing */
        {
            std::scoped_lock lk(g_tx_lock);
            while (g_tx_busy) { g_tx_cv.Wait(g_tx_lock); }
        }
        const u64 t0 = armTicksToNs(armGetSystemTick());
        const bool ok = SendAll(s, static_cast<const u8 *>(buf), len);
        s_busy_ns += armTicksToNs(armGetSystemTick()) - t0;
        if (!ok) { ++s_send_fail; return false; }
        return true;
    }

    bool NetPostAsync(const void *buf, size_t len) {
        if (g_client.load() < 0) { return false; }
        std::scoped_lock lk(g_tx_lock);
        while (g_tx_busy) { g_tx_cv.Wait(g_tx_lock); }
        g_tx_buf = static_cast<const u8 *>(buf);
        g_tx_len = len;
        g_tx_busy = true;
        g_tx_cv.Broadcast();
        return true;
    }

    bool NetWaitAsync(size_t *out_sent) {
        std::scoped_lock lk(g_tx_lock);
        while (g_tx_busy) { g_tx_cv.Wait(g_tx_lock); }
        if (out_sent) { *out_sent = g_tx_done; }
        if (!g_tx_ok) { ++s_send_fail; }
        return g_tx_ok;
    }

    void NetDropClient(const char *why) {
        {
            std::scoped_lock lk(g_tx_lock);
            while (g_tx_busy) { g_tx_cv.Wait(g_tx_lock); }
        }
        const int s = g_client.exchange(-1);
        if (s >= 0) {
            bsdClose(s);
            LogLine("net: viewer disconnected (%s)", why);
        }
    }

    u32 NetBusyPercent() {
        const u64 now = armTicksToNs(armGetSystemTick()), busy = s_busy_ns.load();
        const u64 dt = now - s_busy_t0, db = busy - s_busy_seen;
        s_busy_t0 = now;
        s_busy_seen = busy;
        return dt ? static_cast<u32>(db * 100 / dt > 100 ? 100 : db * 100 / dt) : 0;
    }

    void NetLogStats(const char *who) {
        LogLine("   %s: network - %llu MB sent, %u viewers connected this boot, %u send failures", who,
                static_cast<unsigned long long>(s_bytes.load() / (1024 * 1024)), s_clients.load(), s_send_fail.load());
    }

}
