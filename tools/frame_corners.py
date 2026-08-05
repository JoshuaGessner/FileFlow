#!/usr/bin/env python3
"""Are the transmitting screen's corners square, or rounded away?

WHY THIS MATTERS. Screen localisation finds the quad by streaming the extremes of `x+y` and `x-y`
(F12's O(1) accumulator) and then verifies corner markers through the resulting homography. Both
steps assume the bright region really is a rectangle.

A real phone display is not one. Modern panels have generously rounded corners and often a cutout,
so a full-bleed optical frame has its four corners **physically clipped by the glass**. The extremes
of `x+y` then sit somewhere on a rounded arc rather than at the true corner, every corner estimate is
biased inward by a different amount, the homography is skewed, and marker verification fails — while
every other measurable property of the capture looks healthy.

The simulator renders a mathematically perfect rectangle, so it cannot exhibit this at all. That makes
it exactly the class of defect RISK-024 warns about: a simulator kinder than reality producing
confident wrong conclusions.

Usage:  tools/frame_corners.py <frame.gray> <width> <height>
"""
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < w * h:
        print(f"file is {len(data)} bytes, expected {w * h}")
        return 1

    # Threshold at the midpoint of the observed range: this only has to separate screen from
    # background, and the framing tool already reports the Otsu figure.
    lo, hi = 255, 0
    for i in range(0, w * h, 11):
        v = data[i]
        if v < lo: lo = v
        if v > hi: hi = v
    thr = (lo + hi) // 2

    min_x, min_y, max_x, max_y = w, h, -1, -1
    for y in range(h):
        row = data[y * w:(y + 1) * w]
        for x in range(w):
            if row[x] > thr:
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y
    if max_x < 0:
        print("no bright region")
        return 1

    bw, bh = max_x - min_x + 1, max_y - min_y + 1
    print(f"threshold {thr}   bbox {bw}x{bh} at ({min_x},{min_y})")
    print()

    # For each bounding-box corner, walk the diagonal inward until the first lit pixel. On a true
    # rectangle that distance is ~0; on a rounded corner it is the corner radius, scaled.
    corners = {
        "top-left": (min_x, min_y, 1, 1),
        "top-right": (max_x, min_y, -1, 1),
        "bottom-left": (min_x, max_y, 1, -1),
        "bottom-right": (max_x, max_y, -1, -1),
    }
    print("distance along the inward diagonal to the first lit pixel:")
    worst = 0.0
    for name, (cx, cy, sx, sy) in corners.items():
        d = None
        for step in range(0, min(bw, bh) // 2):
            x, y = cx + sx * step, cy + sy * step
            if 0 <= x < w and 0 <= y < h and data[y * w + x] > thr:
                d = step
                break
        if d is None:
            print(f"  {name:<14} never lit along the diagonal")
            continue
        print(f"  {name:<14} {d} px")
        worst = max(worst, float(d))

    print()
    frac = worst / max(bw, bh)
    print(f"worst corner inset     {worst:.0f} px  ({frac:.3%} of the long axis)")
    if worst <= 3:
        print("VERDICT  corners are effectively square. Rounding is not the problem here.")
    else:
        print("VERDICT  the corners are CUT AWAY. The extremes of x+y and x-y therefore do not")
        print("         land on the true corners, so the homography is skewed and marker")
        print("         verification will fail even though framing, focus and density are fine.")
        print("         Fix on the TRANSMITTER: inset the optical frame from the panel edges so the")
        print("         boundary ring lies entirely on flat glass.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
