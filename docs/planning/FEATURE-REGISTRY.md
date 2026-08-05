# Feature registry

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** COMPONENT-REGISTRY.md, ROADMAP.md, EXPERIMENT-REGISTRY.md

Stable feature IDs. Every user-visible or engineering capability appears here.
Status: `Planned` | `In progress` | `Done` | `Deferred` | `Dropped`.
Priority: `P0` (blocks a milestone) | `P1` (important) | `P2` (valuable) | `P3` (nice).

ID groups: **CAP** capability detection · **TX** transmitter · **RX** receiver ·
**CV** computer vision · **MOD** modulation · **FEC** error correction ·
**FTN** fountain · **PRO** protocol · **FIL** file handling · **ADP** adaptation ·
**BEN** benchmarking · **SIM** simulation · **SEC** security · **UI** user interface ·
**DOC** documentation · **EXP** experiments

---

> **Status labels, corrected 2026-08-04.** Much of this registry still said **Planned** for
> features that had been implemented and documented in findings F7–F19 — a reader would have
> concluded almost nothing was built. That is the documentation-versus-implementation divergence
> RISK-020 tracks and F10 was caught by, so it is corrected here rather than quietly rewritten.
>
> Two statuses are now kept apart, because collapsing them over-claims:
>
> - **Implemented** — the code exists and is covered by tests.
> - **Acceptance met** — the experiment named on the entry's own *Acceptance* line has actually
>   run. Most acceptance criteria here demand hardware or an unrun experiment, so **implemented
>   rarely implies accepted**, and several entries below say so explicitly.
>
> Everything not marked otherwise is simulator-only and therefore `[HYP]` (RISK-024).

## CAP — Capability detection

### CAP-01 — Camera capability enumeration
**Description.** Enumerate hardware level, formats, resolutions, FPS ranges, high-speed
sizes/ranges, manual-control support, timestamp source, rolling-shutter skew.
**Value.** Everything downstream depends on knowing what the device can do.
**Phase.** 2 · **Priority.** P0 · **Depends on.** — · **Subsystem.** C02
**Acceptance.** Produces a serialisable `DeviceProfile` for every reference device;
unknown devices get a conservative tier.
**Performance.** None (startup only). **Security.** None.
**Docs.** [android-camera-pipeline.md](../research/android-camera-pipeline.md) ·
**Experiments.** EXP-007 · **Status.** **Implemented; run on hardware 2026-08-04 (F27).**
One correction from contact with the SDK: rolling-shutter skew is a `CaptureResult` key, not
a `CameraCharacteristics` one, so a startup probe **cannot** obtain it and this description's
inclusion of it was wrong (F25). It must come from the recorder.

### CAP-02 — Display capability enumeration
**Description.** Supported display modes, refresh rates, native resolution, VRR behaviour.
**Phase.** 2 · **Priority.** P0 · **Subsystem.** C02
**Acceptance.** Reports achievable `Fd` per device, verified not merely advertised.
**Docs.** [android-display-pipeline.md](../research/android-display-pipeline.md) ·
**Experiments.** EXP-006 · **Status.** **Enumeration implemented; ACCEPTANCE NOT MET.**
The probe lists every display mode (F27: 1440×3120, 1080×2340, 720×1560, each at 120 and
60 Hz — QHD+ is *not* restricted to 60 on this device, and there is no 144 Hz mode). But the
acceptance criterion asks for **achievable `Fd`, verified**, and `Fd` is not the refresh rate.
No display state has ever been presented or counted, because no transmitter exists. This
feature is **not done** and is deliberately not marked so.

### CAP-03 — Capability *verification* (not just enumeration)
**Description.** Actively test that advertised capabilities are honoured — manual exposure
actually applied, requested FPS actually delivered, requested display mode actually held.
**Value.** Vendors misreport (RISK-011). Trusting characteristics produces silent failures.
**Phase.** 2 · **Priority.** P1 · **Depends on.** CAP-01, CAP-02 · **Subsystem.** C02
**Acceptance.** Detects at least one known discrepancy on the reference set, or confirms
none exists.
**Experiments.** EXP-007 · **Status.** **Partly done (F28).** On the SM-S948U1 the recorder
reads every manual setting back from `CaptureResult` rather than trusting the request, and
**confirms no discrepancy** for exposure (exact), ISO (quantised 400→398), `CONTROL_AE_MODE`,
`EDGE_MODE` and `NOISE_REDUCTION_MODE`. Requested frame rate is also verified delivered
(59.04 of 60).
**Still unverified:** that `EDGE_MODE` OFF *behaves* as off — reporting a mode is one step
short of honouring it, and proving it needs a known high-spatial-frequency target, i.e. a
transmitter. Also unverified: whether a requested display mode is *held* (EXP-006), and the
entire ≥120 fps high-speed path (OQ-002).

