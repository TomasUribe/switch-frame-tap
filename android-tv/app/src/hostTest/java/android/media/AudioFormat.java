package android.media;
public final class AudioFormat {
    public static final int CHANNEL_OUT_STEREO=12, ENCODING_PCM_16BIT=2;
    public static final class Builder {
        public Builder setSampleRate(int rate) { return this; }
        public Builder setChannelMask(int mask) { return this; }
        public Builder setEncoding(int encoding) { return this; }
        public AudioFormat build() { return new AudioFormat(); }
    }
}
