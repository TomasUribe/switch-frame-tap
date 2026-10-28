#!/usr/bin/env bash
# zerocopy_test.sh - the zero-copy VAAPI -> OpenGL path (tools/raw-recv/zerocopy.h)
#  always: raw-view builds with it (libva found) and without it (ZEROCOPY=0)
#  SFT_GUI_TESTS=1 on a Wayland desktop with VAAPI: a synthetic clip is shown
#  for a few seconds with and without zero-copy (two windows open briefly);
#  the colour bars must match (BT.709 limited range, same as SDL's NV12 path)
set -u
cd "$(dirname "$0")/../raw-recv"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"; make -s -B raw-view >/dev/null 2>&1' EXIT
make -s -B raw-view ZEROCOPY=0 >/dev/null 2>&1 || { echo "does not build with ZEROCOPY=0"; exit 1; }
if grep -qa "zero-copy (VAAPI" raw-view; then echo "ZEROCOPY=0 still has zero-copy"; exit 1; fi
if pkg-config --exists libva; then
    make -s -B raw-view >/dev/null 2>&1 || { echo "does not build with zero-copy"; exit 1; }
    grep -qa "zero-copy (VAAPI" raw-view || { echo "libva found but no zero-copy in the build"; exit 1; }
fi
[ "${SFT_GUI_TESTS:-0}" = 1 ] && [ -n "${WAYLAND_DISPLAY:-}" ] || exit 0
command -v ffmpeg >/dev/null || exit 0
# a 1080p60 clip shaped like the console's stream (IDR then P frames, one
# slice, no B frames), packed as SFTR packets with frame numbers in kind
ffmpeg -v error -y -f lavfi -i "testsrc2=s=1920x1080:r=60" -t 4 \
    -c:v libx264 -preset veryfast -tune zerolatency -profile:v high -bf 0 -g 600 \
    -x264-params slices=1 -b:v 8M -pix_fmt yuv420p -bsf:v h264_metadata=aud=insert \
    -f h264 "$tmp/c.h264" || exit 1
python3 -I - "$tmp/c.h264" "$tmp/c.h264.sft" <<'PY' || exit 1
import re, struct, sys
data = open(sys.argv[1], 'rb').read()
idx = [m.start() for m in re.finditer(b'\x00\x00\x00\x01\x09', data)] + [len(data)]
with open(sys.argv[2], 'wb') as o:
    for n in range(len(idx) - 1):
        au = data[idx[n]:idx[n + 1]]
        key = 4 if b'\x00\x00\x01\x65' in au[:4096] else 0
        o.write(struct.pack('<IHHIIIIII', 0x52544653, 2, 2 | key, 1920, 1080, 0, len(au), 0, n) + au)
PY
for m in zc nozc; do
    extra=; [ $m = nozc ] && extra=--no-zero-copy
    XDG_DATA_HOME="$tmp/data" timeout 30 ./raw-view --file "$tmp/c.h264.sft" --paced --seconds 4 --shot "$tmp/$m.bmp" $extra >/dev/null 2>"$tmp/$m.log"
done
grep -q 'decode: zero-copy (VAAPI' "$tmp/zc.log" || { echo "zero-copy did not start: $(grep zero-copy "$tmp/zc.log")"; exit 0; }
grep -q 'zero-copy off' "$tmp/zc.log" && { echo "zero-copy failed mid-run: $(grep 'zero-copy off' "$tmp/zc.log")"; exit 1; }
python3 -I - "$tmp/zc.bmp" "$tmp/nozc.bmp" <<'PY'
import struct, sys
def load(p):
    b = open(p, 'rb').read()
    off, w, h, bpp = struct.unpack_from('<I', b, 10)[0], *struct.unpack_from('<ii', b, 18), struct.unpack_from('<H', b, 28)[0]
    stride = (w * bpp // 8 + 3) & ~3
    def px(x, y):   # y from the top
        row = (h - 1 - y) if h > 0 else y
        i = off + row * stride + x * bpp // 8
        return b[i + 2], b[i + 1], b[i]
    return abs(w), abs(h), px
wa, ha, a = load(sys.argv[1]); wb, hb, b = load(sys.argv[2])
# the two windows may differ by a pixel or two (a tiling desktop): same place, relative
assert abs(wa - wb) <= 4 and abs(ha - hb) <= 4, ((wa, ha), (wb, hb))
# testsrc2's colour bars, sampled away from the moving parts (left 6/7 of the picture, middle rows)
worst = 0
for k in range(6):
    f = (k + 0.5) / 7.2
    pa, pb = a(int(wa * f), int(ha * 0.47)), b(int(wb * f), int(hb * 0.47))
    worst = max(worst, max(abs(p - q) for p, q in zip(pa, pb)))
if worst > 3:
    sys.exit(f'colours differ by up to {worst}')
PY
