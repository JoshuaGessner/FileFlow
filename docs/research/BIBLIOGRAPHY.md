# Annotated bibliography

> **Status:** Draft — actively growing
> **Owner:** Research
> **Last reviewed:** 2026-08-02
> **Related:** PRIOR-ART-MATRIX.md, RESEARCH-PLAN.md

## Conventions

Each entry carries:

```
[KEY] Author(s). "Title." Venue, Year. URL (accessed YYYY-MM-DD).
Access: full text | abstract only | secondary description
Type: peer-reviewed | standard/RFC | official platform documentation | vendor blog | demo | news
Relevance: why it matters to FileFlow
Findings: what we actually took from it, with metrics labelled
Limitations: what the measurement does not establish
```

`Access` is mandatory. A citation whose access level is `secondary description` **may not
be used as evidence** for a design decision — only as a pointer to work we should read.

---

## A. Screen-camera communication — visible modulation (our direct lineage)

### [COBRA]
Hao, T., Zhou, R., Xing, G. "COBRA: Color barcode streaming for smartphone systems."
ACM MobiSys, 2012. https://www.researchgate.net/publication/254462781 (accessed 2026-08-02).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** The origin of colour-barcode streaming between phone screen and phone
camera. Structurally the closest ancestor of FileFlow's transmitter.
**Findings:** Reported as using a 2D colour barcode carrying 2 bits per pixel-block with
HSV-space decoding. No verified rate figures — we have not read the paper.
**Limitations:** Predates high-refresh phone panels; 2012-era camera pipelines.
**Action:** RT-01 — obtain primary text.

### [RAINBAR]
Wang, Q., et al. "Rain Bar: Robust application-driven visual communication using color
barcodes." IEEE ICDCS, 2015. https://ieeexplore.ieee.org/document/7164939/ (accessed 2026-08-02).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** Improves on COBRA specifically in **block localisation and frame
synchronisation** — the two problems our CV and frame-phase subsystems must solve.
**Findings:** Reported to improve stability and capacity over COBRA. No verified figures.
**Action:** RT-01.

### [RDCODE]
"RDCode: robust dynamic barcode." (Full citation to be established.)
**Access:** secondary description. **Type:** peer-reviewed (to confirm).
**Relevance:** **High.** Uses a packet-frame-block structure with error correction at
three levels — intra-block, inter-block and inter-frame. That layering is structurally
analogous to FileFlow's intra-frame FEC + cross-frame fountain design (ADR-0009), and if
it holds up under primary reading it is independent support for that architecture.
**Findings:** Reported to at least double transmission rate versus COBRA. Metric type
unverified — "transmission rate" may be raw or goodput.
**Action:** RT-01, high priority.

### [SHIFTCODE]
"Capturing the Shifting Shapes: Enabling Efficient Screen-Camera Communication with a
Pattern-based Dynamic Barcode." Proc. ACM IMWUT, vol. 2, no. 1, 2018.
https://dl.acm.org/doi/10.1145/3191784 (accessed 2026-08-02 — **HTTP 403, not retrieved**).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** **Highest of the visible branch.** Explicitly targets the rolling-shutter
**frame-mixture** problem — the same problem FileFlow's M4 mode addresses — by encoding
in *pattern shifts* rather than colour.
**Findings:** Reported ≥2× goodput improvement over conventional colour-barcode systems.
A secondary summary mentions ~320 kbps at close range with a greyscale two-colour mode
outperforming a four-colour mode. **Both figures unverified.** The claim that greyscale
beats four-colour, if true, is directly relevant to our M2-vs-M3 decision and would be
evidence *against* assuming colour is a win.
**Action:** RT-01, highest priority. Obtain via institutional access.

### [FAREQR]
Wang, Han, et al. "FareQR: Fast and reliable screen-camera transfer system for mobile
devices using QR code." https://www.semanticscholar.org/paper/5c29ec9d512476b542b9bc1bee65f5509bdd5d17 (accessed 2026-08-02).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** The "optimise QR instead of replacing it" alternative. Represents the road
not taken (ADR-0006); understanding how far QR can be pushed calibrates our advantage.
**Action:** RT-01.

