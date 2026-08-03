# The first three engineering experiments

> **Status:** Draft — recommended immediate work
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** EXPERIMENT-REGISTRY.md, ROADMAP.md

These three are recommended first because each one **can invalidate a large amount of
planned work**, and each is cheap relative to what it de-risks. Ordered by how much they
change if the answer surprises us.

They are deliberately *not* the most interesting experiments. They are the ones whose
answers everything else depends on.

---

## 1. EXP-007 — Camera frame-rate and access-path characterisation

**Run first. It determines the receiver's fundamental shape.**

### Why first
Phase 0 research established `[FACT]` that Android's constrained high-speed capture session
permits only encoder or preview surfaces — **no `ImageReader`, therefore no CPU-accessible
`YUV_420_888` at ≥120 fps.** The receiver architecture forks on what is actually available:

- If CPU `YUV_420_888` reaches 60 fps at adequate resolution → build the CPU path first,
  as planned, and defer the GPU path to Phase 8.
- If the GPU path delivers distinct 120 fps frames → milestone 6 has a basis, and the GPU
  sampler becomes a first-class deliverable rather than a later addition.
- If neither → **milestone 6 should be reported as not achievable on this hardware**, and
  the roadmap should stop treating it as a plan.

Writing the wrong sampler first costs weeks. This experiment costs days and needs no
optical link at all.

### What it must establish, per reference device
1. Maximum fps with CPU-accessible `YUV_420_888` without drops, at resolutions sufficient
   for 96×160, 120×200 and 144×240 grids.
2. Whether a high-speed session via `SurfaceTexture` yields **genuinely distinct** frames
   at 120 fps — verified by displaying a known counting pattern, not by trusting the
   frame rate.
3. Available high-speed resolutions versus grid requirements (RISK-002).
4. GPU readback latency, and whether it serialises the pipeline.
5. Whether requested manual controls (`EDGE_MODE`, `NOISE_REDUCTION_MODE`, `TONEMAP_MODE`,
   exposure, ISO, focus, AWB) are actually honoured — check returned `CaptureResult`
   metadata, not just that the request was accepted (OQ-016).
6. Whether any **lossless** recording path exists for the capture harness (OQ-023).

### Deliverable
A per-device table plus a recommendation on which capture path is primary, and an explicit
statement on whether milestone 6 is testable.

### Cost
Low. A capability-probe app plus a counting-pattern display. No decoder, no protocol,
no optical link. Can start immediately once reference hardware exists.

**Blocked by:** OQ-033 (reference device selection). Procure hardware first.

---

## 2. EXP-006 — Display state rate verification

**Run second, in parallel with EXP-007 if hardware allows.**

### Why
`Fd` appears **linearly** in every goodput projection, and every scenario in the
performance model assumes requested display states are actually presented. That assumption
is currently unvalidated. `Surface.setFrameRate` is documented as a hint the platform may
refuse `[FACT]`, and VRR/LTPO panels may change mode mid-transfer.

If a device that advertises 120 Hz actually presents 78 distinct states per second under
our rendering load, every projection built on it is wrong by 35% — and we would not know
unless we measured.

### What it must establish
1. Actual distinct presented states per second at requested 30, 60 and 120 Hz, per device.
2. Missed and duplicated frame rates.
3. Whether presentation can be **verified in-app** per frame (`ASurfaceTransaction`
   completion callbacks vs FrameTimeline), or only offline via Perfetto (OQ-004).
4. Whether a requested mode is **held** across a multi-minute render at maximum brightness,
   or whether thermal/power management drops it (OQ-005).
5. Whether OpenGL ES pacing suffices or Vulkan is needed (ADR-0004).

### Method
Render a known counting pattern (frame index encoded as a large, trivially decodable
pattern). Capture ground truth two ways: Perfetto traces with `gfx`/SurfaceFlinger
categories, and a second device recording the screen. Compare intended versus presented
sequences.

The counting pattern is deliberately simple — this experiment tests the *display*, and
using our real optical frame format would confound display errors with decode errors.

### Deliverable
Verified `Fd` per device per requested rate, with a discrepancy rate; a decision on the
presentation-verification mechanism; a go/no-go on Vulkan.

### Cost
Low-moderate. A renderer and Perfetto analysis. No camera, no decoder.

---

## 3. EXP-010 — Differential versus absolute decoding (simulator)

**Run third — as soon as Phase 1's simulator exists. Needs no device.**

### Why
ADR-0008 makes differential modulation the primary optimisation path after the binary
baseline, and it is **the lowest-confidence decision in the initial ADR set**. We found no
primary evidence supporting it, and the arithmetic cuts against two of its four schemes:

- **M1b (complementary frames) halves `Fd`.** It must more than double `Pc × Rfec` just to
  break even.
- **M1a/M1d propagate errors** through the differential chain when a frame is lost — in a
  channel where frame loss is normal and expected.
- **M1c (cell versus local pilot)** has none of these problems, and is arguably just good
  photometric calibration.

Additionally, ChromaCode's measurements show the channel's dominant failure is *frame-level
events*, not static photometric distortion `[LIT]` — and differential modulation addresses
static distortion. It may be solving a problem that is not the bottleneck.

If M1's temporal schemes do not pay, that effort should go to M2 (four-level), which the
performance model says is **required** to reach milestone 4 at 60 display states/s.

### What it must establish
Ranking of M1a, M1b, M1c, M1d against well-calibrated absolute M0, on **end-to-end
goodput**, across:
- static-distortion severity (vignetting, non-uniformity, exposure offset)
- frame-loss rate (0% to 40%)
- clock-drift rate

### Method
Entirely in the offline simulator. Exact ground truth, no camera variability, fully
parallel, hundreds of configurations. This is precisely what ADR-0010 built the simulator
for.

**Caveat:** simulator results are `[HYP]` until Phase 2 calibration (RISK-024). A clear,
large margin should be trusted; a narrow one should wait for hardware confirmation.

### Deliverable
A keep/drop decision per scheme, and either confirmation of ADR-0008 or its supersession.

### Cost
Low once the simulator exists. This is the cheapest way to test a load-bearing assumption
before building it on hardware.

---

## What these three deliberately exclude

**EXP-001 (maximum resolvable grid)** is arguably more interesting and more central. It is
not in the first three because it needs a working detection and sampling pipeline, which
depends on knowing which capture path to build (EXP-007). Running it fourth, immediately
after, is right.

**EXP-002 (QR baseline)** is essential for G4 but does not gate any architectural decision,
so it can proceed in parallel whenever there is capacity.

## Sequencing

```
Procure reference hardware (OQ-033)
        │
        ├──► EXP-007 (camera paths) ──┐
        │                              ├──► EXP-001 (grid sweep) ──► Phase 2
        └──► EXP-006 (display rate) ──┘

Phase 1 simulator ──► EXP-010 (differential) ──► ADR-0008 confirmed or superseded
                 └──► EXP-011, EXP-012 (coding) in parallel
```

EXP-007 and EXP-006 need hardware but no software beyond probes. EXP-010 needs the
simulator but no hardware. **All three can therefore proceed concurrently** once those two
prerequisites are in place, which is the main reason this particular set was chosen.
