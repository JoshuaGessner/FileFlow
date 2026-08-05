#!/usr/bin/env bash
# Run one on-device transmit and print what it managed to SUBMIT (C03).
#
# Usage:  tools/android_transmit.sh [cols] [rows] [divisor] [seconds] [nsym] [serial]
#
# `divisor` presents a new optical state every Nth vsync: 1 = every refresh, 2 = every other.
# `serial` targets one device when two are attached (`adb devices` lists them).
#
# ⚠ THE NUMBER THIS PRINTS IS NOT `Fd`. It is states *submitted*, which is an upper bound on Fd. A
# transmitter cannot confirm presentation -- that is why every frame header carries its own sequence
# number -- so Fd is measured by the RECEIVER decoding those numbers, and needs both devices.
# Reporting a submission rate as Fd would be the unlabelled-rate defect ADR-0012 forbids.
#
# Deletes the previous report before launching, for the reason F28 records: a script that waits for
# a report file to *exist* will happily print the previous run's.
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
COLS="${1:-144}"
ROWS="${2:-240}"
DIVISOR="${3:-1}"
SECONDS_RUN="${4:-20}"
NSYM="${5:-32}"
SERIAL="${6:-}"

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
adbx shell run-as dev.fileflow rm -f files/transmit-report.txt 2>/dev/null
if adbx shell run-as dev.fileflow test -f files/transmit-report.txt 2>/dev/null; then
    echo "FATAL: could not delete the old report; refusing to run and risk reporting it" >&2
    exit 1
fi

adbx logcat -c
adbx shell "am start -n dev.fileflow/.TransmitActivity \
    --ei cols $COLS --ei rows $ROWS --ei divisor $DIVISOR \
    --ei seconds $SECONDS_RUN --ei nsym $NSYM" 2>&1 | tail -1

echo "transmitting for ${SECONDS_RUN}s ..."
WAITED=0
LIMIT=$((SECONDS_RUN + 30))
while [ "$WAITED" -lt "$LIMIT" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    if adbx shell run-as dev.fileflow test -f files/transmit-report.txt 2>/dev/null; then
        break
    fi
done
sleep 1

echo "=================== REPORT ==================="
adbx shell run-as dev.fileflow cat files/transmit-report.txt 2>&1 || echo "no report produced"

echo "=================== ERRORS (if any) ==================="
adbx logcat -d -s AndroidRuntime:E FileFlow.Tx:E 2>/dev/null | tail -20
