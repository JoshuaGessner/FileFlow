#!/usr/bin/env python3
"""Measure how much of a captured frame the transmitting screen occupies (rig diagnostic).

WHY THIS EXISTS. When every frame fails at geometry, the cause is one of: the screen is not in
frame, it is too small a fraction of the frame, it is out of focus, or the exposure is wrong. Those
call for completely different fixes — move the camera, change the grid, refocus, re-expose — and a
decode log that says "geometry failure" distinguishes none of them.

This measures the bright region's bounding box directly, and turns it into the number that actually
decides resolvability: **pixels per cell**. Prior art shows a density cliff exists (ChromaCode), and
below a few px/cell no detector can work regardless of how good it is.

Usage:  tools/frame_framing.py <frame.gray> <width> <height> <cols> <rows>
"""
import sys


def main() -> int:
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    path = sys.argv[1]
    w, h, cols, rows = (int(a) for a in sys.argv[2:6])

    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < w * h:
        print(f"file is {len(data)} bytes, expected {w * h} for {w}x{h}")
        return 1

    # Otsu on a coarse histogram: the same idea the detector uses, so the threshold this reports is
    # comparable to the one it would pick rather than an arbitrary cut.
    hist = [0] * 256
    for i in range(0, w * h, 7):  # stride-sampled; a full pass is not needed for a threshold
        hist[data[i]] += 1
    total = sum(hist)
    sum_all = sum(i * hist[i] for i in range(256))
    best_t, best_var, w_b, sum_b = 0, -1.0, 0, 0
    for t in range(256):
        w_b += hist[t]
        sum_b += t * hist[t]
        w_f = total - w_b
        if w_b == 0 or w_f == 0:
            continue
        m_b = sum_b / w_b
        m_f = (sum_all - sum_b) / w_f
        var = w_b * w_f * (m_b - m_f) ** 2
        if var > best_var:
            best_var, best_t = var, t

    mean = sum_all / total
    print(f"frame                  {w}x{h}")
    print(f"mean luminance         {mean:.1f}")
    print(f"Otsu threshold         {best_t}")

    # Bounding box of lit pixels, plus the lit count. Streamed, no per-pixel storage -- the same
    # discipline F12 forced on the detector after it allocated a point per lit pixel.
    min_x, min_y, max_x, max_y, lit = w, h, -1, -1, 0
    for y in range(h):
        base = y * w
        row = data[base:base + w]
        for x in range(w):
            if row[x] > best_t:
                lit += 1
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y

    if max_x < 0:
        print("NO LIT PIXELS — the screen is not in frame, or exposure is far too dark")
        return 1

    bw = max_x - min_x + 1
    bh = max_y - min_y + 1
    frame_area = w * h
    print(f"lit pixels             {lit}  ({lit / frame_area:.4f} of frame)")
    print(f"bright bounding box    {bw}x{bh} at ({min_x},{min_y})")
    print(f"bbox area / frame      {bw * bh / frame_area:.4f}")
    print()

    # The screen is portrait, so its LONG axis carries `rows` and its short axis `cols` -- whichever
    # way round the sensor happens to be oriented.
    long_px, short_px = max(bw, bh), min(bw, bh)
    print(f"px per cell (rows)     {long_px / rows:.2f}   ({long_px} px / {rows} rows)")
    print(f"px per cell (cols)     {short_px / cols:.2f}   ({short_px} px / {cols} cols)")
    print()

    # --- in-image rotation, inferred from the bounding box ----------------------------
    #
    # An axis-aligned bounding box around a ROTATED rectangle is much larger than the rectangle. For
    # a screen of aspect r rotated by theta, the box is L(cos+r*sin) x L(sin+r*cos) -- so the box
    # tends toward square as theta approaches 45 degrees, and a near-square box around a 0.46-aspect
    # screen means a large rotation rather than a large screen.
    #
    # This matters because rotation is nearly free for the DETECTOR -- it fits four lines and
    # resolves orientation from the corner markers (F9, F13) -- but expensive for FRAMING: the
    # inflated box is what has to fit inside the camera frame. Rotating the receiver to match the
    # transmitter's orientation can therefore buy a large margin at no optical cost, which is not
    # obvious from a decode log or from px/cell alone.
    import math
    r_aspect = cols / rows
    ratio = short_px / long_px
    theta_deg = None
    true_long = None
    # Solve (t + r)/(1 + r*t) = ratio for t = tan(theta).
    denom = 1.0 - ratio * r_aspect
    if abs(denom) > 1e-9:
        t = (ratio - r_aspect) / denom
        if t >= 0:
            theta = math.atan(t)
            theta_deg = math.degrees(theta)
            spread = math.cos(theta) + r_aspect * math.sin(theta)
            if spread > 1e-9:
                true_long = long_px / spread
    if theta_deg is not None and true_long is not None:
        print(f"inferred rotation      {theta_deg:.1f} deg in-image")
        print(f"implied true screen    {true_long:.0f} x {true_long * r_aspect:.0f} px "
              f"=> {true_long / rows:.2f} px/cell if unrotated")
        if theta_deg > 12.0:
            inflate = (long_px * short_px) / (true_long * true_long * r_aspect)
            print(f"ROTATED                the bounding box is {inflate:.2f}x the screen's own area.")
            print("                       Rotation costs the DETECTOR almost nothing but costs")
            print("                       FRAMING a lot. Align the receiver's orientation with the")
            print("                       transmitter's to reclaim that margin for free.")
    print()

    # --- is the screen CROPPED? ------------------------------------------------------
    #
    # This is checked before density, because it is the failure that density masks. The screen
    # localiser fits four boundary lines and intersects them (F10); if an edge of the always-bright
    # ring lies outside the frame there are not four lines to fit, and detection fails no matter how
    # many pixels per cell there are. A bounding box touching a frame edge is the signature.
    touch = []
    if min_x == 0: touch.append("left")
    if min_y == 0: touch.append("top")
    if max_x == w - 1: touch.append("right")
    if max_y == h - 1: touch.append("bottom")
    if touch:
        print(f"CROPPED                the bright region touches: {', '.join(touch)}")
        print("                       The boundary ring is INCOMPLETE, so the localiser cannot")
        print("                       find four corners. Move the camera BACK until the whole")
        print("                       screen plus a margin is inside the frame (F33).")
        # Infer the true extent from the screen's known aspect, using the uncropped axis.
        aspect = cols / rows  # short : long
        if "left" not in touch and "right" not in touch:
            true_long = long_px
        elif "top" not in touch and "bottom" not in touch:
            true_long = short_px / aspect
        else:
            true_long = None
        if true_long:
            target = 0.85 * max(w, h)
            print(f"                       implied full long axis ~{true_long:.0f} px vs frame "
                  f"{max(w, h)} px")
            print(f"                       => move ~{true_long / target:.2f}x further away")
    else:
        print("ok                     the whole bright region is inside the frame")
    print()

    # --- focus, from edge sharpness ---------------------------------------------------
    #
    # Defocus and insufficient resolution destroy the same thing -- high-spatial-frequency cell
    # structure -- and are indistinguishable in a decode log, so they must be separated here (F32).
    # A well-focused two-level frame is BIMODAL; blur fills the middle.
    y0, y1 = max(0, min_y + bh // 4), min(h, min_y + 3 * bh // 4)
    x0, x1 = max(0, min_x + bw // 4), min(w, min_x + 3 * bw // 4)
    dark = brightc = mid = 0
    for y in range(y0, y1):
        row = data[y * w:(y + 1) * w]
        for x in range(x0, x1):
            v = row[x]
            if v < best_t - 40:
                dark += 1
            elif v > best_t + 40:
                brightc += 1
            else:
                mid += 1
    tot = dark + brightc + mid
    frac_mid = mid / tot if tot else 1.0
    frac_bright = brightc / tot if tot else 0.0
    frac_dark = dark / tot if tot else 0.0
    print(f"interior (centre half) dark {frac_dark:.3f}, bright {frac_bright:.3f}, "
          f"mid {frac_mid:.3f}")
    # Check that BOTH classes are populated before judging sharpness.
    #
    # A low mid-grey fraction was taken as proof that cells were resolved, and it is not: a frame
    # that is 95% dark with 2.6% bright has almost no mid-grey and no readable cells either. That
    # exact frame was reported as "cells ARE being resolved optically" while the decoder failed
    # geometry on 55 of 60 frames (F36). Bimodality means two populated modes, not an empty middle.
    if frac_bright < 0.10:
        print("UNDEREXPOSED           almost nothing is bright: the screen is barely registering.")
        print("                       Raise exposure or the sender's brightness. A low mid-grey")
        print("                       fraction here means an EMPTY frame, not a sharp one.")
    elif frac_dark < 0.10:
        print("OVEREXPOSED            almost nothing is dark: the dark level is washing out, and")
        print("                       the photometric field needs both levels to place a threshold.")
    elif frac_mid > 0.45:
        print("BLURRED                most of the interior is mid-grey: the cells are not resolved.")
        print("                       Either badly out of focus or genuinely below the density")
        print("                       limit. Check the reported focus distance against the rig.")
    elif frac_mid > 0.25:
        print("SOFT                   a lot of mid-grey — marginal focus or marginal density")
    else:
        print("ok                     interior is bimodal: cells ARE being resolved optically")
    print()

    worst = min(long_px / rows, short_px / cols)
    print(f"WORST px/cell          {worst:.2f}  (of the VISIBLE region; meaningless if cropped)")
    if worst < 3.0:
        need = 8.0 / worst
        print("VERDICT  hopeless. No detector resolves a grid at this pitch: the always-bright")
        print("         boundary ring is about one cell wide, so it is barely more than a pixel.")
        print(f"         Move the camera ~{need:.1f}x CLOSER, or use a much coarser grid.")
    elif worst < 5.0:
        print("VERDICT  marginal. Below the densest point the simulator ever swept (5.0 px/cell),")
        print("         and F16 established the simulator cannot judge this regime anyway.")
    elif worst < 8.0:
        print("VERDICT  plausible. Within the range the simulator decoded cleanly, though that")
        print("         null result is not evidence for real optics (F14 retraction, F16).")
    else:
        print("VERDICT  comfortable on pitch. A geometry failure here is NOT about density.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
