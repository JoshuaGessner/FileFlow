# ADR-0007 — Binary luminance as the baseline modulation

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Modulation subsystem
> **Related:** ADR-0008, MODULATION-SPEC.md, EXP-003, EXP-004

## Context

We must pick a first modulation to build the end-to-end system on. The choice determines
how quickly we get a working link and how interpretable early failures are.

## Decision

**M0 — binary luminance** (one bit per cell, dark versus bright) is the baseline. It is
used for the first end-to-end implementation and **permanently** for headers and other
critical fields, regardless of what payload modulation is later selected.

## Alternatives

1. **Start with four-level luminance (M2).** Rejected: two bits per cell is attractive but
   halves noise margin, requires working photometric calibration *before* anything works
   at all, and makes early failures ambiguous — is the problem geometry, timing, or level
   slicing?
2. **Start with colour (M3).** Rejected more firmly: adds colour calibration, white-balance
   locking, chroma subsampling and per-device primary variation to the list of things that
   must all work before the first bit arrives.
3. **Start with differential (M1).** Tempting, since M1 is the intended optimisation path
   (ADR-0008). Rejected as the *starting* point because differential decoding requires
   correct frame-phase classification and state pairing to work first — more moving parts
   before first light. M1 follows immediately after M0.

## Consequences

**Positive.** Maximum Euclidean symbol separation for a given brightness range. No
photometric calibration needed to get *something* working. Unambiguous failures: if binary
does not decode, the problem is geometry, timing or tracking — not level slicing. Fastest
route to milestone 2.

**Negative.** One bit per cell. The performance model shows **binary at 60 display states
per second does not reach milestone 4 (200 KB/s) on any candidate grid** — optimistic
binary lands around 145 KB/s. Binary is a starting point, not a destination.

**Neutral.** Keeping binary permanently for headers costs almost nothing (headers are a
tiny cell fraction) and buys substantial robustness.

## Evidence

- Binary maximises separation between symbols for a fixed dynamic range — elementary and
  uncontested.
- Performance model output: M0/M1 binary at 60 states/s reaches ~50 KB/s (96×160),
  ~78 KB/s (120×200), ~112 KB/s (144×240) at the expected channel; ~145 KB/s in the
  optimistic 144×240 case. All **below** the 200 KB/s milestone. `[HYP — model output,
  not measurement]`
- A secondary summary of ShiftCode reports its greyscale two-colour mode outperforming its
  four-colour mode. `[LIT — unverified]` If it replicates, binary's robustness advantage
  may persist further up the rate curve than expected.

## Risks

| Risk | Mitigation |
|---|---|
| Binary cannot reach the goodput milestones alone | Acknowledged explicitly; M2/M3 are planned, gated on measurement |
| Team over-invests in optimising binary | Roadmap moves to M1/M2 once milestone 3 is met |
| Even binary fails at the densest grids | EXP-001 finds the density limit before we commit to a grid |

## Validation plan

- **EXP-003** — binary brightness separation: measure achievable contrast between dark and
  bright cells across distance, angle and ambient light, per device pair.
- **EXP-004 / EXP-005** — exposure and ISO optimisation for maximum separation.
- **EXP-001** — maximum resolvable binary grid.

Binary luminance's role as the *baseline and header modulation* is not in doubt. What
requires validation is how far it can be pushed on grid density before the error rate
makes further density counterproductive.
