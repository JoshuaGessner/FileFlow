# Research plan

> **Status:** Draft
> **Owner:** Research
> **Last reviewed:** 2026-08-02
> **Related:** BIBLIOGRAPHY.md, EXPERIMENT-REGISTRY.md, OPEN-QUESTIONS.md

## Purpose

Phase 0 exists to make sure we are not rediscovering solved problems, and not building on
platform assumptions that turn out to be false. It ends when the documentation foundation
is complete and the initial ADRs are recorded — not when the literature is exhausted.

Research continues through later phases, but from Phase 1 onward it is **driven by
specific open questions**, not by general survey.

## Method

1. **Primary sources only for evidence.** Platform behaviour comes from official
   documentation or AOSP source. Prior-art claims come from the papers themselves.
   Search-result summaries are pointers, never evidence. Enforced by the `Access` field
   in the bibliography.
2. **Every claim is tagged** `[FACT]` / `[LIT]` / `[HYP]` / `[OPEN]`.
3. **Every unresolved question becomes an open-question entry**, and every question that
   can be answered by measurement becomes an experiment entry.
4. **Findings that contradict the prescribed architecture are surfaced, not buried.**
   The high-speed-session constraint is the first example; there will be others.

## Research areas and status

| Area | Status | Primary sources obtained | Key gaps |
|---|---|---|---|
| **Android display pipeline** | Substantially covered | NDK Choreographer reference; frame-rate docs; AOSP Choreographer.java | Per-frame presentation *verification* mechanism unproven (OQ-004) |
| **Android camera pipeline** | Substantially covered; one major finding | AOSP `CameraDevice.java` javadoc | Real achievable YUV frame rates per device (OQ-001); GPU-path frame distinctness at 120 fps (OQ-002) |
| **Screen-camera prior art — imperceptible branch** | Covered | ChromaCode (full text), Nguyen et al. INFOCOM 2016 (full text) | None critical; contrasting category |
| **Screen-camera prior art — visible branch** | **Weak** | None | **All of it.** COBRA, RainBar, RDCode, ShiftCode, FareQR unread (RT-01) |
| **Visual MIMO / capacity framing** | **Covered** (2026-08-02) | 3 papers, full text, free from WINLAB | Quantitative capacity curves not extracted; framing is cited (RT-03 closed) |
| **Communications theory** | Framework established | Standard textbook material | Choice of code family pending EXP-011 |
| **Fountain coding** | Partially covered | RFC 6330 available; Qualcomm IPR declaration read | Licensing position needs legal review (OQ-010); library quality unassessed (RT-06) |
| **Computer vision** | Framework established | Standard methods | Subpixel sampling accuracy under our conditions unmeasured (EXP-015) |
| **Security** | Framework established | — | Threat model drafted; needs review once protocol stabilises |

## Priority ordering

The research gaps are **not** equally urgent. Ordered by how much they would change our
plans if the answer surprises us:

1. **ShiftCode + RDCode primary text** (RT-01). These two systems attack the exact problems
   our M4 and FEC layers attack. A secondary summary suggests ShiftCode's greyscale mode
   *outperformed* its four-colour mode — if that replicates, it is evidence against M3 and
   would reorder our modulation roadmap.
2. **Achievable camera frame rates with CPU-accessible YUV** (OQ-001, EXP-007). Determines
   whether the primary receive path is CPU or GPU, which is a large implementation fork.
3. ~~**Visual MIMO capacity framing** (RT-03).~~ **DONE 2026-08-02** — three papers read
   in full, free from the authors' lab page. ADR-0006's foundation is now cited rather than
   assumed. Key takeaway: perspective distortion is the dominant limit on multiplexing gain,
   which is the theoretical statement of the density cliff EXP-001 must locate.
4. **RaptorQ licensing** (OQ-010). Determines whether our fountain layer can use the best
   available code or needs an alternative. **Raised in priority** by Phase 1 finding F4:
   measured LT reception overhead of 0.54 under loss is far worse than RaptorQ's ~2% class.
5. Everything else.

## What we will *not* research further in Phase 0

- Imperceptible/hidden systems beyond the two papers already read. The category is
  understood, quantified and rejected (NG4). Further reading has low decision value.
- iOS platform behaviour (NG1).
- Browser capabilities (NG2). The reasons for rejection are structural, not marginal.
- ML-based decoding (NG6). Revisit only once the capture harness has produced a labelled
  dataset, which is a Phase 2+ artefact.

## Deliverables

| Deliverable | Location | Status |
|---|---|---|
| Annotated bibliography | [BIBLIOGRAPHY.md](BIBLIOGRAPHY.md) | Draft, growing |
| Prior-art comparison matrix | [PRIOR-ART-MATRIX.md](PRIOR-ART-MATRIX.md) | Draft, gaps marked |
| Android display research | [android-display-pipeline.md](android-display-pipeline.md) | Draft |
| Android camera research | [android-camera-pipeline.md](android-camera-pipeline.md) | Draft |
| Coding theory notes | [coding-theory.md](coding-theory.md) | Draft |
| FEC/fountain library evaluation | [fec-library-evaluation.md](fec-library-evaluation.md) | Draft |
| Computer vision notes | [computer-vision.md](computer-vision.md) | Draft |

## Research tasks

| ID | Task | Priority | Blocks |
|---|---|---|---|
| RT-01 | Obtain primary text for COBRA, RainBar, RDCode, ShiftCode, FareQR; populate matrix | High | ADR-0006 evidence, M3 planning |
| RT-02 | Determine which visible-branch papers report goodput vs raw rate | High | Prior-art comparability |
| ~~RT-03~~ | ~~Read Visual MIMO capacity papers~~ | **DONE 2026-08-02** | ADR-0006 now evidenced |
| RT-09 | Email the ShiftCode / RDCode authors for PDFs — the realistic route past the ACM paywall (see [PAPER-ACCESS.md](PAPER-ACCESS.md)) | High | RT-01 |
| RT-04 | Assess source-code availability for prior systems | Medium | Possible reproducible baseline |
| RT-05 | Measure our own dynamic-QR baseline (EXP-002) | High | G4 |
| RT-06 | Evaluate libRaptorQ / raptorq-rust quality, maintenance, licence | Medium | OQ-010 |
| RT-07 | Legal review of RaptorQ patent position for smartphone deployment | Medium | OQ-010 |
| RT-08 | Survey open LDPC/RS implementations suitable for NDK + SIMD | Medium | EXP-011 |
