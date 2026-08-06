#!/usr/bin/env python3
"""Per-frame mean luminance across a capture bundle (rig / panel diagnostic).

WHY THIS EXISTS. The aiming analyser reported the same scene alternating between `Ready` (mean
luminance 62) and `TooDark` (mean 22) several times a second, with the rig stationary and the
transmitted content statistically identical from frame to frame. A verdict that flickers is a
symptom; this measures whether the FRAMES themselves flicker.

That distinction decides where the problem is. If successive frames of an unchanging scene differ
this much in total light, nothing downstream can be tuned to fix it -- the receiver is sampling a
source whose brightness varies faster than it can measure, and the candidates are the panel's PWM
dimming beating against the exposure window, or the exposure straddling a display-state transition.
Both are physical, both are invisible to a simulator that renders a mathematically steady screen,
and both would show up in the goodput model as a mysteriously poor `Pc`.

Usage:  tools/frame_luminance.py <bundle_dir> <width> <height> [stride]
"""
import os
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    bundle, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    stride = int(sys.argv[4]) if len(sys.argv) > 4 else 13  # coprime-ish, avoids row aliasing

    frames_dir = os.path.join(bundle, "frames")
    names = sorted(n for n in os.listdir(frames_dir) if n.endswith(".gray"))
    if not names:
        print("no frames")
        return 1

    means = []
    for n in names:
        with open(os.path.join(frames_dir, n), "rb") as fh:
            data = fh.read()
        if len(data) < w * h:
            continue
        total = 0
        count = 0
        for i in range(0, w * h, stride):
            total += data[i]
            count += 1
        means.append(total / count if count else 0.0)

    if len(means) < 2:
        print("not enough frames")
        return 1

    lo, hi = min(means), max(means)
    avg = sum(means) / len(means)
    print(f"frames                 {len(means)}")
    print(f"mean luminance         min {lo:.1f}, mean {avg:.1f}, max {hi:.1f}")
    print(f"range / mean           {(hi - lo) / avg:.3f}" if avg else "")
    print()

    # Frame-to-frame swing is the number that matters, not the overall spread: a slow drift across a
    # run is an AE or thermal effect, whereas large ALTERNATION between neighbours is the signature of
    # something beating against the exposure window.
    deltas = [abs(means[i] - means[i - 1]) for i in range(1, len(means))]
    big = sum(1 for d in deltas if avg and d > 0.25 * avg)
    print(f"neighbour |delta|      mean {sum(deltas)/len(deltas):.1f}, max {max(deltas):.1f}")
    print(f"swings > 25% of mean   {big} of {len(deltas)}")
    print()

    print("first 24 frames:")
    for i, m in enumerate(means[:24]):
        bar = "#" * int(m / 4)
        print(f"  {i:3d}  {m:6.1f}  {bar}")

    print()
    if avg and big > len(deltas) // 4:
        print("FLICKERING — successive frames of one scene differ substantially in total light.")
        print("  Nothing downstream can be tuned around this. Candidates, in order of likelihood:")
        print("    * panel PWM dimming beating against the exposure window (raise TX brightness to")
        print("      100%, which usually lengthens or removes the PWM duty gaps)")
        print("    * the exposure straddling a display-state transition (raise the state period,")
        print("      i.e. a larger divisor, or shorten exposure)")
        print("  Both are physical and neither appears in a simulator that renders a steady screen.")
    else:
        print("ok — frame-to-frame luminance is stable; flicker is not the problem here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
