#!/bin/bash
# v0.7.17: the Linux viewer as an AppImage - one file to download and run,
# made from the portable build (build-linux-portable.sh: FFmpeg, SDL 2 and
# libusb built in; glibc 2.35+ and libudev from the system).
#
# Needs: dist/build-linux-portable/switch-frame-tap-viewer (run
# build-linux-portable.sh VERSION first) and appimagetool ($APPIMAGETOOL, or
# appimagetool on the PATH; without FUSE set APPIMAGE_EXTRACT_AND_RUN=1).
#
#   bash tools/raw-recv/build-appimage.sh 0.7.17
#     -> dist/switch-frame-tap-0.7.17-linux-x86_64.AppImage
set -euo pipefail
VER="${1:?usage: build-appimage.sh VERSION}"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$REPO/tools/raw-recv"
BIN="$REPO/dist/build-linux-portable/switch-frame-tap-viewer"
TOOL="${APPIMAGETOOL:-appimagetool}"
[ -x "$BIN" ] || { echo "no $BIN - run build-linux-portable.sh $VER first"; exit 1; }
command -v "$TOOL" >/dev/null 2>&1 || [ -x "$TOOL" ] || { echo "no appimagetool (set APPIMAGETOOL)"; exit 1; }

APP="$REPO/dist/build-appimage/SwitchFrameTap.AppDir"
rm -rf "$APP"; mkdir -p "$APP/usr/bin" "$APP/usr/share/licenses" "$APP/usr/share/switch-frame-tap"
cp "$BIN" "$APP/usr/bin/"
cp "$HERE/switch-frame-tap.svg" "$APP/switch-frame-tap.svg"
cp "$HERE/70-switch-frame-tap.rules" "$APP/usr/share/switch-frame-tap/"
# the portable package's licences (FFmpeg, SDL 2, libusb, the upscalers)
cp -r "$REPO/dist/build-linux-portable/switch-frame-tap-$VER-linux/licenses/." "$APP/usr/share/licenses/" 2>/dev/null || cp "$REPO/LICENSE" "$APP/usr/share/licenses/switch-frame-tap-GPL-2.0.txt"

# double-clicked, it is the desktop app (--app: waits for the Switch, reconnects,
# keeps a log); with arguments (--check, --update) it passes them on
cat > "$APP/AppRun" <<'SH'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
[ $# -eq 0 ] && set -- --app
exec "$HERE/usr/bin/switch-frame-tap-viewer" "$@"
SH
chmod 755 "$APP/AppRun"

cat > "$APP/switch-frame-tap.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Switch Frame Tap
Comment=Watch and record your Switch, streamed over USB or the network
Exec=switch-frame-tap-viewer --app
PrefersNonDefaultGPU=true
Icon=switch-frame-tap
Terminal=false
Categories=Game;
StartupNotify=true
Keywords=Switch;Nintendo;stream;viewer;game;
X-AppImage-Version=$VER
DESK

OUT="$REPO/dist/switch-frame-tap-$VER-linux-x86_64.AppImage"
rm -f "$OUT"
ARCH=x86_64 "$TOOL" --no-appstream "$APP" "$OUT" > "$REPO/dist/build-appimage/appimagetool.log" 2>&1 || { tail -20 "$REPO/dist/build-appimage/appimagetool.log"; exit 1; }
chmod 755 "$OUT"
echo "== $OUT ($(du -h "$OUT" | cut -f1))"
