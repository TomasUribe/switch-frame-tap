/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.view.Surface;
import android.util.Log;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;

final class StreamEngine {
    interface Listener {
        void status(String message);
        void usbPermission(UsbDevice device);
        void connectedAddress(String ip);
    }
    final StreamStats stats = new StreamStats();
    private final Context context;
    private final Surface surface;
    private final boolean usb, stableAudio;
    private final String address;
    private String rememberedAddress;
    private final Listener listener;
    private volatile boolean stopped, muted;
    private volatile SftProtocol.Source source;
    private volatile DatagramSocket discovery;
    private volatile AudioPump audio;
    private volatile VideoDecoder decoder;
    private final Thread thread;
    StreamEngine(Context context, Surface surface, boolean usb, String address, String lastAddress,
                 boolean stableAudio, boolean muted, Listener listener) {
        this.context = context.getApplicationContext(); this.surface = surface;
        this.usb = usb; this.address = address;
        this.rememberedAddress = lastAddress;
        this.stableAudio = stableAudio; this.muted = muted; this.listener = listener;
        thread = new Thread(this::run, "sft-receiver");
    }
    void start() { thread.start(); }
    void mute(boolean value) { muted = value; AudioPump a = audio; if (a != null) a.mute(value); }
    void stop() {
        stopped = true; thread.interrupt();
        VideoDecoder v = decoder; if (v != null) v.requestStop();
        AudioPump a = audio; if (a != null) a.requestStop();
        DatagramSocket d = discovery; if (d != null) d.close();
        closeSource();
    }
    private void closeSource() {
        SftProtocol.Source s = source;
        if (s != null) try { s.close(); } catch (IOException ignored) { }
    }
    private void status(String message) { if (!stopped) listener.status(message); }
    private void run() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY);
        try {
            while (!stopped) {
                VideoDecoder video = null;
                AudioPump a = null;
                try {
                    SftProtocol.Source connected = usb ? usbConnect() : networkConnect();
                    if (connected == null || stopped) break;
                    source = connected;
                    VideoQueue queue = new VideoQueue();
                    a = new AudioPump(stats, stableAudio); audio = a; a.mute(muted); a.start();
                    video = new VideoDecoder(surface, queue, stats, this::status); decoder = video; video.start();
                    if (stopped) break;
                    status("Connected. Open a game on the Switch…");
                    while (!stopped) {
                        SftProtocol.Packet packet = SftProtocol.readPacket(connected, () -> stopped);
                        stats.bytes += packet.data.length + 32;
                        if (packet.audio()) {
                            stats.audioPackets++; stats.audioBytes += packet.data.length;
                            a.offer(packet);
                        }
                        else {
                            stats.received++; stats.lastVideoNs = packet.receivedNs;
                            queue.offer(packet);
                            stats.videoResets = queue.resets();
                            stats.videoDropped = queue.dropped();
                            // Video IDR recovery must not repeatedly flush the audio sink.
                        }
                    }
                } catch (IOException | RuntimeException e) {
                    if (!stopped) {
                        Log.w("SFT-TV", "Transport", e);
                        status(e.getMessage() + ". Reconnecting…");
                    }
                } finally {
                    closeSource(); source = null;
                    // Cleanup must finish even when stop() interrupted the receiver.
                    Thread.interrupted();
                    if (video != null) video.close();
                    if (a != null) a.close();
                    audio = null;
                    decoder = null;
                    stats.lastRenderedNs = 0; stats.audioQueuedMs = 0;
                }
                pause(800);
            }
        } finally { closeSource(); }
    }
    private SftProtocol.Source usbConnect() throws IOException {
        if (!context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_USB_HOST)) {
            status("This device does not support USB host mode. Use Network instead.");
            while (!stopped) pause(500);
            return null;
        }
        UsbManager manager = (UsbManager)context.getSystemService(Context.USB_SERVICE);
        Set<String> requested = new HashSet<>();
        while (!stopped) {
            UsbDevice device = Transports.find(manager);
            if (device == null) {
                requested.clear();
                status("Waiting for the Switch over USB. Use USB viewer mode and a data cable.");
            } else if (!manager.hasPermission(device)) {
                status("Allow USB access. If permission was denied, select Connect to try again.");
                if (requested.add(device.getDeviceName())) listener.usbPermission(device);
            } else {
                source = new Transports.Usb(manager, device);
                if (stopped) { closeSource(); return null; }
                stats.transport = "USB";
                return source;
            }
            pause(500);
        }
        return null;
    }
    private Transports.Tcp tcp(String host, int port) throws IOException {
        Transports.Tcp tcp = new Transports.Tcp(); source = tcp;
        try {
            if (stopped) throw new IOException("Connection stopped");
            tcp.connect(host, port);
            if (stopped) throw new IOException("Connection stopped");
            stats.transport = "Network · " + host;
            rememberedAddress = host;
            listener.connectedAddress(host);
            return tcp;
        } catch (IOException e) { tcp.close(); source = null; throw e; }
    }
    private SftProtocol.Source networkConnect() throws IOException {
        if (!address.isEmpty()) {
            status("Connecting to " + address + ":9950…");
            return tcp(address, Transports.PORT);
        }
        String recent = rememberedAddress;
        DatagramSocket beacon = new DatagramSocket(null);
        discovery = beacon;
        try {
            beacon.setReuseAddress(true);
            beacon.bind(new InetSocketAddress(Transports.BEACON));
            beacon.setSoTimeout(400);
            long nextTry = 0;
            while (!stopped) {
                if (SftProtocol.ipv4(recent) && System.nanoTime() >= nextTry) {
                    try { return tcp(recent, Transports.PORT); }
                    catch (IOException e) { nextTry = System.nanoTime() + 3_000_000_000L; }
                }
                status("Searching for the Switch on the network. Enter its IP address if it is not found.");
                byte[] bytes = new byte[256];
                DatagramPacket packet = new DatagramPacket(bytes, bytes.length);
                try {
                    beacon.receive(packet);
                    int port = SftProtocol.beaconPort(new String(bytes, 0, packet.getLength(), StandardCharsets.US_ASCII));
                    if (port != Transports.PORT || !(packet.getAddress() instanceof Inet4Address)) continue;
                    recent = packet.getAddress().getHostAddress(); // trust the sender address, not the beacon's IP
                    try { return tcp(recent, port); } catch (IOException ignored) { }
                } catch (SocketTimeoutException ignored) { }
            }
        } finally { beacon.close(); discovery = null; }
        return null;
    }
    private void pause(int ms) {
        if (stopped) return;
        try { Thread.sleep(ms); } catch (InterruptedException ignored) { }
    }
}
