#!/usr/bin/env bash
# EXP-023 — does the adaptive link controller (C14) pick the right intra-frame code rate?
#
# WHAT THIS MEASURES. Two things, and the second is the one that drives a roadmap decision.
#
#  (a) ACCURACY. For each channel the script runs the whole nsym ladder to get brute-force
#      ground truth for the goodput-optimal rung, then asks the controller -- which sees only
#      receiver telemetry from a single run at one reference rung -- to name that optimum. The
#      controller never sees the sweep.
#
#  (b) THE COST OF GUESSING WRONG. The spread between the best and worst rung on each channel
#      is what a fixed, badly-chosen nsym costs. On a one-way link the transmitter has to guess
#      (OQ-013), so this spread is the value of ever fixing that -- and therefore the argument
#      for or against pulling reverse optical control (ADP-04) out of Phase 9.
#
# WHY A SWEEP IS NEEDED AT ALL, given the controller can score the ladder counterfactually:
# the counterfactual predicts FRAME SUCCESS, while the metric is GOODPUT. Fountain reception
# overhead sits between them and is not modelled (see LinkController::ScoreLadder). The sweep
# is what checks that the omission does not change the answer.
#
# ⚠ SIMULATED AND UNCALIBRATED (RISK-024). This tests whether the DECISION RULE is sound. It
# says nothing about which nsym real hardware will want -- the channel model is a guess.
#
# Usage:  tools/adapt_sweep.sh [payload_bytes]
set -uo pipefail

FFSIM=${FFSIM:-./build/desktop-release/sim/ffsim}
PAYLOAD=${1:-131072}
SEED=${SEED:-5}
LADDER=${LADDER:-"8 16 24 32 40 48 64"}
# The rung the controller is allowed to observe from. Deliberately NOT the optimum for any of
# the channels below -- the whole question is whether it can find its way from an arbitrary
# starting point, which is the situation a real session-start guess is in.
REFERENCE_NSYM=${REFERENCE_NSYM:-32}

if [ ! -x "$FFSIM" ]; then
    echo "ffsim not found at $FFSIM — build with: cmake --build build/desktop-release -j" >&2
    exit 1
fi

# Four channels, clean to severe. The impaired one reproduces finding F18's channel exactly so
# the two experiments can be compared directly.
CH_NAMES=("clean" "mild" "F18-impaired" "severe")
CH_FLAGS=(
    "--noise 2"
    "--noise 12 --shot 0.4 --crosstalk 0.10 --occlusion 0.01 --drop 0.03"
    "--noise 26 --shot 0.9 --crosstalk 0.20 --occlusion 0.03 --drop 0.10"
    "--noise 40 --shot 1.2 --crosstalk 0.28 --occlusion 0.06 --drop 0.15"
)

echo "EXP-023 — adaptive intra-frame code rate selection   [SIMULATED, HYP, RISK-024]"
echo "payload ${PAYLOAD} B   seed ${SEED}   ladder: ${LADDER}   reference rung ${REFERENCE_NSYM}"
echo "goodput assumes Fd=60 states/s — an ASSUMPTION, not a measurement (ADR-0012)"
echo

