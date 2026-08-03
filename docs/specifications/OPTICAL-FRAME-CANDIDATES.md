# Optical frame candidate designs

> **Status:** Draft — **three candidates, no winner declared**
> **Owner:** Modulation / frame design
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0006, MODULATION-SPEC.md, PROTOCOL-SPEC.md, EXP-001, EXP-013

## Required elements

Every candidate layout must contain all of these. The candidates differ in *how* they
arrange and size them, not in whether they exist.

| Element | Purpose |
|---|---|
| Four asymmetric corner markers | **Orientation only** — see the implementation note below |
| Persistent screen boundary | Cheap edge tracking, subpixel refinement |
| Timing tracks | Grid registration and frame-phase reference |
| Frame-phase indicator | Lets the receiver identify rolling-shutter mixtures |
| Luminance pilot cells | Photometric normalisation, thresholds, SNR estimation |
| Colour pilot cells | Reserved in **all** layouts, unused until M3 |
| Robust binary header | Sequence, profile, session, FEC/fountain metadata, CRC |
| Dense payload region | The actual data |
| Guard cells | Crosstalk isolation between regions |
| CRC | Detects FEC miscorrection |

## Two hard constraints on every layout

These follow from Phase 0 platform research and are **not negotiable**:

1. **The frame must be self-describing.** Presentation confirmation may be unreliable
   (OQ-004) and camera timestamps may be uncorrelated with display time on devices
   reporting `SENSOR_INFO_TIMESTAMP_SOURCE = UNKNOWN`. `[FACT]` The receiver must
   determine *which display state it is looking at* from the frame content alone — hence
   a sequence number and phase indicator in **every** frame.

> **Implementation note, 2026-08-02 (findings F9/F10).** Building the detector changed how
> two of these elements are used, and the table above now reflects the outcome.
>
> **Localisation and orientation were split.** They are different problems with different
> best signals, and asking the corner markers to do both was making both worse:
>
> - **Localisation → the persistent boundary ring.** Implemented as a 1-cell always-bright
>   perimeter (`CellRole::kBoundary`), ~2.7% of cells at 120×200. Four long straight edges
>   can be fitted as *lines* and intersected; every pixel along an edge constrains the fit,
>   so corners land far more precisely than any small-fiducial centroid — and small fiducials
>   are exactly what degrades first with distance and defocus. This element was **specified
>   here but never implemented in Candidate B** until now (F10). Measured worst cell-centre
>   error against ground truth: **< 0.25 cells**.
>
> - **Orientation → the corner markers, redesigned as a pure 4-way code.** The original
>   corner-specific *notch* gave a minimum pairwise Hamming distance of **1 cell out of 36**
>   (worst-case rotation margin 2.8%), which a single noisy cell could flip — orientation was
>   a coin toss (F9). Freed from also having to be a detection target, the markers now encode
>   a 2-bit corner ID across the whole inner block: **minimum distance 8/36, margin 22.2%**.
>
> Consequence for the candidate comparison below: **the boundary ring is common to all three
> layouts** and is not a discriminator between them. What still discriminates is pilot
> placement, which is what EXP-013 must actually measure.

2. **Markers must appear in every frame.** Reacquisition after occlusion or tracking loss
   cannot wait for a periodic sync frame; time spent searching is time at zero goodput.

## Candidate A — "Border frame"

Markers and pilots confined to a border region; the entire interior is payload.

```
┌─────────────────────────────────────┐
│ M1 ══ timing track ══════════ M2    │   M1..M4 asymmetric corner markers
│ ║                             ║     │   ═ ║  timing tracks
│ ║   ┌───────────────────┐     ║     │
│ ║   │                   │     ║     │
│ ║   │   PAYLOAD         │     ║     │   Header: replicated blocks
│ ║   │   (dense cells)   │     ║     │   in the border, top and bottom
│ ║   │                   │     ║     │
│ ║   └───────────────────┘     ║     │
│ ║                             ║     │
│ M4 ══ timing track ══════════ M3    │
└─────────────────────────────────────┘
```

**Overhead estimate `O` ≈ 0.10–0.14** (border width dependent).

