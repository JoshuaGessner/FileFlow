# EXP-007 raw — CPU capture path delivery, samsung SM-S948U1, 2026-08-04

Raw observations from the C05 CPU-path recorder. Reproduce with `tools/android_capture.sh` and
`tools/frame_cadence.py`.

⚠ **Single run per arm. No spread reported.** BENCHMARK-METHODOLOGY requires median and range for
performance reporting; these are single samples and the *ranking* between arms is the claim, not
the individual figures.

## Provenance

| | |
|---|---|
| Device | samsung SM-S948U1, SoC SM8850 |
| Fingerprint | `samsung/m3quew/m3q:16/BP4A.251205.006/S948U1UES4AZG3_OYM4AZG3:user/release-keys` |
| Android | 16 (API 36) |
| App | `dev.fileflow` 0.1.0-phase2 |
| Camera | id `0` (rear), `YUV_420_888` via `ImageReader`, 6 buffers |
| Conditions | phone face-up on a desk under a desk lamp. **No transmitter** — this measures camera delivery only |
| Frames requested | 300 per arm, 60 fps |

Manual settings were **requested** and **read back from `CaptureResult`**, not assumed:

| Control | Requested | Reported |
|---|---|---|
| `CONTROL_AE_MODE` | OFF | **0 (OFF)** — honoured |
| `SENSOR_EXPOSURE_TIME` | 4,166,666 ns | **4,166,666 ns** — exact |
| `SENSOR_SENSITIVITY` | 400 | **398** — quantised to a supported value |
| `SENSOR_FRAME_DURATION` | 16,666,666 ns | **16,658,337 ns** (60.03 fps ceiling) |
| `LENS_FOCUS_DISTANCE` | 0 (infinity) | **0.0** |
| `EDGE_MODE` | OFF | **0 (OFF)** — honoured |
| `NOISE_REDUCTION_MODE` | OFF | **0 (OFF)** — honoured |

## The three arms

Identical camera configuration in all three. Only the frame size and whether frames were written
to disk differ.

| Arm | Size | Y plane | Writes | Delivered | Duplicates | Worst gap |
|---|---|--:|:--|--:|--:|--:|
| **A** | 1920×1440 | 2.76 MB | **ON** | **32.11 fps** | 0 | 5.00 periods |
| **B** | 1920×1440 | 2.76 MB | **OFF** | **59.04 fps** | not measured | 2.00 periods |
| **C** | 1280×720 | 0.92 MB | **ON** | **56.80 fps** | 0 | 2.00 periods |

Arm B's duplicate count is recorded as *not measured*, not as zero: duplicate detection lives in
the writer, and that arm does not write.

## Cadence analysis, arm C

`tools/frame_cadence.py armC-frames.csv 60`:

```
frames received        300
modal interval         16.6600 ms  =>  60.02 fps cadence
nominal interval       16.6667 ms  =>  60.00 fps requested
median interval        16.6596 ms
mean interval          17.6068 ms

gap events (>1.5x)     17
frames dropped         17  (0.0536 of expected 317)
largest gap            2.00x modal
```

**The sensor runs at exactly the requested rate.** The modal interval is 16.66 ms — 60.02 fps —
and every gap is exactly 2× that, never more. So the shortfall from 60.02 to 56.80 is **17 dropped
frames (5.4%)**, each a single miss. Single misses of a steady cadence point at the
`ImageReader` buffer queue momentarily running dry — the writer not returning a buffer in time —
rather than at any sensor stall.

## Duplicate frames

**0 duplicates across 600 written frames** (arms A and C). A duplicate is a frame byte-identical to
its predecessor; sensor noise makes that decisive, because two real exposures of even a static
scene differ in a large share of their bytes. So the CPU path delivers genuinely distinct frames.

This says **nothing** about the 240 fps constrained high-speed path, which is a different API and
where the research notes place the duplication risk. That arm has not been run.

## Replay through the production chain

The arm C bundle (300 frames, 264 MB) was pulled and fed to `ffreplay`:

```
--- capture bundle ---
sender / receiver        (unrecorded) -> samsung SM-S948U1
app commit               see git; app 0.1.0-phase2
grid                     0x0
capture                  1280x720 @ 60.0 fps, 300 frames
rig                      distance -1.0 cm, angle -1.0 deg, handheld/unspecified

⚠ INCOMPLETE METADATA — not usable as experimental evidence.
  missing: sender_model grid_cols/grid_rows distance_cm source_payload_sha256

layout: value_out_of_range
```

**The harness refused it, correctly.** There was no transmitter, so no grid was recorded, and
`FrameLayout::Create(0, 0)` rejects it. The format round-tripped from a real device — metadata
parsed, all 300 frames found — and C17's incomplete-metadata guard fired on the very first real
capture, which is exactly what it was built early to do (F17).

No real optical frame has been decoded. That needs a transmitter (C03/C04).

## Raw frame CSV

Per-frame timestamps for arm C are not committed — 300 rows of telemetry alongside a 264 MB
capture set that `docs/testing/CAPTURE-HARNESS.md` deliberately keeps out of git. Regenerate with
`tools/android_capture.sh`; the CSV is written to the app's files dir as `capture-frames.csv`.
