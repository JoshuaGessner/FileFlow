# Goals and non-goals

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0001, ADR-0002, ADR-0011

## Goals

### G1 — Arbitrary file transfer over the optical channel only
Any file, any content, any size within device storage limits, transferred from one
Android phone's display to another's camera with no Wi-Fi, cellular, Bluetooth, NFC,
USB or external accessory involved at any point.

### G2 — Maximum verified payload goodput
The primary optimisation objective. Defined in
[PERFORMANCE-PHILOSOPHY.md](PERFORMANCE-PHILOSOPHY.md).

### G3 — Correctness guaranteed end to end
A completed transfer means the receiver holds a file whose cryptographic hash matches
the sender's. Partial, corrupt or silently truncated output is a defect of the highest
severity.

### G4 — Beat dynamic QR streaming decisively
Not marginally. If a purpose-built protocol cannot substantially outperform a QR stream
on the same hardware, the thesis is wrong and we need to know that early. A calibrated
QR-stream baseline is built in Phase 2 for exactly this comparison.

### G5 — Reproducible measurement
Every performance claim traceable to a benchmark run, a methodology, a commit and raw
data that still exists.

### G6 — Safe handling of untrusted optical input
The receiver parses attacker-controlled data. Memory safety, bounds discipline and
resource limits are requirements, not hardening tasks for later.

### G7 — Adaptive link operation
The system measures channel conditions and selects a modulation profile, rather than
requiring the user to understand the physics.

## Non-goals (initial)

These are explicitly **out of scope for the initial system**. They are not permanently
rejected; several are candidate later phases. Recording them as non-goals prevents scope
drift and prevents the architecture being compromised to accommodate them prematurely.

### NG1 — iOS support
Android-first. iOS has different display-pipeline and camera-control APIs and would
force abstraction before we know what the abstraction should hide. See ADR-0001.

### NG2 — Browser / web support
A browser cannot provide frame-accurate presentation control, manual camera exposure and
focus locking, or low-copy YUV access. See ADR-0002.

### NG3 — Broad device compatibility
Reference devices first. A protocol tuned to nothing in particular performs like it.
See ADR-0011.

### NG4 — Imperceptible or hidden signalling
FileFlow's screen is a dedicated transmitter. Imperceptibility is the dominant cost in
the hidden-communication literature and buying it would cost roughly an order of
magnitude of goodput. Documented as a contrasting design category only.

### NG5 — Rolling-shutter-only modem
Rolling shutter is treated as a *channel impairment to model and eventually exploit*
(mode M4), not as the primary carrier.

### NG6 — Machine-learning-first decoding
The decoder is a signal-processing chain with explicit, debuggable stages. ML may later
assist specific sub-problems (blur estimation, mixed-frame classification) once there is
a labelled dataset — which the capture harness is designed to produce.

### NG7 — Network-assisted anything
No side channel, no cloud fallback, no "just use Wi-Fi for the handshake". The optical
channel carries everything, including capability negotiation.

### NG8 — Compression as a throughput claim
Compression may be supported, but compression gains are **never** reported as optical
channel throughput. Highly compressible test data is benchmarked and reported separately.

### NG9 — Bidirectional simultaneous transfer
Reverse optical control (receiver → sender, for NAKs and rate control) is a Phase 9
research item. The initial link is one-way with rateless coding standing in for feedback.

### NG10 — Security guarantees beyond integrity
Authenticated encryption and digital signatures are designed for but optional and
deferred. The initial guarantee is *integrity*, not *confidentiality* — anyone pointing
a camera at the screen receives the data. This is a property of the medium and must be
surfaced to the user, not papered over.

## Explicitly deferred decisions

| Decision | Deferred until | Tracked as |
|---|---|---|
| Final modulation profile | Phase 5 measurements | OQ-006 |
| Final grid geometry | EXP-001 / EXP-013 | OQ-003 |
| FEC code family and library | EXP-011 | OQ-009 |
| Fountain library vs. own implementation | EXP-012 | OQ-010 |
| Colour constellation | EXP-014 | OQ-007 |
| Licence | Before first public release | OQ-021 |
