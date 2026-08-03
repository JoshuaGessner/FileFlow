# Research: communications and coding theory for the optical link

> **Status:** Draft
> **Owner:** FEC / modulation subsystems
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0009, MODULATION-SPEC.md, PROTOCOL-SPEC.md, EXP-011, EXP-012

## Channel framing

The screen-camera link is best modelled as a **spatial MIMO channel with erasures**, not
as a single serial bit pipe.

- Each logical cell is a subchannel. A 120×200 grid is 24,000 parallel subchannels per
  display state.
- The subchannels are **not independent**: neighbouring cells are coupled by defocus,
  lens MTF, display subpixel structure and camera sampling. This is spatial crosstalk,
  and it is the direct analogue of inter-symbol interference in a serial channel.
- The channel is **spatially non-stationary**: centre-of-screen cells have better SNR than
  corners (vignetting, focus falloff, perspective foreshortening, glare). A single global
  threshold is therefore wrong by construction — thresholds must be regional.
- The channel is **time-varying** at two scales: slowly (hand drift, exposure adaptation)
  and abruptly (a frame is lost entirely, or contains a rolling-shutter mixture).

This gives a two-tier error structure that drives the whole coding design:

| Tier | Failure mode | Right tool |
|---|---|---|
| Within a frame | Individual cells misread; errors cluster spatially | Intra-frame FEC with interleaving, soft input |
| Across frames | Entire display states lost, duplicated or unusable | Erasure coding across frames — a fountain code |

Treating both with one mechanism is the standard mistake. A code strong enough to survive
whole-frame loss wastes enormous capacity on frames that arrived fine.

## Capacity intuition (and its limits)

The temptation is to compute a Shannon capacity per cell and multiply by cell count.
This over-predicts badly, because:

- Crosstalk makes subchannels correlated; the effective number of independent
  subchannels is smaller than the cell count. `[HYP]`
- Cell SNR is not uniform across the screen.
- The dominant loss is not Gaussian noise but *frame-level events* — mixing, loss,
  occlusion, glare — which capacity-per-cell does not model at all.

Capacity framing is therefore used here as an **upper-bound sanity check**, not as a
design target. The first-order model in
[PERFORMANCE-MODEL.md](../specifications/PERFORMANCE-MODEL.md) is deliberately a
throughput-accounting model rather than an information-theoretic one, for this reason.

`[OPEN]` OQ-011: what is the effective number of independent spatial subchannels at each
candidate cell pitch? EXP-001 measures this indirectly via symbol error rate versus grid.

## Symbol constellations

| Mode | Alphabet | Notes |
|---|---|---|
| M0/M1 | {dark, bright} | Maximum Euclidean separation for a given brightness range. Most robust. |
| M2 | 4 luminance levels | Nominally 2 bits/cell, but levels are equally spaced in *linear* light while the camera response is non-linear and vendor tone curves are non-linear. Requires `TONEMAP_MODE = CONTRAST_CURVE` with a linear curve and per-region level estimation from pilots. |
| M3 | 4 colours | **Do not assume R/G/B/W.** Constellation choice should be derived experimentally, because: (a) `YUV_420_888` chroma is 2×2 subsampled, so colour cells bleed more than luminance cells; (b) OLED subpixel layouts are not uniform across colours; (c) camera colour filter arrays and white-balance handling vary by vendor. A constellation maximising *decoded* separation on the actual devices may look nothing like the obvious choice. See EXP-014. |

Note the honest possibility, supported by a secondary report about ShiftCode, that
**four-colour modulation may lose to two-level greyscale** once error rates are accounted
for. `[LIT, unverified]` This is why M3 is gated behind measurement rather than scheduled
as an obvious win.

## Soft decisions

The demodulator must emit **log-likelihood ratios**, not hard bits.

Rationale: a cell sampled near the decision boundary and a cell sampled far from it carry
very different information, and a hard-decision FEC decoder throws that away. The standard
result is a coding gain of roughly 2 dB for soft over hard decision decoding on a Gaussian
channel — the exact figure does not transfer to our channel, but the direction certainly
does.

Our LLR sources:
- Distance from the regional threshold, normalised by local noise estimate
- Pilot-derived confidence for the cell's region
- Blur/defocus estimate for the cell's region
- Frame-phase confidence (is this cell in a rolling-shutter transition band?)

The last one is distinctive: cells in the transition band of a mixed frame should be
marked as **erasures with near-zero LLR**, not decoded as noisy bits. Erasure information
is worth far more to the decoder than a wrong guess.

`[HYP]` The soft-confidence representation is 8-bit quantised LLR. Whether 8 bits is
sufficient, and whether the quantisation loss matters, is EXP-011's secondary question.

## Interleaving

Errors in this channel are **spatially clustered** — a glare spot, a fingerprint smudge,
a focus-soft corner or an occlusion damages a contiguous region. A code applied to cells
in raster order sees a long burst and fails.

The frame layout therefore interleaves codeword symbols across the grid so that spatially
adjacent cells belong to *different* codewords. A simple approach is a fixed
pseudo-random permutation seeded by the modulation profile; a structured approach uses a
block interleaver sized to the expected damage radius.

`[OPEN]` OQ-012: what is the expected damage-region size distribution? Measured in
Phase 2 channel characterisation.

## Candidate intra-frame codes

