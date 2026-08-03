# ADR-0012 — Verified payload goodput as the primary performance metric

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Project lead
> **Related:** PERFORMANCE-PHILOSOPHY.md, BENCHMARK-METHODOLOGY.md, RISK-019

## Context

Optical link work offers many numbers that feel like performance and are not: refresh rate,
capture rate, symbol rate, raw bit rate, corrected bit rate. Optimising any of them can
*reduce* the quantity users experience. The screen-camera literature contains this
confusion in abundance, which makes cross-paper comparison unreliable and makes it easy to
adopt a misleading target.

## Decision

**Verified payload goodput is the single primary metric.**

```
goodput = verified original file payload bits / total transfer time
```

- **verified** — the reconstructed file's hash matches the source.
- **original** — pre-compression payload. Compression gains are never counted as channel
  throughput (NG8).
- **total transfer time** — wall clock including acquisition, negotiation, the fountain
  tail and final verification. Not just the steady-state window.

Every performance statement must name which of the seven metrics it refers to. An
unlabelled number is a documentation defect.

## Alternatives

1. **Raw optical bit rate.** Rejected — the headline failure mode this ADR exists to
   prevent.
2. **Corrected decoder bit rate.** Better, but still excludes fountain overhead,
   acquisition time and the completion tail.
3. **Symbol error rate.** A useful diagnostic, a terrible objective — minimised by
   retreating to the most conservative profile, which minimises goodput too.
4. **Time to transfer a standard file.** Equivalent to goodput for a fixed file size, and
   arguably more intuitive. Adopted as a *presentation* format for benchmarks; goodput
   remains the canonical metric because it is size-independent.

## Consequences

**Positive.** Optimisation effort is directed at what users experience. Cross-experiment
comparison is meaningful. Prevents the classic failure of celebrating a raw-rate increase
that error correction consumes.

**Negative.** Goodput is expensive to measure — it requires a complete verified transfer,
so it is a slower signal than component metrics. Mitigated by tracking component metrics as
*diagnostics* while gating decisions on goodput.

**Neutral.** Component metrics remain valuable for debugging; they simply do not decide
anything on their own.

## Evidence

The strongest evidence is ChromaCode's own measurements (MobiCom 2018): `[LIT]`

- Raw throughput **777 kbps** versus data goodput **120 kbps** in the same experiment —
  a **6.5× gap** within one system.
- Under degrading conditions, raw rate fell **697 → 112 kbps** (~6×) while goodput fell
  **56 → 0.47 kbps** (~119×).

The second figure is the decisive one. Raw rate degraded gently while goodput collapsed.
A team monitoring raw rate would have seen a system in mild difficulty; a team monitoring
goodput would have seen a system that had effectively stopped working. **Raw rate is not
merely uninformative — it is actively misleading about system health.**

The authors themselves note that ultimate goodput is much smaller than raw throughput.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| RISK-019: project optimises raw rate instead of useful throughput | Medium | **High** | This ADR; the six-rate discipline; benchmark methodology requires labelling |
| Goodput measurement is slow, tempting proxy use | **High** | Medium | Component metrics allowed as diagnostics, never as acceptance gates |
| Goodput varies so much across conditions that it is hard to act on | Medium | Medium | Report median and range per benchmark category, never a best run |

## Validation plan

Not an empirical decision. Compliance is enforced by review:

- Benchmark reports that fail to label their metric are rejected.
- Optimisation changes are accepted on measured end-to-end goodput, not component metrics.
- Reported figures include the benchmark category, conditions, run count and spread —
  **best-run numbers are never reported as system performance.**
