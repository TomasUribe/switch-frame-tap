/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

final class StreamStats {
    volatile long bytes, received, rendered, skipped, decoderErrors, videoResets, audioResets;
    volatile long decoded, inputSubmitted, unmatchedPts, videoDropped, audioPackets, audioBytes, audioWrittenFrames, audioPlayedFrames;
    volatile long lastRenderedNs, lastVideoNs;
    volatile double decoderMs, consoleMs, audioQueuedMs;
    volatile double audioBufferMs;
    volatile boolean audioMuted;
    volatile int width, height;
    volatile String decoder = "—", audio = "Waiting for audio", transport = "";
}
