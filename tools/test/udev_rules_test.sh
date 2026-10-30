#!/usr/bin/env bash
# udev_rules_test.sh - every udev rule that tags uaccess sorts before
# systemd's 73-seat-late.rules (which turns the tag into the user's ACL), and
# nothing tells the user to install the old 99- name. A 99-switch-frame-tap.rules
# (v0.7.19 and older) left the Switch root-only after a replug: LIBUSB_ERROR_ACCESS.
set -u
cd "$(dirname "$0")/../.."
fail=0
while IFS= read -r f; do
    b=$(basename "$f")
    n=${b%%-*}
    if ! [[ $n =~ ^[0-9]+$ ]] || [ "$n" -ge 73 ]; then
        echo "FAIL: $f tags uaccess but sorts after 73-seat-late.rules"; fail=1
    fi
done < <(grep -rl --include='*.rules' 'uaccess' tools switch-stream flatpak 2>/dev/null)
# instructions/scripts must name the 70- rule (the 99- name only as an old copy to move)
if grep -rn --exclude-dir=.git --exclude-dir=third_party --exclude-dir=test -E '(cp [^ ]*|tee [^ ]*)99-switch-frame-tap\.rules' README.md flatpak tools >/dev/null; then
    echo "FAIL: something still installs 99-switch-frame-tap.rules"; fail=1
fi
[ -f tools/raw-recv/70-switch-frame-tap.rules ] || { echo "FAIL: tools/raw-recv/70-switch-frame-tap.rules missing"; fail=1; }
exit $fail
