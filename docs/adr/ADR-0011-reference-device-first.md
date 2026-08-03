# ADR-0011 — Reference-device-first development

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Project lead
> **Related:** ADR-0001, NG3, RISK-011, RISK-017

## Context

Android device diversity is enormous. Panels differ in technology (OLED/LCD), refresh
capability, subpixel layout and brightness. Cameras differ in sensor, rolling-shutter skew,
supported formats, frame rates, manual-control availability and — critically — in whether
they honour the controls they advertise.

A protocol tuned to work acceptably everywhere performs like it. Meanwhile, every hour
spent on device-compatibility work is an hour not spent learning what the channel can do.

## Decision

Develop against a **small set of named reference devices**, optimise for them, and treat
broad compatibility as a later phase (Phase 10). The capability probe (C02) classifies
unknown devices into conservative tiers rather than attempting to support them well.

Reference devices should be selected to span the axes that matter:

| Axis | Why it matters |
|---|---|
| Panel type (OLED vs LCD) | Subpixel layout, contrast, response time (EXP-021) |
| Max refresh (60 vs 120 Hz) | Bounds `Fd` |
| Camera high-speed capability | Determines whether milestone 6 is testable at all |
| Manual-control completeness | `FULL`/`LEVEL_3` versus `LIMITED` |
| SoC class | Decoder throughput and thermal headroom |

**Specific devices are not named in this ADR** — the selection should be made when the
hardware is procured, recorded in a `DEVICE-MATRIX.md`, and treated as a versioned fact.

## Alternatives

1. **Broad compatibility from the start.** Rejected: forces conservative choices
   everywhere, and we would never learn where the ceiling is.
2. **Single device only.** Rejected: too easy to overfit to one device's quirks and mistake
   them for channel physics. A minimum of two distinct pairings is needed to distinguish
   the two.
3. **Emulator/simulator only until late.** Rejected: the simulator's fidelity is itself
   unvalidated until checked against real captures (ADR-0010).

## Consequences

**Positive.** Deep optimisation. Interpretable results. Manageable test matrix. Faster
iteration. The cross-device matrix benchmark (category 4) still catches gross
device-specific assumptions early.

**Negative.** Results may not generalise — and we will not know how badly until Phase 10.
Risk of encoding a reference device's quirk as a protocol assumption. Limited real-world
applicability of early results.

**Neutral.** The capability probe and adaptive link controller exist partly to make the
eventual generalisation tractable; designing them from the start is not wasted work even
though breadth is deferred.

## Evidence

- Vendor camera implementations are known to be inconsistent, including advertising
  capabilities they do not honour (RISK-011). This is well-attested in Android developer
  experience generally; we have no measurement of our own yet. `[HYP]`
- High-speed video support and its constraints vary by device — `getHighSpeedVideoSizes`
  and `getHighSpeedVideoFpsRangesFor` are per-device enumerations by design. `[FACT]`
- Prior screen-camera work commonly uses desktop monitors as transmitters, so published
  results do not transfer to phone panels — an argument for measuring our own hardware
  rather than relying on literature. `[LIT]`

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| RISK-017: results do not reproduce across devices | **High** | Medium | Cross-device benchmark category from Phase 4, not Phase 10 — early warning, even with few devices |
| Protocol encodes a reference-device quirk | Medium | High | At least two distinct device pairings; capability probe describes rather than assumes |
| Reference hardware becomes obsolete | Medium | Low | `DEVICE-MATRIX.md` is versioned; refresh as needed |

## Validation plan

Not falsifiable as such — it is a scope decision. The relevant check is the **cross-device
matrix benchmark**, run from Phase 4 onward. If goodput varies wildly across even our small
reference set, the "optimise deeply for few devices" premise is under strain and the
adaptive layer needs more investment sooner.
