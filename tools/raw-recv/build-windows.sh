#!/usr/bin/env bash
# Cross-compile the viewer for Windows (x86_64) and pack the release zip:
#
#   bash tools/raw-recv/build-windows.sh 0.2.0   -> dist/switch-frame-tap-0.2.0-windows-viewer.zip
#
# Needs, under ref/win (not committed; see ref/README.md):
#   zig/     Zig 0.16 (ziglang.org) - the C cross-compiler, no sudo needed
#   sdl2/    SDL2-devel-2.32.10-mingw (github.com/libsdl-org/SDL releases)
#   libusb/  libusb-1.0.30.7z, unpacked (github.com/libusb/libusb releases)
#   ffmpeg/  ffmpeg-n8.1-latest-win64-lgpl-shared-8.1 (github.com/BtbN/FFmpeg-Builds)
#
# The .exe is a GUI program (no console window) that starts in raw-view's
# --app mode when double-clicked. Windows needs the WinUSB driver bound to the
# Switch once (Zadig) - README-WINDOWS.txt in the zip says how.
set -e
VER="${1:?usage: build-windows.sh VERSION}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
W="$REPO/ref/win"
ZIG="$W/zig/zig"
OUT="$REPO/dist"
B="$OUT/build-windows"
for d in zig sdl2 libusb ffmpeg; do [ -d "$W/$d" ] || { echo "ref/win/$d is missing (see the comment at the top of this script)"; exit 1; }; done

rm -rf "$B"
mkdir -p "$B"

# the icon (from the same SVG as the Linux launcher) and the version resource
convert -background none -density 384 "$REPO/tools/raw-recv/switch-frame-tap.svg" -define icon:auto-resize=256,64,48,32,16 "$B/switch-frame-tap.ico"
IFS=. read -r V1 V2 V3 <<< "$VER"
cat > "$B/raw-view.rc" <<RC
1 ICON "switch-frame-tap.ico"
1 VERSIONINFO
FILEVERSION $V1,$V2,$V3,0
PRODUCTVERSION $V1,$V2,$V3,0
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "switch-frame-tap"
      VALUE "FileDescription", "Switch Frame Tap - viewer"
      VALUE "FileVersion", "$VER"
      VALUE "ProductName", "Switch Frame Tap"
      VALUE "ProductVersion", "$VER"
      VALUE "LegalCopyright", "GPL-2.0 - github.com/TomasUribe/switch-frame-tap"
      VALUE "OriginalFilename", "SwitchFrameTap.exe"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC

# v0.6.1: FFmpeg built in - ref/win/ffmpeg-min from build-ffmpeg-min.sh (only
# the H.264 decoder, the AAC encoder and the MP4 muxer, statically linked);
# without it, the full prebuilt DLLs as before
MIN="$W/ffmpeg-min"
if [ -f "$MIN/lib/libavcodec.a" ]; then
    FF_INC="$MIN/include"
    FF_LIBS=("$MIN/lib/libavformat.a" "$MIN/lib/libavcodec.a" "$MIN/lib/libavutil.a" -lbcrypt)
    FF_DLLS=()
    echo "== FFmpeg: built in (ref/win/ffmpeg-min)"
else
    FF_INC="$W/ffmpeg/include"
    FF_LIBS=("$W/ffmpeg/lib/libavcodec.dll.a" "$W/ffmpeg/lib/libavutil.dll.a" "$W/ffmpeg/lib/libavformat.dll.a")
    FF_DLLS=("$W/ffmpeg/bin/avcodec-62.dll" "$W/ffmpeg/bin/avutil-60.dll" "$W/ffmpeg/bin/avformat-62.dll" "$W/ffmpeg/bin/swresample-6.dll")
    echo "== FFmpeg: the prebuilt DLLs (run build-ffmpeg-min.sh for a small download)"
fi

echo "== compiling"
"$ZIG" cc -target x86_64-windows-gnu -O2 -Wall -Wextra -DSFT_H264 -DSFT_MP4 \
    -I "$W/sdl2/x86_64-w64-mingw32/include" -I "$W/libusb/include" -I "$FF_INC" \
    "$REPO/tools/raw-recv/raw-view.c" "$B/raw-view.rc" \
    "$W/sdl2/x86_64-w64-mingw32/lib/libSDL2.dll.a" \
    "$W/libusb/MinGW64/static/libusb-1.0.dll.a" \
    "${FF_LIBS[@]}" \
    -lshell32 \
    -Wl,--subsystem,windows \
    -o "$B/SwitchFrameTap.exe"

