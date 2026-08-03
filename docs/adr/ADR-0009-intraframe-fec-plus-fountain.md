# ADR-0009 — Intra-frame FEC plus cross-frame fountain coding

> **Status:** Proposed
> **Date:** 2026-08-02
> **Owner:** FEC subsystem
> **Related:** coding-theory.md, fec-library-evaluation.md, EXP-011, EXP-012, RISK-015, RISK-016

## Context

The channel has a two-tier error structure:

- **Within a frame:** individual cells misread, with errors clustered spatially (glare,
  smudge, focus falloff, occlusion).
- **Across frames:** entire display states lost, duplicated, or unusable (mixed frames,
  tracking loss, camera drops, motion).

These have different statistics and want different tools. Using one mechanism for both is
the standard mistake: a code strong enough to survive whole-frame loss wastes enormous
capacity on the frames that arrived fine.

There is also no reverse channel (NG9), so retransmission-based recovery is unavailable.

## Decision

**Two coding layers:**

1. **Intra-frame FEC** — soft-input error correction within each frame's payload, with
   spatial interleaving so clustered damage spreads across codewords. Uncorrectable frames
   are reported as **erasures**, not dropped silently.
2. **Cross-frame fountain coding** — a rateless erasure code across frames. Systematic:
   source symbols transmitted first, repair symbols after. Transfer completes when the
   receiver has decoded, not after a fixed transmission.

**Provisional code choices** `[HYP]`: LDPC for the payload (soft input is the point),
Reed–Solomon or BCH for the header (short, must be extremely reliable). Fountain scheme
undecided.

## Alternatives

1. **Single strong FEC layer, no fountain.** Rejected: cannot recover a fully lost frame
   without spending its redundancy budget on every frame.
2. **ARQ with a reverse optical channel.** Rejected for the initial system — requires
   bidirectional operation (NG9), adds round-trip latency, and the reverse channel is
   itself a research problem (Phase 9).
3. **Fountain only, no intra-frame FEC.** Rejected: a single bad cell would erase an entire
   frame, wasting tens of thousands of good cells. Catastrophically inefficient.
4. **Fixed-rate block code across frames** (e.g. RS over frame index). Simpler than a
   fountain code, but requires knowing the loss rate in advance — which varies during a
   transfer as the user's hands move.

## Consequences

**Positive.** Frame loss becomes a non-event. No reverse channel needed. Duplicate and
out-of-order frames tolerated by construction. Code rate adaptable per layer independently.
Systematic ordering means a clean channel pays almost no decoding cost.

**Negative.** Two coding layers to implement, tune and test. The fountain decoder is the
largest memory allocation in the system and must be bounded against hostile parameters.
Decoder throughput is a real risk (RISK-015). Fountain licensing is unresolved (RISK-016).

**Neutral.** The layer split maps cleanly onto the protocol stack and onto independent
test vectors.

## Evidence

**Supporting:**

- The two-tier error structure is a direct consequence of the channel and is not in doubt.
- RDCode reportedly uses error correction at three levels — intra-block, inter-block and
  inter-frame — which is structurally the same insight. `[LIT — secondary description
  only, unverified]` If confirmed by primary reading (RT-01), this is meaningful
  independent support.
- Fountain codes are the standard solution for one-way erasure channels without feedback;
  RFC 6330 exists precisely for object delivery over such channels. `[FACT]`
- ChromaCode's data showing goodput collapsing ~119× while raw rate fell only ~6× `[LIT]`
  is evidence that **frame-level and burst events dominate**, which is exactly what the
  fountain layer exists to absorb.

**Unresolved:**

- Which intra-frame code family — EXP-011.
- Which fountain scheme, and whether the best one is legally available — EXP-012, RT-07.

> **Measured 2026-08-02 (findings F18, F19).**
>
> **The intra-frame half was never implemented** until now — only the fountain half existed, so
> a frame with a few unreadable cells was discarded whole. On an impaired channel the transfer
> **never completed** without it; with RS(255, 223) interleaved across 10 codewords it completes
> in 89 frames at 86.3 KB/s. This ADR's central pairing is now real rather than planned.
>
> **Erasure decoding is where the value is.** RS corrects `nsym` erasures but only `nsym/2`
> errors, and the receiver already knows which cells it could not read. Supplying those
> positions doubles the correction budget for free, and it is the reason the layer works.
>
> **Soft-decision input is NOT where the value is, for M0.** The LLR magnitude was measured
> saturated (100% of cells in one band at every noise level — a real quantisation bug, now
> fixed), and even corrected, M0 symbol error rate is 0.00000 up to noise amplitude 120.
> Failures are structural, not decisional. Erasure-marking from confidence never beat a
> threshold of zero. Expect this to change for M2, where levels sit at one third the spacing.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| RISK-015: FEC decoder too slow on mobile, under thermal throttle | Medium | High | EXP-011 measures ARM64 decode time; adaptive code rate; GPU decode as a last resort |
| RISK-016: RaptorQ patent position unfavourable for smartphone deployment | **Medium** | Medium | LT or online codes as fallback, at a few percent overhead cost; legal review (RT-07) before any dependency |
| Fountain decoder memory blow-up on hostile parameters | Medium | High | Hard bounds validated **before** allocation (INPUT-VALIDATION.md) |
| Miscorrection produces wrong data that passes the code's checks | Low | **High** | CRC above the FEC layer, plus end-to-end file hash |

### The RaptorQ licensing caveat

Qualcomm's IETF IPR declaration #1958 for RFC 6330 offers a non-assert commitment for
devices that implement RFC 6330 and **do not implement any wireless wide-area standard**;
devices that do (the declaration's example is UMTS handsets) fall under a
licence-at-standard-rate tier. `[FACT]`

FileFlow targets smartphones, which implement wireless wide-area standards — even though
FileFlow itself uses no radio. On a plain reading our target devices fall in the licensed
tier. **We are not qualified to resolve this and this document does not attempt to.**
It is flagged so that RaptorQ does not become a dependency by default. See RT-07.

## Validation plan

- **EXP-011** — intra-frame code comparison: post-decode frame error rate, ARM64 decode
  time, memory, at several code rates, against simulator-generated error patterns
  including clustered damage. Selection made on **end-to-end goodput**, not on error rate.
- **EXP-012** — fountain reception overhead versus loss rate, and decode time at
  representative block sizes.
- Both run entirely in the offline simulator, so they can begin as soon as Phase 1 exists —
  **no device required**. They should be scheduled early for that reason.
