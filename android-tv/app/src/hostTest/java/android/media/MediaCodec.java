package android.media;
import android.view.Surface;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.concurrent.atomic.AtomicInteger;

/** Minimal driver model: delayed first input, delayed output, modified PTS, stalled HW. */
public final class MediaCodec {
    public static final int INFO_OUTPUT_FORMAT_CHANGED=-2, INFO_OUTPUT_BUFFERS_CHANGED=-3;
    public static final int BUFFER_FLAG_CODEC_CONFIG=2, BUFFER_FLAG_END_OF_STREAM=4;
    public static volatile int inputDelayMs, outputDelayMs;
    public static volatile boolean rewritePts, stallHardware;
    public static final AtomicInteger created=new AtomicInteger();
    private final boolean hardware;
    private final ArrayDeque<Output> outputs=new ArrayDeque<>();
    private final ByteBuffer input=ByteBuffer.allocate(8*1024*1024);
    private long readyNs;
    private static final class Output {
        long pts, readyNs;
        Output(long pts) { this.pts=pts; readyNs=System.nanoTime()+outputDelayMs*1_000_000L; }
    }
    public static final class BufferInfo { public long presentationTimeUs; public int flags,size; }
    private MediaCodec(String name) { hardware=!name.contains("google"); created.incrementAndGet(); }
    public static MediaCodec createByCodecName(String name) throws IOException { return new MediaCodec(name); }
    public void configure(MediaFormat format,Surface surface,Object crypto,int flags) { }
    public void start() { readyNs=System.nanoTime()+inputDelayMs*1_000_000L; }
    public int dequeueInputBuffer(long timeoutUs) {
        if (System.nanoTime()>=readyNs) return 0;
        try { Thread.sleep(Math.max(1,timeoutUs/1000)); } catch (InterruptedException ignored) { }
        return -1;
    }
    public ByteBuffer getInputBuffer(int index) { return input; }
    public void queueInputBuffer(int index,int offset,int length,long pts,int flags) {
        if (!(hardware&&stallHardware)) outputs.add(new Output(pts));
    }
    public int dequeueOutputBuffer(BufferInfo info,long timeoutUs) {
        if (outputs.isEmpty()||outputs.peek().readyNs>System.nanoTime()) return -1;
        Output next=outputs.remove(); info.presentationTimeUs=rewritePts?0:next.pts;
        info.flags=0; info.size=0; // Surface outputs can have no accessible byte buffer.
        return 0;
    }
    public void releaseOutputBuffer(int index,boolean render) { }
    public void releaseOutputBuffer(int index,long timestampNs) { }
    public void stop() { }
    public void release() { outputs.clear(); }
    public static void reset() { inputDelayMs=outputDelayMs=0; rewritePts=stallHardware=false; created.set(0); }
}
