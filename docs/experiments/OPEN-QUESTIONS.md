# Open-question registry

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-03
> **Related:** EXPERIMENT-REGISTRY.md, RISK-REGISTER.md, PHASE1-FINDINGS.md

Every unresolved question that could change a design decision. Questions are closed only
by evidence, and the closing evidence is recorded.

Severity: **Blocking** (a decision cannot be made without it) · **High** (would change the
plan) · **Medium** (would change a component) · **Low** (would refine a parameter).

---

## Platform

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-001** | On reference devices, what is the maximum frame rate at which CPU-accessible `YUV_420_888` is delivered without drops, at a resolution sufficient for our densest grid? | **Blocking** | EXP-007 | Open |
| **OQ-002** | Does the GPU-texture path in a high-speed session deliver genuinely *distinct* frames at 120 fps, or does the vendor pipeline duplicate/interpolate them? | **Blocking** (milestone 6) | EXP-007 | Open |
| **OQ-004** | Which presentation-verification mechanism is reliable per-frame at 60 and 120 Hz? (`ASurfaceTransaction` completion fences vs FrameTimeline vs Perfetto-only) | High | EXP-006 | Open |
| **OQ-005** | Does `setFrameRate` reliably hold a 120 Hz mode for a multi-minute transfer, or does thermal/power management drop it? | High | EXP-006, EXP-020 | Open |
| **OQ-015** | How much active capability *verification* is worth doing at startup versus at session negotiation? | Medium | CAP-03 design | Open |
| **OQ-016** | Do vendor `EDGE_MODE` / `NOISE_REDUCTION_MODE` / `TONEMAP_MODE` settings actually take effect, or are they silently ignored? | High | EXP-007 | Open |
| **OQ-017** | Is `SENSOR_ROLLING_SHUTTER_SKEW` reported, and is the reported value accurate? | Medium | EXP-007, EXP-008 | Open |
| **OQ-018** | Does panel subpixel structure (OLED PenTile vs LCD stripe) measurably affect binary luminance separation at our cell pitches? | Medium | EXP-021 | Open |
| **OQ-023** | Is there a lossless recording path for the capture harness, or does every available recording route compress lossily? | High | EXP-007 | Open |

## Signal and modulation

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-003** | Which frame layout (A/B/C) and grid size? | **Blocking** | EXP-001, EXP-013 | Open |
| **OQ-006** | Which final modulation profile set? | High | EXP-010, EXP-013, EXP-014 | Open |
| **OQ-007** | Which four-colour constellation, if M3 is pursued at all? | Medium | EXP-014 | Open |
| **OQ-011** | What is the effective number of *independent* spatial subchannels at each cell pitch? (Crosstalk means it is fewer than the cell count) | Medium | EXP-001 | Open |
| **OQ-012** | What is the distribution of spatially-clustered damage region sizes? Drives interleaver design | Medium | Phase 2 characterisation | Open |
| **OQ-019** | What is the optimal cell interior sampling margin? | Medium | EXP-015 | Open |
| **OQ-020** | Is persistent tracking actually cheaper **and** more accurate than per-frame detection under handheld motion? | High | EXP-017 | Open |
| **OQ-022** | Which marker patterns maximise detection reliability at our cell pitches under defocus? | Medium | Simulator study | Open |
| **OQ-024** | Where is the density cliff — the grid pitch past which goodput collapses? | High | EXP-001 | Open |

## Coding

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-009** | Which intra-frame code family and rate? | High | EXP-011 | Open |
| **OQ-010** | Which fountain scheme — and is RaptorQ legally available for smartphone deployment given the Qualcomm IPR tiering? | High | EXP-012 + RT-07 legal review | Open |
| **OQ-014** | What fraction of one big core can the FEC decoder use before it competes with capture and reduces `Pc`? | Medium | EXP-011, EXP-016 | Open |
| **OQ-025** | Is 8-bit LLR quantisation sufficient, or does it cost measurable coding gain? | Low | EXP-011 | Open |

