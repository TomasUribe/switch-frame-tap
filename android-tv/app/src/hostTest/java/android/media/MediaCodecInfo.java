package android.media;
public final class MediaCodecInfo {
    private final String name;
    MediaCodecInfo(String name) { this.name=name; }
    public String getName() { return name; }
    public boolean isEncoder() { return false; }
    public boolean isSoftwareOnly() { return name.contains("google"); }
    public String[] getSupportedTypes() { return new String[]{"video/avc"}; }
    public CodecCapabilities getCapabilitiesForType(String type) { return new CodecCapabilities(); }
    public static final class CodecCapabilities {
        public static final String FEATURE_LowLatency="low-latency";
        public boolean isFeatureSupported(String feature) { return false; }
        public VideoCapabilities getVideoCapabilities() { return new VideoCapabilities(); }
    }
    public static final class VideoCapabilities {
        public boolean isSizeSupported(int width,int height) { return true; }
    }
}