### [4DBARCODE]
Langlotz, T., Bimber, O. "Unsynchronized 4D barcodes: coding and decoding
time-multiplexed 2D colorcodes." https://www.researchgate.net/publication/234781882 (accessed 2026-08-02).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** Explicitly handles **unsynchronised** display and camera clocks — one of
our foundational assumptions.
**Action:** RT-01.

---

## B. Screen-camera communication — imperceptible (contrasting category)

These systems are **not** our architecture (NG4). They are included because they are the
most rigorously measured work in the field and they quantify the cost of imperceptibility.

### [CHROMACODE] ★ primary source, full text
Zhang, K., Wu, C., Yang, C., Zhao, Y., Huang, K., Peng, C., Liu, Y., Yang, Z.
"ChromaCode: A Fully Imperceptible Screen-Camera Communication System."
ACM MobiCom, 2018. https://www.cs.purdue.edu/homes/chunyi/pubs/mobicom18-zhang.pdf
(accessed 2026-08-02). DOI: 10.1145/3241539.3241543.
**Access:** **full text.** **Type:** peer-reviewed.
**Relevance:** Best-measured imperceptible system we have read. Sets the realistic ceiling
for the hidden-communication branch and supplies our raw-vs-goodput evidence.
**Findings (all as reported by the authors):**
- Raw throughput **777 kbps**, data goodput **120 kbps**, BER **0.05**. The authors
  explicitly note that ultimate goodput is much smaller than raw throughput —
  a **6.5× gap** within a single system. `[LIT]`
- Cell-size sweep: goodput rose **28 → 137 kbps** as data cell size shrank from 26×17 to
  8×7, then **dropped to 58 kbps** on further shrinking. Non-monotonic with a cliff.
- Across a device/condition set: raw **312–621 kbps**, goodput **5–56 kbps**, BER 0.09–0.12.
- Under a degrading condition: raw fell 697 → 112 kbps while goodput fell 56 → **0.47 kbps**.
  Goodput collapses far faster than raw rate.
- Uses CIELAB lightness modulation at 120 fps content rate.
**Why this matters to us:** the last two findings are the empirical core of our
performance philosophy. Raw rate degraded ~6× while goodput degraded ~119×. Optimising
raw rate is not merely uninformative — it can be actively misleading about system health.
**Limitations:** Imperceptibility constrains modulation depth severely, so the absolute
rates are not an upper bound for a *visible* system. Transmitter is not necessarily a
phone. Distance/angle conditions not extracted in our reading.

### [SPATADAPT] ★ primary source, full text
Nguyen, V., Tang, Y., Ashok, A., Gruteser, M., Dana, K., Hu, W., Wengrowski, E.,
Mandayam, N. "High-Rate Flicker-Free Screen-Camera Communication with Spatially Adaptive
Embedding." IEEE INFOCOM 2016 (35th Annual IEEE International Conference on Computer
Communications). https://www.winlab.rutgers.edu/~gruteser/papers/INFOCOM-FINAL.pdf
(accessed 2026-08-02). IEEE Xplore document 7524512.
**Access:** **full text.** **Type:** peer-reviewed.
**Relevance:** Characterises the flicker-free design space; introduces spatial
content-adaptive encoding via SLIC superpixels.
**Findings (as reported):**
- Average goodput **~22 kbps**, described as significantly outperforming comparators.
- TextureCode comparison: **16.52 kbps** (dynamic scene), **15.16 kbps** (static scene).
- Transmitter at 120 fps (120 Hz panel, 30 fps source content frame-duplicated ×4).
- Notes displays up to 144 Hz and cameras up to 240 fps as the constraint envelope.
- Uses Manchester coding to push the signal above 60 Hz for flicker suppression.
- Observes that a smaller block size (e.g. 12×12) "would produce a throughput of hundreds
  of kbps" — an author projection, **not a measurement**.
**Limitations:** Imperceptibility-constrained. Goodput figures are for embedded data in
cover video, not a dedicated transmitter.

### [TEXTURECODE], [INFRAME], [INFRAME++], [HILIGHT], [DEEPLIGHT], [REVELIO], [MOBISCAN]
Imperceptible-branch systems, known to us so far only through citations inside
[CHROMACODE] and [SPATADAPT], plus search-result metadata.
**Access:** secondary description. Not to be cited as evidence.
**Relevance:** Contrasting category only.

