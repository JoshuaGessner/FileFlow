#!/usr/bin/env bash
# Run one on-device capture and print what the camera actually delivered (EXP-007, C05 CPU path).
#
# Usage:  tools/android_capture.sh [frames] [fps] [maxWidth] [write:true|false] [notes]
#
# The `write` arm is the important one. Writing a 1920x1440 Y plane is 2.76 MB, so 60 fps is
# ~166 MB/s of I/O; when that saturates, unreturned `ImageReader` buffers throttle the camera and
# the result is INDISTINGUISHABLE from a sensor that cannot hit the rate. Running both arms is what
# makes a low delivered rate attributable — see finding F28, where the difference was 32 vs 59 fps
# and the naive reading would have been a false claim about the hardware.
#
# TWO TRAPS THIS SCRIPT EXISTS TO AVOID, both of which bit during F28:
#
#  1. It DELETES the previous report before launching. An earlier version waited for the report
#     file to *exist*, which it already did from the prior run, and printed a stale report that
#     looked like a fresh result.
#
#  2. It single-quotes the notes for the DEVICE's shell. `adb shell` hands its argument to
#     /system/bin/sh, which re-parses it; unquoted parentheses in a note aborted the launch with a
#     syntax error and produced no run at all.
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
FRAMES="${1:-300}"
FPS="${2:-60}"
MAXW="${3:-1920}"
WRITE="${4:-true}"
NOTES="${5:-unspecified conditions}"
# Single quotes are the escape mechanism, so a note containing one would break out of it.
NOTES="${NOTES//\'/}"

APK="app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK" ]; then
    echo "no APK at $APK — build with: ./gradlew :app:assembleDebug" >&2
    exit 1
fi

"$ADB" install -r -t "$APK" 2>&1 | tail -1
"$ADB" shell pm grant dev.fileflow android.permission.CAMERA 2>/dev/null

"$ADB" shell am force-stop dev.fileflow
"$ADB" shell run-as dev.fileflow rm -f files/capture-report.txt files/capture-frames.csv 2>/dev/null
"$ADB" shell run-as dev.fileflow rm -rf files/capture-bundle 2>/dev/null
if "$ADB" shell run-as dev.fileflow test -f files/capture-report.txt 2>/dev/null; then
    echo "FATAL: could not delete the old report; refusing to run and risk reporting it" >&2
    exit 1
fi

"$ADB" logcat -c
"$ADB" shell "am start -n dev.fileflow/.CaptureActivity \
    --ei frames $FRAMES --ei fps $FPS --ei maxWidth $MAXW \
    --ez write $WRITE --es notes '$NOTES'" 2>&1 | tail -1

for _ in $(seq 1 60); do
    sleep 2
    if "$ADB" shell run-as dev.fileflow test -f files/capture-report.txt 2>/dev/null; then break; fi
done
sleep 1

echo "=================== REPORT ==================="
"$ADB" shell run-as dev.fileflow cat files/capture-report.txt 2>&1 || echo "no report produced"

echo "=================== ERRORS (if any) ==================="
"$ADB" logcat -d -s AndroidRuntime:E FileFlow.Capture:E 2>/dev/null | tail -20
