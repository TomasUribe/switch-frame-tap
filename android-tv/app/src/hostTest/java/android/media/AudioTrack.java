package android.media;
import java.nio.ByteBuffer;

/** Native audio does not start until 160 ms have been written, as on deeper TV sinks. */
public final class AudioTrack {
    public static final int MODE_STREAM=1, STATE_INITIALIZED=1, PLAYSTATE_PLAYING=3,
            PERFORMANCE_MODE_LOW_LATENCY=1, WRITE_NON_BLOCKING=1;
    private final int capacityFrames=7680;
    private int threshold=capacityFrames, written, played;
    private boolean playing, started;
    private long clockNs;
    public AudioTrack(AudioAttributes attributes,AudioFormat format,int size,int mode,int id) { }
    public static int getMinBufferSize(int rate,int channels,int encoding) { return 7680*4; }
    public int getState() { return STATE_INITIALIZED; }
    public int getBufferSizeInFrames() { return capacityFrames; }
    public int setStartThresholdInFrames(int value) { threshold=value; return value; }
    public void pause() { update(); playing=false; }
    public void flush() { written=played=0; started=false; }
    public void play() { playing=true; clockNs=System.nanoTime(); }
    public int getPlayState() { return playing?PLAYSTATE_PLAYING:1; }
    public int getPlaybackHeadPosition() { update(); return played; }
    private void update() {
        long now=System.nanoTime();
        if (playing&&started) played=Math.min(written,played+(int)((now-clockNs)*48000/1_000_000_000L));
        clockNs=now;
    }
    public int write(ByteBuffer data,int size,int mode) {
        update();
        int frames=Math.min(size/4,capacityFrames-(written-played));
        written+=frames; data.position(data.position()+frames*4);
        if (!started&&written>=threshold) { started=true; clockNs=System.nanoTime(); }
        return frames*4;
    }
    public void release() { }
    public static final class Builder {
        public Builder setAudioAttributes(AudioAttributes attributes) { return this; }
        public Builder setAudioFormat(AudioFormat format) { return this; }
        public Builder setTransferMode(int mode) { return this; }
        public Builder setBufferSizeInBytes(int bytes) { return this; }
        public Builder setPerformanceMode(int mode) { return this; }
        public AudioTrack build() { return new AudioTrack(null,null,0,0,0); }
    }
}