for i in "${!CH_NAMES[@]}"; do
    name="${CH_NAMES[$i]}"
    flags="${CH_FLAGS[$i]}"

    echo "=============================================================================="
    echo "channel: ${name}"
    echo "  ${flags}"
    echo
    # WORST ERAS is the worst per-codeword ERASURE load, which is what the FEC block reports.
    # It is NOT the correction budget: undetected errors cost two bytes each, so the budget
    # (2*errors + erasures) is higher, and confusing the two is precisely the error that made an
    # earlier version of the controller recommend the wrong rung (F23). The budget appears in the
    # --adapt block below.
    printf '  %6s %8s %8s %12s %10s %9s\n' NSYM FRAMES UNCORR "WORST ERAS" GOODPUT VERIFIED
    printf '  %s\n' "-------------------------------------------------------------------"

    best_rate=""
    best_nsym=""
    worst_rate=""
    for n in $LADDER; do
        # shellcheck disable=SC2086
        out=$("$FFSIM" --payload "$PAYLOAD" --seed "$SEED" --fec-nsym "$n" \
                       --max-frames 40000 $flags 2>&1)

        verified=$(echo "$out" | awk '/^VERIFIED/ {print $2}')
        frames=$(echo "$out"   | awk '/display states presented/ {print $4}')
        uncorr=$(echo "$out"   | awk '/frames uncorrectable/ {print $3}')
        wload=$(echo "$out"    | awk '/worst codeword load/ {print $4}')
        good=$(echo "$out"     | awk '/PAYLOAD GOODPUT/ {print $3}')

        printf '  %6s %8s %8s %12s %10s %9s\n' \
            "$n" "${frames:-?}" "${uncorr:-?}" "${wload:-?}" "${good:---}" "${verified:-NO}"

        # Only a VERIFIED transfer counts. An unverified run has no goodput by definition
        # (PERFORMANCE-PHILOSOPHY: bits that cannot be shown correct do not count).
        if [ "${verified:-no}" = "yes" ] && [ -n "${good:-}" ]; then
            if [ -z "$best_rate" ] || awk "BEGIN{exit !($good > $best_rate)}"; then
                best_rate="$good"; best_nsym="$n"
            fi
            if [ -z "$worst_rate" ] || awk "BEGIN{exit !($good < $worst_rate)}"; then
                worst_rate="$good"
            fi
        fi
    done

    # The controller's answer, from ONE run at the reference rung. It never sees the table above.
    # shellcheck disable=SC2086
    adapt_out=$("$FFSIM" --payload "$PAYLOAD" --seed "$SEED" --fec-nsym "$REFERENCE_NSYM" \
                         --adapt --max-frames 40000 $flags 2>&1)
    rec=$(echo "$adapt_out" | awk '/^recommended nsym/ {print $3}')
    censored=$(echo "$adapt_out" | awk '/^censored observations/ {print $3}')

    echo
    if [ -z "${best_nsym:-}" ]; then
        # No rung verified, so the channel has no optimum and the controller's answer cannot be
        # scored against anything. Saying that plainly beats printing "?" and letting a reader
        # treat a missing ground truth as a controller failure -- or as a success.
        echo "  brute-force optimum      NONE — no rung completes a verified transfer on this"
        echo "                           channel, so there is nothing for the controller to be"
        echo "                           right or wrong about. More parity does not help: the"
        echo "                           damage is error-dominated, and errors cost double."
        echo "  controller recommends    nsym=${rec:-?}  (unvalidatable here)"
    else
        echo "  brute-force optimum      nsym=${best_nsym}  at ${best_rate} KB/s"
        echo "  controller recommends    nsym=${rec:-?}  (from telemetry at nsym=${REFERENCE_NSYM} only)"
    fi
    if [ -n "${censored:-}" ]; then
        echo "  ⚠ ${censored} censored observations — see the run's own warning"
    fi
    if [ -n "${best_rate:-}" ] && [ -n "${worst_rate:-}" ]; then
        awk -v b="$best_rate" -v w="$worst_rate" 'BEGIN{
            printf "  cost of the worst rung   %.1f%% of the optimum (%.1f vs %.1f KB/s)\n",
                   100*(1-w/b), w, b }'
    fi
    if [ -n "${rec:-}" ] && [ -n "${best_nsym:-}" ]; then
        # Rung distance, not byte distance: EXP-023 H2 is stated in ladder steps.
        echo "$LADDER" | awk -v r="$rec" -v b="$best_nsym" '{
            for (i = 1; i <= NF; i++) { if ($i == r) ri = i; if ($i == b) bi = i }
            d = ri - bi; if (d < 0) d = -d
            printf "  rung distance            %d\n", d }'
    fi
    echo
done

echo "=============================================================================="
echo "Reminder: the controller's recommendation CANNOT be applied mid-session. nsym sets the"
echo "FEC message size, hence the fountain symbol size, which the manifest fixes for the whole"
echo "transfer. It is a session-START decision, and on a one-way link the transmitter cannot"
echo "hear it (OQ-013). See findings F20 and F23."
