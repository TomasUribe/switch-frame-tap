#!/bin/bash
# v0.6.1: a minimal FFmpeg for the Windows viewer, linked into SwitchFrameTap.exe.
#
# The viewer needs three pieces of FFmpeg: the H.264 decoder (the stream),
# the AAC encoder and the MP4 muxer (recording, R). The prebuilt LGPL DLLs it
# shipped until v0.6.0 carry every codec FFmpeg has - 130 MB unpacked, 45 MB
# of the 50 MB zip. This builds FFmpeg 8.1 (the same version) with only those,
# as static libraries, cross-compiled with the Zig toolchain the viewer
# already uses. LGPL-2.1+ code inside our GPL-2.0 program is allowed.
#
# Needs: ref/ffmpeg-8.1.tar.xz (https://ffmpeg.org/releases/), nasm (the
# decoder's x86 SIMD code - without it decoding is several times slower),
# ref/win/zig (tools/raw-recv/build-windows.sh). Output: ref/win/ffmpeg-min/.
#
#   bash tools/raw-recv/build-ffmpeg-min.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
W="$REPO/ref/win"
SRC="$REPO/ref/ffmpeg-8.1"
OUT="$W/ffmpeg-min"
command -v nasm >/dev/null || { echo "nasm is needed: sudo apt install nasm"; exit 1; }
[ -d "$SRC" ] || tar -xJf "$REPO/ref/ffmpeg-8.1.tar.xz" -C "$REPO/ref"

# zig as a mingw cross compiler: small wrappers, since configure wants one
# program name per tool
T="$W/zig-wrap"
mkdir -p "$T"
for tool in cc ar ranlib; do
    case $tool in
        cc) printf '#!/bin/sh\nexec "%s/zig/zig" cc -target x86_64-windows-gnu "$@"\n' "$W" > "$T/x86_64-w64-mingw32-gcc" ;;
        ar) printf '#!/bin/sh\nexec "%s/zig/zig" ar "$@"\n' "$W" > "$T/x86_64-w64-mingw32-ar" ;;
        ranlib) printf '#!/bin/sh\nexec "%s/zig/zig" ranlib "$@"\n' "$W" > "$T/x86_64-w64-mingw32-ranlib" ;;
    esac
done
chmod +x "$T"/*

B="$SRC/build-win"
rm -rf "$B"
mkdir -p "$B"
cd "$B"
"$SRC/configure" \
    --prefix="$OUT" \
    --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
    --cc="$T/x86_64-w64-mingw32-gcc" --ar="$T/x86_64-w64-mingw32-ar" --ranlib="$T/x86_64-w64-mingw32-ranlib" \
    --nm=nm --pkg-config=false \
    --enable-static --disable-shared \
    --disable-everything --disable-autodetect --disable-programs --disable-doc \
    --disable-network --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-decoder=h264 --enable-parser=h264 \
    --enable-encoder=aac \
    --enable-muxer=mp4 --enable-protocol=file \
    --enable-x86asm \
    > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
grep -E "^(decoders|encoders|muxers|protocols|External libraries|x86asm)" -A2 "$B/configure.log" | head -20 || true
make -j"$(nproc)" > "$B/make.log" 2>&1 || { tail -40 "$B/make.log"; exit 1; }
rm -rf "$OUT"
make install > "$B/install.log" 2>&1
cp "$SRC/COPYING.LGPLv2.1" "$OUT/COPYING.LGPLv2.1"
echo "== $OUT"
ls -la "$OUT/lib"
