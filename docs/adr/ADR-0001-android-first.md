# ADR-0001 — Android-first implementation

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Project lead
> **Related:** ADR-0011, NG1

## Context

The optical link needs deep control over two subsystems: display presentation timing and
camera capture parameters. The degree of control available differs sharply between mobile
platforms, and building for both simultaneously would require an abstraction layer written
before we know what actually varies — the classic premature-abstraction failure.

We must also choose where to spend limited effort. Supporting two platforms roughly halves
the depth achievable on either.

## Decision

Build for **Android first**, targeting **API 33+** for the deterministic frame-pacing path.
iOS is deferred indefinitely and is an explicit non-goal for the initial system (NG1).

## Alternatives

1. **iOS-first.** iOS offers more consistent hardware, which is genuinely attractive for
   channel characterisation — fewer variables. Rejected because `AVFoundation` gives less
   granular control over display presentation than Android's Choreographer/FrameTimeline,
   and because Android's diversity, while a cost, is also where the interesting
   engineering (capability probing, adaptation) lives.
2. **Cross-platform from day one** (Flutter/React Native/KMP). Rejected: none provide
   frame-accurate presentation control or low-level camera access. The abstraction would
   have to be punched through immediately for the parts that matter.
3. **Both natively, in parallel.** Rejected on effort grounds.

## Consequences

**Positive.** Full access to Camera2/NDK Camera manual controls, `SurfaceView` +
Choreographer + FrameTimeline, NDK for the performance core, and Perfetto for ground-truth
instrumentation. One platform to optimise deeply.

**Negative.** Android device diversity is large and vendor behaviour is inconsistent —
the capability probe (C02) exists because of this decision. API 33 excludes older devices.
No iOS interoperability, which limits real-world usefulness of any eventual product.

**Neutral.** The C++ core is portable by construction (ADR-0003, ADR-0010), so a future
iOS port would reuse the decode chain, though not the capture or render adapters.

## Evidence

- FrameTimeline APIs available from **API 33**. `[FACT]` — NDK Choreographer reference.
- Camera2 exposes `MANUAL_SENSOR`, `MANUAL_POST_PROCESSING`, tonemap, edge and
  noise-reduction control, giving the photometric stability our channel needs. `[FACT]`
- No comparative evaluation of iOS display-timing APIs was performed. This decision rests
  on the Android side being *sufficient*, not on it being *better*. `[OPEN]`

## Risks

| Risk | Mitigation |
|---|---|
| Android fragmentation costs more than expected (RISK-011) | Reference-device-first (ADR-0011); capability probe verifies rather than trusts |
| API 33 floor excludes useful test hardware | Degraded path at lower `Fd` for older devices, added only if needed |
| Vendor camera pipelines behave inconsistently | Measure, do not trust characteristics (C02) |

## Validation plan

This is a scope decision, not an empirical one — it is not falsifiable by experiment.
It should be revisited if Phase 2 channel characterisation shows Android display or camera
behaviour is too non-deterministic to build a reliable link on, which would be surfaced by
EXP-006 and EXP-007.