### CAP-04 — Device tier classification
**Description.** Map a probed device to a supported tier with a permitted profile set.
**Phase.** 4 · **Priority.** P1 · **Depends on.** CAP-01–03 · **Subsystem.** C02
**Status.** **Implemented** (`core/src/device.cpp`, ADR-0014; 19 tests). Correctly returns
**UNSUPPORTED** on the reference device, because `Fd` is unmeasured and the policy refuses to
infer it from the refresh rate. That refusal is the feature working, not a gap in it.

---

## TX — Transmitter

### TX-01 — GPU optical frame renderer
**Description.** Render cell matrices at native resolution, no scaling, Choreographer-paced.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C03 · **Related.** ADR-0004
**Acceptance.** Presents requested states at 30/60 Hz with a measured discrepancy rate;
surface size asserted equal to native mode resolution.
**Performance.** Critical — bounds `Fd`. **Experiments.** EXP-006 · **Status.** Planned

### TX-02 — Optical frame generator
**Description.** Assemble markers, timing tracks, header, pilots, guards, payload, CRC.
**Phase.** 1 (simulator) → 3 (device) · **Priority.** P0 · **Subsystem.** C04
**Acceptance.** Golden-frame vectors reproduce byte-exactly across platforms.
**Docs.** [OPTICAL-FRAME-CANDIDATES.md](../specifications/OPTICAL-FRAME-CANDIDATES.md)
**Status.** Planned

### TX-03 — Presentation verification
**Description.** Determine which frames were actually presented; report discrepancies.
**Value.** `Fd` is otherwise unknowable, and sequence assumptions silently break.
**Phase.** 3 · **Priority.** P1 · **Subsystem.** C03
**Acceptance.** Per-frame presentation outcome logged; agrees with Perfetto ground truth.
**Experiments.** EXP-006 · **Open.** OQ-004 · **Status.** Planned

### TX-04 — Display mode acquisition and hold
**Description.** Request a display mode, wait for it to settle, verify, monitor for change.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C03
**Acceptance.** Transfer never starts during a mode switch; mid-transfer mode change
detected and handled. **Experiments.** EXP-006 · **Status.** Planned

### TX-05 — Brightness control
**Description.** Set and hold maximum brightness for the transfer; restore afterwards.
**Phase.** 3 · **Priority.** P1 · **Subsystem.** C01 · **Security.** None
**Acceptance.** Restored on normal exit, cancellation and crash. **Status.** Planned

---

## RX — Receiver

### RX-01 — CPU capture path (`AImageReader`, YUV Y plane)
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C05 · **Related.** ADR-0005
**Acceptance.** Sustains target fps with a measured drop rate; no per-frame allocation.
**Performance.** Critical. **Experiments.** EXP-007 · **Status.** Planned

### RX-02 — GPU capture path (`SurfaceTexture`, external OES)
**Description.** High-frame-rate path; **the only path available ≥120 fps** `[FACT]`.
**Phase.** 8 · **Priority.** P1 (P0 for milestone 6) · **Subsystem.** C05, C08
**Acceptance.** Delivers genuinely distinct frames at 120 fps; sampler output matches the
CPU path within tolerance. **Experiments.** EXP-007 · **Open.** OQ-002 · **Status.** Planned

### RX-03 — Manual camera control lock
**Description.** Lock exposure, ISO, focus, white balance; disable stabilisation, noise
reduction, edge enhancement; request a linear tone curve.
**Phase.** 2 · **Priority.** P0 · **Subsystem.** C05
**Acceptance.** Settings verified present in returned `CaptureResult` metadata, not just
requested. **Experiments.** EXP-004, EXP-005 · **Status.** Planned

