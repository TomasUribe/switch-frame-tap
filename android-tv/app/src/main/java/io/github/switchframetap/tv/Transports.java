/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;

final class Transports {
    static final int VID = 0x1209, PID = 0x5f1e, PORT = 9950, BEACON = 9951;
    static final class Tcp implements SftProtocol.Source {
        private final Socket socket = new Socket();
        private InputStream input;
        void connect(String host, int port) throws IOException {
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(250);
            socket.connect(new InetSocketAddress(host, port), 900);
            input = socket.getInputStream();
        }
        @Override public int read(byte[] data, int offset, int length) throws IOException {
            try { return input.read(data, offset, length); }
            catch (SocketTimeoutException e) { return 0; }
        }
        @Override public void close() throws IOException { socket.close(); }
    }
    static UsbDevice find(UsbManager manager) {
        for (UsbDevice d : manager.getDeviceList().values())
            if (d.getVendorId() == VID && d.getProductId() == PID) return d;
        return null;
    }
    static final class Usb implements SftProtocol.Source {
        private final UsbManager manager;
        private final String deviceName;
        private final UsbDeviceConnection connection;
        private final UsbInterface iface;
        private final UsbEndpoint endpoint;
        private volatile boolean closed;
        Usb(UsbManager manager, UsbDevice device) throws IOException {
            this.manager = manager; deviceName = device.getDeviceName();
            UsbInterface foundInterface = null;
            UsbEndpoint foundEndpoint = null;
            for (int i = 0; i < device.getInterfaceCount(); i++) {
                UsbInterface candidate = device.getInterface(i);
                if (candidate.getId() != 0) continue;
                for (int j = 0; j < candidate.getEndpointCount(); j++) {
                    UsbEndpoint ep = candidate.getEndpoint(j);
                    if (ep.getAddress() == 0x81 && ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK
                            && ep.getDirection() == UsbConstants.USB_DIR_IN) {
                        foundInterface = candidate; foundEndpoint = ep;
                    }
                }
            }
            if (foundEndpoint == null) throw new IOException("Select USB viewer on the Switch; webcam mode is not supported");
            iface = foundInterface; endpoint = foundEndpoint;
            connection = manager.openDevice(device);
            if (connection == null) throw new IOException("Could not open the USB device; check the permission");
            if (!connection.claimInterface(iface, true)) {
                connection.close(); throw new IOException("The USB interface is busy");
            }
        }
        @Override public int read(byte[] data, int offset, int length) throws IOException {
            if (closed) return -1;
            // 16 KiB works on Android 6/7/8 as well as current Android.
            int n = connection.bulkTransfer(endpoint, data, offset, Math.min(length, 16384), 200);
            if (closed) return -1;
            if (n < 0) {
                if (!manager.getDeviceList().containsKey(deviceName)) return -1;
                return 0; // idle; the protocol parser enforces a deadline on partial packets
            }
            return n;
        }
        @Override public void close() {
            if (closed) return;
            closed = true;
            connection.close(); // releases the claimed interface and cancels outstanding reads
        }
    }
}
