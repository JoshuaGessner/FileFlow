# Architecture overview

> **Status:** Draft
> **Owner:** Architecture
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0003, ADR-0004, ADR-0005, ADR-0010, COMPONENT-REGISTRY.md, DATA-FLOW.md

## Shape of the system

FileFlow is one Android application that can act as either transmitter or receiver. It is
split across a language boundary chosen for a specific reason:

```
┌──────────────────────────────────────────────────────────────┐
│ Kotlin — application shell                                   │
│ lifecycle, permissions, file picking, storage, UI, session UX│
└───────────────────────────┬──────────────────────────────────┘
                            │ narrow JNI boundary
                            │ (session control + progress, NOT per-frame)
┌───────────────────────────┴──────────────────────────────────┐
│ C++ / NDK — performance core                                 │
│                                                              │
│  TRANSMIT                    RECEIVE                         │
│  ├ optical frame generator   ├ camera capture service        │
│  ├ FEC + fountain encoder    ├ screen tracker                │
│  └ GPU renderer (GLES/VK)    ├ frame-phase classifier        │
│                              ├ rectifier + cell sampler      │
│                              ├ photometric calibration       │
│                              ├ soft demodulator              │
│                              ├ intra-frame FEC decoder       │
│                              ├ fountain decoder              │
│                              └ file reconstruction + verify  │
│                                                              │
│  SHARED: adaptive link controller, telemetry, protocol codec │
└──────────────────────────────────────────────────────────────┘
```

**The JNI boundary never carries per-frame data.** It carries session setup, progress
updates and completion. Camera frames, symbols, packets and file bytes stay in native
memory for their whole life. Crossing JNI 60–120 times a second with frame-sized payloads
would defeat the point of the native core.

## The constraint that shaped the receiver

Android's constrained high-speed capture session — the only portable route to ≥120 fps —
**does not permit `ImageReader` output**. Frames at 120 fps are available only as GPU
surfaces (`SurfaceTexture`) or encoder input. `[FACT]` See
[android-camera-pipeline.md](../research/android-camera-pipeline.md).

The receiver therefore has **two capture paths**, not one:

| Path | Rate | Frame access | Cell sampling |
|---|---|---|---|
| **CPU path** | ≤60 fps | `AImageReader`, `YUV_420_888`, Y plane direct | NEON over the Y plane |
| **GPU path** | ≥120 fps | `SurfaceTexture` → `GL_TEXTURE_EXTERNAL_OES` | Compute/fragment shader; only soft symbols read back |

Both paths converge on the same soft-symbol buffer, and everything downstream of the cell
sampler is identical. That convergence point is the most important interface in the system:
it is what keeps the two paths from becoming two decoders.

The GPU path is not a fallback — for milestone 6 it is the *only* path. But the CPU path
is what Phases 1–5 use, because it is simpler to debug and 60 fps is enough to reach
milestone 4.

## Off-device execution is a hard requirement

The entire decode chain — from soft symbols through file verification, and ideally from
rectification onward — **must build and run on a desktop**, with no Android dependency.
This is not a convenience. It is what makes the offline simulator (Phase 1) and the
recorded-frame harness meaningful: they must exercise *the same decoder* as the live
receiver, or their results do not transfer.

Consequences for the code:
- Android APIs are confined to thin adapters at the edges (capture, render, storage).
- The decode chain takes buffers and configuration, not `AImage` or JNI handles.
- No decode-path component may depend on Android headers.

Enforced by building a desktop target in CI from the start. See ADR-0010.

## Threading model

| Thread | Work | Priority |
|---|---|---|
| **UI (Kotlin main)** | Lifecycle, user interaction, progress | Normal |
| **TX render** | Choreographer-driven; generates and presents optical frames | Display-critical |
| **TX encode** | FEC + fountain symbol generation, ahead of the renderer | High |
| **RX capture** | Camera callbacks; must never block | Real-time-ish, highest |
| **RX decode pool** | Rectify, sample, demodulate, FEC — parallel across frames | High |
| **RX assembly** | Fountain decode, file writing, hashing | Normal |
| **Telemetry** | Metric aggregation, logging | Low |

