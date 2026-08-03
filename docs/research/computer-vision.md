# Research: computer vision for screen acquisition and sampling

> **Status:** Draft
> **Owner:** CV subsystem
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0006, EXP-015, EXP-017, EXP-018, OQ-008

## The pipeline

```
camera frame → screen detection → homography → tracking → rectification
            → photometric normalisation → subpixel cell sampling → soft symbols
```

Detection runs once (and on reacquisition). Everything after it runs every frame and is
on the critical path.

## The central bet

QR detection re-solves localisation from scratch on every frame. FileFlow detects once and
then **tracks**, which should be an order of magnitude cheaper per frame and should also be
*more accurate*, because tracking can use a motion model and sub-pixel refinement across
frames rather than re-deriving geometry from a single noisy image. `[HYP]` — this is one of
the load-bearing hypotheses of the whole project (EXP-017).

The risk is that tracking drifts or loses lock under handheld motion, and that
reacquisition is slow enough to cost more than per-frame detection would have. The
mitigation is a persistent screen boundary and always-present corner markers in every
frame, so reacquisition never needs to wait for a special sync frame.

## Screen detection

The transmitter's screen is a bright quadrilateral on a darker background — an easier
target than a general object. Candidate approaches:

| Approach | Notes |
|---|---|
| **Asymmetric corner markers** | Primary approach. Four distinct markers whose asymmetry resolves rotation and reflection unambiguously. Unlike QR's three-plus-one arrangement, all four can be different, giving orientation from any three (robust to one occluded corner). |
| **Bright-quad segmentation** | Threshold, find contours, filter by area/convexity, approximate to 4 points. Cheap first pass to seed marker search. |
| **Edge/line detection + intersection** | Hough or LSD lines, intersect for corners. Gives subpixel corner accuracy from long edges, which is better than corner-detector accuracy. Good refinement stage. |

**Design requirement:** markers must be present in **every** optical frame, not only in a
periodic sync frame. Reacquisition after occlusion must be possible from any single frame.

## Homography

Four point correspondences give the 3×3 projective transform from display plane to image
plane. Practical notes:

- Use **all four corners plus edge-derived constraints**, solved in a least-squares sense,
  rather than the minimal four-point solution — the minimal solution is noise-sensitive.
- **Normalise coordinates** before solving (Hartley normalisation). Skipping this is a
  classic source of conditioning problems.
- Refine with an iterative photometric or feature-based alignment against the expected
  frame structure.
- The homography assumes a **planar** display, which is correct, and a **pinhole** camera,
  which is not — hence lens distortion below.

## Lens distortion

A homography cannot represent radial distortion. At the edges of a wide-angle phone camera
this is significant, and it manifests as cell sampling points drifting off-centre toward
the frame periphery — precisely where SNR is already worst.

Options:
1. **Per-device calibration** stored in the capability profile. Most accurate; requires a
   calibration step.
2. **Self-calibration from the optical frame itself.** Our transmitted frame is a known
   dense regular grid — which is, in effect, a calibration target. Timing tracks and pilot
   rows give a dense set of known-position features. This is an elegant option specific to
   our situation and worth pursuing. `[HYP]`
3. **Ignore distortion, shrink the usable area.** Crude fallback.

Option 2 is attractive and under-explored. Recorded as OQ-008.

## Tracking

| Method | Cost | Notes |
|---|---|---|
| **Corner re-detection in a predicted ROI** | Low | Simplest. Predict marker positions from the previous homography, search a small window. Probably sufficient for the stationary and rigid-mount cases. |
| **Sparse optical flow (Lucas–Kanade)** on marker and pilot features | Low-moderate | Good under moderate motion. Pyramidal implementation handles larger displacement. |
| **Edge tracking** on the screen boundary | Low | The screen boundary is a strong, long, high-contrast feature — cheap and accurate for subpixel refinement. |
| **Dense/ML tracking** | High | Not justified (NG6). |

**Reacquisition strategy:** a state machine — `SEARCHING → ACQUIRED → TRACKING → DEGRADED
→ LOST → SEARCHING`. Transition to `DEGRADED` on rising residuals or falling pilot SNR,
*before* lock is actually lost, so the adaptive controller can react. Time spent in
`SEARCHING` is time at zero goodput and must be logged as such.

## Subpixel cell sampling

This is where accuracy is won or lost, and it is the most under-appreciated stage.

Each logical cell maps to a quadrilateral region in the camera image. Sampling options:

