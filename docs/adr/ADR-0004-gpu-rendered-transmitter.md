# ADR-0004 — GPU-rendered transmitter

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** TX subsystem
> **Related:** ADR-0001, EXP-006, RISK-003

## Context

The transmitter must present up to 34,560 individually-valued cells per frame, at 60–120
distinct states per second, at native panel resolution with **no resampling anywhere**,
and must know which frames were actually presented.

Resampling is the key constraint: any scaling in the pipeline puts cell boundaries on
fractional pixels, which raises spatial crosstalk and directly reduces `Pc`.

## Decision

Render with **OpenGL ES 3.x onto a `SurfaceView`**, paced by the NDK Choreographer using
`AChoreographer_postVsyncCallback` and the FrameTimeline APIs (API 33+). Surface buffer
size is pinned to the panel's native mode resolution.

**Vulkan is deferred**, not rejected — adopted only if OpenGL ES pacing proves
insufficient.

## Alternatives

1. **CPU rasterisation into a `Bitmap`/`Canvas`.** Rejected: per-frame CPU cost for
   ~34k cells, plus the composition path may resample.
2. **`TextureView` instead of `SurfaceView`.** **Rejected firmly.** `TextureView` is
   composited into the view hierarchy, adding latency and a resampling opportunity.
   `SurfaceView` gets its own layer and can be presented directly.
3. **Vulkan from the start** (with `VK_GOOGLE_display_timing`). Deferred: better
   presentation control, materially higher complexity. Revisit if EXP-006 shows OpenGL ES
   pacing is inadequate.
4. **Video playback of a pre-rendered stream.** **Rejected outright** — lossy video
   compression destroys exactly the high-spatial-frequency structure we modulate. Worth
   stating explicitly because it is a tempting shortcut for generating test content.

## Consequences

**Positive.** Trivial GPU cost (flat quads), no CPU rasterisation, precise vsync targeting,
per-frame presentation timelines, straightforward native-resolution rendering.

**Negative.** API 33 floor for the FrameTimeline path. OpenGL ES gives less presentation
control than Vulkan. GL context and surface lifecycle handling is fiddly across
Android lifecycle events.

**Neutral.** The renderer is one of the few genuinely Android-coupled components, so it
sits behind an interface and is excluded from the off-device build.

## Evidence

- FrameTimeline API family (`AChoreographer_postVsyncCallback`,
  `AChoreographerFrameCallbackData_*`) is available from **API 33**. `[FACT]`
- `Surface.setFrameRate` is a **hint the platform may refuse**; mode switches may take
  ~2 seconds and change Choreographer timing. `[FACT]` This is why the session has an
  explicit "display mode settled" precondition before a transfer starts.
- Whether presentation can be *confirmed* per-frame at 60/120 Hz is **unproven** — this is
  OQ-004 and the main open risk in this decision. `[OPEN]`

## Risks

| Risk | Mitigation |
|---|---|
| Requested frames not actually presented (RISK-003) | **Every optical frame carries its own sequence number and phase indicator**, so the receiver never depends on the transmitter's intended schedule |
| VRR/LTPO panel idling down mid-transfer | Request fixed mode, verify, monitor for mode changes, abort/renegotiate on change |
| Something in the pipeline resamples | Assert surface size equals native mode resolution at session start |
| OpenGL ES pacing insufficient | Vulkan fallback path, gated on EXP-006 |

## Validation plan

**EXP-006** — verify with Perfetto traces that requested display states are actually
presented at 30, 60 and 120 Hz on each reference device, and measure the discrepancy rate.
This experiment also determines whether Vulkan is needed. Until it runs, the "60 distinct
states per second" assumption underpinning the performance model is unvalidated.