### RX-04 — Frame-phase classification
**Description.** Classify frames clean / mixed / duplicate; locate the transition band.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C07
**Acceptance.** Classification accuracy measured against simulator ground truth.
**Performance.** Critical. **Experiments.** EXP-008 · **Status.** Planned

### RX-05 — Back-pressure and deliberate frame dropping
**Description.** When decode falls behind, drop at a defined point with a logged reason —
never queue unboundedly.
**Value.** Prevents a throughput problem becoming an OOM crash.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C05 · **Security.** Resource exhaustion.
**Status.** Planned

---

## CV — Computer vision

### CV-01 — Static pattern detection and rectification
**Description.** Detect the screen, estimate homography, rectify. **Milestone 1.**
**Phase.** 2 · **Priority.** P0 · **Subsystem.** C06
**Acceptance.** Detects and rectifies a static test pattern across distance and angle sets
with measured geometric error. **Status.** **Implemented** (`core/src/detect.cpp`,
`core/src/geometry.cpp` — DLT with Hartley normalisation). Worst cell-centre error **<0.25
cells** head-on, surviving 20° yaw + 12° pitch (F9, F10). **Acceptance NOT met:** measured in
the simulator against synthetic ground truth, never across a real distance/angle set.
Localisation must never depend on payload content — it did once, and zero-padded frames became
undetectable (F15).

### CV-02 — Persistent tracking
**Description.** Maintain the homography across frames; state machine with reacquisition.
**Value.** The core efficiency bet of ADR-0006.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C06
**Acceptance.** Cheaper *and* at least as accurate as per-frame detection (EXP-017).
**Experiments.** EXP-017 · **Open.** OQ-020 · **Status.** **Implemented**
(`core/src/tracker.cpp`; state machine `searching → tracking → degraded → lost`).
**Cheaper: yes** — 0.185 of a full-image scan, via a boundary *annulus* rather than a bounding
box, which is the detail that made the saving hold in the dense-framing regime (F13).
**At least as accurate: yes, but the original evidence was a bug.** The apparent reliability
gap was F15; with that fixed both paths decode identically. **Tracking buys speed, not
accuracy.** Simulated throughout, so EXP-017 proper has not run.

### CV-03 — Subpixel cell sampling with interior margin
**Phase.** 2 · **Priority.** P0 · **Subsystem.** C08
**Acceptance.** Interior-margin parameter swept; optimum reported with a response surface.
**Performance.** Critical — largest per-frame cost. **Experiments.** EXP-015, EXP-016
**Open.** OQ-019 · **Status.** **Implemented, CPU path only** (`core/src/sampler.cpp`).
Samples *through* the homography rather than warping first — ~4 samples per cell instead of
~100 pixels. **Acceptance NOT met:** the interior margin has not been swept and no response
surface exists (EXP-015). The **GPU path and its mandatory CPU/GPU equivalence test do not
exist** (EXP-016, ADR-0005).

### CV-04 — Photometric normalisation
**Description.** Vignetting field, exposure normalisation, gamma linearisation, regional
thresholds from distributed pilots.
**Phase.** 3 · **Priority.** P0 · **Subsystem.** C09 · **Status.** **Implemented**
(`core/src/photometric.cpp`). Estimates dark and bright levels as smooth *fields* from the
pilot lattice, so it corrects lens vignetting and transmitter-side angular falloff (RISK-025)
identically without assuming a cause. Two findings shaped it: multiplicative falloff alone does
**not** defeat a global threshold — a spatially varying black-level lift does (F7); and
interpolation hides occlusion unless the **pilot-fit residual** is checked, which is the
receiver's first real detection-confidence signal (F8).

### CV-05 — Blur and confidence estimation
**Description.** Per-region blur estimate feeding LLRs and the adaptive controller.
**Phase.** 5 · **Priority.** P1 · **Subsystem.** C09 · **Status.** **Partly implemented.**
The *confidence* half exists as `PhotometricField::ResidualAt` (F8) and generalises to
occlusion, glare and tracking error alike. **No blur estimate exists**, so the half this entry
is named for is outstanding.

