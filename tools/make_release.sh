#!/usr/bin/env bash
# Build every Switch-side piece and pack the SD-card zip for a release:
#
#   bash tools/make_release.sh 0.1.0      -> dist/switch-frame-tap-0.1.0-switch.zip
#
# The zip unpacks onto the root of the SD card. It carries no config.ini, so
# an update never overwrites the user's settings (the sysmodule's defaults
# are the manager's defaults), and no test arm file.
set -e
VER="${1:?usage: make_release.sh VERSION}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG=devkitpro/devkita64:latest
OUT="$REPO/dist"
STAGE="$OUT/stage-switch"

[ -f "$REPO/ref/libtesla/include/tesla.hpp" ] || { echo "ref/libtesla is missing (git clone https://github.com/WerWolv/libtesla ref/libtesla)"; exit 1; }

echo "== sysmodule"
MAKEFLAGS=-j8 bash "$REPO/tier4/applet-mitm/build.sh" >/dev/null
echo "== overlay and manager"
for d in overlay manager; do
    docker run --rm -u "$(id -u):$(id -g)" -v "$REPO":/w -w "/w/$d" "$IMG" bash -lc 'make -j8' >/dev/null
done

rm -rf "$STAGE"
mkdir -p "$STAGE/atmosphere/contents/0100000000000C20/flags" "$STAGE/switch/.overlays" "$STAGE/switch/switch-frame-tap"
cp "$REPO/tier4/applet-mitm/applet-mitm.nsp" "$STAGE/atmosphere/contents/0100000000000C20/exefs.nsp"
: > "$STAGE/atmosphere/contents/0100000000000C20/flags/boot2.flag"
# lets sysmodule managers (ovl-sysmodules and the like) list it by name
cat > "$STAGE/atmosphere/contents/0100000000000C20/toolbox.json" <<JSON
{
    "name": "Switch Frame Tap",
    "tid": "0100000000000C20",
    "requires_reboot": true
}
JSON
cp "$REPO/overlay/switch-frame-tap.ovl" "$STAGE/switch/.overlays/"
cp "$REPO/manager/switch-frame-tap.nro" "$STAGE/switch/switch-frame-tap/"

ZIP="$OUT/switch-frame-tap-$VER-switch.zip"
rm -f "$ZIP"
(cd "$STAGE" && zip -qr "$ZIP" atmosphere switch)
rm -rf "$STAGE"
echo "== $ZIP"
unzip -l "$ZIP"

# the Linux viewer: its source, the udev rule and the desktop-app installer,
# laid out as in the repo so the README's commands work from the unpacked folder
echo "== linux viewer"
make -s -B -C "$REPO/tools/raw-recv" raw-view | grep -q "WITH H.264"
LSTAGE="$OUT/stage-linux/switch-frame-tap-$VER-linux-viewer"
rm -rf "$OUT/stage-linux"
mkdir -p "$LSTAGE/tools/raw-recv"
for f in raw-view.c raw-view Makefile 99-switch-frame-tap.rules install-launcher.sh switch-frame-tap.svg README.md; do
    cp "$REPO/tools/raw-recv/$f" "$LSTAGE/tools/raw-recv/"
done
cp "$REPO/LICENSE" "$LSTAGE/"
cat > "$LSTAGE/README.txt" <<TXT
Switch Frame Tap $VER - the Linux viewer

  sudo apt install build-essential libusb-1.0-0-dev libsdl2-dev libavcodec-dev libavutil-dev
  sudo cp tools/raw-recv/99-switch-frame-tap.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  bash tools/raw-recv/install-launcher.sh

Then open "Switch Frame Tap" from the desktop or the applications menu.
tools/raw-recv/raw-view is a prebuilt binary (x86_64, Ubuntu 22.04); the
installer rebuilds it from raw-view.c for your system.
https://github.com/TomasUribe/switch-frame-tap
TXT
TGZ="$OUT/switch-frame-tap-$VER-linux-viewer.tar.gz"
tar -C "$OUT/stage-linux" -czf "$TGZ" "switch-frame-tap-$VER-linux-viewer"
rm -rf "$OUT/stage-linux"
echo "== $TGZ"
tar -tzf "$TGZ"
