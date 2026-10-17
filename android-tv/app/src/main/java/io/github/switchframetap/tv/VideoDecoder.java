/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.view.Surface;
import android.util.Log;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.HashSet;
import java.util.Set;

final class VideoDecoder implements AutoCloseable {
    interface Status { void accept(String value); }
    private static final String MIME = "video/avc";
    private final Surface surface;
    private final VideoQueue queue;
    private final StreamStats stats;
    private final Status status;
    private final Thread thread;
    private volatile boolean stopped;
    private MediaCodec codec;
    private final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
    private final Map<Long, Stamp> stamps = new LinkedHashMap<>();
    private final Set<String> failedCodecs = new HashSet<>();
    private String codecName = "";
    private int width, height;
    private long lastPts, ptsOriginNs, firstInputNs, lastOutputNs;
    private int inputsSinceOutput;
    private static final class Stamp {
        final long receivedNs, ageUs;
        Stamp(SftProtocol.Packet p) { receivedNs = p.receivedNs; ageUs = p.consoleAgeUs; }
    }
    VideoDecoder(Surface surface, VideoQueue queue, StreamStats stats, Status status) {
        this.surface = surface; this.queue = queue; this.stats = stats; this.status = status;
        thread = new Thread(this::run, "sft-video");
    }
    void start() { thread.start(); }
    void requestStop() {
        stopped = true; queue.close(); thread.interrupt();
    }
    @Override public void close() {
        requestStop();
        try { thread.join(1000); } catch (InterruptedException ignored) { }
    }
    private void run() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY);
        SftProtocol.Packet pending = null;
        SftProtocol.Packet configuredPacket = null;
        long inputWaitNs = 0;
        try {
            while (!stopped) {
                try {
                    if (codec != null) drain();
                    if (pending == null) pending = queue.poll(4);
                    if (pending == null) continue;
                    if (pending != configuredPacket && (pending.discontinuity || codec == null
                            || pending.width != width || pending.height != height)) {
                        if (!pending.key()) { pending = null; continue; }
                        releaseCodec();
                        if (pending.width != width || pending.height != height) failedCodecs.clear();
                        width = pending.width; height = pending.height;
                        configure(pending);
                        configuredPacket = pending;
                        inputWaitNs = System.nanoTime();
                    }
                    int index = codec.dequeueInputBuffer(2000);
                    if (index < 0) {
                        if (System.nanoTime() - inputWaitNs > 500_000_000L)
                            throw new IOException("The decoder is not responding");
                        continue;
                    }
                    ByteBuffer buffer = codec.getInputBuffer(index);
                    if (buffer == null || buffer.capacity() < pending.data.length)
                        throw new IOException("The decoder cannot accept this frame size; lower the quality on the Switch");
                    ((java.nio.Buffer)buffer).clear(); buffer.put(pending.data);
                    if (ptsOriginNs == 0) ptsOriginNs = pending.receivedNs;
                    long pts = Math.max(lastPts + 1, (pending.receivedNs - ptsOriginNs) / 1000);
                    lastPts = pts; stamps.put(pts, new Stamp(pending));
                    while (stamps.size() > 128) stamps.remove(stamps.keySet().iterator().next());
                    codec.queueInputBuffer(index, 0, pending.data.length, pts, 0);
                    stats.inputSubmitted++; inputsSinceOutput++;
                    if (firstInputNs == 0) firstInputNs = System.nanoTime();
                    long progress = lastOutputNs == 0 ? firstInputNs : lastOutputNs;
                    if (inputsSinceOutput >= 8 && System.nanoTime() - progress > 1_500_000_000L)
                        throw new IOException("The decoder accepts input but produces no video output");
                    inputWaitNs = System.nanoTime(); pending = null; configuredPacket = null;
                } catch (IOException | RuntimeException e) {
                    stats.decoderErrors++;
                    if (codec != null) failedCodecs.add(codecName);
                    releaseCodec(); queue.recover(); pending = null; configuredPacket = null;
                    status.accept("Video: " + e.getMessage() + ". Waiting for another keyframe…");
                    Log.w("SFT-TV", "Video", e);
                }
            }
        } catch (InterruptedException ignored) {
        } finally { releaseCodec(); }
    }
    private static boolean software(MediaCodecInfo info) {
        if (Build.VERSION.SDK_INT >= 29) return info.isSoftwareOnly();
        String n = info.getName().toLowerCase(java.util.Locale.ROOT);
        return n.startsWith("omx.google.") || n.startsWith("c2.android.") || n.contains(".sw.");
    }
    private static boolean lowLatency(MediaCodecInfo info) {
        try { return Build.VERSION.SDK_INT >= 30 && info.getCapabilitiesForType(MIME)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency); }
        catch (IllegalArgumentException e) { return false; }
    }
    private void configure(SftProtocol.Packet packet) throws IOException {
        byte[][] csd = SftProtocol.parameterSets(packet.data);
        List<MediaCodecInfo> candidates = new ArrayList<>();
        for (MediaCodecInfo item : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (item.isEncoder() || failedCodecs.contains(item.getName())) continue;
            for (String type : item.getSupportedTypes()) if (type.equalsIgnoreCase(MIME)) {
                try {
                    if (item.getCapabilitiesForType(MIME).getVideoCapabilities().isSizeSupported(width, height)) candidates.add(item);
                } catch (RuntimeException ignored) { }
                break;
            }
        }
        Collections.sort(candidates, (a,b) -> Integer.compare(score(a), score(b)));
        String lastFailure = "No H.264 decoder is available for " + width + "×" + height;
        for (MediaCodecInfo item : candidates) {
            for (int optimized = 1; optimized >= 0; optimized--) {
                MediaCodec attempt = null;
                try {
                    MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
                    format.setByteBuffer("csd-0", ByteBuffer.wrap(csd[0]));
                    format.setByteBuffer("csd-1", ByteBuffer.wrap(csd[1]));
                    format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, SftProtocol.MAX_VIDEO);
                    if (optimized == 1) {
                        format.setInteger(MediaFormat.KEY_PRIORITY, 0);
                        if (Build.VERSION.SDK_INT >= 30 && lowLatency(item)) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
                        if (Build.VERSION.SDK_INT >= 24) {
                            format.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709);
                            format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
                        }
                    }
                    attempt = MediaCodec.createByCodecName(item.getName());
                    attempt.configure(format, surface, null, 0); attempt.start();
                    codec = attempt; codecName = item.getName();
                    stats.width = width; stats.height = height;
                    stats.decoder = codecName + (software(item) ? " · software" : " · hardware")
                            + (optimized == 1 && lowLatency(item) ? " · low latency" : "");
                    status.accept("Receiving " + width + "×" + height + (software(item) ? " · compatibility mode" : ""));
                    return;
                } catch (IOException | RuntimeException e) {
                    lastFailure = item.getName() + ": " + e.getMessage();
                    if (attempt != null) try { attempt.release(); } catch (RuntimeException ignored) { }
                }
            }
        }
        throw new IOException(lastFailure);
    }
    private static int score(MediaCodecInfo item) { return software(item) ? 2 : lowLatency(item) ? 0 : 1; }
    private void drain() {
        int latest = -1;
        Stamp latestStamp = null;
        for (int n = 0; n < 32; n++) {
            int index = codec.dequeueOutputBuffer(info, 0);
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED || index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (index < 0) break;
            if ((info.flags & (MediaCodec.BUFFER_FLAG_CODEC_CONFIG | MediaCodec.BUFFER_FLAG_END_OF_STREAM)) != 0) {
                codec.releaseOutputBuffer(index, false); continue;
            }
            Stamp stamp = stamps.remove(info.presentationTimeUs);
            if (stamp == null && !stamps.isEmpty()) {
                // SFTR's encoder has no B frames. Some TV codecs rewrite timestamps;
                // FIFO metadata is safe here, and image output must not depend on it.
                stamp = stamps.remove(stamps.keySet().iterator().next()); stats.unmatchedPts++;
            }
            stats.decoded++; lastOutputNs = System.nanoTime(); inputsSinceOutput = 0;
            if (latest >= 0) { codec.releaseOutputBuffer(latest, false); stats.skipped++; }
            latest = index; latestStamp = stamp;
        }
        if (latest >= 0) {
            double elapsed = latestStamp == null ? 0 : (System.nanoTime() - latestStamp.receivedNs) / 1_000_000.0;
            boolean show = !stopped;
            // Present the newest decoded output immediately. A fixed 120 ms cutoff
            // could discard every frame on a TV during startup or in a deeper pipeline.
            if (show) codec.releaseOutputBuffer(latest, System.nanoTime());
            else codec.releaseOutputBuffer(latest, false);
            if (show) {
                stats.decoderMs = elapsed; stats.consoleMs = latestStamp == null ? 0 : latestStamp.ageUs / 1000.0;
                stats.lastRenderedNs = System.nanoTime(); stats.rendered++;
            } else stats.skipped++;
        }
    }
    private void releaseCodec() {
        MediaCodec old = codec; codec = null; stamps.clear();
        lastPts = ptsOriginNs = firstInputNs = lastOutputNs = 0; inputsSinceOutput = 0;
        if (old != null) {
            try { old.stop(); } catch (RuntimeException ignored) { }
            try { old.release(); } catch (RuntimeException ignored) { }
        }
    }
}