### CV-06 — Lens distortion handling
**Description.** Correct radial distortion, ideally self-calibrated from the transmitted grid.
**Phase.** 5 · **Priority.** P2 · **Subsystem.** C06 · **Open.** OQ-008 · **Status.** Planned

### CV-07 — Motion detection and degradation marking
**Phase.** 5 · **Priority.** P1 · **Subsystem.** C06 · **Status.** Planned

---

## MOD — Modulation

### MOD-01 — M0 binary luminance · **Phase.** 1 · **P0** · C10 · ADR-0007 · **Implemented** (`core/src/modulation.cpp`, `M0Modulator`)
### MOD-02 — M1 differential (4 schemes) · **Phase.** 5 · **P1** · C10 · ADR-0008 · EXP-010 · Planned
### MOD-03 — M2 four-level luminance · **Phase.** 6 · **P0 for milestone 4** · C10 · EXP-013 · Planned
### MOD-04 — M3 four-colour · **Phase.** 6 · **P2** · C10 · EXP-014 · Planned
### MOD-05 — M4 mixed-frame-aware · **Phase.** 7 · **P1 for milestone 5** · C10 · Planned
### MOD-06 — Soft symbol LLR generation
**Description.** All modes emit calibrated LLRs plus erasure flags; never hard bits.
**Value.** Preserving soft information is a core architectural property.
**Phase.** 1 · **Priority.** P0 · **Subsystem.** C10
**Acceptance.** LLR calibration test — empirical error rates match LLR-implied
probabilities within tolerance. **Status.** **Implemented; acceptance NOT met, and cannot be
met for M0 as written.** The path emits LLRs plus erasure flags throughout
(`core/src/modulation.cpp`, `test_llr_quality.cpp`). A real bug was fixed here: a fixed
quantisation scale saturated **every** cell into one band at every noise level, so the "soft"
signal carried no information at all (F19).
But the calibration test compares empirical error rates against LLR-implied probabilities, and
M0's measured symbol error rate is **0.00000** up to noise amplitude 120 — there are no errors
to calibrate against. **F19's scope is narrower than it reads:** that sweep used image-path
pixel noise; the cell-sample path with 0.20 crosstalk *does* produce undetected errors in
quantity (F23). This acceptance becomes meaningful at M2, where levels sit at one third the
spacing.

---

## FEC / FTN — Coding

### FEC-01 — Intra-frame payload code · **Phase.** 1 · **P0** · C11 · EXP-011 · **Implemented** (`core/src/intra_fec.cpp`, interleaved Reed-Solomon with errors-and-erasures decoding). **Load-bearing, not an optimisation: without it the transfer does not complete on an impaired channel at all** (F18). EXP-011 has not run, so RS is a working default rather than a justified choice (OQ-009)
### FEC-02 — Header code (very low rate, replicated) · **Phase.** 1 · **P0** · C11 · **Implemented** (`core/src/frame.cpp`, `HeaderCodec`; RS with 32 parity symbols, replicated copies majority-voted before correction)
### FEC-03 — Spatial interleaver · **Phase.** 1 · **P0** · C11 · Open OQ-012 · **Implemented** (`core/src/intra_fec.cpp`). Not optional: optical damage is spatially clustered, and without interleaving one burst exceeds a single codeword's budget while its neighbours sit idle. The scatter property is asserted on its own, so a broken interleave reports as a broken interleave rather than a mysterious FEC regression (F18)
### FEC-04 — Soft-input decoding · **Phase.** 1 · **P0** · C11 · **NOT implemented, and deliberately deferred.** The decoder consumes **erasure positions**, not LLRs — which is where the value turned out to be, since RS corrects `nsym` erasures but only `nsym/2` errors (F18). Erasure-marking from LLR magnitude was swept and never helped: zero was optimal at every setting (F19). A genuinely soft-input code (LDPC) is EXP-011's question
### FEC-05 — Erasure signalling upward
**Description.** Uncorrectable frames reported as erasures, never as best-effort data.
**Phase.** 1 · **P0** · C11 · **Implemented.** The implementation once did exactly the opposite — occluded cells became zeros and were handed to the fountain decoder as valid, and a fountain code cannot tell a damaged symbol from a good one, so the damage propagated through peeling and surfaced only as a final hash mismatch (F2). Fixed with a `payload_crc` in every frame header, verified before ingestion.
### FTN-01 — Systematic fountain encoder · **Phase.** 1 · **P0** · C12 · **Implemented** (`core/src/fountain.cpp`, LT with a robust soliton distribution; source symbols emitted first so a clean channel pays almost no fountain cost). RaptorQ remains blocked on licensing (RISK-016, OQ-010)
### FTN-02 — Fountain decoder with bounded memory · **Phase.** 1 · **P0** · C12 · **Security.** Allocation limits · **Implemented** (`core/src/fountain.cpp`; every bound enforced before any allocation derived from it)
### FTN-03 — Duplicate and out-of-order tolerance · **Phase.** 1 · **P0** · C12 · **Implemented** — tolerated by construction rather than by special-casing
### FTN-04 — Rateless completion detection · **Phase.** 4 · **P0** · C12 · **Implemented**; measured LT reception overhead is far worse than RaptorQ's ~2% class (0.54 on an impaired channel, F4), which is what raises the value of resolving OQ-010