| Pros | Cons |
|---|---|
| Lowest overhead — maximum payload area | **Pilots only at the periphery** — the illumination field must be *extrapolated* into the centre, which is exactly where extrapolation is least reliable |
| Simple to generate and index | Vignetting and focus falloff are worst at the border, so pilots sit in the lowest-SNR region |
| Payload region is contiguous — good for SIMD and shader access patterns | Header in the border shares that low-SNR region — bad for `H` |
| Clean separation of function | A single edge occlusion (a finger, a case) can remove markers *and* pilots *and* header together |

**Assessment:** best raw payload fraction, worst photometric and robustness properties.
The correlated-failure mode in the last row is the serious objection.

## Candidate B — "Distributed pilot lattice"

Pilots and phase indicators distributed on a regular lattice throughout the payload area.

```
┌─────────────────────────────────────┐
│ M1 ══════════════════════════ M2    │
│ ║ ·  ·  ·  ·  ·  ·  ·  ·  ·  · ║    │   ·  pilot cells on a lattice
│ ║   ▓▓▓▓▓▓▓ HEADER ▓▓▓▓▓▓▓     ║    │      (e.g. every 16th cell)
│ ║ ·  ·  ·  ·  ·  ·  ·  ·  ·  · ║    │
│ ║      PAYLOAD  ·   PAYLOAD    ║    │   Header: centre, largest cells,
│ ║ ·  ·  ·  ·  ·  ·  ·  ·  ·  · ║    │   highest-SNR region
│ ║      PAYLOAD  ·   PAYLOAD    ║    │
│ ║ ·  ·  ·  ·  ·  ·  ·  ·  ·  · ║    │
│ M4 ══════════════════════════ M3    │
└─────────────────────────────────────┘
```

**Overhead estimate `O` ≈ 0.14–0.18** (lattice density dependent).

| Pros | Cons |
|---|---|
| **Illumination field is interpolated, not extrapolated** — far more accurate | Higher overhead |
| Per-region thresholds available everywhere, including the centre | Payload region is non-contiguous — more complex indexing, worse cache/shader access patterns |
| Header in the highest-SNR central region → best `H` | Lattice may interact with the display subpixel structure to produce moiré `[HYP]` |
| Local occlusion degrades locally rather than globally | Pilot lattice pitch is another parameter to tune |
| Blur and SNR estimated per region, feeding better LLRs | |

**Assessment:** best photometric and robustness properties, moderate overhead cost. The
interpolation-versus-extrapolation advantage is significant and applies to every frame.

## Candidate C — "Hierarchical tiles"

Screen divided into independently-decodable tiles, each with its own mini-markers, pilots
and header, plus global corner markers.

```
┌─────────────────────────────────────┐
│ M1 ══════════════════════════ M2    │
│ ║ ┌───────┐┌───────┐┌───────┐  ║    │  Each tile:
│ ║ │+ tile │|+ tile │|+ tile │  ║    │   + mini-marker
│ ║ │  hdr  ││  hdr  ││  hdr  │  ║    │   local header + pilots
│ ║ └───────┘└───────┘└───────┘  ║    │   independently decodable
│ ║ ┌───────┐┌───────┐┌───────┐  ║    │
│ ║ │+ tile ││+ tile ││+ tile │  ║    │  Global header replicated
│ ║ └───────┘└───────┘└───────┘  ║    │  across tiles
│ M4 ══════════════════════════ M3    │
└─────────────────────────────────────┘
```

**Overhead estimate `O` ≈ 0.20–0.26.**

| Pros | Cons |
|---|---|
| **Partial-frame recovery** — a tile survives even if the rest of the frame fails | **Highest overhead by a wide margin** |
| Strong occlusion tolerance: lose a tile, keep the rest | Per-tile headers are many small failure points |
| **Natural fit for rolling-shutter mixtures**: tile rows correspond to time slices, so a mixed frame yields clean tiles from the top and bottom states | Most complex to implement, generate and test |
| Tiles map naturally to parallel decode work units | Tile boundaries waste cells on guards |
| Local geometry per tile tolerates lens distortion without global correction | |

**Assessment:** highest overhead, best degradation behaviour. The rolling-shutter property
in row three is strategically interesting — it makes M4 mixed-frame recovery structurally
easy rather than an unmixing problem. **That may be worth the overhead, and it is exactly
the kind of trade that must be measured rather than argued.**

## Comparison

