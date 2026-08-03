# ADR-0006 — Full-screen spatial symbol matrix rather than QR codes

> **Status:** Proposed
> **Date:** 2026-08-02
> **Owner:** Architecture
> **Related:** ADR-0007, ADR-0008, EXP-001, EXP-002, EXP-017, G4

## Context

The incumbent approach to screen-to-camera file transfer is a stream of QR codes. QR was
designed for one-shot scanning of small payloads by cheap scanners under adversarial
conditions — printed on boxes, at angles, partially damaged, under any lighting. Every
design choice reflects that, and most of them are wrong for a cooperative,
phone-to-phone, high-rate link.

## Decision

Replace QR entirely with a **purpose-built full-screen spatial symbol matrix**: a dense
grid of independently modulated cells, with asymmetric corner markers, distributed pilot
cells, timing tracks, a heavily-protected header, and a persistent tracked geometry.

## Alternatives

1. **Optimised QR streaming** (the FareQR direction). Rejected as the architecture, but
   **we will build a calibrated QR-stream baseline in Phase 2** (EXP-002) — we need a
   measured comparison, not an assumed one. G4 depends on this.
2. **Multiple QR codes tiled on screen.** Gains parallelism while keeping QR's per-code
   overhead and per-frame redetection cost. Rejected as strictly dominated.
3. **Hybrid — QR for handshake, dense grid for payload.** Genuinely reasonable: QR's
   robustness is well-suited to initial acquisition. Rejected because our corner markers
   plus protected header must work standalone anyway (reacquisition after occlusion cannot
   depend on a special frame), so QR would be redundant weight.

## Consequences

**Positive.** Nearly all screen area carries payload. Detection cost is paid once, not per
frame. Soft information is preserved to the FEC decoder. Code rate is adaptive rather than
fixed at QR's discrete levels. Frame layout is tunable to the measured channel.

**Negative.** We must build everything QR libraries give for free: detection, tracking,
rectification, sampling, error correction, framing. Substantially more code, and more
places to be wrong. No interoperability with anything.

**Neutral.** The result is unrecognisable as a barcode, which is fine (NG4 already
abandons imperceptibility).

## Evidence

**Supporting:**

- ChromaCode's own cell-size sweep shows goodput rising **28 → 137 kbps** as cell size
  shrank from 26×17 to 8×7 — spatial density does buy rate, up to a limit. `[LIT]`
- Visual MIMO's framing of screen-camera links as spatial MIMO channels is the theoretical
  basis for treating cells as parallel subchannels. **Primary papers now read** (RT-03 closed
  2026-08-02, [VISUALMIMO-CISS11] and two others, full text). The analysis states that
  multiplexing gain requires transmit elements to remain **separable at the receiver**, and
  that **perspective distortion is the dominant limit** — the role multipath fading plays in
  RF MIMO. That is the principled statement of the density cliff EXP-001 must locate, and it
  justifies treating spatial crosstalk rather than noise as the binding constraint. `[LIT]`
- RDCode's reported packet-frame-block structure with intra-block, inter-block and
  inter-frame error correction suggests other groups converged on layered coding over
  dense grids. `[LIT — secondary description only, unverified]`

**Complicating:**

- The same ChromaCode sweep shows goodput **collapsing to 58 kbps** when cells shrink
  further. `[LIT]` Density has a cliff, and its location is channel-specific. We do not
  know where ours is — hence EXP-001 rather than a chosen grid.
- A secondary summary of ShiftCode reports its **greyscale two-colour mode outperforming
  its four-colour mode**. `[LIT — unverified]` If true, this is evidence that added symbol
  complexity does not automatically pay, which tempers our M2/M3 expectations though not
  this ADR directly.
- **We have no primary-source numbers for any visible-branch system.** The direct lineage
  of this decision (COBRA → RainBar → RDCode → ShiftCode) is unread. RT-01.
- **The "tracking is cheaper" claim is now measured, and it depends entirely on the search
  region** (finding F13, 2026-08-02). The first implementation dilated the previous quad's
  bounding box and saved **nothing at all** once the screen filled 64% of the frame — a
  configuration we expect to operate in, since filling the frame maximises pixels per cell.
  (An earlier version of this bullet justified that with a "detection needs ~6 px/cell" floor
  from finding F14. **F14 has been retracted** — that floor was an artifact of the F15 bug and
  the corrected boundary turned out to be a `min_area_fraction` config threshold, not optics.
  Tight framing is still desirable for density; it is not *forced* by any limit we have
  demonstrated.)
  Scanning a **boundary annulus** instead fixed it, since the corner extremes lie on the
  boundary and the interior scan was pure waste. Measured pixels examined, tracked ÷ full
  acquisition: **0.554** at 64% screen fill, **0.185** in the headline case, ~0.075 at 7.5%
  fill. `[HYP — simulated]`
  The claim therefore holds, but with an important caveat now on the record: **the saving is
  not intrinsic to tracking**, it is a property of how tightly the search region hugs the
  boundary, and the obvious implementation threw it away precisely where it mattered most.
  The other benefits of tracking — temporal consistency of the homography, fewer chances to
  lock onto the wrong bright object — remain separate claims that still need their own
  evidence and should not be folded into the cost argument.
- **End-to-end A/B through `ffsim --image-path`, identical frames and channel** `[HYP — simulated]`:

  | | per-frame acquisition | tracked |
  |---|--:|--:|
  | geometry pixels/frame | 2,400,000 (1.000) | **1,547,138 (0.645)** |
  | decoder throughput | 399 fps | **770 fps** |
  | frames to complete | 64 | 64 |
  | header success `H` | 1.0000 | 1.0000 |
  | verified | yes | yes |

  **1.93× decoder throughput at identical decode quality.** Reproduce with
  `ffsim --payload 16384 --image-path --image-size 1200 2000 [--no-tracking]`.
- **Tracking buys speed, NOT accuracy.** An earlier reading of this same A/B appeared to show
  tracking being far *more reliable* (447 frames and `H`=0.33 for per-frame acquisition versus
  64 frames and `H`=1.0 tracked). That gap was entirely a detector bug in which localisation
  depended on payload content (finding F15), not a property of tracking. With the bug fixed
  both paths decode identically. Recorded because the wrong conclusion was attractive and
  would have become a load-bearing belief about why this ADR's central bet works.

## Risks

| Risk | Mitigation |
|---|---|
| Dense grids suffer moiré (RISK-005) | Grid sweep (EXP-001) finds the practical density limit; cell interior-margin sampling |
| Tracking is not actually cheaper or more accurate than redetection (RISK-019 adjacent) | EXP-017 measures this directly — it is a load-bearing hypothesis |
| We rebuild QR badly | Golden vectors, simulator-first development, and a measured QR baseline to compare against |
| The dense-grid thesis rests on unread literature | RT-01, RT-03 |

## Validation plan

- **EXP-001** — maximum resolvable binary grid per device pair and distance. Finds our
  density cliff.
- **EXP-002** — calibrated dynamic-QR baseline on the same hardware. **This ADR is not
  validated until we have beaten a real QR implementation by a real margin**, measured in
  goodput, on the same devices, in the same conditions.
- **EXP-017** — tracking versus per-frame detection: cost and accuracy.

If EXP-002 shows a well-tuned QR stream within, say, 2× of our system, the added
complexity of this decision is not justified and the ADR should be reconsidered.
