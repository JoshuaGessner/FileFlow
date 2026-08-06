# Offline channel simulator plan

> **Status:** Draft
> **Owner:** Testing / simulation
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0010, C16, RISK-024, EXP-010, EXP-011, EXP-012

> ### ⚠ Missing impairment: corner occlusion (F34)
>
> The renderer draws a mathematically perfect rectangle, so it **cannot** reproduce the failure that
> blocked the project's first real decode: a phone's rounded display corners physically cut away the
> corners of a full-bleed optical frame — measured at 20–77 camera pixels, over ten cells — which
> breaks both the corner-extreme search and marker verification while every other property of the
> capture looks healthy.
>
> This is the third impairment whose *absence* produced a confident wrong conclusion (F14's
> retraction, F16's missing density cliff, F34). **SIM-03 calibration should not claim the model
> matches reality until the model can express this failure**, because otherwise it fits a model
> structurally incapable of the thing being calibrated against.

## Purpose

Generate optical frame sequences, apply controlled channel impairments, and feed the
**real decode chain** — with exact ground truth and no hardware. It exists so that
decisions about coding, modulation and frame layout can be made from data before any
device work, and so that decoder regressions are caught deterministically.

## Non-purpose

**The simulator is not a predictor of field performance until it is calibrated** against
recorded real captures in Phase 2. Until then every simulator output is tagged `[HYP]`.
This is RISK-024, the second-highest-priority risk in the register, and it deserves
repeating because a simulator that models a kinder channel than reality produces
*confident wrong conclusions* — worse than no simulator at all.

## Structure

```
config (versioned) ──► frame generator (C04, the real one)
                          │
                          ▼
                   impairment pipeline
                          │
                          ▼
                  CaptureSource ──► real decode chain (C06–C13)
                          │
                          ▼
              metrics + ground-truth comparison + diagnostics
```

The frame generator and decode chain are **the production components**, not
reimplementations. If they were reimplemented, the simulator would test the wrong code.

## Impairment pipeline

Applied in an order that approximates physical reality. Order matters — applying noise
before blur models a different sensor than applying blur before noise.

| # | Impairment | Parameters | Models |
|---|---|---|---|
| 1 | Display pixel structure | Subpixel layout, fill factor | Panel emission structure |
| 2 | Perspective transformation | Homography from distance/angle | Camera viewpoint |
| 3 | Lens distortion | Radial k1,k2, tangential p1,p2 | Real lens geometry |
| 4 | Defocus blur | PSF radius, per-region variation | Focus error, field curvature |
| 5 | Motion blur | Direction, magnitude | Hand motion during exposure |
| 6 | Spatial crosstalk | Kernel | Optical + sensor charge spread |
| 7 | Vignetting | Cos⁴ or measured profile | Lens falloff |
| 8 | Glare / local saturation | Position, intensity, extent | Reflections, bright spots |
| 9 | Exposure change | Gain over time | AE drift when locking fails |
| 10 | Gamma / tone curve | Curve | Camera response non-linearity |
| 11 | White-balance change | Channel gains over time | AWB drift |
| 12 | Colour-channel mixing | 3×3 matrix | Display primaries → CFA response |
| 13 | Shot noise | Photon count model | Signal-dependent noise |
| 14 | Gaussian read noise | σ | Sensor read noise |
| 15 | Camera sampling | Sensor grid, sensor pitch | Spatial sampling and aliasing (moiré) |
| 16 | Chroma subsampling | 4:2:0 | `YUV_420_888` chroma loss |
| 17 | Rolling-shutter mixture | Skew, boundary position | The mixed-frame problem |
| 18 | Partial occlusion | Region, opacity | Fingers, cases, obstructions |
| 19 | Frame drops | Rate, burstiness | Camera pipeline drops |
| 20 | Duplicate frames | Rate | Clock mismatch |
| 21 | Timing drift | ppm | Independent display and camera clocks |

**Order rationale:** 1–3 are geometric and happen before the optics. 4–8 are optical.
9–12 are photometric/ISP. 13–14 are sensor noise, applied at the sensor stage where shot
noise is signal-dependent. 15–16 are sampling and format. 17–21 are temporal, applied at
the sequence level rather than per frame.

## Run configuration

Every run is fully described by a versioned configuration file. **No run without a config
file**; no results accepted without their config committed alongside.

```yaml
# Example: sim/configs/handheld-moderate.yaml
version: 1
seed: 20260802          # every stochastic impairment derives from this
description: "Typical indoor handheld, 30 cm, slight angle"

source:
  payload: deterministic-random   # or a path
  size_bytes: 1048576

link:
  grid: [120, 200]
  layout: B                        # candidate layout
  modulation: M0
  fec: {family: ldpc, rate: 0.80}
  fountain: {scheme: lt, block_size: 4096}
  display_states_per_second: 60

geometry:
  distance_cm: 30
  angle_deg: 15
  lens_distortion: {k1: -0.12, k2: 0.03}

optics:
  defocus_psf_radius_px: 1.2
  motion_blur: {magnitude_px: 0.8, direction_deg: 30}
  crosstalk_kernel: gaussian_0.6
  vignetting: cos4
  glare: null

photometric:
  gamma: 2.2
  exposure_drift_per_s: 0.01
  white_balance_drift_per_s: 0.0

sensor:
  resolution: [1920, 1080]
  capture_fps: 60
  shot_noise: true
  read_noise_sigma: 2.0
  chroma_subsampling: "420"
  rolling_shutter_skew_ms: 12.0

temporal:
  frame_drop_rate: 0.05
  frame_drop_burstiness: 0.3
  duplicate_rate: 0.10
  clock_drift_ppm: 50

output:
  metrics: true
  diagnostics: [rectified_frames, error_maps, llr_histograms]
```

**Reproducibility rule:** the same config plus the same commit must produce byte-identical
results. Any non-determinism is a bug — it makes regression testing meaningless.

## Outputs

Per run:

| Output | Purpose |
|---|---|
| Symbol error rate | Demodulator quality |
| Frame error rate | Post-FEC frame outcome |
| Header failure rate | Measures `H` directly |
| Erasure rate | Feeds the fountain layer |
| FEC outcome distribution | Corrected / uncorrectable / **miscorrected** |
| Fountain overhead | Measures `Rfountain` directly |
| **Reconstructed-file correctness** | The end-to-end truth |
| Processing time per stage | Performance budgeting |
| **Ground-truth comparison** | Geometric error, phase-classification accuracy, per-cell error map — *only available in simulation* |

### Visual diagnostics
Rectified frames, per-cell error maps (showing spatial clustering, which drives interleaver
design), LLR histograms split by true symbol value (the direct test of LLR calibration),
and transition-band overlays for mixed frames.

The **per-cell error map** is the most valuable diagnostic: it makes spatial error
clustering visible, which is otherwise inferred indirectly.

## Sweep capability

Sweeps are the point. A sweep runs the cross-product of parameter ranges, in parallel,
and emits a table plus a response surface.

```bash
sim sweep --config base.yaml \
          --vary link.grid=96x160,120x200,144x240 \
          --vary optics.defocus_psf_radius_px=0.5:2.5:0.25 \
          --repeat 20 --out data/experiments/EXP-001/raw/
```

`--repeat` with different seeds is mandatory for anything stochastic — a single run is an
anecdote here just as much as on hardware.

## Validation of the simulator itself

| Check | Requirement |
|---|---|
| Identity channel | No impairments ⇒ perfect decode. Failure = decoder bug, not channel effect |
| Monotonicity | Increasing an impairment must not *improve* results (catches sign errors) |
| Determinism | Same seed + config + commit ⇒ identical output |
| **Phase 2 calibration** | Impairment parameters fitted to recorded captures; predicted vs measured symbol error rate reported **as a number** |

The last row is the one that matters. **Exit criterion for trusting simulator results:**
predicted and measured symbol error rates agreeing within a stated tolerance across the
tested conditions. Until then, simulator conclusions are hypotheses.

## Scope discipline for Phase 1

The temptation is to build a complete optical model. Resist it. Phase 1 needs enough
fidelity to make coding and layout decisions — impairments 1–7, 13–17, 19–21 at moderate
fidelity. Glare, occlusion and colour mixing (8, 12) can be crude initially since M3 is
deferred anyway.

**Phase 1 is not done when the simulator is realistic. It is done when it is useful enough
to run EXP-010, EXP-011 and EXP-012.**