## Architecture and protocol

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-013** | **On a one-way link, how does the transmitter learn channel state at all?** Profile cycling wastes display states; reverse optical control is Phase 9. This is the least-designed part of the architecture. **Partially quantified 2026-08-03 (EXP-023, F20):** the receiver *can* now identify the goodput-optimal code rate exactly, from telemetry it already produces — so the open part is narrowed to **delivery**, not estimation. A wrong fixed guess costs **50–93%** of available goodput on simulated channels, which raises the value of ADP-04 considerably. Still open, and still the least-designed area | **High** | Design work + Phase 5 measurement; re-measure the spread on real captures | Open |
| **OQ-008** | Can we self-calibrate lens distortion from the transmitted grid, avoiding per-device calibration? | Medium | Simulator study + Phase 2 | Open |
| **OQ-026** | What is the correct behaviour when the app is backgrounded mid-transfer — pause or abort? | Low | Design decision | Open |
| **OQ-027** | Should the receiver hash incrementally during reception (early corruption detection) or only at the end? | Low | Design decision | Open |
| **OQ-036** | What thermal policy should C14 apply (ADP-05)? Higher `nsym` costs more decode work, but no ARM64 decode-cost measurement exists, so any policy today would be invented. C14 deliberately ships **without** a `ThermalState` input rather than fake one | Medium | EXP-011 (decode cost), EXP-020 (throttling curve) | Open |
| **OQ-037** | **Should the fountain symbol size be decoupled from frame capacity?** Today `nsym` → FEC message size → fountain symbol size → manifest, which is fixed for the session, so **no capacity-changing knob can move mid-transfer** (F20). A fixed small symbol size carrying an integer number of whole symbols per frame would unblock mid-session rate adaptation without touching the fountain mathematics, and would also address F4's ~27% block-padding waste. Cost: more header bookkeeping per frame, and a protocol version bump | **High** | Design work; becomes an ADR if taken | Open |

## Measurement and process

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-028** | How faithful must the simulator's impairment model be before its conclusions transfer to hardware? | **High** | Phase 2 calibration (SIM-03) | Open |
| **OQ-029** | What is the real `Pc` on reference hardware? The model assumes 0.55–0.80; this is a guess and it is high-leverage | **High** | EXP-009 | Open |

## Research gaps

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-030** | What do the visible-branch systems (COBRA, RainBar, RDCode, ShiftCode, FareQR) actually report, and in which metric? | High | RT-01, RT-02 | Open |
| **OQ-031** | What does the Visual MIMO capacity analysis actually say? Our dense-grid thesis currently rests on an unread foundation | High | RT-03 | Open |
| **OQ-032** | Does ShiftCode's greyscale mode really outperform its four-colour mode? If so it is evidence against M3 | Medium | RT-01 | Open |

## Project

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| ~~OQ-021~~ | ~~Which licence for this project?~~ | — | **CLOSED 2026-08-02: Apache-2.0** (ADR-0013 D4) | **Closed** |
| ~~OQ-033~~ | ~~Which specific reference devices?~~ | — | **CLOSED 2026-08-02: Pixel 8 + Galaxy S26 Ultra.** See [DEVICE-MATRIX.md](../planning/DEVICE-MATRIX.md). Both OLED, so EXP-021 (OLED vs LCD) cannot run as written | **Closed** |
| **OQ-034** | Does the S26 Ultra's Privacy Display / Black Matrix restrict the optical channel even when disabled, and does it cause transmitter-side radial luminance falloff at close range? | **High** | EXP-003, EXP-018 (add Privacy-on/off arms) | Open |
| **OQ-035** | Does Samsung restrict ≥60 fps capture for third-party apps on the S26 Ultra? If so the Pixel 8 is our only high-frame-rate receiver and milestone 6 rests entirely on it | **High** | EXP-007 | Open |

---

## The five most consequential

1. **OQ-001 / OQ-002 — camera frame-rate and access-path reality.** Determines a large
   implementation fork and whether milestone 6 has any basis at all.
2. **OQ-029 — the real `Pc`.** The model's weakest input and among its highest-leverage
   variables. If `Pc` is ~0.35 rather than ~0.70, every projection halves.
3. **OQ-003 / OQ-024 — grid and layout, and where the density cliff sits.** The literature
   shows a cliff exists; we do not know where ours is, and the model does not contain one.
4. **OQ-013 / OQ-037 — adaptation on a one-way link, and the protocol constraint underneath
   it.** Still genuinely under-designed. EXP-023 narrowed it usefully: estimation is solved
   (the receiver names the optimal rung exactly, for free), so what remains is delivery — and
   the first obstacle is not the one-way link but our own manifest, which fixes the fountain
   symbol size for the session and therefore freezes every capacity-changing knob (OQ-037).
5. **OQ-028 — simulator fidelity.** Phase 1 produces conclusions that several architectural
   decisions depend on, and their validity is unknown until Phase 2 calibration.