| Property | A: Border | B: Lattice | C: Tiles |
|---|---|---|---|
| Overhead `O` | **0.10–0.14** | 0.14–0.18 | 0.20–0.26 |
| Photometric accuracy | Poor (extrapolated) | **Best (interpolated)** | Good (per-tile local) |
| Header success `H` | Poor (low-SNR border) | **Best (centre)** | Good (replicated) |
| Occlusion tolerance | **Poor (correlated failure)** | Good | **Best** |
| Mixed-frame friendliness | Poor | Moderate | **Best** |
| Implementation complexity | **Lowest** | Moderate | Highest |
| Payload access pattern | **Contiguous** | Fragmented | Tiled (parallel-friendly) |
| Moiré risk from structure | Low | Moderate `[HYP]` | Moderate |

**No winner is declared.** The overhead differences (0.10 versus 0.26) look decisive but
are not: the model's sensitivity analysis shows `O` has only ~7% goodput swing for a ±20%
change, while `Pc` and `H` — which B and C improve — have ~40% and ~22% swings
respectively. **A layout that costs 10% more cells but raises `Pc` by 15% wins easily.**

This is precisely why the choice must be measured.

## Grid size candidates

Starting hypotheses only:

| Grid | Cells | Modelled goodput, binary @60 | Modelled goodput, four-level @60 |
|---|---|---|---|
| 96 × 160 | 15,360 | 50 KB/s | 100 KB/s |
| 120 × 200 | 24,000 | 78 KB/s | 156 KB/s |
| 144 × 240 | 34,560 | 112 KB/s | 224 KB/s |

(Model at `O`=0.15, `Rfec`=0.80, `Fd`=60, `Pc`=0.70, `H`=0.98, `Rftn`=0.95. **Model
output, not measurement.**)

**Warning from the literature:** ChromaCode's cell-size sweep showed goodput rising as
cells shrank, then **collapsing** past a threshold (137 kbps → 58 kbps). `[LIT]` The table
above is monotonic because the model has no density cliff in it. The real curve has one.
Finding it is EXP-001's job, and the table should be understood as valid only up to a
density limit we have not yet located.

## Automated sweep plan (EXP-013)

Rather than choosing parameters by argument, sweep them:

**Parameters:** grid width × height (including non-square cell aspect ratios), cell
interior sampling margin, guard cell width, pilot lattice pitch (B), tile size (C),
marker size, header cell size and replication factor.

**Procedure:**
1. Simulator sweep across the full parameter space under several channel severities.
   Cheap, parallel, exact ground truth. Narrows the space by an order of magnitude.
2. On-device sweep of the surviving candidates at 30/50/80 cm and 0/15/30/45°.
3. Rank on **end-to-end goodput**, not symbol error rate.
4. Report the full response surface, not just the winner — a sharp optimum is fragile and
   a broad plateau is preferable even at slightly lower peak goodput.

Step 4 matters: a configuration that is 5% better but sits on a cliff edge will lose
badly to hand tremor and distance variation in real use.

## Header contents

Regardless of layout:

| Field | Size (provisional) | Notes |
|---|---|---|
| Magic / version | 8 bits | Protocol versioning |
| Session ID | 32 bits | Distinguishes concurrent/adjacent sessions |
| Sequence number | 32 bits | Display state index; **essential** given unreliable presentation confirmation |
| Modulation profile ID | 8 bits | Which `LinkProfile` this frame uses |
| Frame phase indicator | 4 bits | Cycling pattern for mixture detection |
| FEC parameters | 16 bits | Code, rate, interleaver seed |
| Fountain metadata | 48 bits | Block ID, symbol ID / ESI |
| Payload length | 24 bits | **Validated against a hard bound before allocation** |
| Flags | 8 bits | Last block, profile change pending, etc. |
| Extension field length | 8 bits | Forward compatibility |
| CRC-32 | 32 bits | Over the whole header |

Header is encoded with a very low code rate, replicated at spatially separated positions,
and rendered with the largest cells in the frame. `H` appears directly in the goodput model
and over-protecting the header is cheap.

## Orientation handling

Four *distinct* asymmetric markers (not QR's three-plus-alignment scheme) mean orientation
is recoverable from **any three visible markers**, giving tolerance to one occluded corner.
Marker patterns should be chosen for low cross-correlation so that a partially-occluded
marker is not mistaken for a different one.

`[OPEN]` OQ-022: what marker patterns maximise detection reliability at our cell pitches
under defocus? This is a small, self-contained simulator study.
