# Modulation specification (staged)

> **Status:** Draft
> **Owner:** Modulation subsystem
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0007, ADR-0008, EXP-003, EXP-010, EXP-013, EXP-014

Five staged modes. Each is defined with its alphabet, pilot needs, calibration,
demodulation, soft-confidence representation, failure modes, processing cost, required
experiments, and the conditions for enabling it or falling back from it.

**Modes are gated on measurement, not scheduled.** A mode is enabled when it demonstrably
increases end-to-end goodput on the target channel, not when it is implemented.

---

## M0 — Binary luminance

**Status:** Baseline. Permanent for headers.

| Aspect | Definition |
|---|---|
| **Alphabet** | {dark, bright} — 1 bit/cell |
| **Pilots** | Minimum: enough to establish per-region dark and bright reference levels |
| **Calibration** | Regional threshold from pilot dark/bright midpoint; vignetting correction |
| **Demodulation** | Sample → normalise → compare against regional threshold |
| **Soft confidence** | `LLR ∝ (sample − threshold) / σ_region`, quantised to 8 bits |
| **Failure modes** | Saturation from glare (both levels clip → region unusable, must be erased not decoded); insufficient contrast at distance; ambient washout |
| **Processing cost** | Lowest of all modes |
| **Enable when** | Always available; the fallback of last resort |
| **Fall back to** | Nothing — this is the floor. If M0 fails, reduce grid density or abort |

**Experiments:** EXP-001 (max resolvable grid), EXP-003 (brightness separation),
EXP-004/005 (exposure/ISO).

---

## M1 — Differential binary luminance

**Status:** Proposed optimisation path — **see ADR-0008 for why confidence is low.**

Four schemes to compare, which differ fundamentally in what they cost:

### M1a — Current frame versus prior frame
Bit encoded as change/no-change between consecutive display states.
- **Cost:** requires correct frame pairing → depends on frame-phase classification.
- **Risk:** a lost frame breaks the chain and can corrupt two states, not one.

### M1b — Payload frame plus complementary frame
Each payload state followed by its photometric complement; decode the difference.
- **Cost: halves `Fd` for payload purposes.** Two states carry one state's data.
- **Benefit:** extremely robust — the difference cancels all static distortion exactly.
- **Break-even:** must more than double `Pc × Rfec` to justify halving `Fd`. **High bar.**

### M1c — Cell versus local pilot
Each payload cell decoded relative to a nearby pilot in the same frame.
- **Cost:** cells for pilots (spatial), **not** display states (temporal).
- **No error propagation. No phase-pairing dependency. No `Fd` penalty.**
- Arguably this is excellent photometric calibration rather than differential modulation —
  the distinction is thin, and that is fine.

### M1d — Temporal difference plus spatial normalisation
Combines M1a with per-region spatial normalisation.
- **Cost:** highest complexity; inherits M1a's error propagation.

| Aspect | Definition |
|---|---|
| **Alphabet** | {change, no-change} or {above-reference, below-reference} — 1 bit/cell |
| **Pilots** | M1c needs a dense pilot lattice (favours frame Candidate B) |
| **Calibration** | Reduced or eliminated — the point of the mode |
| **Soft confidence** | LLR from difference magnitude relative to noise estimate |
| **Failure modes** | Error propagation (M1a/d); halved state rate (M1b); pilot occlusion (M1c) |
| **Processing cost** | Low; one extra frame buffer for temporal schemes |
| **Enable when** | EXP-010 shows a goodput gain over calibrated M0 **on the same channel** |
| **Fall back to** | M0 |

**Honest assessment:** M1c is likely to survive on its merits. M1b's arithmetic is
unfavourable and it should be tested early and cheaply so it can be discarded quickly if
the model is right. M1a/M1d inherit error propagation in a channel where frame loss is
normal — which is the wrong direction. See ADR-0008.

**Experiments:** EXP-010 — all four schemes versus calibrated M0, compared on **end-to-end
goodput**, in the simulator first.

---

## M2 — Four-level luminance

**Status:** Planned. **Required to reach milestone 4 unless `Fd` exceeds 60.**