---

## PRO / FIL — Protocol and file handling

### PRO-01 — Optical frame header codec · **Phase.** 1 · **P0** · C04/C11 · **Implemented** (`core/src/frame.cpp`); every field bounds-checked before use and fuzzed (`fuzz/fuzz_frame_header.cpp`, `fuzz_header_codec.cpp`)
### PRO-02 — Session layer with periodic re-announcement
**Description.** Session header repeated so a late-joining receiver can synchronise.
**Phase.** 4 · **P0** · **Security.** All fields bounds-checked · Planned
### PRO-03 — Protocol versioning and TLV extensions
**Description.** Unknown extensions skipped by length, never rejected.
**Phase.** 4 · **P1** · Planned
### PRO-04 — Deterministic test vectors · **Phase.** 1 · **P0** · C18 · Planned
### FIL-01 — File manifest and SHA-256 · **Phase.** 4 · **P0** · C13 · **Implemented** (`core/src/transfer.cpp`, `core/src/hash.cpp`); every manifest bound enforced before any allocation derived from it
### FIL-02 — Streamed reconstruction to temp file
**Description.** Bounded memory regardless of file size; randomised name; `O_EXCL`;
app-private directory; moved into place only after verification.
**Phase.** 4 · **P0** · **Security.** Temp-file safety · **NOT implemented as specified.**
`FileReceiver::Finish` reassembles **in memory** and returns a buffer; there is no temp file and no
streaming. That is acceptable at current test sizes and **not** acceptable against the 4 GB
`kMaxFileSize` bound the manifest already permits, so this remains real outstanding work rather
than a formality.
### FIL-03 — Hash verification gate
**Description.** Mismatch ⇒ hard failure, nothing delivered.
**Phase.** 4 · **P0** · **Security.** Integrity guarantee · **Implemented.** A mismatch returns `kHashMismatch` and yields **nothing** — never a partial or unverified result. Tested against corruption that evades every lower-layer checksum (`Transfer.HashGateCatchesCorruptionThatEvadesEveryChecksum`), which matters because a malicious transmitter controls the frame CRCs too (F2).
### FIL-04 — Filename sanitisation · **Phase.** 4 · **P0** · **Security.** Path traversal · **Implemented** (`SanitiseFileName`); the received name is treated as a display hint, never a path
### FIL-05 — Optional compression (reported separately) · **Phase.** 10 · **P3** · **Security.** Decompression bombs · Deferred

---

## ADP — Adaptation

