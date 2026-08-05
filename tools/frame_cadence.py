#!/usr/bin/env python3
"""Frame-cadence analysis for a capture run's per-frame CSV (EXP-007).

WHY THIS EXISTS. An average frame rate cannot distinguish two completely different problems:

  * a sensor genuinely running slower than requested, and
  * a sensor running at exactly the requested rate whose frames are being DROPPED downstream.

They demand opposite fixes — the first is a camera-configuration or hardware limit, the second is a
consumer that is too slow — and finding F28 turned on telling them apart. The discriminator is the
MODAL inter-frame interval: dropped frames add whole multiples of the true period, so they cannot
shift the most common value. If the mode matches the nominal period, the sensor is fine.

Usage:  tools/frame_cadence.py <capture-frames.csv> [nominal_fps]

The CSV is written by CaptureActivity alongside the bundle (index, timestamp_ns, delta_ns).
"""
import csv
import statistics
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    path = sys.argv[1]
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    period = 1e9 / fps

    deltas = []
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            d = int(row["delta_ns"])
            if d > 0:  # the first row has no predecessor
                deltas.append(d)

    if not deltas:
        print("no usable deltas in", path)
        return 1

    # Bucket to 0.01 ms. Fine enough to separate 60 fps from 59, coarse enough that sensor jitter
    # does not scatter the mode across neighbouring bins.
    buckets: dict[int, int] = {}
    for d in deltas:
        key = round(d / 10_000)
        buckets[key] = buckets.get(key, 0) + 1
    modal = max(buckets, key=lambda k: buckets[k]) * 10_000

    gaps = [d / modal for d in deltas]
    dropped = sum(round(g) - 1 for g in gaps if round(g) >= 2)
    expected = len(deltas) + 1 + dropped

    print(f"file                   {path}")
    print(f"frames received        {len(deltas) + 1}")
    print(f"modal interval         {modal / 1e6:.4f} ms  =>  {1e9 / modal:.2f} fps cadence")
    print(f"nominal interval       {period / 1e6:.4f} ms  =>  {fps:.2f} fps requested")
    print(f"median interval        {statistics.median(deltas) / 1e6:.4f} ms")
    print(f"mean interval          {statistics.fmean(deltas) / 1e6:.4f} ms")
    print()
    print(f"gap events (>1.5x)     {sum(1 for g in gaps if g > 1.5)}")
    print(f"frames dropped         {dropped}  ({dropped / expected:.4f} of expected {expected})")
    print(f"largest gap            {max(gaps):.2f}x modal")
    print()
    print("A modal interval matching the nominal period means the sensor IS running at the")
    print("requested rate, so a shortfall in AVERAGE fps is dropped frames rather than a slow")
    print("camera. Gaps of exactly 2x point at a consumer that missed single frames (buffer-return")
    print("latency); larger or ragged gaps point at stalls.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
