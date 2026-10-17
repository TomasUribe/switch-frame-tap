/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import java.util.ArrayDeque;

/** Never discard a P frame then decode its dependants. Recover at an IDR. */
final class VideoQueue {
    private final ArrayDeque<SftProtocol.Packet> queue = new ArrayDeque<>();
    private boolean waitingKey = true, closed;
    private int bytes;
    private long lastSequence = -1;
    private int width, height;
    private long resets, dropped;
    synchronized void offer(SftProtocol.Packet p) {
        if (closed) return;
        boolean changed = width != p.width || height != p.height;
        // Gaps, session restarts and changed SPS/resolution all invalidate references.
        boolean gap = lastSequence >= 0 && p.sequence != ((lastSequence + 1) & 0xffffffffL);
        lastSequence = p.sequence; width = p.width; height = p.height;
        boolean late = !queue.isEmpty() && p.receivedNs - queue.peekFirst().receivedNs > 100_000_000L;
        if (gap || changed || late || queue.size() >= 6 || bytes + p.data.length > 16 * 1024 * 1024) {
            dropped += queue.size(); queue.clear(); bytes = 0; waitingKey = true; resets++;
        }
        if (waitingKey) {
            if (!p.key()) { dropped++; return; }
            p.discontinuity = true; waitingKey = false;
        }
        queue.addLast(p); bytes += p.data.length; notifyAll();
    }
    synchronized SftProtocol.Packet poll(long timeoutMs) throws InterruptedException {
        if (queue.isEmpty() && !closed) wait(timeoutMs);
        SftProtocol.Packet p = queue.pollFirst();
        if (p != null) bytes -= p.data.length;
        return p;
    }
    synchronized void recover() {
        dropped += queue.size(); queue.clear(); bytes = 0; waitingKey = true; resets++;
    }
    synchronized void close() { closed = true; queue.clear(); bytes = 0; notifyAll(); }
    synchronized long resets() { return resets; }
    synchronized long dropped() { return dropped; }
}
