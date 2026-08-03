# ADR-0005 — Dual CPU and GPU camera receive paths

> **Status:** Proposed
> **Date:** 2026-08-02
> **Owner:** RX subsystem
> **Related:** ADR-0003, EXP-007, RISK-001, RISK-002, OQ-001, OQ-002

## Context

The project's stated architecture pairs a "Camera2 or NDK Camera receiver" with "low-copy
YUV camera processing". Phase 0 research found that this pairing **cannot hold at high
frame rates.**

From the AOSP javadoc for `CameraDevice.createConstrainedHighSpeedCaptureSession`
— the only portable route to ≥120 fps capture: `[FACT]`

> "a high speed capture session will only support up to 2 output Surfaces"

> "All Surfaces must be either video encoder surfaces (acquired by
> `MediaRecorder#getSurface` or `MediaCodec#createInputSurface`) or preview surfaces
> (obtained from `SurfaceView`, `SurfaceTexture` via `Surface#Surface(SurfaceTexture)`)."

`ImageReader` is **not** in the permitted list. There is therefore **no CPU-accessible
`YUV_420_888` output in a high-speed session at all.**

This directly affects milestone 6 (investigating >1 MB/s on 120 Hz/120 fps devices), which
depends on capturing at 120 fps.

## Decision

Implement **two capture paths behind one interface**, converging at the soft-symbol buffer:

| Path | Frame rate | Access | Cell sampling |
|---|---|---|---|
| **CPU path** | ≤60 fps | `AImageReader`, `YUV_420_888`, Y plane | NEON over the Y plane |
| **GPU path** | ≥120 fps | `SurfaceTexture` → `GL_TEXTURE_EXTERNAL_OES` | Compute/fragment shader; only soft symbols read back |

The **CPU path is the primary path for Phases 1–5** (it is simpler to debug and 60 fps
suffices to reach milestone 4). The **GPU path is required for milestone 6** and is not
optional there.

Everything downstream of the cell sampler is shared and identical.

## Alternatives

1. **CPU-only, capped at ≤60 fps.** Simplest, and adequate through milestone 5. Rejected
   as the sole approach because it forecloses milestone 6 entirely.
2. **GPU-only for both paths.** Attractive for uniformity, and defensible. Rejected for
   now because debugging a shader-based decoder while simultaneously bringing up the
   optical link would compound two hard problems. Worth revisiting once the link works —
   if the GPU path proves strictly better, collapsing to one path is a real simplification.
3. **`MediaCodec` encoder surface, decode later.** **Rejected** — lossy video compression
   destroys the high-spatial-frequency cell structure we are modulating. This also
   constrains the capture harness: recordings must be lossless or they are not
   representative. `[HYP]` — confirm in EXP-007.
4. **Vendor high-speed extensions.** Non-portable, undocumented. Not considered.

## Consequences

**Positive.** Milestone 6 stays reachable. GPU sampling of ~34,560 independent cells is
embarrassingly parallel and the readback is ~1/50th of frame size. The `CaptureSource`
abstraction that makes this possible is the same one the simulator and replay harness use.

**Negative.** **Two implementations of the most performance-critical component (C08).**
They can diverge. Mitigated by a mandatory CPU/GPU output-equivalence test in CI.
GPU readback latency may serialise the pipeline. Shader debugging is materially harder.

**Neutral.** The GPU path may turn out to be better at *all* frame rates, in which case
the CPU path becomes a fallback rather than the primary.

## Evidence

- High-speed session surface restrictions. `[FACT]` — AOSP `CameraDevice.java`.
- Y plane of `YUV_420_888` is full-resolution 8-bit luminance, sufficient for M0/M1/M2
  with no chroma involvement. `[FACT]`
- Chroma is 2×2 subsampled, constraining M3 colour cell size. `[FACT]`
- Whether 120 fps GPU-path frames are genuinely *distinct* (rather than duplicated by the
  vendor pipeline) is **unverified** — OQ-002. If they are not distinct, milestone 6 is
  unreachable regardless of path. `[OPEN]`

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| RISK-001: no accessible uncompressed 120 fps frames on any reference device | Medium | GPU path; if it also fails, milestone 6 is reported as not achievable and we say so |
| RISK-002: high-speed mode restricts resolution below what our grid needs | Medium-High | `getHighSpeedVideoSizes` enumerated in the capability probe; grid adapted to available resolution |
| Two sampler implementations diverge | Medium | Mandatory equivalence test in CI |
| GPU readback stalls the pipeline | Medium | Asynchronous readback with double buffering; measure in EXP-007 |

## Validation plan

**EXP-007** is the gating experiment and should run early — it determines a large
implementation fork. It must establish, per reference device:

1. Maximum frame rate at which CPU-accessible `YUV_420_888` is delivered without drops, at
   a resolution sufficient for each candidate grid.
2. Whether a high-speed session delivers genuinely distinct frames via `SurfaceTexture` at
   120 fps.
3. Available high-speed resolutions versus grid requirements.
4. GPU readback latency and whether it serialises the pipeline.
5. Whether any lossless recording path exists for the capture harness.

Until EXP-007 runs, **milestone 6 has no evidence behind it** and should be described as
an open research question, not a plan.
