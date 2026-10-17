package android.media;
import java.nio.ByteBuffer;
public final class MediaFormat {
    public static final String KEY_MAX_INPUT_SIZE="max-input-size", KEY_PRIORITY="priority",
            KEY_LOW_LATENCY="low-latency", KEY_COLOR_STANDARD="color-standard", KEY_COLOR_RANGE="color-range";
    public static final int COLOR_STANDARD_BT709=1, COLOR_RANGE_LIMITED=2;
    public static MediaFormat createVideoFormat(String type,int width,int height) { return new MediaFormat(); }
    public void setByteBuffer(String key,ByteBuffer value) { }
    public void setInteger(String key,int value) { }
}
