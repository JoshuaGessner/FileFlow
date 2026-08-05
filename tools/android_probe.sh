#!/usr/bin/env bash
# Run the C02 capability probe on a device and print its report (EXP-007 enumeration half).
#
# Usage:  tools/android_probe.sh [serial]
#
# `serial` targets one device when several are attached (`adb devices` lists them). With two devices
# connected and no serial, adb refuses rather than guessing — which is the behaviour we want.
#
# Deletes the previous report before launching, for the reason F28 records.
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
SERIAL="${1:-}"

# A function rather than an array: macOS ships bash 3.2, where expanding an empty array under
# `set -u` is an error rather than an empty list.
adbx() {
    if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi
}

APK="app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK" ]; then
    echo "no APK at $APK — build with: ./gradlew :app:assembleDebug" >&2
    exit 1
fi

adbx install -r -t "$APK" 2>&1 | tail -1
adbx shell am force-stop dev.fileflow
adbx shell run-as dev.fileflow rm -f files/probe-report.txt 2>/dev/null
if adbx shell run-as dev.fileflow test -f files/probe-report.txt 2>/dev/null; then
    echo "FATAL: could not delete the old report; refusing to run and risk reporting it" >&2
    exit 1
fi

adbx shell am start -n dev.fileflow/.ProbeActivity >/dev/null 2>&1
for _ in $(seq 1 20); do
    sleep 1
    if adbx shell run-as dev.fileflow test -f files/probe-report.txt 2>/dev/null; then break; fi
done

echo "=================== PROBE REPORT ==================="
adbx shell run-as dev.fileflow cat files/probe-report.txt 2>&1 || echo "no report produced"
