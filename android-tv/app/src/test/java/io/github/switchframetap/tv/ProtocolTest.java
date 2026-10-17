package io.github.switchframetap.tv;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;

/** Exercises transport fragmentation and reference recovery, not Android mocks. */
public final class ProtocolTest {
    private static int checks;
    private static void check(boolean value, String name) {
        if (!value) throw new AssertionError(name); checks++;
    }
    private static final class Chunks implements SftProtocol.Source {
        final ByteArrayInputStream stream;
        int calls;
        Chunks(byte[] data) { stream = new ByteArrayInputStream(data); }
        @Override public int read(byte[] b, int offset, int length) {
            if (++calls % 4 == 0) return 0; // idle timeout between partial reads
            return stream.read(b, offset, Math.min(length, calls % 17 + 1));
        }
        @Override public void close() { }
    }
    private static byte[] wire(int flags, int width, int height, long seq, byte[] payload) {
        byte[] h = new byte[32 + payload.length];
        put(h,0,0x52544653); h[4] = 2; h[6] = (byte)flags;
        put(h,8,width); put(h,12,height); put(h,16,25000); put(h,20,payload.length); put(h,28,seq);
        System.arraycopy(payload,0,h,32,payload.length); return h;
    }
    private static void put(byte[] h, int i, long value) { for (int j=0;j<4;j++) h[i+j]=(byte)(value >>> (j*8)); }
    private static void invalid(byte[] data, String name) throws Exception {
        try { SftProtocol.readPacket(new Chunks(data), () -> false); throw new AssertionError(name); }
        catch (IOException expected) { checks++; }
    }
    private static SftProtocol.Packet frame(long seq, boolean key, int w, long ns) {
        return new SftProtocol.Packet(2 | (key ? 4 : 0), w,720,seq,20000,new byte[100],ns);
    }
    public static void main(String[] args) throws Exception {
        byte[] nal = {0,0,0,1,0x67,0x64,0,0,1,0x68,0x12,0,0,1,0x65,9};
        byte[] audio = {1,2,3,4,5,6,7,8};
        ByteArrayOutputStream concatenated = new ByteArrayOutputStream();
        concatenated.write(wire(8,48000,2,1,audio)); concatenated.write(wire(6,1280,720,0xffffffffL,nal));
        Chunks source = new Chunks(concatenated.toByteArray());
        SftProtocol.Packet a = SftProtocol.readPacket(source, () -> false);
        SftProtocol.Packet v = SftProtocol.readPacket(source, () -> false);
        check(a.audio() && Arrays.equals(a.data,audio), "interleaved PCM with partial reads and timeouts");
        check(v.key() && v.sequence == 0xffffffffL && v.consoleAgeUs == 25000, "unsigned fields and v2 age");
        check(Arrays.equals(v.data,nal), "exact video boundary");
        byte[][] csd = SftProtocol.parameterSets(nal);
        check(csd[0][4] == 0x67 && csd[1][3] == 0x68, "mixed 3/4 byte Annex-B start codes");
        invalid(Arrays.copyOf(wire(6,1280,720,0,nal),37), "truncated payload");
        byte[] bad = wire(6,1280,720,0,nal); bad[0]=0; invalid(bad,"bad magic");
        bad = wire(6,1280,720,0,nal); bad[4]=1; invalid(bad,"old version");
        bad = wire(6,1280,720,0,nal); put(bad,20,0xffffffffL); invalid(bad,"unsigned oversized allocation rejected");
        invalid(wire(8,44100,2,0,audio), "unsupported sample rate");
        invalid(wire(8,48000,2,0,new byte[3]), "unaligned PCM");
        invalid(wire(2,4096,720,0,nal), "invalid dimensions");
        invalid(wire(10,1280,720,0,nal), "audio/video flag collision");
        try { SftProtocol.parameterSets(new byte[]{0,0,1,0x65,1}); throw new AssertionError("missing SPS"); }
        catch (IOException expected) { checks++; }
        try { SftProtocol.readPacket(new Chunks(new byte[0]), () -> true); throw new AssertionError("cancellation"); }
        catch (IOException expected) { checks++; }
        check(SftProtocol.ipv4("192.168.1.50") && !SftProtocol.ipv4("256.1.1.1")
                && !SftProtocol.ipv4("1.2.3.4x") && !SftProtocol.ipv4("1..3.4"), "strict manual IP");
        check(SftProtocol.beaconPort("SFTAP1 192.168.1.3 9950 Switch Frame Tap") == 9950
                && SftProtocol.beaconPort("SFTAP1 1.2.3.4 65536") == -1
                && SftProtocol.beaconPort("OTHER 1.2.3.4 9950") == -1, "beacon validation");
        VideoQueue q = new VideoQueue();
        q.offer(frame(0,false,1280,0)); check(q.poll(1) == null,"initial P ignored");
        q.offer(frame(1,true,1280,0)); check(q.poll(1).discontinuity,"start at key frame");
        q.offer(frame(3,false,1280,0)); check(q.poll(1) == null,"gap invalidates P references");
        q.offer(frame(4,true,1280,0)); check(q.poll(1).discontinuity,"gap recovers at IDR");
        q.offer(frame(5,false,1920,0)); check(q.poll(1) == null,"resolution change waits for IDR");
        q.offer(frame(6,true,1920,0)); check(q.poll(1).discontinuity,"resolution change recovers");
        for(int i=7;i<=13;i++) q.offer(frame(i,false,1920,0));
        check(q.poll(1)==null,"overflow discards complete pending dependency chain");
        q.offer(frame(14,false,1920,0)); check(q.poll(1)==null,"overflow ignores dependent P");
        q.offer(frame(15,true,1920,0)); check(q.poll(1).discontinuity,"overflow recovers at key");
        q.offer(frame(16,false,1920,0)); q.offer(frame(17,false,1920,110_000_000L));
        check(q.poll(1)==null,"aged queue invalidates references");
        q.offer(frame(0,true,1920,120_000_000L)); check(q.poll(1).discontinuity,"new session sequence restart");
        VideoQueue wrap = new VideoQueue(); wrap.offer(frame(0xffffffffL,true,1280,0)); wrap.poll(1);
        wrap.offer(frame(0,false,1280,0)); check(!wrap.poll(1).discontinuity,"unsigned sequence wrap");
        q.close(); q.offer(frame(1,true,1280,0)); check(q.poll(1)==null,"closed queue");
        Path fixture = Paths.get(args[0], "logs/m83-runI/stream-first10.sft");
        if (Files.exists(fixture)) {
            Chunks real = new Chunks(Files.readAllBytes(fixture));
            int frames = 0;
            while (real.stream.available() > 0) {
                SftProtocol.Packet packet = SftProtocol.readPacket(real, () -> false);
                check(!packet.audio() && packet.width == 1280 && packet.height == 720,"real Switch wire packet");
                SftProtocol.parameterSets(packet.data); frames++;
            }
            check(frames > 0, "real NVENC fixture parsed");
            System.out.println("Real Switch fixture: " + frames + " frames");
        } else System.out.println("Real Switch fixture unavailable; synthetic tests completed");
        System.out.println("PASS: " + checks + " checks");
    }
}
