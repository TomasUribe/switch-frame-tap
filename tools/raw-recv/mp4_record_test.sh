#!/bin/bash
# v0.5: the viewer's MP4 recording, end to end without a console: a real
# console recording (logs/m83-runI, 10 H.264 frames) with a 440 Hz tone added
# as game audio before each frame, replayed through raw-view --mp4, then
# checked by mp4-check: every frame decodes, the audio is AAC at 440 Hz.
set -e
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 - "$tmp/tone.sft" <<'PY'
import math, struct, sys
src = open('logs/m83-runI/stream-first10.sft', 'rb').read()
out = bytearray(); o = 0; n = 0
while o + 32 <= len(src):
    hdr = src[o:o + 32]; length = struct.unpack_from('<I', hdr, 20)[0]
    pcm = b''.join(struct.pack('<hh', v, v) for v in
                   (int(8000 * math.sin(2 * math.pi * 440 * (n * 800 + k) / 48000)) for k in range(800)))
    n += 1
    out += struct.pack('<IHHIIIIII', 0x52544653, 2, 8, 48000, 2, 0, len(pcm), 0, n) + pcm
    out += src[o:o + 32 + length]; o += 32 + length
open(sys.argv[1], 'wb').write(out)
PY
make -s -C tools/raw-recv raw-view mp4-check >/dev/null
tools/raw-recv/raw-view --file "$tmp/tone.sft" --headless --mp4 "$tmp/out.mp4" >/dev/null 2>&1
tools/raw-recv/mp4-check "$tmp/out.mp4" --expect-tone 440 | tee "$tmp/check.txt" | tail -1 | grep -qx OK
grep -q "stream 0: 10 packets, 10 frames decoded, 0 errors" "$tmp/check.txt"
