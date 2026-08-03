# Terminology

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02

Precise vocabulary is load-bearing here. Most confusion in screen-camera literature —
and most inflated performance claims — comes from conflating the terms below.

## Rates (the six that must never be conflated)

| Term | Definition | Units |
|---|---|---|
| **Display refresh rate** | Rate at which the panel scans out, whether or not content changed. | Hz |
| **Display state rate** (`Fd`) | Rate of *distinct* optical frames actually presented. ≤ refresh rate. | states/s |
| **Camera capture rate** | Rate at which the sensor delivers frames to the app. | fps |
| **Raw optical symbol rate** | Payload cells × `Fd`. Symbols on the wire. | symbols/s |
| **Raw encoded bit rate** | Symbol rate × bits per symbol. Before any coding loss. | bit/s |
| **Corrected decoder bit rate** | After FEC overhead, erasures and lost states. | bit/s |
| **Payload goodput** | Verified original file bits ÷ total transfer wall-clock time. **The metric.** | bit/s or KB/s |

"Refresh rate" and "state rate" differ whenever the panel refreshes faster than we change
content, or when a requested frame is not actually presented. Verifying that intended
frames were presented is its own engineering problem — see
[android-display-pipeline.md](../research/android-display-pipeline.md).

## Optical structure

| Term | Definition |
|---|---|
| **Cell** | The smallest independently modulated spatial unit of the transmitted image. |
| **Grid** | The full-screen arrangement of cells, e.g. 120×200. |
| **Logical cell** | A cell as defined by the protocol, independent of how many physical display pixels back it. |
| **Cell pitch** | Physical display pixels per logical cell. |
| **Optical frame** | One complete transmitted image: markers, header, pilots, payload, guards. |
| **Display state** | One distinct optical frame actually presented on the panel. |
| **Camera frame** | One image delivered by the receiver's camera. |
| **Clean frame** | A camera frame containing exactly one display state. |
| **Mixed frame** | A camera frame containing a rolling-shutter mixture of two or more consecutive display states. |
| **Duplicate frame** | A camera frame containing a display state already decoded. |
| **Marker** | Asymmetric corner fiducial used for localisation and orientation. |
| **Pilot cell** | A cell with content known to the receiver, used for photometric calibration. |
| **Guard cell** | An unmodulated or fixed cell separating regions, reducing crosstalk. |
| **Timing track** | A structured row/column encoding frame phase or grid registration. |
| **Quiet region** | Screen area deliberately left unmodulated to aid detection. |

## Channel and signal

| Term | Definition |
|---|---|
| **Homography** | The 3×3 projective transform mapping display plane to camera image plane. |
| **Rectification** | Warping a camera frame back to display-plane coordinates using the homography. |
| **Spatial crosstalk** | Energy from one cell leaking into a neighbour's sample, via optics, defocus or display structure. |
| **Inter-symbol interference (ISI)** | Crosstalk in the temporal dimension: a display state contaminating the next. |
| **Moiré** | Aliasing artefact from interference between the display pixel grid and the camera sensor grid. |
| **Soft decision** | A demodulator output carrying confidence (e.g. an LLR), not just a hard bit. |
| **LLR** | Log-likelihood ratio. The standard soft-decision representation for FEC input. |
| **Erasure** | A symbol known to be missing or untrustworthy, as opposed to one decoded wrongly. |
| **Photometric normalisation** | Correcting for exposure, vignetting, gamma and white balance before slicing symbols. |
| **Frame phase** | Where a camera frame falls relative to display-state boundaries. |

## Coding

| Term | Definition |
|---|---|
| **Intra-frame FEC** | Error correction applied within a single optical frame's payload. |
| **Cross-frame coding** | Recovery of entire lost optical frames, here via a fountain code. |
| **Fountain code** | A rateless erasure code generating unlimited repair symbols from `k` source symbols. |
| **Systematic** | A code whose output begins with the unmodified source symbols. |
| **Reception overhead** | Symbols needed beyond `k` to decode, expressed as a fraction. Lower is better. |
| **Code rate** (`Rfec`) | Payload bits ÷ transmitted bits for the intra-frame code. |
| **Rateless completion** | Transfer ends when the receiver has decoded, not after a fixed transmission. |

## Model variables

Used in [PERFORMANCE-MODEL.md](../specifications/PERFORMANCE-MODEL.md) and
`tools/perf_model/perf_model.py`:

| Symbol | Meaning |
|---|---|
| `N` | Total logical cells per optical frame |
| `O` | Non-payload cell fraction (markers, pilots, header, guards) |
| `B` | Raw bits per payload cell |
| `Rfec` | Intra-frame FEC code rate |
| `Fd` | Distinct display states per second |
| `Pc` | Fraction of display states captured cleanly and decoded |
| `Pm` | Fraction of mixed states from which data is recovered |
| `H` | Header success probability |
| `Rfountain` | Fountain efficiency (payload ÷ delivered symbols) |

## Modulation modes

| Mode | Name | Nominal bits/cell |
|---|---|---|
| **M0** | Binary luminance | 1 |
| **M1** | Differential binary luminance | 1 |
| **M2** | Four-level luminance | 2 |
| **M3** | Four-colour | 2 |
| **M4** | Mixed-frame-aware temporal-spatial | variable |

Full definitions in [MODULATION-SPEC.md](../specifications/MODULATION-SPEC.md).

## Evidence tags

| Tag | Meaning |
|---|---|
| `[FACT]` | Verified against a primary source; citation required. |
| `[LIT]` | Reported in published literature; citation and access note required. |
| `[HYP]` | Our unvalidated design hypothesis; linked experiment required. |
| `[OPEN]` | Unresolved; must appear in the open-question registry. |

## Usage note

"Throughput" is banned as an unqualified term in this repository. Write "raw encoded bit
rate" or "payload goodput". If a cited paper says "throughput", quote it as the paper's
term and state which of our metrics it corresponds to — or state that this is unclear.
