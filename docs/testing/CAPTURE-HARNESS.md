# Recorded-frame test harness

> **Status:** Draft
> **Owner:** Testing
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0010, C17, SIMULATOR-PLAN.md, OQ-023

## Purpose

Replay prerecorded camera frames through the decode chain **exactly as the live receiver
would**, via the shared `CaptureSource` interface.

This is what makes optical decoder development tractable:

- **Deterministic regression tests** — a decoder change either still decodes the recording
  or it does not, with no lighting or hand-steadiness variable
- **Replaying difficult captures** — the hard cases become permanent tests instead of
  anecdotes
- **Comparing algorithms** on identical input, which live capture can never provide
- **Profiling without camera variability**
- **Sharing datasets** across machines and contributors
- **Testing the desktop build** against real-world data
- **Preserving experimental evidence** — recordings outlive the sessions that produced them
- **Calibrating the simulator** (SIM-03) — the only ground truth we have about the real
  channel

That last use is the reason this harness must exist before, not after, the simulator is
trusted.

## The compression problem

**Lossy video compression destroys exactly what we are measuring.** Our signal is
high-spatial-frequency cell structure; video codecs are designed to discard precisely that
in favour of perceptual quality. A recording made through H.264 is not a recording of our
channel.

Compounding this: the ≥120 fps high-speed capture path permits only encoder or preview
surfaces `[FACT]` — so on that path, the obvious recording route *is* a lossy encoder.

**OQ-023** asks whether any lossless path exists (raw frame dump to storage at rate; a
lossless codec; reduced-rate capture for recording purposes only). **EXP-007 must answer
this**, because if no lossless path exists at high frame rates, then high-frame-rate
captures cannot be recorded faithfully, and the harness is limited to the ≤60 fps CPU path.

That would be a real limitation and should be stated plainly rather than worked around
with a lossy recording that quietly misrepresents the channel.

## Storage format

```
captures/
  2026-08-02_pixel-to-pixel_30cm_0deg_office/
    metadata.yaml            # the full record below
    frames/
      000000.y8              # raw Y plane, or
      000000.yuv420          # full YUV if chroma is needed (M3)
    capture_results/         # per-frame CaptureResult metadata
      000000.json
    transmitted/
      expected_frames.bin    # ground truth: what the TX intended to display
      payload.bin            # the source file
      manifest.json
    notes.md
```

Frames are stored raw. Storage cost is high — a 1920×1080 Y8 frame is ~2 MB, so 60 fps for
10 seconds is ~1.2 GB. This is accepted; a curated set of short, difficult captures is far
more valuable than a large set of easy ones.

**Curation rule:** keep captures that are *informative* — near the edge of decodability,
or exhibiting a specific failure. A capture that decodes perfectly teaches little after
the first one.

## Required metadata

Every capture carries all of this. A capture missing metadata is uninterpretable later and
should be discarded — which is why the recording tool must collect it automatically rather
than relying on the operator.

```yaml
capture_id: 2026-08-02_pixel-to-pixel_30cm_0deg_office
timestamp: 2026-08-02T14:32:11Z
operator: <name>

sender:
  model: <device model>
  os_build: <full build fingerprint>
  panel_type: OLED | LCD
  native_resolution: [W, H]
  display_mode: {resolution: [W, H], refresh_hz: 60}
  brightness: 1.0                 # normalised, and the raw platform value
  app_commit: <git sha>

receiver:
  model: <device model>
  os_build: <full build fingerprint>
  camera_id: "0"
  hardware_level: FULL | LEVEL_3 | LIMITED | LEGACY
  resolution: [W, H]
  fps_requested: 60
  fps_achieved: 58.7              # measured, not requested
  session_type: normal | constrained_high_speed
  surface_type: image_reader | surface_texture
  exposure_time_ns: 4000000
  iso: 200
  focus_mode: OFF
  focus_distance_diopters: 3.3
  awb_mode: OFF
  color_correction_gains: [1.9, 1.0, 1.0, 1.6]
  edge_mode: OFF
  noise_reduction_mode: OFF
  tonemap_mode: CONTRAST_CURVE
  stabilization: OFF
  timestamp_source: REALTIME | UNKNOWN
  rolling_shutter_skew_ns: 12000000
  app_commit: <git sha>

geometry:
  distance_cm: 30
  angle_deg: 0
  mounting: rigid_mount | handheld | tripod
  motion_condition: static | light_handheld | heavy_handheld

environment:
  ambient_lux: 320
  light_source: fluorescent | led | daylight | mixed
  notes: "overhead office lighting, no direct glare"

link:
  modulation_profile: M0
  grid: [120, 200]
  layout: B
  fec: {family: ldpc, rate: 0.80}
  fountain: {scheme: lt, block_size: 4096}

payload:
  source: deterministic_random
  size_bytes: 1048576
  sha256: <hex>

expected_result:
  should_decode: true
  expected_goodput_range_kbps: [500, 900]
  notes: "baseline good-conditions capture"
```

**`fps_achieved` versus `fps_requested`** is deliberately separate — the gap is data, and
recording only the request would hide it.

**`expected_result`** matters: a capture recorded as a *failure case* is as valuable as a
success, and the harness must be able to assert "this should still fail in this way"
so that a change which accidentally makes it pass is investigated rather than celebrated.

## Regression use

```bash
harness replay captures/ --assert-expectations --report out.json
```

Runs every capture through the current decoder, compares against `expected_result`, and
fails CI on regression. New captures are added whenever a novel failure is found in the
field — that is the mechanism by which field experience becomes permanent test coverage.

## Relationship to the simulator

| | Simulator | Capture harness |
|---|---|---|
| Ground truth | **Exact** (per-cell, geometric, phase) | Partial (transmitted content known; geometry not) |
| Realism | **Unvalidated** until calibrated | **Real by construction** |
| Cost per case | Near zero | High (hardware, time, storage) |
| Parameter sweeps | **Yes, freely** | No |
| Novel failure modes | Only those modelled | **Whatever reality produces** |

They are complements. The simulator explores parameter space cheaply; the harness checks
that the space being explored resembles reality. Neither alone is sufficient — and the
harness is what keeps the simulator honest (RISK-024).
