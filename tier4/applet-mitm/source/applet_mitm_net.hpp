/*
 * v0.7: the network transport - the stream over the LAN (Wi-Fi or the dock's
 * Ethernet), so a docked console streams too: docked, the dock owns the USB
 * port.
 *
 * With `network = 1` in config.ini (token "net") the module listens on TCP
 * port 9950 and, while no viewer is connected, broadcasts a small UDP beacon
 * on port 9951 once a second so the PC viewer finds the console by itself.
 * A connected viewer gets exactly what USB carries - SFTR headers, H.264 and
 * game audio - and takes priority over USB while it is connected.
 *
 * Sockets are set up the way SysDVR's sysmodule does it (GPL-2.0,
 * sysmodule/source/net/sockets.c): bsd:u with a static transfer-memory block
 * instead of libnx's default (which wants several MB a sysmodule does not
 * have), and the listener recreated when the console wakes from sleep.
 * Frames are sent by their own thread, so a slow network never stalls the
 * capture; the stream waits for the previous frame as it does on USB.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern bool g_net_armed;                     /* "net": the network transport is on */
    extern std::atomic<bool> g_tx_net;           /* this stream goes to the network client */

    /* starts the network thread (waits for the console to be online) */
    void StartNet();

    /* a viewer is connected over the network */
    bool NetClientPresent();

    /* the console's IP (0 = offline), for the log / status */
    u32 NetIp();

    /* the transport functions, as the USB ones (applet_mitm_log.hpp) */
    bool NetSendBuffer(const void *buf, size_t len);
    bool NetPostAsync(const void *buf, size_t len);
    bool NetWaitAsync(size_t *out_sent);

    /* the stream ended because of us or the client: drop the connection */
    void NetDropClient(const char *why);

    void NetLogStats(const char *who);

    /* v0.7 rate control: how much of the time since the last call the
     * sender spent writing to the network, in percent. Near 100 = the link
     * is full and frames are queueing. */
    u32 NetBusyPercent();

}