### ADP-01 — Channel state estimation
**Description.** Receiver-side channel state from telemetry the decode path already produces:
the distribution of per-frame worst codeword correction budget (`2·errors + erasures`), plus
pre-FEC loss accounting kept separate so a tracking failure never reads as a code-rate problem.
**Value.** One 256-bin histogram, one increment per frame, no allocation — and it is a
*sufficient statistic* for the whole code-rate ladder (F20).
**Phase.** 5 · **P1** · C14 · **Status.** **IMPLEMENTED 2026-08-03** (`ChannelEstimator`).
Measured in EXP-023. Findings F20, F21, F23.
### ADP-02 — Profile selection with hysteresis
**Description.** Goodput-optimal rung selection over a parity ladder, with asymmetric margins
and dwell ("fall back fast, promote slowly"), scoring expected payload bytes per display state
rather than symbol error rate.
**Value.** Picked the brute-force optimum exactly on 3/3 scoreable EXP-023 channels.
**Phase.** 5 · **P1** · C14 · **Status.** **IMPLEMENTED 2026-08-03** (`LinkController`).
**Note.** The margin cannot be chosen independently of the ladder spacing — a margin wider than
one rung's gain silently strands the controller below the achievable rate, so `Create` rejects
it (F22).
### ADP-03 — Transmitter profile schedule (one-way link)
**Description.** With no feedback, cycle profiles so the receiver can use what works.
**Value.** The honest consequence of a one-way link.
**Phase.** 5 · **P1** · C14 · **Open.** OQ-013, **OQ-037** · **Status.** **BLOCKED, not merely
planned.** Cycling `nsym` (or profile, or grid) mid-session changes the fountain symbol size,
which the manifest fixes for the whole transfer — so there is nothing to cycle until OQ-037 is
resolved (F20). What exists instead is a session-start recommendation the transmitter has no way
to receive.
### ADP-04 — Reverse optical control · **Phase.** 9 · **P2** · Deferred
**Value, now quantified.** EXP-023 measures the cost of a wrong fixed code-rate guess at
**50–93%** of available goodput on simulated channels. That is a large prize for closing the
loop, and an argument for re-examining this feature's phase once the same spread is measured on
real captures. Simulated and uncalibrated (RISK-024), so not yet grounds to move it.
### ADP-05 — Thermal-aware degradation · **Phase.** 5 · **P1** · C14 · RISK-010 · **Open.**
OQ-036 · **Status.** Planned, and deliberately **not** stubbed: C14 omits a `ThermalState` input
entirely rather than carry an invented policy, because the decode-cost measurements that would
justify one (EXP-011, EXP-020) have not run.

---

## BEN / SIM — Measurement

### BEN-01 — Goodput measurement with stated numerator and denominator · **Phase.** 3 · **P0** · C15 · **Implemented in the simulator only** (`ffsim` names every rate it prints and labels `Fd` as an assumption). **C15 itself does not exist**, so there is no telemetry system, no session record, and no on-device measurement — and no goodput figure has ever been measured on hardware
### BEN-02 — Per-frame telemetry with zero hot-path allocation · **Phase.** 3 · **P0** · C15 · Planned
### BEN-03 — Six benchmark categories · **Phase.** 4 · **P0** · Planned
### BEN-04 — Reproducible benchmark reports (median + range, never best run) · **Phase.** 4 · **P0** · Planned
### BEN-05 — QR baseline implementation
**Description.** A calibrated dynamic-QR stream measured on the same hardware.
**Value.** G4 cannot be claimed without it.
**Phase.** 2 · **Priority.** P0 · **Experiments.** EXP-002 · **Status.** Planned
### SIM-01 — Channel simulator with configurable impairments · **Phase.** 1 · **P0** · C16 · **Implemented** (`sim/`, `ffsim`). ⚠ Uncalibrated — RISK-024
### SIM-02 — Reproducible run configuration files · **Phase.** 1 · **P0** · C16 · **NOT implemented.** `ffsim` takes flags only. Runs *are* reproducible from flags plus the seed, which it always echoes, so the property is satisfied while the artefact this entry names is not — worth keeping open, since a sweep's configuration currently lives in a shell script rather than in a versioned file next to its results
### SIM-03 — Simulator calibration against real captures
**Description.** Fit impairment parameters to recorded captures; report predicted-versus-
measured error. **Value.** Without this, simulator results are not predictive.
**Phase.** 2 · **Priority.** P0 · **Status.** Planned
### SIM-04 — Recorded-frame replay harness · **Phase.** 2 · **P0** · C17 · **Implemented** (`harness/`, `ffreplay`); proven bit-identical to live decode (F17) and exercised on a real device capture (F29)
### SIM-05 — `CaptureSource` abstraction · **Phase.** 1 · **P0** · **Implemented** (`core/include/fileflow/capture_source.h`); simulator and replay harness both enter the chain through it