| Aspect | Definition |
|---|---|
| **Alphabet** | 4 luminance levels — 2 bits/cell nominal |
| **Level placement** | **Not** equally spaced in code value. Camera response is non-linear even with a linear tone curve requested, and perceived/sensed spacing differs. Levels should be placed to equalise *decision margin in sensor counts*, derived experimentally |
| **Pilots** | All four levels must be present in the pilot set, distributed across regions |
| **Calibration** | Per-region four-level slicing thresholds from pilots; requires `TONEMAP_MODE = CONTRAST_CURVE` with a linear curve, plus a software fallback if the device ignores it |
| **Demodulation** | Sample → normalise → nearest-level with per-region thresholds |
| **Soft confidence** | Per-bit LLRs derived from distance to the two nearest level boundaries. Gray-coding the levels so adjacent levels differ in one bit is **essential** — it makes the common error (off-by-one level) a single-bit error |
| **Failure modes** | Threshold drift; compressed dynamic range at distance/angle; a level pair merging under blur; non-linear device response defeating calibration |
| **Processing cost** | Moderate — more thresholds, more LLR computation |
| **Enable when** | Measured per-region SNR supports four levels with margin **and** end-to-end goodput improves over M0/M1 |
| **Fall back to** | M0/M1 on rising symbol error rate or falling pilot SNR |

**Why this mode matters:** the performance model's sensitivity analysis shows `B` is among
the highest-leverage variables (~40% goodput swing for ±20%). M2 is the most direct route
to milestone 4 at 60 display states/s.

**Why it might not pay:** halving noise margin raises symbol error rate, which forces
`Rfec` down and pushes `Pc` down. The model's M2 scenarios already assume `Rfec` drops
from 0.80 to 0.75 to compensate. If the real cost is larger, M2 could be goodput-neutral
or negative. `[HYP]`

**Experiments:** EXP-013 (four-level luminance evaluation), plus level-placement
optimisation.

---

## M3 — Four-colour

**Status:** Planned, low confidence.

| Aspect | Definition |
|---|---|
| **Alphabet** | 4 colours — 2 bits/cell nominal. **Do not assume red/green/blue/white** |
| **Constellation** | **Derived experimentally.** Must maximise separation *after* the full display → optics → CFA → ISP → YUV chain, not in display colour space |
| **Pilots** | Colour pilot cells, reserved in all frame layouts from the start |
| **Calibration** | AWB locked (`CONTROL_AWB_MODE = OFF` + explicit `COLOR_CORRECTION_GAINS`); estimate and invert the 3×3 display-primary → camera-response matrix per region |
| **Demodulation** | Sample chroma → decorrelate → nearest constellation point |
| **Soft confidence** | Distance to nearest constellation boundaries in the decorrelated space |
| **Failure modes** | **Chroma is 2×2 subsampled in `YUV_420_888`** `[FACT]` → colour cells must be ≥2×2 sensor pixels, so M3 forces *larger* cells than M2 at the same sensor resolution; AWB drift; per-device primary variation; OLED subpixel layout differences between colours |
| **Processing cost** | Highest of the static modes — chroma planes must be read and processed, which M0/M1/M2 skip entirely |
| **Enable when** | EXP-014 shows a goodput gain over M2 **at the cell size each mode actually requires** |
| **Fall back to** | M2 |

**The critical comparison** is not "4 colours versus 4 levels at the same grid". It is
"4 colours at the grid chroma subsampling permits versus 4 levels at the grid luminance
permits". Because chroma is subsampled 2×2, M3 may need up to 4× the cell area for the
same reliability — which would **more than cancel** the 2-bits-per-cell benefit it shares
with M2. On this reasoning M3 starts behind M2, not ahead.

Supporting hint: a secondary summary of ShiftCode reports its greyscale two-colour mode
outperforming its four-colour mode. `[LIT — unverified, RT-01]`

**Experiments:** EXP-014 (candidate colour constellations), gated behind M2 results.

---

## M4 — Mixed-frame-aware temporal-spatial modulation

**Status:** Research. Phase 7.