PKG="$B/switch-frame-tap-$VER-windows-viewer"
mkdir -p "$PKG/licenses"
cp "$B/SwitchFrameTap.exe" "$PKG/"
cp "$W/sdl2/x86_64-w64-mingw32/bin/SDL2.dll" "$W/libusb/MinGW64/dll/libusb-1.0.dll" "$PKG/"
[ ${#FF_DLLS[@]} -gt 0 ] && cp "${FF_DLLS[@]}" "$PKG/"
cp "$REPO/LICENSE" "$PKG/licenses/switch-frame-tap-GPL-2.0.txt"
cp "$W/sdl2/LICENSE.txt" "$PKG/licenses/SDL2-zlib.txt"
if [ ${#FF_DLLS[@]} -gt 0 ]; then cp "$W/ffmpeg/LICENSE.txt" "$PKG/licenses/FFmpeg-LGPL.txt"; else cp "$MIN/COPYING.LGPLv2.1" "$PKG/licenses/FFmpeg-LGPL-2.1.txt"; fi
cp /usr/share/doc/fonts-dejavu-core/copyright "$PKG/licenses/DejaVu-fonts.txt"
cp /usr/share/common-licenses/LGPL-2.1 "$PKG/licenses/libusb-LGPL-2.1.txt"
if [ ${#FF_DLLS[@]} -gt 0 ]; then
FF_NOTE="  avcodec-62.dll,
  avformat-62.dll,
  avutil-60.dll,
  swresample-6.dll        FFmpeg 8.1, LGPL build (BtbN FFmpeg-Builds, ffmpeg-n8.1-latest-win64-lgpl-shared-8.1)
                                    LGPL-2.1+      https://ffmpeg.org  https://github.com/BtbN/FFmpeg-Builds
The LGPL libraries' source is at those addresses; being DLLs, they can be
replaced with other builds of the same version."
else
FF_NOTE="FFmpeg 8.1 (libavcodec, libavformat, libavutil; LGPL-2.1+, https://ffmpeg.org) is
built into SwitchFrameTap.exe, configured with only the H.264 decoder, the AAC
encoder and the MP4 muxer (tools/raw-recv/build-ffmpeg-min.sh in the source).
FFmpeg's source is at https://ffmpeg.org/releases/ffmpeg-8.1.tar.xz; this
program's full source, with the build scripts to relink it against another
FFmpeg, is at the address above (GPL-2.0)."
fi
cat > "$PKG/licenses/NOTICE.txt" <<TXT
Switch Frame Tap $VER - Windows viewer. GPL-2.0, https://github.com/TomasUribe/switch-frame-tap

It ships these libraries, unmodified, as DLLs:
  SDL2.dll                2.32.10   zlib license   https://github.com/libsdl-org/SDL
  libusb-1.0.dll          1.0.30    LGPL-2.1       https://github.com/libusb/libusb

$FF_NOTE

The menu's text is drawn from DejaVu Sans and DejaVu Sans Bold glyphs built
into SwitchFrameTap.exe (Bitstream Vera licence, DejaVu changes public domain;
DejaVu-fonts.txt).
TXT
cat > "$PKG/README-WINDOWS.txt" <<'TXT'
Switch Frame Tap - Windows viewer
=================================

Watch and play your Switch on this PC over a USB-C cable.

ONE-TIME SETUP: the USB driver
------------------------------
Windows needs the generic WinUSB driver for the Switch before any program can
talk to it. Do this once:

  1. On the Switch: install Switch Frame Tap (the Switch zip from the same
     release), reboot, and connect the Switch (handheld) to this PC with the
     USB-C cable.
  2. Download Zadig: https://zadig.akeo.ie/  and run it.
  3. Options -> List All Devices.
  4. Pick "Switch Frame Tap" in the list (USB ID 1209 5F1E).
     Make sure it is that one - not your mouse, keyboard or anything else.
  5. Set the driver on the right to "WinUSB" and click "Install Driver"
     (or "Replace Driver"). Wait for it to finish.

Only Switch Frame Tap's USB ID gets the driver; nothing else on the PC
changes. Tip: the Switch only shows up in Zadig while the sysmodule is running
(a few seconds after the console boots).

USING IT
--------
Double-click SwitchFrameTap.exe. The window shows what it is waiting for and
the keys, and shows the game as soon as one is running. It reconnects by itself when the Switch
reboots or the cable is unplugged. F11 or a double-click: fullscreen. Esc
leaves fullscreen or closes the window. M mutes the game's sound.

RECORDING
---------
Press R to record, R again to stop. The recording starts at the next
keyframe (within a second) and is saved as an MP4 in your Videos folder,
under "Switch Frame Tap": the console's own H.264 picture, untouched, and the
game's sound. A red dot shows while it records. 1080p at High quality is about 10 MB a second
(~600 MB a minute); it stops by itself if the disk gets nearly full.

If the title bar says the Switch "has no USB driver", do the setup above.

Keep the DLLs next to SwitchFrameTap.exe.

More: https://github.com/TomasUribe/switch-frame-tap
TXT
(cd "$B" && rm -f "$OUT/switch-frame-tap-$VER-windows-viewer.zip" && zip -qr "$OUT/switch-frame-tap-$VER-windows-viewer.zip" "switch-frame-tap-$VER-windows-viewer")
echo "== $OUT/switch-frame-tap-$VER-windows-viewer.zip"
unzip -l "$OUT/switch-frame-tap-$VER-windows-viewer.zip" | tail -n +1