### [VRCODES]
Woo, G., Lippman, A., Raskar, R. "VRCodes: Unobtrusive and active visual codes for
interaction by exploiting rolling shutter." (IEEE ISMAR, ~2012.)
https://researchgate.net/publication/261297786 (accessed 2026-08-02).
**Access:** secondary description. **Type:** peer-reviewed.
**Relevance:** The canonical rolling-shutter-exploiting design. FileFlow explicitly does
**not** adopt a rolling-shutter-only modem (NG5), but M4 needs this literature.

---

## C. Visual MIMO and channel framing

### [VISUALMIMO-CISS11] ★ primary source, full text
Ashok, A., Gruteser, M., Mandayam, N., Dana, K. "Characterizing Multiplexing and Diversity
in Visual MIMO." CISS (Conference on Information Sciences and Systems), 2011.
https://www.winlab.rutgers.edu/~aashok/visualmimo/aashok_ciss11.pdf (accessed 2026-08-02).
**Access:** **full text** (free from the authors' lab page). **Type:** peer-reviewed.
**Relevance:** **The theoretical foundation of this project**, and until now uncited.
Formalises the screen/array-to-camera link as a MIMO channel in which transmitter elements
map to camera pixel elements, and derives an analytical channel capacity for it.
**Findings:**
- Multiplexing gain requires each transmit element to map to distinguishable camera pixels;
  when elements are no longer separable at the receiver the system falls back to diversity.
- **Perspective distortion is the dominant limit**, playing a role analogous to multipath
  fading in RF MIMO — it degrades element separability and therefore capacity.
- Multiplexing and diversity are not independently attainable; they trade off continuously.
**Why this matters to FileFlow:** it is the principled statement of the bet in ADR-0006.
Our dense grid is a multiplexing play, and this paper says the binding constraint is
whether cells stay separable at the receiver under perspective distortion — which is
precisely the "density cliff" EXP-001 must locate empirically. It also justifies treating
crosstalk as the primary capacity limit rather than noise (coding-theory.md).
**Limitations:** analysis targets LED arrays at longer range, not a phone display at 30 cm.
The framing transfers; the numbers do not.

### [VISUALMIMO-MOBICOM] ★ primary source, full text
Ashok, A., Gruteser, M., Mandayam, N., Dana, K., et al. "Challenge: Mobile Optical Networks
Through Visual MIMO." ACM MobiCom.
https://www.winlab.rutgers.edu/~gruteser/papers/mobicom45cp-ashok.pdf (accessed 2026-08-02).
**Access:** **full text.** **Type:** peer-reviewed.
**Relevance:** Programme-level statement of the visual-MIMO concept and its challenges.

### [VISUALMIMO-CV] ★ primary source, full text
Yuan, W., Dana, K., et al. "Computer Vision Methods for Visual MIMO Optical System."
https://www.winlab.rutgers.edu/~gruteser/papers/VisualMIMO.pdf (accessed 2026-08-02).
**Access:** **full text.** **Type:** peer-reviewed.
**Relevance:** The CV half — detection, perspective handling and radiometric calibration
for camera-display links. Directly relevant to components C06/C08/C09.

**RT-03 status: substantially closed.** The capacity framing that ADR-0006 rests on is now
read and cited rather than assumed. Remaining: extract quantitative capacity curves if we
later want a formal upper bound (currently the model is throughput-accounting, deliberately
— see coding-theory.md).

---

## D. Standards and coding

### [RFC6330] ★ primary source
Luby, M., Shokrollahi, A., Watson, M., Stockhammer, T., Minder, L. "RaptorQ Forward Error
Correction Scheme for Object Delivery." IETF RFC 6330, August 2011.
https://datatracker.ietf.org/doc/html/rfc6330 (accessed 2026-08-02).
**Access:** full text available. **Type:** IETF standards-track RFC.
**Relevance:** The reference systematic fountain code. Candidate for the cross-frame layer.

### [IPR1958] ★ primary source — read carefully before adopting RaptorQ
Qualcomm Incorporated (submitted by Thomas R. Rouse, VP QTL Patent Counsel). IPR
disclosure #1958 covering RFC 6330. https://datatracker.ietf.org/ipr/1958/ (accessed 2026-08-02).
**Access:** full text. **Type:** IETF IPR declaration.
**Findings:** Two-tier commitment. (a) For devices implementing RFC 6330 **that also
implement a wireless wide-area standard** (the example given is UMTS-compatible handsets),
Qualcomm offers licences without an additional incremental royalty above its standard
rate. (b) For devices implementing RFC 6330 **without** a wireless wide-area standard,
Qualcomm commits it "will not assert any such claim against any party," subject to a
defensive-termination reservation. Qualcomm acquired Digital Fountain and its IPR.
**Why this needs care:** FileFlow runs on **smartphones**, which do implement wireless
wide-area standards. On a plain reading, our deployment target falls in tier (a) — the
"licence available at standard rate" tier — **not** the tier (b) non-assert. This is not a
settled conclusion and we are not qualified to reach one; it is a flag that **RaptorQ
adoption requires legal review before it becomes a dependency.** Recorded as RISK-016 and
OQ-010.

### Coding-theory background
Standard references for LDPC, BCH, Reed–Solomon, polar codes and fountain codes are
listed in [coding-theory.md](coding-theory.md) rather than duplicated here.

---

## E. Platform documentation (primary)

### [AOSP-CAMERADEVICE] ★ primary source
AOSP, `platform_frameworks_base`, `core/java/android/hardware/camera2/CameraDevice.java`.
https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/master/core/java/android/hardware/camera2/CameraDevice.java (accessed 2026-08-02).
**Access:** full source. **Type:** official platform source.
**Findings:** Normative constraints on constrained high-speed capture sessions — max 2
surfaces; encoder or preview surfaces only; sizes from `getHighSpeedVideoSizes`; FPS from
`getHighSpeedVideoFpsRangesFor`; requests only via `createHighSpeedRequestList` submitted
through `captureBurst`/`setRepeatingBurst`. **`ImageReader` is not permitted.**
**Relevance:** Single most architecturally consequential fact found so far. See
[android-camera-pipeline.md](android-camera-pipeline.md) and RISK-002.

### [NDK-CHOREOGRAPHER] ★ primary source
Android NDK reference, "Choreographer."
https://developer.android.com/ndk/reference/group/choreographer (accessed 2026-08-02).
**Findings:** FrameTimeline API family introduced at **API level 33**.
**Relevance:** Sets our minimum API level for deterministic frame pacing (ADR-0004).

### [AND-FRAMERATE]
Android Developers, "Frame rate."
https://developer.android.com/media/optimize/performance/frame-rate (accessed 2026-08-02).
**Findings:** `setFrameRate` is a hint the platform may refuse; mode switches may take
~2 s; Choreographer timing changes as a result.

### [AND-REFRESH]
Android Developers, "Optimize refresh rates."
https://developer.android.com/games/optimize/display-refresh-rate-change (accessed 2026-08-02).

### [AND-HRR-BLOG]
Android Developers Blog. "High refresh rate rendering on Android." April 2020.
https://android-developers.googleblog.com/2020/04/high-refresh-rate-rendering-on-android.html (accessed 2026-08-02).
**Type:** vendor blog — **not peer-reviewed**, treat as guidance not specification.

---

## F. Software and libraries

### [RAPTORQ-RUST]
cberner/raptorq — RaptorQ (RFC 6330) in Rust. Apache-2.0.
https://github.com/cberner/raptorq (accessed 2026-08-02). See also issue #116 discussing
potential Qualcomm patent exposure. **Access:** repository metadata + issue title only.

### [LIBRAPTORQ]
LucaFulchir/libRaptorQ — RaptorQ RFC 6330, C++11, systematic encoding, large blocks.
https://github.com/LucaFulchir/libRaptorQ (accessed 2026-08-02).
**Access:** repository description only. Licence and maintenance status to verify (RT-06).

### [OPENRQ]
OpenRQ — RFC 6330 implementation in Java. https://openrq-team.github.io/openrq/
(accessed 2026-08-02). Java; would need JNI or reimplementation for our NDK core.

Full evaluation in [fec-library-evaluation.md](fec-library-evaluation.md).

---

## Reading queue (ordered)

1. ShiftCode (IMWUT 2018) — mixed-frame handling, closest to M4
2. RDCode — layered ECC structure, closest to our FEC/fountain split
3. Visual MIMO capacity papers — theoretical backing for the dense-grid thesis
4. RainBar — localisation and frame sync
5. COBRA — lineage baseline
6. FareQR — the QR-optimisation alternative
7. RFC 6330 in full — before any fountain implementation work
