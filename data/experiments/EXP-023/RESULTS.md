# EXP-023 — Adaptive intra-frame code rate selection

> **Status:** Complete (simulated portion)
> **Owner:** C14 / adaptation
> **Last reviewed:** 2026-08-03
> **Related:** EXPERIMENT-REGISTRY.md (EXP-023), PHASE1-FINDINGS.md F20–F23, OQ-013, OQ-037

⚠ **Every number here is `[HYP]`.** The channel model is uncalibrated (RISK-024). This
experiment tests whether the controller's **decision rule** is sound. It does **not** say which
`nsym` real hardware will want, and the goodput figures are simulator outputs, not measurements.

## Provenance

| | |
|---|---|
| Run date | 2026-08-03 |
| Script | `tools/adapt_sweep.sh` (reproduces every number below) |
| Raw | `raw/adapt_sweep_seed5_v2.txt` |
| Superseded raw | `raw/adapt_sweep_seed5.txt` — same run, but its table header mislabelled the erasure-load column as "WORST BUDGET". Kept, not deleted (CONTRIBUTING); the distinction between those two quantities is the subject of F23, so the mislabel is exactly the confusion under study |
| Payload | 131,072 B, deterministic from seed |
| Seed | 5 |
| Grid | 120×200, layout Candidate B, M0 |
| Ladder | `nsym ∈ {8, 16, 24, 32, 40, 48, 64}` |
| Reference rung | 32 — the only rung the controller observes |
| `Fd` | 60 states/s, **assumed, not measured** (ADR-0012) |
| Frame cap | 40,000 display states |
| Runs per cell | 1 — see *Limitations* |

Metric throughout: **verified payload goodput** (ADR-0012). Unverified transfers have no
goodput and are recorded as `NO`, not as a low number.

## Result: accuracy (EXP-023 part a)

The controller sees one run at `nsym = 32` and names the rung it believes maximises expected
payload. The sweep independently runs all seven rungs for ground truth. The controller never
sees the sweep.

| Channel | Brute-force optimum | Controller's pick | Rung distance |
|---|---|---|--:|
| clean | 8 @ 120.0 KB/s | **8** | **0** |
| mild | 8 @ 112.9 KB/s | **8** | **0** |
| F18-impaired | 24 @ 86.3 KB/s | **24** | **0** |
| severe | none — no rung verifies | 40 | not scoreable |

## Result: the cost of a wrong guess (EXP-023 part b)

Spread across the ladder on each channel. This is what a fixed `nsym` chosen for the wrong
channel costs, and therefore the value of ever closing the loop (OQ-013).

| Channel | Best | Worst | Cost of the worst rung |
|---|--:|--:|--:|
| clean | 120.0 KB/s | 60.0 KB/s | **50.0%** |
| mild | 112.9 KB/s | 37.5 KB/s | **66.8%** |
| F18-impaired | 86.3 KB/s | 5.9 KB/s | **93.2%** |

## Full ladder, F18-impaired channel

`--noise 26 --shot 0.9 --crosstalk 0.20 --occlusion 0.03 --drop 0.10`. Reproduces F18's channel
exactly, so the two experiments are directly comparable.

| `nsym` | Frames | Uncorrectable | Worst erasure load | Goodput | Verified |
|--:|--:|--:|--:|--:|:--|
| 8 | 1310 | 1052 | 9 | 5.9 KB/s | yes |
| 16 | 117 | 11 | 9 | 65.6 KB/s | yes |
| **24** | **89** | **0** | **9** | **86.3 KB/s** | yes |
| 32 | 89 | 0 | 9 | 86.3 KB/s | yes |
| 40 | 89 | 1 | 9 | 86.3 KB/s | yes |
| 48 | 112 | 0 | 9 | 68.6 KB/s | yes |
| 64 | 241 | 0 | 9 | 31.9 KB/s | yes |

Two things to read here:

1. **The worst erasure load is 9 at every rung.** That is H1's invariance, visible directly.
2. **Yet 11 frames fail at `nsym = 16`, where 9 ≤ 16.** Erasures alone therefore cannot explain
   the failures — undetected errors can, at two parity bytes each. The measured worst *budget*
   on this channel is 30 (mean 14, p95 19). This is F23, and it is why an erasure-only estimator
   recommended 16 and lost 24% of the available goodput.

The optimum is a **plateau** (24, 32, 40 all at 86.3 KB/s), not a peak. Consistent with EXP-013's
preference for broad optima, and it means a session-start guess landing anywhere in the middle of
the ladder is fine on this channel — the penalty is at the extremes.

## The severe channel, and why it is not a failure of the controller

`--noise 40 --shot 1.2 --crosstalk 0.28 --occlusion 0.06 --drop 0.15`. **No rung completes a
verified transfer** — every one hits the 40,000-frame cap with ~70% of frames uncorrectable, and
even `nsym = 64` fails. The worst erasure load is 18, far inside a 64-byte budget, so again the
damage is error-dominated: at 0.28 crosstalk the budget required exceeds 64.

**No amount of parity fixes this channel**, so there is no optimum and nothing for the controller
to be right or wrong about. What the run does demonstrate is the censoring limitation behaving as
documented: **28,497 of 40,000 observations censored**, which makes every rung above the
reference look equally perfect, and the controller duly picks the cheapest of those (40). The
tool prints the censored count and the resulting optimism warning rather than presenting a
clean-looking recommendation. See F23.

## Limitations

- **Uncalibrated channel model** (RISK-024). The decision rule is validated; the channels are not.
- **One run per cell, no spread reported.** BENCHMARK-METHODOLOGY requires median and range for
  performance reporting. These figures are *deterministic* given the seed, so a repeat is
  identical — but that makes them a single sample of channel randomness, not a distribution over
  it. The rung *ranking* is the claim; the individual KB/s values should not be quoted as
  system performance, and are not milestones (CONTRIBUTING rule 1).
- **The counterfactual is exact for a fixed damage pattern** — proven to 1e-12 in
  `LinkCounterfactual.OneRungPredictsTheWholeLadder`. End to end it is only near-exact, because
  changing `nsym` changes the symbol size and hence the rendered bytes, and the *error* component
  of the damage is weakly content-dependent (the erasure component is not, by F15's design). At
  `nsym = 16` the controller predicted P(decode) = 0.823 from telemetry at 32, against 0.906
  measured in that rung's own run — a different sample, not a systematic gap.
- **Predictions above the reference rung are optimistic** whenever the reference rung loses
  frames, by exactly the censored share.
- **Single seed.** A seed sweep would strengthen the accuracy claim and has not been run.

## What this changes

- C14's estimator and policy are implemented and tested (`core/src/link.cpp`, 25 tests).
- C14's **actuation** is blocked by the protocol, not by the controller (F20). OQ-037 proposes
  decoupling the fountain symbol size from frame capacity, which would also address F4's ~27%
  block-padding waste.
- OQ-013 now has a number: the cost of guessing wrong is 50–93% on these channels. Large enough
  to raise the value of ADP-04, not yet grounds to move it — that needs real captures.
