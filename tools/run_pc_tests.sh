#!/usr/bin/env bash
# run_pc_tests.sh - every check this project can run without a console.
#
#   bash tools/run_pc_tests.sh
#
# Needs: python3 with av (PyAV), numpy, pillow; a C/C++ compiler; and for the
# receiver test libusb-1.0, SDL2 and libavcodec development packages
#   (Debian/Ubuntu: libusb-1.0-0-dev libsdl2-dev libavcodec-dev libavutil-dev).
# The module itself builds separately: MAKEFLAGS=-j$(nproc) bash tier4/applet-mitm/build.sh
set -u
cd "$(dirname "$0")/.."
fail=0
run() {
    local name="$1"; shift
    local out
    if out=$("$@" 2>&1); then
        printf '  PASS  %s\n' "$name"
    else
        printf '  FAIL  %s\n%s\n' "$name" "$(printf '%s\n' "$out" | tail -15)"
        fail=1
    fi
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "switch-frame-tap PC-side checks"
run "arm-file parser (whole tokens, numbers)" \
    bash -c "g++ -std=c++20 -Itier4/applet-mitm/source tier4/applet-mitm/test/armfile_test.cpp -o $tmp/armfile_test && $tmp/armfile_test"
run "generated headers are current (nvenc_replay gen is a no-op)" \
    bash -c "python3 tools/nvenc_replay.py gen >/dev/null && python3 tools/vic_csc.py gen >/dev/null && git diff --quiet -- tier4/applet-mitm/source/nvenc_grc_idr.h tier4/applet-mitm/source/nvenc_grc_hdrs.h tier4/applet-mitm/source/nvenc_grc_p.h tier4/applet-mitm/source/vic_csc_bt709.h"
run "nvenc_replay: SPS/PPS writer, stream headers, Run F check path" python3 tools/nvenc_replay.py selftest
run "vic_csc: the VIC matrix law, BT.709/601 designs, Run H verifies" python3 tools/vic_csc.py selftest
run "vic_csc: Run H's 528 values under the law" python3 tools/vic_csc.py verify logs/m82-runH-vic-csc.bin
run "nvframe_check: layouts, colour, NVENC-read diagnosis" python3 tools/nvframe_check.py selftest
run "nvp_check: IDR + P GOP, drift test" python3 tools/nvp_check.py selftest
run "raw-view builds (with H.264)" bash -c "make -s -B -C tools/raw-recv raw-view | grep -q 'WITH H.264'"
run "stream end to end: console packets -> raw-view -> H.264" python3 tools/sft_stream_test.py
run "sft_tool: recording stats, gaps, decode check, head, drift test" python3 tools/sft_tool.py selftest
run "nvrec: Run E's grc scan decodes as committed" \
    bash -c "python3 tools/nvrec.py logs/m79-runE-grc-scan.bin --out $tmp/setups > $tmp/scan.txt && diff <(grep -v -- '-> ' logs/m79-runE-grc-scan.txt | sed 's/[[:space:]]*\$//') <(grep -v -- '-> ' $tmp/scan.txt | sed 's/[[:space:]]*\$//') >/dev/null"
if [ $fail -eq 0 ]; then echo "all PC-side checks passed"; else echo "SOME CHECKS FAILED"; fi
exit $fail
