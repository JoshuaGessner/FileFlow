# ADR-0008 — Differential modulation as the primary optimisation path

> **Status:** Proposed — **lowest-confidence decision in the initial set**
> **Date:** 2026-08-02
> **Owner:** Modulation subsystem
> **Related:** ADR-0007, MODULATION-SPEC.md, EXP-010

## Context

The prescribed architecture names differential modulation as the primary optimisation
path after the binary baseline. The idea: decode *changes* between related display states
rather than absolute brightness, so that fixed channel distortion — vignetting, uneven
illumination, per-cell display non-uniformity, exposure offset — cancels out.

## Decision

Adopt **M1 (differential binary luminance)** as the first optimisation after M0, and
evaluate four differential schemes:

1. Current frame versus prior frame
2. Payload frame plus complementary frame
3. Cell versus local pilot
4. Temporal difference plus spatial normalisation

## Alternatives

1. **Go straight from M0 to M2 (four-level).** Bits per cell is the variable the model is
   most sensitive to, and M2 directly doubles it. This is a serious alternative and may
   well be the better path.
2. **Better photometric calibration instead of differential coding.** Scheme 3
   (cell versus local pilot) is arguably this alternative wearing a differential costume —
   the distinction between "differential against a pilot" and "well-calibrated absolute
   decoding" is thinner than it first appears.
3. **Skip M1 entirely.** Defensible if EXP-010 shows no advantage.

## Consequences

**Positive.** If it works, differential decoding removes a whole class of channel
distortion without spending cells on calibration, and makes the link robust to exposure
drift — which the camera will do despite our best locking efforts on some devices.

**Negative — and this is the crux.** The temporal differential schemes (1 and 2) have a
serious cost that must be stated plainly:

- **Scheme 2 (payload + complement) halves `Fd` for payload purposes.** Two display states
  carry one state's worth of data. To break even it must more than double the usable cell
  count or the code rate. That is a high bar.
- **Scheme 1 (frame versus prior frame) requires correct frame pairing**, which requires
  correct frame-phase classification. In a channel where mixed frames are normal and whole
  frames are lost routinely, a lost frame breaks the differential chain and can corrupt
  *two* states rather than one — error propagation the absolute scheme does not have.
- Both temporal schemes are **more sensitive to display/camera clock drift**, not less.

Scheme 3 (cell versus local pilot) has none of these problems, because it is spatial
rather than temporal. It costs cells for pilots instead of costing states.

**Neutral.** M1 keeps one bit per cell nominally; the benefit, if any, appears as higher
`Pc` and `Rfec`, not higher `B`.

## Evidence

**This is the weakest-evidenced decision in the set, and it should be treated that way.**

- We found **no primary-source evidence** during Phase 0 research that differential
  screen-camera modulation outperforms well-calibrated absolute modulation. The visible-
  branch literature that would address this is unread (RT-01).
- The theoretical argument for cancelling fixed distortion is sound.
- The theoretical argument against — halved state rate, error propagation, dependence on
  phase classification — is equally sound, and the prescribed architecture does not
  address it.
- ChromaCode's degradation data (raw fell 6×, goodput fell 119× under worsening
  conditions) `[LIT]` shows the channel's dominant problem is frame-level events, not
  static photometric distortion. **Differential modulation addresses static distortion.
  It does not address the thing that actually destroys goodput.** This is the strongest
  argument against prioritising it.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Halving `Fd` costs more than differential decoding gains | **Medium-High** | High | EXP-010 compares end-to-end goodput, not error rate |
| Error propagation through the differential chain on frame loss | Medium | Medium | Periodic absolute reference frames; scheme 3 avoids it entirely |
| Effort spent on M1 that would be better spent on M2 | Medium | Medium | Run EXP-010 early and cheaply in the simulator, before implementing on device |
| We pursue this because it was prescribed rather than because it measured well | **Medium** | High | This ADR; explicit gate below |

## Validation plan

**EXP-010** — differential versus absolute decoding, compared on **end-to-end goodput**,
in the simulator first (cheap, fast, no device needed), across a range of static-distortion
severities and frame-loss rates.

**Explicit gate:** if EXP-010 shows the temporal differential schemes do not beat
well-calibrated absolute decoding on goodput, **M1 is dropped from the optimisation path
and effort moves to M2**, and this ADR is superseded. Scheme 3 (cell versus local pilot)
may be retained independently as a photometric-calibration technique regardless of what
happens to the temporal schemes.

We should be prepared for that outcome. It is a genuine possibility, not a formality.
