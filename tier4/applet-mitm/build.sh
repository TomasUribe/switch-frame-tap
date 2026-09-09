#!/usr/bin/env bash
# Build the standalone mitm module inside a copy of the Atmosphere tree.
set -e
REPO=/home/tomas/switch-stream-project
AMS=$REPO/ref/Atmosphere
SRC=$REPO/tier4/applet-mitm
DST=$AMS/stratosphere/applet-mitm
IMG=devkitpro/devkita64:latest
UIDGID="$(id -u):$(id -g)"

mkdir -p "$DST"
# docker builds as root; reclaim ownership before touching the tree
docker run --rm -v "$AMS":/ams "$IMG" chown -R "$UIDGID" /ams/stratosphere/applet-mitm >/dev/null 2>&1 || true

# tier4 libstratosphere patch (idempotent): non-domain mitm sub-object forwarding
python3 "$SRC/patch_libstrat.py" "$AMS"

rsync -a --delete \
  --exclude build.sh --exclude '*.nsp' --exclude patch_libstrat.py --exclude out/ --exclude build/ \
  "$SRC"/ "$DST"/

set +e
docker run --rm -v "$AMS":/ams -w /ams/stratosphere/applet-mitm "$IMG" bash -lc 'make nx_release 2>&1'
rc=$?
set -e

docker run --rm -v "$AMS":/ams "$IMG" chown -R "$UIDGID" /ams/stratosphere/applet-mitm >/dev/null 2>&1 || true
find "$DST/out" -name '*.nsp' -exec cp {} "$SRC"/ \; 2>/dev/null || true

if [ $rc -eq 0 ] && ls "$SRC"/*.nsp >/dev/null 2>&1; then
  ls -l "$SRC"/*.nsp; echo OK
else
  echo "BUILD FAILED (rc=$rc)"; exit 1
fi
