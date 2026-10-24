#!/usr/bin/env bash
# Build the standalone mitm module inside a copy of the Atmosphere tree.
set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# AMS_TREE picks another Atmosphere checkout (v0.7.11: ref/Atmosphere-1.12.0 for
# firmware 23); its .nsp is then named after it, next to the default build
AMS="${AMS_TREE:-$REPO/ref/Atmosphere}"
case "$AMS" in /*) ;; *) AMS="$REPO/$AMS" ;; esac
TAG="${AMS_TREE:+-$(basename "$AMS")}"
SRC=$REPO/tier4/applet-mitm
DST=$AMS/stratosphere/applet-mitm
IMG="${DKP_IMG:-devkitpro/devkita64:latest}"
UIDGID="$(id -u):$(id -g)"
# rootless podman (also as "docker", podman-docker): the container's root IS
# this user, so the files are ours already - a chown to our uid inside would
# hand them to a sub-uid instead (permission denied on the next run)
OWN() { docker run --rm -v "$AMS":/ams "$IMG" chown -R "$UIDGID" /ams/stratosphere/applet-mitm >/dev/null 2>&1 || true; }
if docker --version 2>/dev/null | grep -qi podman; then OWN() { :; }; fi

mkdir -p "$DST"
# docker builds as root; reclaim ownership before touching the tree
OWN

# tier4 libstratosphere patch (idempotent): non-domain mitm sub-object forwarding
python3 "$SRC/patch_libstrat.py" "$AMS"

# by content, and a changed file gets the time of the copy: with the sources'
# own times an older checkout (another branch) kept the newer objects of the
# last build, and the link failed or, worse, used them
rsync -rlpgoD --checksum --delete \
  --exclude build.sh --exclude '*.nsp' --exclude patch_libstrat.py --exclude out/ --exclude build/ --exclude test/ \
  "$SRC"/ "$DST"/

set +e
docker run --rm -e MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}" -v "$AMS":/ams -w /ams/stratosphere/applet-mitm "$IMG" bash -lc 'make nx_release 2>&1'
rc=$?
set -e

OWN
NSP="$(find "$DST/out" -name '*.nsp' | head -1)"
[ -n "$NSP" ] && cp "$NSP" "$SRC/applet-mitm$TAG.nsp"

if [ $rc -eq 0 ] && [ -f "$SRC/applet-mitm$TAG.nsp" ]; then
  ls -l "$SRC/applet-mitm$TAG.nsp"; echo OK
else
  echo "BUILD FAILED (rc=$rc)"; exit 1
fi
