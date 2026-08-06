#!/usr/bin/env bash
# Two-device optical link test: one phone transmits, the other records (EXP-006 / EXP-001 groundwork).
#
# Usage:
#   tools/android_link.sh <tx_serial> <rx_serial> [cols] [rows] [divisor] [frames] [rx_max_width]
#                         [distance_cm] [angle_deg] [notes]
#
# WHY THE FIRST RUN SHOULD USE A HIGH `divisor`. Presenting a new state every Nth vsync makes each
# state persist across several camera frames, which nearly guarantees some CLEAN (non-mixed) captures.
# That isolates the question "do the optics resolve this grid at all?" from "can we catch a state
# before it changes?" — two failures that look identical in a decode log and have nothing to do with
# each other. Raise the state rate only once the slow case decodes.
#
# ⚠ The rig figures are ARGUMENTS, not measurements this script can take. An unrecorded distance
# stays at its sentinel and `ffreplay` says so loudly: a capture whose conditions were not recorded
# is not evidence (C17, CAPTURE-HARNESS).
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
TX_SERIAL="${1:?tx serial required — see: adb devices}"
RX_SERIAL="${2:?rx serial required — see: adb devices}"
COLS="${3:-120}"
ROWS="${4:-260}"
DIVISOR="${5:-8}"
FRAMES="${6:-240}"
RX_MAXW="${7:-2688}"
DISTANCE_CM="${8:--1}"
ANGLE_DEG="${9:--1}"
NOTES="${10:-two-device link test}"
# 0 = derive from the frame period. Set explicitly to sweep exposure (EXP-004).
EXPOSURE_NS="${11:-0}"
# Gate recording on the aim analyser reporting Ready. Off by default so a scripted run records
# whatever is in front of it and the report says what that was -- but available, because a run that
# refuses to record a hopeless frame beats one that produces 60 undecodable ones.
AIM="${12:-false}"
NOTES="${NOTES//\'/}"

NSYM=32
PAYLOAD=131072
SEED=5
TX_SECONDS=$((FRAMES / 10 + 40))

tx() { "$ADB" -s "$TX_SERIAL" "$@"; }
rx() { "$ADB" -s "$RX_SERIAL" "$@"; }

APK="app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK" ] || { echo "no APK at $APK — ./gradlew :app:assembleDebug" >&2; exit 1; }

TX_MODEL=$(tx shell getprop ro.product.model 2>/dev/null | tr -d '\r')
RX_MODEL=$(rx shell getprop ro.product.model 2>/dev/null | tr -d '\r')
echo "TX  $TX_SERIAL  $TX_MODEL      RX  $RX_SERIAL  $RX_MODEL"
echo "grid ${COLS}x${ROWS}   divisor $DIVISOR   frames $FRAMES   rx max width $RX_MAXW"
echo

echo "--- installing on both ---"
tx install -r -t "$APK" 2>&1 | tail -1
rx install -r -t "$APK" 2>&1 | tail -1
rx shell pm grant dev.fileflow android.permission.CAMERA 2>/dev/null

echo "--- waking and unlocking both devices ---"
# Both are required, for different reasons: the transmitter cannot display on a dark screen, and
# Android refuses camera access to a background process, which is what an activity launched onto a
# locked device is. The first attempt failed with CAMERA_DISABLED for precisely that.
for dev in tx rx; do
    $dev shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
    $dev shell wm dismiss-keyguard >/dev/null 2>&1
done
sleep 1

echo "--- clearing previous artefacts (F28: a stale report reads as a fresh result) ---"
tx shell am force-stop dev.fileflow
rx shell am force-stop dev.fileflow
tx shell run-as dev.fileflow rm -f files/transmit-report.txt 2>/dev/null
rx shell run-as dev.fileflow rm -f files/capture-report.txt files/capture-frames.csv 2>/dev/null
rx shell run-as dev.fileflow rm -rf files/capture-bundle 2>/dev/null

# The payload hash is a property of (seed, size) alone, so the receiver can be told it up front and
# a completed transfer can be CHECKED rather than merely reported (G3).
PAYLOAD_SHA=$(tx shell run-as dev.fileflow cat files/transmit-report.txt 2>/dev/null \
    | awk '/payload sha256/ {print $3}')

echo "--- starting transmitter for ${TX_SECONDS}s ---"
tx shell "am start -n dev.fileflow/.TransmitActivity \
    --ei cols $COLS --ei rows $ROWS --ei divisor $DIVISOR \
    --ei seconds $TX_SECONDS --ei nsym $NSYM --ei payload $PAYLOAD" >/dev/null 2>&1

# Let the display mode settle and the first states go out before recording. Capturing during mode
# acquisition would record frames from a panel that is still changing state rate.
sleep 5

echo "--- recording $FRAMES frames on the receiver ---"
rx shell "am start -n dev.fileflow/.CaptureActivity \
    --ei frames $FRAMES --ei fps 60 --ei maxWidth $RX_MAXW \
    --ei gridCols $COLS --ei gridRows $ROWS \
    --es senderModel '$TX_MODEL' --es profile 'M0' \
    --ed distanceCm $DISTANCE_CM --ed angleDeg $ANGLE_DEG \
    --es motion 'rigid/propped' \
    --el payloadBytes $PAYLOAD \
    --el exposureNs $EXPOSURE_NS --ez aim $AIM \
    --es notes '$NOTES'" >/dev/null 2>&1

for _ in $(seq 1 90); do
    sleep 2
    if rx shell run-as dev.fileflow test -f files/capture-report.txt 2>/dev/null; then break; fi
done
sleep 1

echo
echo "=================== RECEIVER REPORT ==================="
rx shell run-as dev.fileflow cat files/capture-report.txt 2>&1 || echo "no receiver report"

echo
echo "=================== TRANSMITTER REPORT ==================="
tx shell run-as dev.fileflow cat files/transmit-report.txt 2>&1 || echo "(still running — expected)"

tx shell am force-stop dev.fileflow
rx shell am force-stop dev.fileflow
echo
echo "Pull and decode with:  tools/android_pull_bundle.sh $RX_SERIAL"