---

## SEC — Security

### SEC-01 — Bounds validation before allocation · **Phase.** 3 · **P0** · **Implemented** across the parsers, with a distinct `Error` code per limit so a fuzzer crash or field report says *which* bound was hit. The rule earned itself: the detector once sized an allocation from attacker-controlled pixel content, up to 762 MB per frame on a hostile all-bright image (F12)
### SEC-02 — Integer overflow discipline in all parsers · **Phase.** 3 · **P0** · **Implemented** (checked arithmetic, `kSizeOverflow`); built with `-Wconversion -Wsign-conversion -Werror` so implicit narrowing cannot reappear quietly
### SEC-03 — Fuzzing of protocol parsers · **Phase.** 4 · **P0** · **Implemented** — 5 targets in `fuzz/`. ⚠ **Linux CI only:** Apple clang ships no libFuzzer, so local macOS development has ASan/UBSan but not the fuzzing half of that defence (F5)
### SEC-04 — Session isolation (session ID, no cross-session state) · **Phase.** 4 · **P1** · Planned
### SEC-05 — Malformed FEC/fountain input handling · **Phase.** 4 · **P0** · **Implemented** (`fuzz/fuzz_reed_solomon.cpp`, `fuzz_manifest.cpp`); erasure positions are validated and deduplicated before use, because duplicates would inflate the locator's degree and let the decoder claim more correction budget than it has
### SEC-06 — Camera privacy disclosure
**Description.** Clear indication when the camera is active; no frame retention beyond the
session unless the user explicitly enables capture recording.
**Phase.** 4 · **P0** · Planned
### SEC-07 — Optical eavesdropping disclosure
**Description.** Surface to the user that anyone with line of sight receives the data.
**Value.** A property of the medium; hiding it would be misleading.
**Phase.** 4 · **P1** · Planned
### SEC-08 — Optional authenticated encryption · **Phase.** 10 · **P2** · Deferred
### SEC-09 — Optional digital signatures · **Phase.** 10 · **P3** · Deferred

---

## UI — User interface

### UI-01 — File selection and transfer initiation · **Phase.** 3 · **P0** · C01 · Planned
### UI-02 — Aiming and alignment guidance
**Description.** Help the user frame the transmitting screen; show lock status and quality.
**Value.** Acquisition time is dead time in the goodput denominator.
**Phase.** 4 · **P1** · Planned
### UI-03 — Progress and completion reporting · **Phase.** 4 · **P0** · Planned
### UI-04 — Failure reporting with actionable cause
**Description.** Distinguish "move closer", "hold steadier", "too dark", "hash mismatch".
**Phase.** 4 · **P1** · Planned
### UI-05 — Screen-on and brightness lifecycle · **Phase.** 3 · **P1** · Planned
### UI-06 — Visual comfort mitigation
**Description.** Warning before a high-contrast flickering display; option to reduce.
**Value.** RISK-013. **Phase.** 4 · **P1** · Planned

---

## DOC — Documentation

### DOC-01 — Documentation index and map · **Phase.** 0 · **P0** · C20 · **Done**
### DOC-02 — ADR set · **Phase.** 0 · **P0** · **Done** (12 initial ADRs)
### DOC-03 — Registries (feature, experiment, risk, open questions) · **Phase.** 0 · **P0** · **Done**
### DOC-04 — Automated link checking in CI · **Phase.** 1 · **P2** · **DONE**
`tools/check_links.py`, run by the `docs` CI job on every push. It checks that links *resolve*;
it cannot detect a document whose prose has gone stale, which is the failure mode that actually
recurs here (see RISK-020's 2026-08-03 update). Exact link and file counts are deliberately not
quoted — the tool prints them, and a hardcoded count is itself a thing that drifts.
### DOC-05 — Documentation/implementation divergence review · **Phase.** 3 · **P1** · RISK-020 · **First pass done 2026-08-04, and it found a lot.** ~30 entries in this registry still said *Planned* for work completed and documented in F7–F19; three separate stale claims were corrected in PHASE1-FINDINGS' own "not validated" list, one of which had gone stale twice within a single day. Recurring, so it needs to be a **periodic** review rather than a one-off task (F30)
