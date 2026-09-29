#!/usr/bin/env bash
# Install the viewer as a desktop app: a double-clickable "Switch Frame Tap"
# on the desktop and in the applications menu. It opens a window at once,
# waits for the Switch, reconnects when it comes back, and closes only when
# you close it (raw-view --app). F11 or a double-click: fullscreen.
#
#   bash tools/raw-recv/install-launcher.sh            # install / update
#   bash tools/raw-recv/install-launcher.sh --remove   # uninstall
#
# Needs the viewer's build dependencies and the udev rule (README, quick start).
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HOME/.local/bin/switch-frame-tap-viewer"
ICON="$HOME/.local/share/icons/hicolor/scalable/apps/switch-frame-tap.svg"
APPS="$HOME/.local/share/applications/switch-frame-tap.desktop"
DESK="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")/switch-frame-tap.desktop"

if [ "$1" = "--remove" ]; then
    rm -f "$BIN" "$ICON" "$APPS" "$DESK"
    echo "removed"
    exit 0
fi

make -C "$HERE" raw-view >/dev/null
grep -q "raw-view built WITH H.264" < <(make -C "$HERE" -B raw-view 2>&1) || { echo "raw-view has no H.264 decoder - install libavcodec-dev (README)"; exit 1; }
[ -f /etc/udev/rules.d/99-switch-frame-tap.rules ] || echo "note: the udev rule is not installed - the viewer may not be able to open the Switch (README, quick start step 2)"

mkdir -p "$(dirname "$BIN")" "$(dirname "$ICON")" "$(dirname "$APPS")" "$(dirname "$DESK")"
install -m 755 "$HERE/raw-view" "$BIN"
install -m 644 "$HERE/switch-frame-tap.svg" "$ICON"
cat > "$APPS" <<EOF
[Desktop Entry]
Type=Application
Name=Switch Frame Tap
Comment=Watch and play your Switch on this PC over USB
Exec=$BIN --app
Icon=$ICON
Terminal=false
Categories=Game;
StartupNotify=true
EOF
chmod 755 "$APPS"
install -m 755 "$APPS" "$DESK"
# GNOME's desktop only launches files marked trusted ("Allow Launching")
gio set "$DESK" metadata::trusted true 2>/dev/null || true
update-desktop-database "$(dirname "$APPS")" 2>/dev/null || true
echo "installed: $BIN"
echo "launcher:  $DESK (and in the applications menu)"
