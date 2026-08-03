# ADR-0010 — Offline simulator before aggressive optimisation

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Testing / architecture
> **Related:** ADR-0003, SIMULATOR-PLAN.md, CAPTURE-HARNESS.md, ROADMAP.md

## Context

Optical link development on real hardware is slow and noisy. Every measurement varies with
lighting, hand position, distance, angle, thermal state and device pairing. Distinguishing
a real decoder improvement from a lucky capture requires many runs, and a regression can
hide for weeks behind measurement noise.

Meanwhile, most of the decoder's hard problems — FEC selection, fountain overhead,
interleaver design, phase classification accuracy, LLR calibration — do not require a
camera at all.

## Decision

Build the **offline channel simulator (Phase 1) before the live modem**, and require that
the simulator, the recorded-frame replay harness and the live receiver all feed **the same
decode chain** through a common `CaptureSource` interface.

This makes off-device execution of the decode chain a **hard architectural requirement**:
no decode-path component may depend on Android headers.

## Alternatives

1. **Build the live system first, add a simulator later.** The usual approach. Rejected:
   a simulator retrofitted to an Android-coupled decoder cannot exercise the real decoder,
   so its results do not transfer — which is the entire value proposition.
2. **Simulator only, no replay harness.** Rejected: a simulator models the channel we
   *think* we have. Recorded real captures are the only check on that, and they are also
   the only way to build regression tests from genuinely difficult conditions.
3. **Device-farm automation instead of simulation.** Complementary, not a substitute, and
   far more expensive per experiment.

## Consequences

**Positive.**
- Deterministic, reproducible decoder regression tests.
- Experiments (EXP-011, EXP-012, EXP-008, EXP-010) can run before any device work,
  in parallel, at scale, with exact ground truth — which the camera can never provide.
- Ground-truth homography, ground-truth frame phase and ground-truth symbol values allow
  measuring *component* accuracy, not just end-to-end success.
- Difficult captures become permanent regression tests.
- Desktop debugging tools apply to the whole decode chain.

**Negative.**
- Real work before the first photon crosses the gap. This will feel slow, and there will be
  pressure to skip it.
- The simulator is itself software that can be wrong, and a **too-kind simulator produces
  confident wrong conclusions** — the most dangerous failure mode in this project.
- The `CaptureSource` abstraction constrains the capture code's design.

**Neutral.** The desktop build doubles as a CI target, which we would want regardless.

## Evidence

- The design constraint is structural and follows from ADR-0003's off-device requirement.
- Several of our highest-value early experiments (FEC comparison, fountain overhead, phase
  classification accuracy, differential-versus-absolute) require **exact ground truth**,
  which only simulation provides. This is the strongest concrete argument.
- No measurement supports this ADR; it is a process decision. `[OPEN]` — its cost is real
  and its benefit is projected.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Simulator models a kinder channel than reality | **High** | **High** | Phase 2 calibrates the simulator against recorded real captures; impairment parameters derived from measurement, not intuition. **No simulator result is treated as predictive until this calibration is done.** |
| Time spent on the simulator delays first light | Medium | Medium | Scope Phase 1 tightly: enough impairments to be useful, not a complete optical model |
| Team optimises against simulator artefacts | Medium | High | Every optimisation validated on device before acceptance |

The first risk deserves emphasis: **a simulator is a hypothesis about the channel.** Until
Phase 2 calibration, its outputs are hypotheses too, and must be tagged `[HYP]`.

## Validation plan

- Identity-channel self-test: with no impairments, decoding must be perfect. A failure here
  is a decoder bug, not a channel effect.
- **Phase 2 calibration** is the real validation: record real captures with known
  transmitted content, fit the simulator's impairment parameters to them, and measure how
  well simulator-predicted symbol error rates match measured ones. The gap between them is
  the simulator's credibility, and it should be reported as a number.
- Exit criterion for trusting simulator results: predicted versus measured symbol error
  rate agreeing within a stated tolerance across the tested conditions.
