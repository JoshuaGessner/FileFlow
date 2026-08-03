#!/usr/bin/env python3
"""Find grid geometries whose cells are an INTEGER number of physical display pixels.

Why this matters (docs/research/android-display-pipeline.md): if a logical cell is a
non-integer number of panel pixels, cell boundaries land on fractional pixels. The
transmitter cannot render them crisply, the receiver's sampler sees systematically
asymmetric energy per cell, and spatial crosstalk rises -- for no gain whatsoever, since
integer pitches are freely available nearby.

Consequence: the optimal grid is PANEL-DEPENDENT. Two reference devices with different
native resolutions want different grids, which is fine because the receiver reads the grid
from the frame header rather than assuming it.

Usage:
    python3 tools/grid_fit.py
    python3 tools/grid_fit.py --width 1080 --height 2400 --min-cells 15000
"""

from __future__ import annotations

import argparse

# Reference devices. Keep in sync with docs/planning/DEVICE-MATRIX.md.
PANELS = {
    "Pixel 8": (1080, 2400),
    "Galaxy S26 Ultra": (1440, 3120),
}

# Candidate grids named in the project charter.
CHARTER_GRIDS = [(96, 160), (120, 200), (144, 240)]


def integer_pitch_grids(w: int, h: int, min_cells: int, max_cells: int,
                        min_pitch: int = 6, max_pitch: int = 20):
    """All (cols, rows) where cell pitch divides the panel exactly in both axes."""
    out = []
    for px in range(min_pitch, max_pitch + 1):
        if w % px:
            continue
        for py in range(min_pitch, max_pitch + 1):
            if h % py:
                continue
            cols, rows = w // px, h // py
            cells = cols * rows
            if not (min_cells <= cells <= max_cells):
                continue
            # Keep cells roughly square in physical terms; wildly anisotropic cells
            # complicate the sampler and the crosstalk model for no benefit.
            aspect = max(px, py) / min(px, py)
            if aspect > 1.5:
                continue
            out.append((cols, rows, px, py, cells, aspect))
    return sorted(out, key=lambda t: -t[4])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--min-cells", type=int, default=10000)
    ap.add_argument("--max-cells", type=int, default=40000)
    args = ap.parse_args()

    panels = ({"custom": (args.width, args.height)}
              if args.width and args.height else PANELS)

    for name, (w, h) in panels.items():
        print(f"\n=== {name}  ({w}x{h}) ===")

        print("  charter grids:")
        for cols, rows in CHARTER_GRIDS:
            px, py = w / cols, h / rows
            ok = (w % cols == 0) and (h % rows == 0)
            flag = "INTEGER" if ok else "fractional -- avoid"
            print(f"    {cols:>4}x{rows:<4} -> {px:5.2f} x {py:5.2f} px/cell   {flag}")

        print("  integer-pitch alternatives (largest first):")
        for cols, rows, px, py, cells, aspect in integer_pitch_grids(
                w, h, args.min_cells, args.max_cells)[:8]:
            print(f"    {cols:>4}x{rows:<4} -> {px:>2} x {py:<2} px/cell  "
                  f"{cells:>6,} cells  aspect {aspect:.2f}")


if __name__ == "__main__":
    main()