| Aspect | Definition |
|---|---|
| **Alphabet** | Variable — models a camera frame as a *mixture* of consecutive display states rather than a sample of one |
| **Concept** | With rolling shutter, a camera frame's row *r* is exposed at a known offset. If the display state boundary falls at row `r_b`, rows above see state *k* and rows below state *k+1*, with a transition band between. Rather than discarding such frames, decode both regions and erase only the band |
| **Pilots** | Requires a reliable frame-phase indicator and timing tracks to locate `r_b` per frame |
| **Calibration** | Needs `SENSOR_ROLLING_SHUTTER_SKEW` where available `[FACT]`, or estimation from the optical signal |
| **Demodulation** | Locate transition band → decode above-band as state *k*, below-band as state *k+1* → erase the band |
| **Soft confidence** | Near-zero LLR (erasure) inside the band; normal confidence outside; graded confidence near band edges |
| **Failure modes** | Band mislocation corrupting both states; two boundaries in one frame at high `Fd`; drift changing band position between frames |
| **Processing cost** | Moderate — mostly bookkeeping over the existing sampler |
| **Enable when** | Frame-phase classification is reliable (EXP-008) and `Fd > `camera fps |
| **Fall back to** | Erase mixed frames entirely (the M0–M3 behaviour) |

**Why this is the big lever.** The model's `Pm` term is the difference between the M2@120
scenario (227 KB/s) and the M4@120 scenario (576 KB/s). At high `Fd`, most frames are
mixed, so recovering even 30% of them is a large gain. This is the most promising path to
milestone 5 and beyond.

**Structural note:** frame layout Candidate C (hierarchical tiles) makes M4 substantially
easier, because tile rows already correspond to time slices — a mixed frame yields clean
tiles from the top and bottom states with no unmixing at all. This is a strong argument for
Candidate C that only becomes visible when M4 is considered, and it is a good example of
why the frame-layout decision should not be made before the modulation roadmap is understood.

**Experiments:** EXP-008 (mixed-frame classification), then M4 recovery-rate measurement.

---

## Profile selection and fallback

The adaptive link controller (C14) selects the active profile. General rules:

- **Never enable a mode that has not demonstrated a goodput gain** on the current channel.
- **Fall back fast, promote slowly** — a mode change costs a resynchronisation, so
  oscillation is expensive. Hysteresis is mandatory.
- Header always uses M0, regardless of payload mode.
- Profile ID is carried in every frame header so the receiver never has to guess.

`[OPEN]` OQ-013 — on a one-way link, the transmitter has no channel feedback. The initial
design has it cycle through a defined profile schedule while the receiver uses whichever
decodes. This wastes display states on profiles that do not work, and the waste is
proportional to how many profiles are in the schedule. **This is an under-designed area of
the architecture** and is the main argument for prioritising reverse optical control
(Phase 9) earlier than currently planned.

> **Amended 2026-08-03 from EXP-023 (findings F20, F22).** Three corrections to the rules above,
> all from building the controller.
>
> 1. **"Fall back fast, promote slowly" cannot be implemented with margins alone.** A promote
>    margin is only ever large or small *relative to the gain one profile step offers*, and a
>    margin exceeding that gain does not slow promotion down — it **stops it permanently**, at
>    whatever profile the controller happened to reach, with nothing looking wrong. On the
>    implemented parity ladder, adjacent rungs differ by 3.35% of frame capacity, so a
>    perfectly ordinary-looking 10% margin strands the link ~6% below its achievable rate for
>    the whole session. The asymmetry must come mostly from the **dwell**, and the margin must
>    be validated against the ladder (F22). `LinkController::Create` rejects the bad pairing.
>
> 2. **"Profile ID is carried in every frame header so the receiver never has to guess" is not
>    sufficient for the schedule to work.** Changing profile changes per-frame payload capacity,
>    which changes the fountain symbol size, which `FileManifest` fixes for the whole session —
>    so ADP-03's cycling has nothing it can legally cycle. The blocker is our own protocol, not
>    the one-way link, and it is reached first (OQ-037, F20).
>
> 3. **The waste is now bounded on the other side too.** A *fixed* profile chosen badly costs
>    50–93% of available goodput on simulated channels (EXP-023). So the schedule's cost has to
>    be compared against that, not against an idealised perfectly-chosen constant.
