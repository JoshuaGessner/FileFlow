# Prior-art comparison matrix

> **Status:** Draft — partially populated, several rows need primary-source verification
> **Owner:** Research
> **Last reviewed:** 2026-08-02
> **Related:** BIBLIOGRAPHY.md, ADR-0006

## How to read this table

**Rates from different papers are not comparable unless the metric matches.** The
`Metric type` column states what the number actually is. Where a paper reports both raw
and goodput figures, both are given. Where only one is available, the other is `—`, not
an estimate.

`Access` records whether we read the full text, only an abstract, or only a secondary
description. Rows sourced from search-result summaries rather than the paper itself are
marked `secondary` and **must not be cited as evidence** until upgraded.

## Matrix

| System | Venue / Year | TX hardware | RX hardware | Distance | Display rate | Camera rate | Modulation | Grid / code | Raw rate | Goodput | Error rate | Conditions | Visible? | Sync | FEC | Source available | Relevance | Access |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **Dynamic QR stream** (baseline) | n/a — common practice | Any screen | Any phone | ~10–30 cm | typically 5–15 states/s | 30 fps | Binary, per-module | QR v40 max 177×177 | — | order of 1–10 KB/s in practice | — | Handheld | Visible | Per-frame re-detection | Reed–Solomon (fixed levels) | Many OSS libs | **Baseline to beat.** Must be measured ourselves, not cited. | We will measure (EXP-002) |
| **ChromaCode** | ACM MobiCom 2018 | 120 Hz screen | Smartphone | not recorded here | 120 fps content | not recorded | Lightness modulation in CIELAB, adaptive embedding | Cell size swept 26×17 → 8×7 | **777 kbps** | **120 kbps** | BER 0.05 | Stationary | **Imperceptible** | — | Yes (unspecified here) | Not stated | Contrasting category. Best-in-class *hidden* goodput; shows the imperceptibility tax. | **Full text** |
| **Spatially adaptive embedding** (Nguyen et al.) | IEEE INFOCOM (see note) | LCD up to 144 Hz | Camera up to 240 fps | not recorded here | 120 fps | up to 240 fps | Superpixel-based intensity, Manchester-coded | Superpixel regions | — | **~22 kbps avg** | — | Stationary | **Flicker-free** | Manchester | Yes | Not stated | Contrasting category. Confirms imperceptible systems live in tens of kbps. | **Full text** |
| **TextureCode** (as measured in the above paper) | MobiSys 2015 (orig.) | as above | as above | — | — | — | Texture-adaptive embedding | — | — | **16.52 kbps** (dynamic), **15.16 kbps** (static) | — | Stationary | Imperceptible | — | — | — | Contrasting category. | Full text of *comparison*, not of original paper |
| **COBRA** | ACM (year to verify) | Phone screen | Phone camera | Short | — | — | 2D colour barcode, 2 bits/pixel-block, HSV decode | — | — | — | — | Handheld | Visible | — | — | — | Direct ancestor: visible colour barcode streaming. | **secondary** |
| **RainBar** | IEEE (2015, to verify) | Screen | Camera | — | — | — | Colour barcode, improved block locating + frame sync | — | — | — | — | — | Visible | Improved over COBRA | Yes | — | Directly relevant: localisation + sync are our CV problems. | **secondary** |
| **RDCode** | (to verify) | Screen | Camera | — | — | — | Dynamic barcode; packet-frame-block structure | Three-level: intra-block, inter-block, inter-frame | — | reported ≥2× COBRA | — | — | Visible | — | Three-level ECC | — | **Highly relevant** — layered intra/inter-frame ECC is close to our FEC+fountain split. | **secondary** |
| **ShiftCode** | ACM IMWUT vol. 2 no. 1, 2018 | Screen | Camera | Varies | — | — | **Pattern/shift-based** rather than colour, specifically to survive rolling-shutter frame mixture | — | — | reported ≥2× conventional; ~320 kbps cited at close range in a related summary | — | — | Visible | — | — | — | **Highly relevant** — directly attacks the mixed-frame problem that our M4 targets. | **secondary** (DL blocked, 403) |
| **FareQR** | (to verify) | Screen | Phone | — | — | — | QR-based, optimised for speed + reliability | QR | — | — | — | — | Visible | — | RS | — | Relevant as the "make QR faster" alternative we are explicitly not taking. | **secondary** |
| **Visual MIMO** | Rutgers WINLAB, multiple papers | LED array / screen | Camera | Varies, incl. long range | — | — | Spatial multiplexing over camera pixels | — | — | — | — | — | Visible | — | — | — | **Conceptual foundation** — the screen-as-MIMO-channel framing this project rests on. | **secondary** |
| **VRCodes** | (Woo et al., ~2012) | Screen | Rolling-shutter camera | — | — | — | Exploits rolling shutter for unobtrusive codes | — | — | — | — | — | Near-imperceptible | Rolling shutter | — | — | Contrasting: the rolling-shutter-only approach we explicitly reject as primary (NG5). | **secondary** |
| **HiLight** | (Li et al., ~2015) | Screen | Camera | — | — | — | Pixel translucency modulation over arbitrary content | — | — | low (tens of kbps class) | — | — | Imperceptible | — | — | — | Contrasting category. | **secondary** (cited within ChromaCode) |
| **InFrame / InFrame++** | (Wang et al.) | Screen | Camera | — | — | — | Dual-layer: video for humans, data for camera | — | — | compared in Nguyen et al. | — | — | Imperceptible | — | — | — | Contrasting category. | **secondary** (cited within Nguyen et al.) |
| **Unsynchronized 4D barcodes** | (Langlotz & Bimber) | Screen | Camera | — | — | — | Time-multiplexed 2D colour codes, no sync required | — | — | — | — | — | Visible | **Explicitly unsynchronised** | — | — | Relevant: asynchronous display/camera clocks are our assumption too. | **secondary** |
| **SoftLight** | (2016) | Screen | Camera | — | — | — | Adaptive VLC over screen-camera link, rateless-adjacent | — | — | — | — | — | Visible | — | Adaptive coding | — | Relevant: adaptive rate selection is our ADP subsystem. | **secondary** |