Rules:
- The capture thread **only** acquires the frame and hands off a reference. Any work done
  there costs camera frames directly, and dropped frames reduce `Pc`, which reduces
  goodput. This is the tightest real-time constraint in the system.
- The decode pool is sized from measured core count and thermal state, not hard-coded.
- Back-pressure is explicit: if decode falls behind, frames are **dropped deliberately at
  a defined point with a logged reason**, never queued unboundedly. An unbounded queue
  turns a throughput problem into an out-of-memory crash.

## Memory ownership

- **Pool everything on the hot path.** No allocation per frame. Buffer pools are sized at
  session start from the negotiated grid and profile.
- Camera images are reference-counted; the pool owner returns them to the camera promptly.
  Holding camera buffers stalls the capture pipeline and drops frames.
- The fountain decoder's working set is the largest single allocation and is bounded by
  the negotiated block size — which is validated against a hard limit **before**
  allocation (see [INPUT-VALIDATION.md](../security/INPUT-VALIDATION.md)).
- Received file data is streamed to a temporary file, not held in memory. A 100 MB
  transfer must not require 100 MB of RAM.

## Layering

```
File layer        original file, manifest, hash, safe reconstruction
Session layer     capabilities, metadata, state, completion, (later) reverse control
Fountain layer    cross-frame erasure recovery
FEC layer         intra-frame correction, erasure handling
Optical frame     header, payload symbols, pilots, sequence, CRC
Optical physical  rendering, capture, geometry, timing, symbols
```

Each layer is independently testable with deterministic vectors. Full definition in
[PROTOCOL-SPEC.md](../specifications/PROTOCOL-SPEC.md).

## Adaptation

The adaptive link controller sits beside the stack rather than inside it, reading
telemetry from every layer (pilot SNR, symbol error rate, header failures, clean-frame
ratio, decode latency, thermal state) and selecting the modulation profile and code rate.

With a one-way link the controller on the **transmitter** has no direct channel feedback.
The initial design has the transmitter cycle through a defined profile schedule while the
receiver decodes whichever profile works — a real cost, and an under-designed area flagged
as OQ-013. Reverse optical control (Phase 9) is the principled fix.

> **Amended 2026-08-03 (EXP-023, findings F20–F23).** The controller now exists
> (`core/src/link.cpp`) and this section was optimistic in two ways worth correcting.
>
> **It reads telemetry, but not the telemetry listed above.** The implemented estimator does not
> use pilot SNR, symbol error rate or decode latency. It uses one quantity — the worst
> per-codeword **correction budget** (`2·errors + erasures`) — because that single integer is a
> *sufficient statistic* for the entire code-rate ladder, and the richer signals turned out to be
> unnecessary for the decision. Thermal state is absent deliberately (OQ-036).
>
> **"Selecting the modulation profile and code rate" is not currently possible mid-session.**
> Both change per-frame payload capacity, which changes the fountain symbol size, which the
> session manifest fixes for the whole transfer. So the controller **recommends** and does not
> actuate; rate selection is a session-start decision. The blocker is our own protocol and is
> reached *before* the one-way link becomes the problem (OQ-037). The profile-cycling design
> described above therefore has nothing it can legally cycle until that is resolved.

## What is deliberately absent

- No network code of any kind, including for capability exchange (NG7).
- No dynamic code loading, no plugin system.
- No ML inference in the decode path (NG6).
- No abstraction layer for iOS or web. Adding one before we know what varies would be
  guessing (NG1, NG2).

## Key interfaces

| Interface | Between | Why it matters |
|---|---|---|
| `SoftSymbolBuffer` | Cell sampler → demodulator | The convergence point of the CPU and GPU capture paths. Must be identical for both. |
| `FrameDecodeResult` | FEC decoder → fountain decoder | Carries decoded packets *and* erasure information. Erasures are as valuable as data. |
| `CaptureSource` | Camera / simulator / recorded-frame harness → decode chain | Lets the same decoder run live, simulated and replayed. The foundation of ADR-0010. |
| `LinkProfile` | Controller → TX generator and RX demodulator | The single description of the current modulation, grid, code rate and frame layout. |

`CaptureSource` is the interface that makes the testing strategy work, and it should be
designed before the live receiver, not retrofitted.
