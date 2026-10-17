/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Build;
import android.util.Log;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Arrays;

/** PCM playback never blocks the USB/TCP reader or the video decoder. */
final class AudioPump implements AutoCloseable {
    private static final int BYTES_PER_SECOND = 48000 * 4, MAX_QUEUED = BYTES_PER_SECOND * 80 / 1000;
    private final ArrayDeque<SftProtocol.Packet> queue = new ArrayDeque<>();
    private final StreamStats stats;
    private final boolean stable;
    private final Thread thread;
    private volatile boolean stopped, muted, failed;
    private int bytes, generation;
    AudioPump(StreamStats stats, boolean stable) {
        this.stats = stats; this.stable = stable;
        thread = new Thread(this::run, "sft-audio");
    }
    void start() { thread.start(); }
    synchronized void offer(SftProtocol.Packet packet) {
        if (stopped || muted || failed) return;
        if (bytes + packet.data.length > MAX_QUEUED) { queue.clear(); bytes = 0; }
        if (packet.data.length > MAX_QUEUED) {
            byte[] tail = Arrays.copyOfRange(packet.data, packet.data.length - MAX_QUEUED, packet.data.length);
            packet = new SftProtocol.Packet(packet.flags, packet.width, packet.height, packet.sequence,
                    0, tail, packet.receivedNs);
        }
        queue.addLast(packet); bytes += packet.data.length; notifyAll();
    }
    synchronized void reset() { queue.clear(); bytes = 0; generation++; notifyAll(); }
    synchronized void mute(boolean value) {
        stats.audioMuted = value;
        if (muted != value) { muted = value; reset(); }
    }
    void requestStop() {
        stopped = true;
        synchronized (this) { queue.clear(); bytes = 0; notifyAll(); }
        thread.interrupt();
    }
    @Override public void close() {
        requestStop();
        try { thread.join(1000); } catch (InterruptedException ignored) { }
    }
    private AudioTrack createTrack() {
        int min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) throw new IllegalStateException("PCM output is not available");
        AudioTrack.Builder builder = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(Math.max(min, BYTES_PER_SECOND * (stable ? 60 : 20) / 1000));
        if (Build.VERSION.SDK_INT >= 26) builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
        AudioTrack track = builder.build();
        if (track.getState() != AudioTrack.STATE_INITIALIZED) {
            track.release(); throw new IllegalStateException("Could not initialize audio output");
        }
        return track;
    }
    private void run() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
        AudioTrack track = null;
        int seenGeneration = -1;
        long writtenFrames = 0, headWrap = 0, lastHead = 0;
        SftProtocol.Packet pending = null;
        ByteBuffer buffer = null;
        ByteBuffer scratch = ByteBuffer.allocateDirect(SftProtocol.MAX_AUDIO);
        try {
            // Some older TV drivers reject the performance hint; retry without it.
            try { track = createTrack(); }
            catch (RuntimeException e) {
                track = new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build(),
                        new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build(),
                        Math.max(11520, AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO,
                                AudioFormat.ENCODING_PCM_16BIT)), AudioTrack.MODE_STREAM, 0);
                if (track.getState() != AudioTrack.STATE_INITIALIZED) throw e;
            }
            stats.audio = "PCM 48 kHz stereo";
            stats.audioBufferMs = track.getBufferSizeInFrames() / 48.0;
            if (Build.VERSION.SDK_INT >= 31) {
                try { track.setStartThresholdInFrames(Math.min(track.getBufferSizeInFrames(), stable ? 2880 : 960)); }
                catch (RuntimeException e) { Log.w("SFT-TV", "Audio start threshold", e); }
            }
            while (!stopped) {
                int currentGeneration;
                synchronized (this) { currentGeneration = generation; }
                if (currentGeneration != seenGeneration) {
                    track.pause(); track.flush();
                    writtenFrames = headWrap = lastHead = 0; pending = null; buffer = null;
                    seenGeneration = currentGeneration; stats.audioResets++;
                }
                if (pending == null) {
                    synchronized (this) {
                        if (queue.isEmpty() && !stopped) wait(20);
                        if (generation != seenGeneration) continue;
                        pending = queue.pollFirst();
                        if (pending != null) bytes -= pending.data.length;
                    }
                    if (pending == null) { stats.audioQueuedMs = queuedMs(track, writtenFrames, headWrap, lastHead); continue; }
                    buffer = scratch;
                    ((java.nio.Buffer)buffer).clear();
                    buffer.put(pending.data);
                    ((java.nio.Buffer)buffer).flip();
                }
                if (muted || System.nanoTime() - pending.receivedNs > 120_000_000L) {
                    // Drop stale software data; do not empty a TV's native startup buffer.
                    pending = null; buffer = null; continue;
                }
                if (track.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) track.play();
                long head = track.getPlaybackHeadPosition() & 0xffffffffL;
                if (head < lastHead) headWrap += 1L << 32;
                lastHead = head;
                long queued = Math.max(0, writtenFrames - headWrap - head);
                int softBytes; synchronized (this) { softBytes = bytes; }
                stats.audioQueuedMs = queued / 48.0 + softBytes * 1000.0 / BYTES_PER_SECOND;
                stats.audioPlayedFrames = headWrap + head;
                // AudioTrack bounds its own queue. An 80 ms flush limit can prevent
                // playback forever when a TV requires >80 ms of data to start.
                int n = track.write(buffer, buffer.remaining(), AudioTrack.WRITE_NON_BLOCKING);
                if (n < 0) throw new IllegalStateException("Audio output error: " + n);
                writtenFrames += n / 4;
                stats.audioWrittenFrames += n / 4;
                if (!buffer.hasRemaining()) { pending = null; buffer = null; }
                if (n == 0) Thread.sleep(2);
            }
        } catch (InterruptedException ignored) {
        } catch (RuntimeException e) {
            failed = true; stats.audio = "Audio unavailable: " + e.getMessage();
            Log.w("SFT-TV", "Audio", e);
        } finally {
            if (track != null) { try { track.pause(); track.flush(); } catch (RuntimeException ignored) { } track.release(); }
            synchronized (this) { queue.clear(); bytes = 0; }
            stats.audioQueuedMs = 0;
        }
    }
    private static double queuedMs(AudioTrack track, long written, long wrap, long last) {
        long head = track.getPlaybackHeadPosition() & 0xffffffffL;
        if (head < last) wrap += 1L << 32;
        return Math.max(0, written - wrap - head) / 48.0;
    }
}
