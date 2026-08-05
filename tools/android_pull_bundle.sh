#!/usr/bin/env bash
# Pull a capture bundle off a device and run it through ffreplay.
#
# Usage:  tools/android_pull_bundle.sh [serial] [dest_dir]
#
# The bundle is pulled OUTSIDE the repository by default. Raw capture sets are large and
# docs/testing/CAPTURE-HARNESS.md keeps them out of git deliberately; only derived reports are
# committed.
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
SERIAL="${1:-}"
OUT="${2:-/tmp/fileflow-captures}"

adbx() {
    if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi
}

cd "$(dirname "$0")/.." || exit 1
mkdir -p "$OUT"
rm -rf "$OUT/capture-bundle"

echo "=== pulling bundle (app-private, so via run-as + tar) ==="
adbx exec-out run-as dev.fileflow tar c -C files capture-bundle 2>/dev/null > "$OUT/bundle.tar"
wc -c "$OUT/bundle.tar"
tar xf "$OUT/bundle.tar" -C "$OUT" || { echo "tar extract failed"; exit 1; }
rm -f "$OUT/bundle.tar"

FRAMES=$(ls "$OUT/capture-bundle/frames" 2>/dev/null | wc -l | tr -d ' ')
echo "frames: $FRAMES"
echo
echo "--- capture.meta ---"
cat "$OUT/capture-bundle/capture.meta" 2>/dev/null

echo
echo "=== per-frame cadence ==="
adbx exec-out run-as dev.fileflow cat files/capture-frames.csv 2>/dev/null > "$OUT/capture-frames.csv"
if [ -s "$OUT/capture-frames.csv" ]; then
    python3 tools/frame_cadence.py "$OUT/capture-frames.csv" 60
fi

echo
echo "=== ffreplay through the production decode chain ==="
./build/desktop-release/harness/ffreplay "$OUT/capture-bundle" 2>&1 | head -80
