package io.github.switchframetap.tv;
import android.media.MediaCodec;
import android.view.Surface;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

/** Run production worker loops, with platform drivers replaced by timing models. */
public final class PlaybackTest {
    private static byte[] frame;
    private static void require(boolean condition,String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static StreamStats video(int frames) throws Exception {
        VideoQueue queue=new VideoQueue(); StreamStats stats=new StreamStats();
        VideoDecoder decoder=new VideoDecoder(new Surface(),queue,stats,s -> { });
        decoder.start();
        try {
            for (int i=0;i<frames;i++) {
                queue.offer(new SftProtocol.Packet(6,1280,720,i,20000,frame,System.nanoTime()));
                Thread.sleep(33);
            }
            Thread.sleep(250);
        } finally { decoder.close(); }
        return stats;
    }
    public static void main(String[] args) throws Exception {
        java.nio.file.Path fixture=Paths.get(args[0],"logs/m83-runI/stream-first10.sft");
        if (Files.exists(fixture)) {
            byte[] wire=Files.readAllBytes(fixture);
            frame=Arrays.copyOfRange(wire,32,32+(int)SftProtocol.u32(wire,20));
        } else {
            // Standalone source ZIP: only framing is needed by the driver doubles.
            frame=new byte[]{0,0,0,1,0x67,0x64,0,0,1,0x68,0x12,0,0,1,0x65,9};
            System.out.println("No capture available; using synthetic framing for driver timing regressions");
        }
        MediaCodec.reset(); MediaCodec.inputDelayMs=60;
        StreamStats delayed=video(6);
        require(delayed.rendered>0,"first input buffer arrives late: decoder must eventually render");
        require(MediaCodec.created.get()==1,"pending first IDR must not repeatedly recreate decoder");
        System.out.println("PASS: delayed first input buffer, one codec configuration");
        MediaCodec.reset(); MediaCodec.outputDelayMs=180; MediaCodec.rewritePts=true;
        StreamStats pts=video(14);
        require(pts.rendered>0&&pts.unmatchedPts>0,"rewritten timestamps must not discard all Surface outputs");
        require(pts.decoderMs>=120,"driver latency >120 ms must not produce a black screen");
        System.out.println("PASS: rewritten PTS and 180 ms decoder output");
        MediaCodec.reset(); MediaCodec.stallHardware=true;
        StreamStats fallback=video(65);
        require(fallback.decoderErrors>0&&fallback.decoder.contains("software")&&fallback.rendered>0,
                "accepted input with no output must fall back at an IDR");
        System.out.println("PASS: stalled hardware decoder falls back to software");
        StreamStats audioStats=new StreamStats(); AudioPump audio=new AudioPump(audioStats,false); audio.start();
        try {
            for (int i=0;i<22;i++) {
                audio.offer(new SftProtocol.Packet(8,48000,2,i,0,new byte[3840],System.nanoTime()));
                Thread.sleep(20);
            }
            require(audioStats.audioBufferMs==160,"test native sink must require 160 ms buffer");
            require(audioStats.audioPlayedFrames>0,"native buffer must fill beyond 80 ms and start playback");
            require(audioStats.audioResets==1,"startup must not repeatedly flush native audio");
        } finally { audio.close(); }
        System.out.println("PASS: 160 ms native audio startup without perpetual resets");
    }
}
