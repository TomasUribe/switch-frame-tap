#!/bin/bash
# v0.7.11: the Linux viewer as one binary for any distribution (Fedora, Arch,
# Bazzite, SteamOS...), not only Ubuntu/Debian with its -dev packages.
#
# Built in: FFmpeg 8.1 with only what the viewer uses (H.264 decoder, AAC
# encoder, MP4 muxer - as build-ffmpeg-min.sh does for Windows), SDL 2 built
# from source (it loads X11, Wayland, PulseAudio/PipeWire and ALSA from the
# system at runtime, so the binary does not need the exact versions Ubuntu's
# SDL2 was linked against) and libusb. Left to the system: glibc (2.35, this
# Ubuntu 22.04 build host - Fedora 36+, Arch, SteamOS 3 and Bazzite have newer)
# and libudev (every systemd distribution).
#
# Needs: ref/ffmpeg-8.1.tar.xz, ref/SDL2-2.32.10.tar.gz, nasm, the X11/Wayland/
# PulseAudio/ALSA -dev headers, libusb-1.0-0-dev, libudev-dev.
#
#   bash tools/raw-recv/build-linux-portable.sh 0.7.11
#     -> dist/switch-frame-tap-0.7.11-linux-portable.tar.gz
set -euo pipefail
VER="${1:?usage: build-linux-portable.sh VERSION}"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$REPO/tools/raw-recv"
L="$REPO/ref/linux"
SDLV=2.32.10
mkdir -p "$L"

