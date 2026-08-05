#!/usr/bin/env bash
# Fast aiming aid: transmit, grab a few frames, and report the framing. ~20 s per iteration.
#
# Usage:  tools/android_aim.sh <tx_serial> <rx_serial> [cols] [rows] [rx_max_width]
#
# WHY THIS EXISTS. Screen localisation fits the four lines of the always-bright boundary ring and
# intersects them (F10). If any edge of that ring falls outside the camera frame there are not four
# lines to fit, and EVERY frame fails geometry — regardless of focus, exposure or pixels per cell.
# That failure is invisible in a decode log, which reports only "geometry failure", and it cost three
# full capture-and-decode cycles to see the first time (F33).
#
# So this checks the one thing that must hold before any other measurement means anything: is the
# whole screen inside the frame, with margin on all four sides?
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
TX_SERIAL="${1:?tx serial required}"
RX_SERIAL="${2:?rx serial required}"
COLS="${3:-120}"
ROWS="${4:-260}"
RX_MAXW="${5:-1920}"
FRAMES=3

tx() { "$ADB" -s "$TX_SERIAL" "$@"; }
rx() { "$ADB" -s "$RX_SERIAL" "$@"; }

cd "$(dirname "$0")/.." || exit 1
OUT=/tmp/fileflow-aim

for dev in tx rx; do
    $dev shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
    $dev shell wm dismiss-keyguard >/dev/null 2>&1
done

tx shell am force-stop dev.fileflow
rx shell am force-stop dev.fileflow
rx shell run-as dev.fileflow rm -f files/capture-report.txt 2>/dev/null
rx shell run-as dev.fileflow rm -rf files/capture-bundle 2>/dev/null

tx shell "am start -n dev.fileflow/.TransmitActivity \
    --ei cols $COLS --ei rows $ROWS --ei divisor 8 --ei seconds 40 --ei nsym 32" >/dev/null 2>&1
sleep 4

rx shell "am start -n dev.fileflow/.CaptureActivity \
    --ei frames $FRAMES --ei fps 30 --ei maxWidth $RX_MAXW \
    --ei gridCols $COLS --ei gridRows $ROWS \
    --es notes 'aiming check'" >/dev/null 2>&1

for _ in $(seq 1 25); do
    sleep 1
    if rx shell run-as dev.fileflow test -f files/capture-report.txt 2>/dev/null; then break; fi
done

SIZE=$(rx shell run-as dev.fileflow cat files/capture-report.txt 2>/dev/null \
       | awk '/^  size /{print $2}')
W=${SIZE%x*}
H=${SIZE#*x}

mkdir -p "$OUT"
rm -rf "$OUT/capture-bundle"
rx exec-out run-as dev.fileflow tar c -C files capture-bundle 2>/dev/null > "$OUT/b.tar"
tar xf "$OUT/b.tar" -C "$OUT" 2>/dev/null
rm -f "$OUT/b.tar"

tx shell am force-stop dev.fileflow
rx shell am force-stop dev.fileflow

FRAME=$(ls "$OUT/capture-bundle/frames"/*.gray 2>/dev/null | tail -1)
if [ -z "${FRAME:-}" ] || [ -z "${W:-}" ]; then
    echo "no frame captured — is the receiver awake, and the camera permission granted?"
    exit 1
fi

echo "capture $W x $H, grid ${COLS}x${ROWS}"
python3 tools/frame_framing.py "$FRAME" "$W" "$H" "$COLS" "$ROWS"
