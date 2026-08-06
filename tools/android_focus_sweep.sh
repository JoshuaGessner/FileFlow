#!/usr/bin/env bash
# Sweep the receiver's focus and report which setting the CHANNEL likes (EXP-004 groundwork).
#
# Usage:  tools/android_focus_sweep.sh <tx_serial> <rx_serial> [cols] [rows]
#
# WHY NOT JUST USE AUTOFOCUS. The camera's AF maximises contrast for a general scene. What decides
# whether this link works is the SEPARATION between the two luminance levels at the pilot lattice,
# and those objectives are not the same. On this rig AF converged on a value that looked reasonable
# and produced a separation of 5.1 against a threshold of 12 -- unreadable -- while geometry was
# perfect. Worse, locking that converged value made it permanent for the whole capture.
#
# So this sweeps focus and reads back the metric the decoder actually cares about, which the aiming
# analyser already computes per frame. The best setting is the one to pass as `focusDiopters`.
set -u

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
TX="${1:?tx serial required}"
RX="${2:?rx serial required}"
COLS="${3:-120}"
ROWS="${4:-260}"

# 10.5 cm is this lens's closest focus; beyond ~40 cm the grid is unresolvable anyway, so the useful
# range is roughly 2.5 to 9.5 diopters.
DIOPTERS="2.5 3.5 4.5 5.5 6.5 7.5 8.5"

tx() { "$ADB" -s "$TX" "$@"; }
rx() { "$ADB" -s "$RX" "$@"; }

cd "$(dirname "$0")/.." || exit 1

for d in "$TX" "$RX"; do
    "$ADB" -s "$d" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
    "$ADB" -s "$d" shell wm dismiss-keyguard >/dev/null 2>&1
done

tx shell am force-stop dev.fileflow
tx shell "am start -n dev.fileflow/.TransmitActivity \
    --ei cols $COLS --ei rows $ROWS --ei divisor 8 --ei seconds 400 --ei nsym 32" >/dev/null 2>&1
sleep 4

printf '%10s  %10s  %8s  %8s  %8s\n' DIOPTERS "DISTANCE" "SEP" "blur" "px/CELL"
printf '%s\n' "---------------------------------------------------------------"

BEST_D=""
BEST_SEP=-1

for d in $DIOPTERS; do
    rx shell am force-stop dev.fileflow
    rx logcat -c
    rx shell "am start -n dev.fileflow/.AimActivity \
        --ei cols $COLS --ei rows $ROWS --ef focusDiopters $d" >/dev/null 2>&1
    sleep 5

    # SEPARATION is the metric, not mid-fraction.
    #
    # Mid-fraction counts pixels lying between the two levels, which only means "sharp" when two
    # levels exist. Defocus bad enough to collapse them puts almost everything in one class and drives
    # mid-fraction to zero, so the blurriest setting scores best -- this sweep duly picked 40 cm for a
    # subject at 20 cm before it was corrected (F40). Separation cannot be gamed that way: levels that
    # have merged ARE the failure.
    line=$(rx logcat -d -s FileFlow.Aim:I 2>/dev/null | tail -8 \
           | awk '{for(i=1;i<=NF;i++){if($i=="sep")s=$(i+1); if($i=="mid")m=$(i+1);
                                      if($i=="px/cell")p=$(i+1)}
                  if(s!=""){ss+=s; sm+=m; sp+=p; n++}}
                  END{if(n>0) printf "%.0f %.3f %.1f", ss/n, sm/n, sp/n; else printf "NA NA NA"}')
    sep=$(echo "$line" | cut -d' ' -f1)
    blur=$(echo "$line" | cut -d' ' -f2)
    pxc=$(echo "$line" | cut -d' ' -f3)
    cm=$(awk -v x="$d" 'BEGIN{printf "%.1f", 100/x}')

    printf '%10s  %8s cm  %8s  %8s  %8s\n' "$d" "$cm" "$sep" "$blur" "$pxc"

    if [ "$sep" != "NA" ]; then
        if awk -v a="$sep" -v b="$BEST_SEP" 'BEGIN{exit !(a>b)}'; then
            BEST_SEP="$sep"
            BEST_D="$d"
        fi
    fi
done

rx shell am force-stop dev.fileflow
tx shell am force-stop dev.fileflow

echo
if [ -n "$BEST_D" ]; then
    echo "BEST: $BEST_D diopters (~$(awk -v x="$BEST_D" 'BEGIN{printf "%.0f", 100/x}') cm), separation $BEST_SEP"
    echo "  (the photometric field erases below 12, so anything under ~40 is not usable)"
    echo "Pass it to a run as the focusDiopters argument."
else
    echo "No usable samples — is the transmitter visible to the receiver?"
fi