| Code | Soft input | Burst tolerance | Speed on ARM | Assessment for FileFlow |
|---|---|---|---|---|
| **Reed–Solomon** | Hard (soft variants exist but are complex) | Excellent (symbol-oriented) | Very fast with SIMD; well-understood | Strong candidate for the **header**, where we want symbol-level burst tolerance and simplicity. Poor fit for payload because it wastes our soft information. |
| **BCH** | Hard | Moderate | Fast | Reasonable header option. Binary, short blocks. |
| **LDPC** | **Yes — native** | Good with interleaving | Good; iterative but parallelisable, SIMD-friendly | **Primary payload candidate.** Soft-input by design, excellent performance near capacity at the block lengths we have (thousands of bits per frame). Decoder cost is the main risk. |
| **Polar** | Yes (SC/SCL) | Moderate | List decoding is expensive | Interesting but decoder complexity and less mature open tooling. Secondary candidate. |
| **Non-binary LDPC/RS** | Yes | Excellent for M2/M3 (symbol = 2 bits) | Slower | Attractive for multilevel modes where a symbol error corrupts 2 bits together. Worth evaluating in EXP-011 once M2 exists. |
| **Convolutional / turbo** | Yes | Moderate | Viterbi is cheap at short constraint length | Fallback. Largely superseded by LDPC for this block-length regime. |

**Provisional direction** `[HYP]`: LDPC for the payload (soft input is the whole point),
Reed–Solomon or BCH for the header (short, must decode from a hard-ish read, must be
extremely reliable). Decided by EXP-011; recorded in ADR-0009 as *Proposed*.

## Header protection

The header is the single point of failure: lose it and the entire frame is unusable even
if every payload cell decoded perfectly. It therefore gets:

- The largest cells in the frame (better SNR per symbol)
- Placement in the highest-SNR screen region (centre, away from corners)
- A very low code rate (heavy redundancy)
- Full replication across spatially separated copies
- Its own CRC

`H` (header success probability) appears directly in the goodput model, and the
sensitivity analysis shows goodput is materially sensitive to it. Over-protecting the
header is cheap insurance: it is a tiny fraction of total cells.

## Fountain layer

**Why rateless.** With frame loss as a normal event, an ARQ scheme would need a reverse
channel we do not have (NG9) and would stall on round trips. A fountain code makes loss a
non-event: the transmitter emits repair symbols indefinitely and the receiver signals
completion by... having completed. This is the single cleanest fit between a coding
technique and a system constraint anywhere in this design.

**Systematic transmission order matters.** Send source symbols first, repair symbols
after. On a clean channel the receiver decodes with near-zero decoding work; only a
degraded channel pays the decoding cost.

| Scheme | Overhead | Complexity | Licensing | Assessment |
|---|---|---|---|---|
| **LT codes** | Higher, variable | Low | Clear (patents expired/expiring) | Simple, implementable in-house, predictable. Fallback with a known cost. |
| **Raptor (RFC 5053)** | Low, ~2% class | Moderate | Encumbered | Superseded by RaptorQ. |
| **RaptorQ (RFC 6330)** | **Very low** — near-optimal | Higher | **Requires legal review — see below** | Best technical choice. Licensing is the blocker, not the engineering. |
| **Online codes** | Moderate | Low-moderate | Generally clear | Under-explored alternative worth a look. |
| **Random linear network coding over GF(256)** | Near-optimal overhead | O(k²) decode — too slow at large k | Clear | Viable only for small block sizes; could work if we chunk the file. Worth evaluating since our per-block k may be modest. |

### The RaptorQ licensing problem `[FACT]`

Qualcomm's IETF IPR declaration #1958 for RFC 6330 makes a two-tier commitment. The
non-assert tier applies to devices that implement RFC 6330 and **do not implement any
wireless wide-area standard**. The other tier — licence available at Qualcomm's standard
royalty rate — is illustrated with UMTS-compatible handsets.

FileFlow targets smartphones. Smartphones implement wireless wide-area standards. On a
plain reading, our target devices fall in the licensed tier rather than the non-assert
tier, **even though FileFlow itself uses no radio at all**.

We are not qualified to resolve this, and this document does not attempt to. It is flagged
so the decision is made deliberately: **RaptorQ must not become a dependency before legal
review** (RT-07, OQ-010, RISK-016). The mitigation if review is unfavourable is to use LT
or online codes and accept a few percent more reception overhead — which the model shows
costs far less goodput than most people assume.

## Rate adaptation

The adaptive link controller selects modulation profile and code rate from measured
channel state. Inputs: symbol error rate, header failure rate, clean-frame ratio, pilot
SNR, blur estimate. Because the variables are coupled (see
[PERFORMANCE-PHILOSOPHY.md](../vision/PERFORMANCE-PHILOSOPHY.md)), adaptation must be
validated on **end-to-end goodput**, not on symbol error rate. A controller that minimises
SER by dropping to the most conservative profile is a bad controller.

`[OPEN]` OQ-013: with a one-way link, how does the transmitter learn the channel state at
all? Options: (a) the receiver never tells it, and the transmitter cycles through profiles
in a known pattern so the receiver uses whichever works; (b) reverse optical control
(Phase 9); (c) the receiver's own screen shows a code the sender's camera reads. Option
(a) is the only one available in the initial one-way design and has a real cost. This is
an under-specified area of the current architecture and needs design work.
