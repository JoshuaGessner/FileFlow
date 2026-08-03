#!/usr/bin/env bash
# Grid-density sweep through the IMAGE path — a simulator dry run of EXP-001.
#
# WHAT THIS IS FOR: [VISUALMIMO-CISS11] says multiplexing gain holds only while transmit
# elements stay separable at the receiver, and ChromaCode's own sweep shows goodput rising
# with density and then COLLAPSING past a threshold. Every screen-camera system has that
# cliff; its location is channel-specific. This finds ours in simulation, cheaply, before
# hardware — and cross-checks the ~6 px/cell detection floor (finding F14) against
# end-to-end goodput rather than detection alone.
#
# ⚠ SIMULATED AND UNCALIBRATED (RISK-024). The SHAPE of the curve is the output; the exact
# cliff location must be re-measured on real captures before it is treated as fact.
#
# Usage:  tools/grid_sweep.sh [image_width] [image_height]
set -uo pipefail

FFSIM=${FFSIM:-./build/desktop-release/sim/ffsim}
W=${1:-1200}
H=${2:-2000}
# The payload MUST be large enough that per-frame capacity determines how long the transfer
# takes. With a small payload every grid finishes in the same number of frames (one symbol per
# frame, block under-filled), goodput is pinned at payload/(frames/Fd), and the sweep silently
# measures nothing at all -- every row reports an identical figure. 512 KB needs many symbols
# per block even at the largest grid here.
PAYLOAD=${PAYLOAD:-524288}

if [ ! -x "$FFSIM" ]; then
    echo "ffsim not found at $FFSIM — build with: cmake --build build/desktop-release -j" >&2
    exit 1
fi

# Grids to sweep. Aspect held near the charter's 0.6 so px/cell is comparable across rows.
GRIDS="48x80 72x120 96x160 108x180 120x200 132x220 144x240 156x260 168x280 192x320"

printf '%-10s %8s %9s %10s %8s %8s %10s %9s\n' \
    GRID CELLS PX/CELL "GEOM ERR" FRAMES "HDR H" "GOODPUT" VERIFIED
printf '%s\n' "----------------------------------------------------------------------------------"

for g in $GRIDS; do
    cols=${g%x*}
    rows=${g#*x}

    out=$("$FFSIM" --cols "$cols" --rows "$rows" --payload "$PAYLOAD" \
                   --image-path --image-size "$W" "$H" --max-frames 40000 2>&1)

    # A grid too small to carry its own coded header is a configuration limit, not a channel
    # result. Report it as such rather than letting it masquerade as a decode failure.
    if echo "$out" | grep -q "too small for the coded header"; then
        printf '%-10s %8d %9s %10s %8s %8s %10s %9s\n' \
            "$g" "$((cols * rows))" "-" "-" "-" "-" "-" "header too big"
        continue
    fi

    # ffsim exits non-zero when the transfer does not verify; that is a data point, not an
    # error, so the sweep records it and continues.
    verified=$(echo "$out" | awk '/^VERIFIED/ {print $2}')
    frames=$(echo "$out"  | awk '/display states presented/ {print $4}')
    hdr=$(echo "$out"     | awk '/header success H/ {print $4}')
    geo=$(echo "$out"     | awk '/worst geometric error/ {print $4}')
    good=$(echo "$out"    | awk '/PAYLOAD GOODPUT/ {print $3}')
    detfail=$(echo "$out" | awk '/detection failures/ {print $3}')

    # Screen fills ~80% of image height; cell pitch follows from the grid's column count.
    pxcell=$(python3 -c "print(f'{0.8*$H*($cols/$rows)/$cols:.2f}')")
    cells=$((cols * rows))

    [ -z "${good:-}" ] && good="--"
    [ "${verified:-no}" = "yes" ] || verified="NO (detfail=${detfail:-?})"

    printf '%-10s %8d %9s %10s %8s %8s %10s %9s\n' \
        "$g" "$cells" "$pxcell" "${geo:-?}" "${frames:-?}" "${hdr:-?}" "${good:-?}" "$verified"
done

printf '\n%s\n' "px/cell is the RECEIVER-side pitch (finding F14 puts the detection floor near 6)."
printf '%s\n' "Goodput assumes Fd=60 states/s — an ASSUMPTION, not a measurement (ADR-0012)."
