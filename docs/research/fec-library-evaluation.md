# FEC and fountain library evaluation

> **Status:** Draft — no selection made
> **Owner:** FEC subsystem
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0009, EXP-011, EXP-012, OQ-009, OQ-010, RISK-015, RISK-016

## Selection criteria

A dependency in the per-frame decode path is a long-term commitment. Popularity is not a
criterion. In priority order:

1. **Licence compatibility** — including patent position, not just the source licence.
2. **Soft-decision input support** — a hard-decision-only payload decoder discards the
   demodulator's most valuable output.
3. **Performance on ARM64** — must sustain the corrected bit rate within our CPU budget,
   with NEON where possible.
4. **Maintenance status** — last commit, issue responsiveness, release cadence.
5. **Portability** — must build for Android NDK *and* for desktop, because the simulator
   and recorded-frame harness run the same decoder (ADR-0010).
6. **Memory behaviour** — bounded, pre-allocatable, no per-frame heap churn.
7. **Integration risk** — API surface, build system, transitive dependencies.
8. **Testability** — deterministic behaviour, availability of test vectors.

## Candidates — fountain / erasure layer

| Library | Language | Licence | Soft input | Maintenance | Portability | Assessment |
|---|---|---|---|---|---|---|
| **libRaptorQ** (LucaFulchir) | C++11 | To verify (RT-06) | N/A (erasure code) | To verify | Good — C++11, no exotic deps | Technically the most direct fit for our C++ core. Blocked on licence verification and the RaptorQ patent question. |
| **raptorq** (cberner) | Rust | Apache-2.0 | N/A | Appears active | Would need FFI into the NDK core | Clean licence on the *code*; patent question is orthogonal and unresolved (repo issue #116 raises it). Rust-in-NDK adds build complexity. |
| **OpenRQ** | Java | To verify | N/A | To verify | **Poor** — JVM, wrong side of our JNI boundary | Not viable for the NDK core. Useful only as a reference implementation for test vectors. |
| **Own LT code** | C++ | Ours | N/A | Ours | Ideal | Fallback with a known cost: higher reception overhead than RaptorQ, but no licensing exposure and full control. Implementation is genuinely tractable. |
| **Own RLNC over GF(256)** | C++ | Ours | N/A | Ours | Ideal | Near-optimal overhead but O(k²) decode. Viable only with small per-block k. Worth measuring — our block sizes may be small enough. |

**Status: no selection.** The decision is gated on EXP-012 (overhead measurement) and
RT-07 (legal review). See the RaptorQ patent discussion in
[coding-theory.md](coding-theory.md#the-raptorq-licensing-problem-fact).

**Important:** the technically best choice (RaptorQ) is not obviously the available
choice. Planning should assume the fallback is realistic, because the fallback's cost —
a few percent additional reception overhead — is small relative to the uncertainty in
`Pc`. We should not distort the schedule around getting RaptorQ.

## Candidates — intra-frame FEC layer

| Library | Language | Licence | Soft input | Assessment |
|---|---|---|---|---|
| **Own LDPC** | C++ | Ours | **Yes** | Most likely outcome for the payload code. We need a specific code matched to our block length and error structure, plus NEON-optimised min-sum decoding. Off-the-shelf LDPC libraries are usually tied to a standard's code set (Wi-Fi, DVB-S2, 5G NR) whose parameters do not match our frame geometry. |
| **Standard-derived LDPC codes** (802.11n, DVB-S2, 5G NR base graphs) | — | Code definitions are public in the standards | Yes | Using a well-studied *code* while writing our own decoder is attractive — the code's properties are known and published. Check patent status per standard. |
| **Reed–Solomon** — e.g. `cm256`, `leopard`, or a small in-house GF(256) implementation | C/C++ | Varies (BSD-ish common) | No | Good fit for the **header** and for erasure-ish symbol-level work. Fast with SIMD. |
| **BCH** — small in-house implementation | C++ | Ours | No | Simple, adequate for short header codewords. |

**Provisional direction** `[HYP]`: in-house LDPC for payload (soft input, geometry-matched),
in-house or small-library RS/BCH for the header. Confirmed or rejected by EXP-011.

The bias toward in-house implementation here is deliberate and is a real cost — it is
justified only because (a) our block geometry is unusual, (b) we need soft input end to
end, (c) we need identical behaviour on-device and off-device, and (d) the decoder is on
the hot path so we need to control its memory and SIMD behaviour. If EXP-011 shows an
off-the-shelf code performs adequately, taking it is the better outcome and we should.

## Evaluation procedure (EXP-011 / EXP-012)

Both experiments run **entirely in the offline simulator** — no device required, so they
can begin as soon as Phase 1 exists.

1. Generate representative error patterns from the simulator's channel model: independent
   symbol errors, spatially clustered errors, full-frame erasures, mixed-frame partial
   erasures.
2. For each candidate intra-frame code, at several code rates, measure: post-decode frame
   error rate, decode time per frame on the target ARM64 core, and peak memory.
3. For each candidate fountain scheme, measure reception overhead versus loss rate, and
   decode time at representative block sizes.
4. Feed the measured `Rfec`, frame error rate and `Rfountain` back into
   `tools/perf_model/perf_model.py` and compare **end-to-end goodput**, not component
   metrics.
5. Record all raw output in `data/experiments/EXP-011/raw/` and `EXP-012/raw/`.

Step 4 is the point of the exercise. A code with better error performance but a decoder
too slow to keep up reduces goodput to zero — see RISK-015.

## Throughput budget

The decoder must sustain the corrected bit rate. From the performance model, the M2
optimistic scenario has a corrected rate of ~2.3 Mb/s; the M4 research scenario reaches
~4.9 Mb/s. Those are modest by codec standards, but they must be achieved:

- on a mobile core, sharing the SoC with camera capture, GPU sampling and rendering
- while thermally throttled (RISK-010)
- with bounded latency, so the fountain decoder is not the bottleneck at the transfer tail

`[OPEN]` OQ-014: what fraction of one big core can the FEC decoder use before it starts
competing with the capture path and reducing `Pc`? This is a systems question, not a
coding question, and it is easy to get wrong by optimising the decoder in isolation.

## Decision status

| Decision | Status | Gate |
|---|---|---|
| Intra-frame code family | **Proposed: LDPC payload + RS/BCH header** | EXP-011 |
| Fountain scheme | **Undecided** | EXP-012 + RT-07 legal review |
| Build vs. buy | **Leaning build**, with explicit willingness to buy if EXP-011 says so | EXP-011 |
