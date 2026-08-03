# ADR-0002 — Native installed application, not a browser application

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Project lead
> **Related:** ADR-0003, NG2

## Context

A browser implementation would be attractive for distribution: no install, works across
platforms, easy to share. `getUserMedia`, `WebGL`/`WebGPU` and `WebCodecs` provide
camera access and GPU rendering. The question is whether the browser can provide the
*control* this link needs, not whether it can access a camera at all.

## Decision

Build a **native installed Android application**. Browser support is an explicit non-goal
(NG2).

## Alternatives

1. **Pure web application.** Rejected — see evidence below.
2. **Hybrid: web UI, native core.** Rejected as strictly worse than native for our case;
   adds a boundary without removing any constraint.
3. **Web receiver, native transmitter** (or vice versa). Interesting for reach — a web
   receiver would let anyone receive without installing. Rejected for the initial system
   because the receiver is the side that needs the *most* control (manual exposure, focus
   lock, high frame rate, low-copy access). If anything, the transmitter is the more
   web-feasible side, and that is the less useful half.

## Consequences

**Positive.** Manual camera control, frame-accurate presentation, native SIMD, GPU compute
without WebGPU's restrictions, low-copy buffer access, thermal and performance visibility.

**Negative.** Installation friction. Platform-locked. Harder casual demonstration.

**Neutral.** A browser receiver could be revisited later as a low-rate compatibility mode
using a robust profile — it would not need to be fast to be useful for interoperability.

## Evidence

Browser APIs cannot provide, on current standards:

- **Manual exposure/ISO/focus lock.** `MediaStreamTrack.applyConstraints` exposes a
  limited, inconsistently-implemented subset. Exposure *time* and ISO are generally not
  settable with the precision we need. Without exposure control, exposure drift becomes an
  uncontrolled channel disturbance. `[HYP — based on standards coverage, not measured]`
- **Frame-accurate presentation.** `requestAnimationFrame` gives no equivalent of
  FrameTimeline's per-frame vsync targeting and no presentation confirmation. We cannot
  know which frames were actually shown, and `Fd` becomes unknowable.
- **Low-copy YUV access.** Frames arrive as already-processed RGB via canvas/WebGL paths;
  the vendor pipeline's sharpening, noise reduction and tone curve are already baked in
  and cannot be disabled.
- **Tone curve / edge / noise-reduction control.** No web equivalent at all. These are
  nonlinear spatial filters applied to a dense symbol grid — exactly what we need to turn
  off.

**Honest caveat:** these are reasoned from API surface coverage, not from measurement. We
did not build a browser prototype. The conclusion is confident because several of these
gaps are structural (no presentation confirmation exists in the platform at all), but the
specific claim about `applyConstraints` support levels would benefit from verification if
anyone seriously proposes the web path.

## Risks

| Risk | Mitigation |
|---|---|
| Install friction limits adoption | Accepted; this is a research project, not a consumer product yet |
| Web APIs improve and this decision ages badly | Revisit when WebGPU compute + advanced camera constraints are broadly shipped |

## Validation plan

Not falsifiable by our experiments. Revisit if a future standard provides presentation
confirmation and manual sensor control.