# ---- v0.7.13: the GPU decoders' build-time pieces (headers only; nothing of
# them is linked): NVIDIA's codec headers - FFmpeg loads the NVIDIA driver
# itself at runtime - and libva's headers, with placeholder libva / libva-drm
# libraries that only exist so FFmpeg's configure check passes. The viewer
# reaches the system's own libva through gen_va_shim.py's loader instead.
D="$L/deps"
NVH=n13.0.19.0; VAV=2.24.1
if [ ! -f "$D/.ready-$NVH-$VAV" ]; then
    echo "== GPU decoder headers (nv-codec-headers $NVH, libva $VAV)"
    rm -rf "$D"; mkdir -p "$D"
    tar -xzf "$REPO/ref/nv-codec-headers-$NVH.tar.gz" -C "$D"
    make -s -C "$D/nv-codec-headers-$NVH" PREFIX="$D/ffnvcodec" install
    tar -xzf "$REPO/ref/libva-$VAV.tar.gz" -C "$D"
    mkdir -p "$D/va/include/va" "$D/va/lib/pkgconfig"
    cp "$D/libva-$VAV"/va/*.h "$D/libva-$VAV/va/drm/va_drm.h" "$D/va/include/va/"
    # VA-API 1.14 (libva 2.14, Ubuntu 22.04): FFmpeg then only uses functions
    # every libva 2.14+ has, so the loader finds all of them on older systems too
    sed -e 's/@VA_API_MAJOR_VERSION@/1/; s/@VA_API_MINOR_VERSION@/14/; s/@VA_API_MICRO_VERSION@/0/; s/@VA_API_VERSION@/1.14.0/' \
        "$D/libva-$VAV/va/va_version.h.in" > "$D/va/include/va/va_version.h"
    echo 'void vaInitialize(void) {}' | gcc -shared -fPIC -x c - -o "$D/va/lib/libva.so"
    echo 'void vaGetDisplayDRM(void) {}' | gcc -shared -fPIC -x c - -o "$D/va/lib/libva-drm.so"
    for n in libva libva-drm; do
        printf 'Name: %s\nDescription: placeholder for configure\nVersion: 1.14.0\nCflags: -I%s\nLibs: -L%s -l%s\n' \
            "$n" "$D/va/include" "$D/va/lib" "${n#lib}" > "$D/va/lib/pkgconfig/$n.pc"
    done
    touch "$D/.ready-$NVH-$VAV"
fi

# ---- FFmpeg, static, only the viewer's pieces (v0.7.13: + VAAPI and NVDEC)
FF="$L/ffmpeg-min"
if [ ! -f "$FF/lib/libavcodec.a" ] || [ ! -f "$FF/.hw-$NVH-$VAV" ]; then
    echo "== FFmpeg 8.1 (minimal, static, VAAPI + NVDEC)"
    rm -rf "$FF"
    [ -d "$REPO/ref/ffmpeg-8.1" ] || tar -xJf "$REPO/ref/ffmpeg-8.1.tar.xz" -C "$REPO/ref"
    B="$REPO/ref/ffmpeg-8.1/build-linux"
    rm -rf "$B"; mkdir -p "$B"; cd "$B"
    PKG_CONFIG_PATH="$D/ffnvcodec/lib/pkgconfig:$D/va/lib/pkgconfig" "$REPO/ref/ffmpeg-8.1/configure" \
        --prefix="$FF" --enable-static --disable-shared --enable-pic \
        --disable-everything --disable-autodetect --disable-programs --disable-doc \
        --disable-network --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
        --enable-decoder=h264 --enable-parser=h264 \
        --enable-encoder=aac \
        --enable-muxer=mp4 --enable-protocol=file \
        --enable-vaapi --enable-ffnvcodec --enable-cuda --enable-nvdec \
        --enable-hwaccel=h264_vaapi,h264_nvdec \
        --pkg-config-flags=--static \
        --enable-x86asm > configure.log 2>&1 \
        < /dev/null || { tail -30 configure.log; exit 1; }
    grep -q "h264_vaapi" configure.log && grep -q "h264_nvdec" configure.log || { echo "FFmpeg: no VAAPI/NVDEC hwaccel"; grep -E "^Enabled hwaccels" -A3 configure.log; exit 1; }
    make -j"${JOBS:-6}" > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make install > install.log 2>&1
    cp "$REPO/ref/ffmpeg-8.1/COPYING.LGPLv2.1" "$FF/"
    touch "$FF/.hw-$NVH-$VAV"
fi

# ---- SDL 2, static, backends loaded at runtime
SDL="$L/sdl2"
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    echo "== SDL $SDLV (static, dynamic backends)"
    [ -d "$L/SDL2-$SDLV" ] || tar -xzf "$REPO/ref/SDL2-$SDLV.tar.gz" -C "$L"
    B="$L/SDL2-$SDLV/build-portable"
    rm -rf "$B"; mkdir -p "$B"; cd "$B"
    ../configure --prefix="$SDL" --enable-static --disable-shared --with-pic \
        --enable-video-x11 --enable-x11-shared --enable-video-wayland --enable-wayland-shared \
        --enable-pulseaudio --enable-pulseaudio-shared --enable-alsa --enable-alsa-shared \
        --enable-libdecor --enable-libdecor-shared --enable-video-kmsdrm --enable-kmsdrm-shared \
        --disable-video-vulkan --disable-esd --disable-arts --disable-nas --disable-sndio --disable-jack \
        > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    grep -E "^(  )?(Video drivers|Audio drivers|Input drivers)|SDL2 Configure Summary" -A2 configure.log | head -20 || true
    make -j"${JOBS:-6}" > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make install > install.log 2>&1
    cp "$L/SDL2-$SDLV/LICENSE.txt" "$SDL/"
fi

# ---- the viewer
echo "== raw-view (portable)"
OUT="$REPO/dist/build-linux-portable"
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$HERE"
# v0.7.17: + include/ itself - raw-view.c includes <SDL2/SDL.h>, which only the
# system's own SDL2 headers answered (an older SDL's headers over the 2.32 library;
# a clean build host has none)
SDL_CFLAGS="-I$SDL/include $("$SDL/bin/sdl2-config" --cflags)"
SDL_LIBS=$("$SDL/bin/sdl2-config" --static-libs)
LIBUSB_A="$(pkg-config --variable=libdir libusb-1.0)/libusb-1.0.a"
[ -f "$LIBUSB_A" ] || { echo "no static libusb at $LIBUSB_A (install libusb-1.0-0-dev / libusb-devel)"; exit 1; }
python3 "$HERE/gen_va_shim.py" "$OUT/va_shim.c" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a"
gcc -O2 -Wall -Wextra -DSFT_H264 -DSFT_MP4 -DSFT_VA_SHIM -DSFT_VERSION="\"$VER\"" \
    -I"$FF/include" $SDL_CFLAGS $(pkg-config --cflags libusb-1.0) \
    -o "$OUT/switch-frame-tap-viewer" raw-view.c "$OUT/va_shim.c" \
    "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" \
    $SDL_LIBS \
    "$LIBUSB_A" -ludev -lpthread -lm -ldl
strip "$OUT/switch-frame-tap-viewer"
echo "   needs from the system: $(readelf -d "$OUT/switch-frame-tap-viewer" | awk '/NEEDED/{gsub(/[\[\]]/,"",$5); printf "%s ", $5}')"
echo "   glibc: up to $(objdump -T "$OUT/switch-frame-tap-viewer" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)"

# ---- the package
P="$OUT/switch-frame-tap-$VER-linux"
mkdir -p "$P/licenses"
cp "$OUT/switch-frame-tap-viewer" "$HERE/70-switch-frame-tap.rules" "$HERE/switch-frame-tap.svg" "$P/"
cp "$REPO/LICENSE" "$P/licenses/switch-frame-tap-GPL-2.0.txt"
cp "$REPO/third_party/fsr1/license.txt" "$P/licenses/AMD-FidelityFX-FSR1-MIT.txt"; cp "$REPO/third_party/nis/licence.txt" "$P/licenses/NVIDIA-Image-Scaling-MIT.txt"   # v0.7.11: the upscalers' shaders
cp "$FF/COPYING.LGPLv2.1" "$P/licenses/FFmpeg-LGPL-2.1.txt"
cp "$SDL/LICENSE.txt" "$P/licenses/SDL2-zlib.txt"
cp /usr/share/doc/libusb-1.0-0/copyright "$P/licenses/libusb-LGPL-2.1.txt" 2>/dev/null || true
cat > "$P/install.sh" <<'SH'
#!/bin/sh
# Switch Frame Tap viewer: install for this user (no root needed), then the
# USB rule once (root) so the viewer may open the Switch.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BIN="$HOME/.local/bin/switch-frame-tap-viewer"
ICON="$HOME/.local/share/icons/hicolor/scalable/apps/switch-frame-tap.svg"
APPS="$HOME/.local/share/applications/switch-frame-tap.desktop"
refresh_menu() {
    update-desktop-database "$(dirname "$APPS")" 2>/dev/null || true
    gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental 2>/dev/null || true
}
if [ "$1" = "--remove" ]; then
    rm -f "$BIN" "$ICON" "$APPS"
    refresh_menu
    echo "removed"
    exit 0
fi
mkdir -p "$(dirname "$BIN")" "$(dirname "$ICON")" "$(dirname "$APPS")"
install -m 755 "$D/switch-frame-tap-viewer" "$BIN"
install -m 644 "$D/switch-frame-tap.svg" "$ICON"
cat > "$APPS" <<DESK
[Desktop Entry]
Type=Application
Name=Switch Frame Tap
Comment=Watch and record your Switch, streamed over USB or the network
Exec=$BIN --app
PrefersNonDefaultGPU=true
Icon=switch-frame-tap
Terminal=false
Categories=Game;
StartupNotify=true
X-KDE-StartupNotify=true
Keywords=Switch;Nintendo;stream;viewer;game;
DESK
chmod 755 "$APPS"
refresh_menu
echo "installed: $BIN (and Switch Frame Tap in the applications menu)"
echo "check:     $BIN --check"
echo "log:       ${XDG_STATE_HOME:-$HOME/.local/state}/switch-frame-tap/viewer.log"
if [ -f /etc/udev/rules.d/99-switch-frame-tap.rules ]; then
    echo
    echo "The USB rule has an old name that is applied too late (LIBUSB_ERROR_ACCESS); once:"
    echo "  sudo mv /etc/udev/rules.d/99-switch-frame-tap.rules /etc/udev/rules.d/70-switch-frame-tap.rules && sudo udevadm control --reload-rules && sudo udevadm trigger"
elif [ ! -f /etc/udev/rules.d/70-switch-frame-tap.rules ]; then
    echo
    echo "Once, so the viewer may open the Switch over USB (asks for your password):"
    echo "  sudo cp \"$D/70-switch-frame-tap.rules\" /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger"
fi
SH
chmod +x "$P/install.sh"
cat > "$P/README.txt" <<TXT
Switch Frame Tap $VER - the Linux viewer, for any distribution (x86_64)

  ./install.sh

installs it for your user (the applications menu: Switch Frame Tap) and shows
the one command, with sudo, that lets it open the Switch over USB. Or run it
from here: ./switch-frame-tap-viewer --app

  ./install.sh --remove     uninstall it again

KDE Plasma: the launcher uses Wayland natively; if the window ever freezes,
run "switch-frame-tap-viewer --app" in a terminal to see the reason and the
log in ~/.local/state/switch-frame-tap/viewer.log. "switch-frame-tap-viewer
--check" prints the session, video driver, audio driver, udev rule, USB
devices and network status.

One binary: FFmpeg (H.264, AAC, MP4), SDL 2 and libusb are built in. It
uses your system's glibc (2.35 or newer), libudev, and its X11 or Wayland
and PulseAudio/PipeWire for the window and the sound.
https://github.com/TomasUribe/switch-frame-tap
TXT
tar -C "$OUT" -czf "$REPO/dist/switch-frame-tap-$VER-linux.tar.gz" "switch-frame-tap-$VER-linux"
echo "== $REPO/dist/switch-frame-tap-$VER-linux.tar.gz"
tar -tzf "$REPO/dist/switch-frame-tap-$VER-linux.tar.gz"