| Method | Accuracy | Cost |
|---|---|---|
| Nearest-neighbour at cell centre | Poor — quantisation error, sensitive to sub-pixel registration error | Lowest |
| **Bilinear at cell centre** | Better; still sensitive to crosstalk | Low |
| **Weighted average over the cell's interior** (excluding a border margin) | **Best** — averages out sensor noise and moiré while excluding neighbour bleed | Moderate |
| Matched filter / deconvolution | Best in principle; needs a PSF estimate | High |

**The interior-margin idea matters.** Sampling only the central fraction of each cell
(say the middle 50–60% by area) discards the edges where neighbouring cells bleed in.
This trades a little noise averaging for a large reduction in crosstalk, and it costs
nothing at transmission time. `[HYP]` — the optimal margin is a free parameter to sweep
in the simulator and then on device (part of EXP-001).

**GPU implementation:** cell sampling is a gather over ~34,560 independent regions —
ideal for a fragment or compute shader, and *mandatory* on the high-frame-rate path where
frames only exist as GPU textures (see
[android-camera-pipeline.md](android-camera-pipeline.md)). The shader outputs a small
soft-symbol texture that is read back; readback volume is ~1/50th of the frame.

**CPU implementation:** NEON, processing multiple cells per vector operation, with the
Y plane accessed directly and stride respected. EXP-016 compares the two.

## Photometric normalisation

Before slicing symbols, remove everything about the image that is not signal:

1. **Vignetting / illumination field** — estimate a low-order surface (e.g. bi-quadratic)
   from pilot cells distributed across the grid, and divide it out.
2. **Exposure drift** — normalise against known-brightness pilot cells per frame.
3. **Gamma / tone curve** — linearise. Best handled by forcing a linear camera tone curve
   (`TONEMAP_MODE = CONTRAST_CURVE`), with a software fallback estimated from a pilot
   ramp if the device ignores it.
4. **Local contrast** — after global correction, per-region thresholds from local pilots.

Order matters: global multiplicative corrections first, then local additive/threshold
adjustments.

**Why pilots must be spatially distributed, not clustered:** a corner block of pilots
tells you nothing about the illumination field's shape in the middle. Pilot placement is a
frame-layout decision with direct photometric consequences — see
[OPTICAL-FRAME-CANDIDATES.md](../specifications/OPTICAL-FRAME-CANDIDATES.md).

## Blur and confidence estimation

Blur (defocus and motion) is the dominant driver of spatial crosstalk. We need a
**per-region** blur estimate, because focus falls off toward the periphery and motion blur
is directional.

Candidate estimators:
- Edge-spread measurement on known high-contrast pilot transitions
- Local variance / gradient energy in regions of known pattern
- Comparing measured pilot cell contrast against expected

The blur estimate feeds two consumers: the **soft-symbol LLR** (blurrier region → lower
confidence) and the **adaptive link controller** (persistent blur → drop to a coarser
grid or more robust modulation).

## Motion detection

Handheld motion causes both geometric change (tracked) and motion blur (not correctable).
A fast motion detector — frame-to-frame homography delta magnitude — lets the receiver
mark frames as motion-degraded and the controller respond. Marking a motion-blurred frame
as erased is far better than decoding it into a burst of wrong bits with high confidence.

## Colour calibration (M3 only)

Required before any colour constellation is usable:
- Lock white balance (`CONTROL_AWB_MODE = OFF` + explicit `COLOR_CORRECTION_GAINS`)
- Estimate the 3×3 display-primaries → camera-response matrix from colour pilot cells
- Invert it per region to decorrelate colour channels

The colour channels are **not** independent: display primaries, camera CFA and colour
processing all mix them, and `YUV_420_888` subsamples chroma 2×2. Colour pilot cells are
reserved in every frame layout from the start (even in M0/M1, where they go unused) so
that enabling M3 later does not require a frame-format change.

## Open questions

| ID | Question |
|---|---|
| OQ-008 | Can we self-calibrate lens distortion from the transmitted grid, avoiding per-device calibration? |
| OQ-011 | Effective number of independent spatial subchannels at each cell pitch? |
| OQ-012 | Distribution of spatially-clustered damage region sizes (drives interleaver design)? |
| OQ-019 | What is the optimal cell interior sampling margin? |
| OQ-020 | Is tracking actually cheaper *and* more accurate than per-frame detection under handheld motion? (EXP-017) |