## What this table already tells us

1. **The imperceptible/hidden branch tops out around 120 kbps goodput** (ChromaCode,
   full text). That is ~15 KB/s — roughly **1/13th of milestone 4**. `[LIT]`
   Since ChromaCode is a strong, recent, peer-reviewed result, this is good evidence that
   **imperceptibility is the binding constraint** in that branch, and good justification
   for FileFlow's decision to abandon it (NG4).

2. **The raw-vs-goodput gap is large and real.** ChromaCode: 777 kbps raw → 120 kbps
   goodput, a 6.5× reduction, in the authors' own measurements. Any planning that treats
   raw rate as goodput will be wrong by roughly this factor. This directly validates the
   project's performance philosophy.

3. **Cell size dominates.** ChromaCode's own sweep shows goodput rising from 28 kbps to
   137 kbps as cell size shrinks from 26×17 to 8×7, then **collapsing to 58 kbps** when
   cells shrink further. `[LIT]` That non-monotonic shape — improving, then falling off a
   cliff — is exactly what our grid-size sweep (EXP-001) must find for our own channel.
   It is strong evidence that grid size cannot be chosen by reasoning and must be measured.

4. **The visible-barcode branch (COBRA → RainBar → RDCode → ShiftCode) is our direct
   lineage,** and we currently have **no primary-source numbers for any of it.** This is
   the single biggest gap in our literature review.

5. **RDCode's three-level ECC (intra-block, inter-block, inter-frame) is structurally the
   same idea as our intra-frame FEC + cross-frame fountain split.** If that comparison
   holds up under primary reading, it is meaningful independent support for ADR-0009.

## Gaps to close (tracked as research tasks)

| Task | What is needed | Priority |
|---|---|---|
| RT-01 | Obtain and read primary text for COBRA, RainBar, RDCode, ShiftCode, FareQR. Populate all `—` cells or mark them "not reported by the paper". | **High** — this is our direct lineage |
| RT-02 | Establish whether any of the visible-branch systems report *goodput* as we define it, or only raw/link rates. | High |
| RT-03 | Read Visual MIMO primary papers for the capacity framing. | Medium |
| RT-04 | Determine source-code availability for each system; any reproducible baseline is worth far more than a cited number. | Medium |
| RT-05 | Measure our own dynamic-QR baseline on reference hardware (EXP-002). **We should not cite anyone else's QR number.** | **High** |

## Known limitations of these measurements

- Most screen-camera papers use a **desktop monitor or tablet as transmitter**, not a
  phone. Phone panels differ in brightness, subpixel layout, size and refresh behaviour.
  A number obtained with a 144 Hz desktop LCD does not transfer to a phone-to-phone link.
- Distances, angles and ambient lighting are inconsistently reported.
- Several papers report the best configuration rather than a distribution.
- "Throughput" is used inconsistently across this literature; in several cases it is
  impossible to tell from an abstract which of our six metrics is meant. Rows marked
  `secondary` are especially prone to this.
