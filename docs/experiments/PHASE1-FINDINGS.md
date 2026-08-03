# Phase 1 findings — scaffolding and first implementation

> **Status:** Current
> **Owner:** Architecture
> **Last reviewed:** 2026-08-03
> **Related:** ADR-0006, ADR-0009, ADR-0013, OPTICAL-FRAME-CANDIDATES.md,
> PERFORMANCE-MODEL.md, MODULATION-SPEC.md, EXPERIMENT-REGISTRY.md

Findings F1–F23 from implementing the Phase 1 core. Recorded here because several are
**design defects the specifications did not anticipate**, one is a **retraction** (F14), and
several are model-versus-implementation gaps. Almost all were surfaced by tests or by building
a consumer for something that already existed — not by inspection.

**If you read only a few:** F14 (retracted — do not plan around a detection floor), F15
(localisation must never depend on payload), F18 (intra-frame FEC is load-bearing), F20 (the
code-rate ladder can be scored for free, and the answer cannot be delivered), F23 (erasures are
not what the code spends).

---

## F1 — The distributed pilot lattice produced only ONE reference level `[FACT]`

**Severity: high. Would have silently broken photometric normalisation on every frame.**

`PilotValue()` alternated on `(col + row) % 2`. But lattice points sit at
`col, row ≡ pitch/2 (mod pitch)`, so `col + row` is **always even** across the entire
lattice — every pilot rendered bright, and the receiver could never estimate the dark
reference. `EstimateReference()` fell back to a default dark of 0 against a measured bright,
placing the threshold at roughly half the true value.

On an identity channel this still decoded (the threshold was wrong but on the right side of
every sample), which is exactly why it would have survived casual testing and then failed
in the field as soon as the channel had any offset.

**Fix:** alternate on **lattice indices** (`col/pitch + row/pitch`), not raw coordinates.

**Generalisation worth remembering:** any parity or periodic function evaluated over a
sub-lattice can be constant over that sub-lattice. This will recur in timing tracks, guard
patterns and the M3 colour constellation. Caught by
`M0.DegenerateSeparationForcesErasures`.

---

## F2 — The fountain layer ingested corrupted symbols `[FACT]`

**Severity: high. Violated a rule the protocol spec already stated.**

PROTOCOL-SPEC says "uncorrectable ⇒ erasure signalled upward… never pass through
best-effort data." The implementation did exactly the opposite: occluded cells became
zeros, were packed into a symbol, and were handed to the fountain decoder as if valid.

**A fountain code is an ERASURE code, not an error-detecting one.** It cannot distinguish a
damaged symbol from a good one. A single corrupt symbol propagates through peeling into
every symbol that references it, and surfaces only as a final SHA-256 mismatch — with no
way to attribute the failure or recover from it. The transfer fails completely rather than
degrading.

**Fix:** a `payload_crc` field in every frame header. The receiver verifies it before
ingesting; a mismatch makes the frame a **frame erasure**, which is precisely what the
fountain layer is designed to absorb. The transfer now completes correctly instead of
failing.

**Consequences worth noting:**
- Frame header grew by 4 bytes (`kPlainSize` 28 → 32).
- This CRC is **not** a substitute for the end-to-end SHA-256. A malicious transmitter
  controls the CRC too, so the hash gate remains the last line of defence — covered by
  `Transfer.HashGateCatchesCorruptionThatEvadesEveryChecksum`.
- Caught by `EndToEnd.OcclusionProducesErasuresAndStillVerifies`.

**Architectural lesson:** the erasure-propagation discipline in DATA-FLOW.md is not
self-enforcing. Every boundary where soft information becomes bytes needs an explicit
integrity check, or the erasure information is silently discarded.

---

## F3 — Measured layout overhead is far below the modelled range `[FACT]`

The implemented Candidate B layout at 120×200 gives **`O` = 0.0831**. The performance model
assumed **0.12–0.20**.

The model was pessimistic: 22,005 of 24,000 cells carry payload. This is good news, but it
means **the model's scenarios understate goodput slightly** for this layout, and the
`O` inputs should be updated once the layout stabilises.

> **Amended 2026-08-02.** First recorded as `O` = 0.0595 (22,573 payload cells). Adding the
> persistent boundary ring (finding F10) spent 568 cells, moving `O` to 0.0831. Recorded
> rather than silently corrected: the ring is a real and deliberate cost paid for screen
> localisation, and the point of the original finding — that the model's `O` band is too
> pessimistic — survives it comfortably.

Caveat before anyone gets excited: the model's sensitivity analysis shows `O` has only a
~7% goodput swing, so this is the least important variable to have been wrong about. The
high-leverage inputs (`Pc`, `Fd`, `B`, `Rfec`) remain entirely unvalidated.

**Action:** revisit `O` in `tools/perf_model/perf_model.py` after EXP-013 selects a layout.
Do not retune now — the layout is not final.

---

## F4 — Simulated goodput, first measurement `[HYP]`

From `ffsim` at 120×200, M0 binary, assumed `Fd` = 60 states/s:

| Channel | Payload goodput | Fountain overhead |
|---|---|---|
| Identity (no impairment) | **120.0 KB/s** | 0.00 |
| Noise σ=12, shot 0.6, crosstalk 0.15, γ=2.2, vignetting 0.35, 20% frame loss | **33.0 KB/s** | 0.54 |

**These are simulator outputs, not measurements.** They are `[HYP]` until Phase 2
calibration (SIM-03, RISK-024) and the channel model is currently uncalibrated and
probably kind.

Two observations that matter:

1. **The clean figure (120 KB/s) exceeds the model's binary@60 expected case (78 KB/s)**,
   consistent with F3's lower overhead and with there being no payload FEC yet
   (`Rfec` effectively 1.0). It is not evidence of anything except internal consistency.

2. **The impaired figure collapses to 33 KB/s** — a 3.6× drop — driven mostly by **LT
   fountain overhead of 0.54**, far worse than RaptorQ's ~2% class. This is early,
   uncalibrated evidence that **the LT fallback is expensive**, and it raises the value of
   resolving the RaptorQ licensing question (RT-07, RISK-016). EXP-012 should quantify this
   properly and the result should feed directly into whether legal review is worth pursuing.

Also note: **block padding waste.** 262,144 payload bytes into 2 blocks × 64 symbols ×
2,821 bytes = 361,088 bytes of capacity — roughly 27% padding. Variable last-block sizing
is an obvious optimisation not yet implemented.

---

## F5 — Apple clang ships no libFuzzer `[FACT]`

`libclang_rt.fuzzer_osx.a` is not present in the Xcode toolchain, so fuzz targets cannot be
built with the macOS system compiler. They are opt-in (`FILEFLOW_BUILD_FUZZ`, default OFF)
and run on Linux in CI; local macOS fuzzing needs Homebrew LLVM.

This matters because D1 chose plain C++20 **without** the hardened-parser hedge, making
fuzzing and sanitisers the primary defence on the attacker-facing surface. ASan and UBSan
do work with Apple clang and run clean, so local coverage is not absent — but the fuzzing
half of that defence is CI-only on macOS development machines.

---

## F6 — The colour-pilot rule starved the WHITE reference level `[FACT]`

**Found:** 2026-08-02, while building the photometric field (component C09).

The layout reserves every fourth pilot-lattice point for colour (unused until M3). The rule
was `(i+j) % 4 == 0`. But `PilotValue()` alternates bright/dark on `(i+j)` parity, and
`(i+j)%4==0` implies `(i+j)` is **even** — so the reservation drew **exclusively from the
bright class**. Measured over a 64×64 lattice: bright 25%, dark 50%. **The white level was
estimated from half as many pilots as the black level.**

That is exactly backwards. The white level is the one degraded by angular luminance falloff
(RISK-025) and by any exposure shortfall, so it needs *more* support than the black level,
not half. Dark cells sit near zero and are comparatively stable.

**Fix:** `(i + 2j) % 4 == 0` selects the same 25% of lattice points while splitting evenly
across parities — measured bright 37.5%, dark 37.5%. One line, in `grid.cpp`.

**Why it was invisible:** every existing test used a flat illumination field, where one
bright pilot is as good as fifty. The defect only bites when the bright level *varies*,
which is the case we care about. Regression test:
`PhotometricField.PilotLatticeIsBalancedAcrossBothLevels`.

---

## F7 — Multiplicative falloff alone does NOT defeat a global threshold `[FACT]`

**Found:** 2026-08-02, while writing the RISK-025 mitigation test.

The first version of the test applied radial luminance falloff (corners at 40% of centre)
and expected a global threshold to fail. **It did not — zero errors.**

The reason is a genuine property of the channel, not a test bug. With a near-zero black
level, the global threshold sits at roughly half the mean bright level, so the *bright* level
must fall by **more than ~50%** before a cell crosses it. Pure multiplicative dimming is
therefore surprisingly benign for binary luminance.

What *does* defeat a global threshold is a **spatially varying black-level lift** —
directional glare, i.e. ambient light reflecting off the panel — because it squeezes from
the other side. Glare on one region plus falloff on another makes the two regions' level
ranges *overlap*, and then no single threshold separates both.

**Consequences:**
1. The simulator's impairment set needs a **directional glare gradient**, not just uniform
   black lift. Added to the test renderer; should be promoted into `sim/`.
2. RISK-025's severity estimate should be **revised down for falloff alone** and **up for
   falloff combined with off-axis glare** — which is the realistic handheld case, since the
   same off-axis geometry that dims a privacy display also catches reflections.
3. EXP-003 and EXP-018 must vary ambient lighting **and** angle together. Varying either
   alone will under-report the problem.

**Measured at the settings now in the test:** global thresholding → **39 payload errors**
(~1.3% raw BER, all confident wrong bits); interpolated field → **0**.

---

## F8 — Interpolation hides occlusion unless residual is checked `[FACT]`

**Found:** 2026-08-02, same session.

Inverse-distance weighting is *dangerously* well-behaved. When a block of the frame was
occluded (pilots reading a flat mid-grey), the field simply blended in pilots from outside
the damaged area and returned a **confident, healthy-looking reference** for the occluded
region. Local separation stayed large, nothing was erased, and every payload cell there
would have decoded to a confident wrong bit — the most expensive failure mode there is.

The interpolator cannot detect this from its output; it needs to notice that **its own model
does not fit**. Added a per-node **pilot-fit residual**: the weighted spread of contributing
pilots about their fitted level. Where pilots that should agree do not, the region is
erased regardless of how healthy its separation looks.

**This generalises well beyond occlusion** — it is the same signal for a finger over the
screen, a glare hotspot, a tracking error that has shifted the sampling grid off the cells,
or a partially-visible frame. It is the first real **detection-confidence** mechanism in the
receiver, and the optical-frame spec already called for confidence fields without saying how
they would be computed. This is how.

Exposed as `PhotometricField::ResidualAt()` so telemetry can report *why* cells were erased.
Test: `PhotometricField.OccludedRegionErasesRatherThanGuesses`.

---

## F9 — The corner markers could not resolve orientation at all `[FACT]`

**Found:** 2026-08-02, when every screen-detection test failed at once.

The markers distinguished corners by a **notch of 0–3 cells** knocked out of a 6×6 block.
Measured pairwise Hamming distances between the four corner patterns:

| | id0 | id1 | id2 | id3 |
|---|--:|--:|--:|--:|
| **id0** | — | **1** | 2 | 3 |
| **id1** | | — | **1** | 2 |
| **id2** | | | — | **1** |

**Minimum distance: 1 cell out of 36.** Worst-case rotation margin **0.028** — a 2.8%
signal that a single noisy cell flips. Orientation was effectively a coin toss, and a
90° error decodes to pure garbage.

Worse, the notch rule (`lc == 3 && lr < 3 + corner_id`) hardcoded index 3, so it silently
produced **no notch at all** for the top-left corner and broke entirely for
`marker_size < 4`.

**Fix — and the reason it is a redesign, not a patch:** once localisation moved to the
persistent boundary ring, the markers stopped needing to be a *detection* target and became
purely a **4-way code**. Designed as such: outer edge always bright (anchors the ring),
`d==1` always dark (every corner keeps visible dark structure), and the entire inner block
carries a 2-bit corner ID as {solid bright, solid dark, checker, inverse checker}.

**Minimum distance 8/36; worst-case rotation margin 0.222 — 8× better.** All eleven
detection tests passed immediately on the redesign.

**Generalisable lesson:** the notch was designed when markers were expected to do both
localisation *and* orientation, and it was a reasonable compromise for that job. Splitting
the two responsibilities did not just simplify the detector — it removed the constraint that
was making the code weak. Regression test asserts the Hamming property directly
(`MarkerCodeHasEnoughHammingDistanceToResolveRotation`), not merely its downstream effect,
so a future redesign cannot quietly reintroduce a weak code.

---

## F10 — The layout had no persistent boundary, though the spec required one `[FACT]`

**Found:** 2026-08-02, while designing screen localisation.

`OPTICAL-FRAME-CANDIDATES.md` lists "persistent screen boundary — cheap edge tracking,
subpixel refinement" as a required frame element. **Candidate B never implemented one.**
Perimeter cells were payload (left/right edges) or alternating timing track (top/bottom).

This is a documentation-versus-implementation divergence of exactly the kind
CONTRIBUTING.md warns about, and it was only exposed by trying to build the component that
depended on it.

**Implemented:** a 1-cell always-bright perimeter ring (`CellRole::kBoundary`), with the
timing tracks moved one cell inward. Costs `2*(cols+rows)-4` cells — ~2.7% at 120×200.

**Why it is worth 2.7%:** it converts screen localisation from "find and centroid four small
fiducials" into "fit four long straight lines and intersect them". Every pixel along an edge
constrains the fit, so the corners come out far more accurately, and unlike small fiducials
the signal does not degrade first with distance and defocus. Measured worst cell-centre error
against ground truth: **< 0.25 cells** across the frame.

It also makes the boundary *persistent* in the useful sense — it is present in every frame
regardless of payload, so a frame whose payload decode fails still contributes a geometry
update, which is what makes tracking cheaper than re-acquisition (the ADR-0006 bet).

---

## F11 — `FF_ASSIGN_OR_RETURN` collided with itself `[FACT]`

**Found:** 2026-08-02, the first time the macro was used twice in one function.

```c
#define FF_ASSIGN_OR_RETURN(decl, expr)  auto _ff_r_##__LINE__ = (expr); ...
```

`##` pastes **before** `__LINE__` expands, so every expansion declared the same literal
identifier `_ff_r___LINE__`. Two uses in a scope → redefinition error. Fixed with the
standard two-level indirection (`FF_CONCAT` → `FF_CONCAT_INNER`).

Low severity — it fails loudly at compile time and cannot produce wrong runtime behaviour.
Recorded because it is a good example of a defect that looks fine, passes every test, and is
simply *waiting* for the first caller to do the obvious thing. The whole macro existed for
exactly the two-fallible-calls-in-a-row case and had never been used for it.

---

## F12 — The detector allocated a point per lit pixel `[FACT]`

**Found:** 2026-08-02, reviewing the detector's hot path.

`ScreenDetector::Detect` collected every lit pixel into a `std::vector<Point2>` and then
scanned it for four extremes. Cost of that vector, computed for real capture resolutions:

| Capture | Lit points | Transient allocation |
|---|--:|--:|
| 1080p preview | 0.5 M | 8 MB / frame |
| 4K video | 2.1 M | 32 MB / frame |
| Pixel 8, 12 MP | 3.1 M | **48 MB / frame** |
| S26 Ultra, full 200 MP | 49.9 M | **762 MB / frame** |

At 4K60 that is **~1.85 GB/s of transient allocation**. Three separate problems:

1. Unusable on the hot path at any realistic capture resolution.
2. The allocation size is chosen by **attacker-controlled pixel content** — a hostile
   all-bright frame maximises it. Resource exhaustion via untrusted input (THREAT-MODEL T2).
3. All of it to compute four extremes, which need **no storage whatsoever**.

**Fix:** a streaming accumulator, O(1) state, zero allocation. Note the seeding: the
accumulators start at ±1e300, not 0, because a lit pixel at (0,0) has both sum and diff equal
to zero and a 0-seeded accumulator would silently refuse to record it — which would shift the
detected top-left corner inward on any screen flush with the image origin. Regression tests:
`FullyLitImageCostsNoAllocation`, `FindsAScreenTouchingTheImageOrigin`.

---

## F13 — The tracking saving is geometry-bound, and vanishes in our target regime `[HYP]`

**Found:** 2026-08-02, measuring ADR-0006's central claim in our own code.

ADR-0006 asserts a persistent tracked grid beats repeated independent detection. Now that
`ScreenTracker` exists, that claim is measurable. Pixels examined per frame, tracked vs
full acquisition, simulated:

| Screen area / frame area | tracked ÷ acquired pixels |
|--:|--:|
| 0.645 | **1.000 — no saving at all** |
| 0.300 | 0.673 |
| 0.133 | 0.299 |
| 0.075 | 0.168 |

The relationship is simply `ratio ≈ (1 + 2·margin)² × (screen area / frame area)`. The saving
comes from **how little of the frame the screen occupies**, not from tracking being clever.

**The uncomfortable part:** we *want* the screen to fill the camera frame, because that
maximises pixels per cell and therefore resolvable grid density. So the configuration we are
optimising for is exactly the one where this benefit of tracking is **zero**.

This does not sink ADR-0006, but it does narrow it. Tracking still buys temporal consistency
of the homography and fewer opportunities to lock onto the wrong bright object — but those
are *different* claims and must be argued and measured separately rather than folded into
"tracking is cheaper". The pixel-count argument, stated plainly, is much weaker than the ADR
currently implies.

### RESOLVED, same day — boundary-annulus search

The corner extremes lie **on** the boundary ring, so scanning the quad's interior finds
nothing and costs everything. Replacing the dilated-bounding-box window with an annulus
around the boundary:

| Screen area / frame | bounding box | **annulus** | improvement |
|--:|--:|--:|--:|
| 0.645 | 1.000 | **0.554** | 1.8× |
| 0.300 | 0.673 | **0.299** | 2.3× |
| 0.133 | 0.299 | **0.133** | 2.2× |
| 0.075 | 0.168 | **0.075** | 2.2× |

Headline case: **0.415 → 0.185**. The saving now holds across the whole range, including
the dense-framing regime where it had previously vanished entirely.

The ratio now lands at roughly the screen-area fraction itself, which is the expected result:
annulus area ≈ perimeter × band width, and with band width proportional to quad size that is
proportional to quad area.

One design detail that is load-bearing: the annulus is built by scaling the quad about its
**centroid**, not by insetting a bounding box. A bbox annulus does not follow a *rotated*
quad's boundary, so it would miss the ring exactly when the receiver is held at an angle —
the common case, not the exception. Test: `AnnulusTracksARotatedScreen`.

A second consequence: tracked frames now reuse the binarisation threshold from acquisition
rather than recomputing Otsu, which would have cost a full-image pass and defeated the point.
That is consistent with the design rather than a shortcut — exposure, ISO and white balance
are locked for the session precisely so the photometric channel stays stable — and it is
self-correcting, since drift fails marker verification and triggers reacquisition.

**Status of the ADR-0006 concern:** largely resolved for the pixel-cost claim. The wider point
stands and is worth keeping: the saving was never *intrinsic* to tracking, it is a property of
the search region, and it took a measurement to notice that the obvious implementation threw
it away in the one regime that matters.

---

## F14 — ~~Detection has a floor of roughly 6 pixels per cell~~ **RETRACTED** `[WRONG]`

> **RETRACTED 2026-08-02, the same day it was recorded.** The claim was an artifact of the
> F15 bug, and the corrected number is not a resolution limit either. The original text is
> kept below because the retraction is more instructive than the finding was, and because
> deleting unfavourable results is forbidden (CONTRIBUTING.md).

### What was originally claimed

A sweep of apparent screen size on a 48×80 grid put the detection floor **between 5.42 and
6.25 px/cell**, and I drew operating-envelope conclusions from it — notably that a 144-column
grid with the screen filling only 70% of frame width would fail outright, making tight framing
close to mandatory.

### Why it was wrong, twice over

**First error: it measured the F15 bug.** That sweep ran *before* the payload-dependence fix.
Shrinking the screen also shrank the lit-pixel count, and the old area test compared lit pixels
against the image area — so small screens were refused for having too few *lit pixels*, not for
being unresolvable. Re-running the identical sweep after the F15 fix moves the boundary from
5.42–6.25 down to **3.75–4.58 px/cell**.

**Second error: the remaining boundary is a config threshold, not optics.** Checking the
geometry of the four sweep points against `min_area_fraction = 0.03`:

| px/cell | quad area ÷ image | vs 0.03 | observed |
|--:|--:|:--|:--|
| 6.25 | 0.0750 | pass | detected |
| 5.42 | 0.0563 | pass | detected |
| 4.58 | 0.0403 | pass | detected |
| 3.75 | **0.0270** | **fail** | refused |

The transition sits **exactly** on `min_area_fraction`. The detector never reached a resolution
limit anywhere in that sweep — it was rejecting screens for being too small a *fraction of the
frame*, which is a deliberate configuration choice about what counts as a plausible screen.

### What we actually know

**We have not found the density cliff.** The independent grid sweep
(`tools/grid_sweep.sh`) decodes cleanly all the way to **192×320 at 5.00 px/cell** — header
success 1.0000, worst geometric error 0.28 cells, verified transfer, goodput rising
monotonically 120 → 240 KB/s across the range.

**And that null result should not be believed.** The renderer does nearest-neighbour cell
lookup plus an optional box blur. It does **not** model display subpixel structure, camera
sensor MTF or anti-alias filtering, moiré between the display grid and the sensor grid, or
diffraction — and those are precisely the mechanisms that produce a density cliff in real
systems. ChromaCode measured a real cliff on real hardware; our simulator finding no cliff is
the expected consequence of not modelling the physics that causes one, **not** evidence that
none exists.

### Consequences

- ADR-0006 and two source comments cited the ~6 px/cell figure. Corrected.
- The operating-envelope conclusions drawn from it were unfounded and are withdrawn. Tight
  framing may still be desirable for pixels-per-cell, but it is **not** forced by a detection
  floor we have demonstrated.
- **EXP-001 cannot be answered in simulation with the current renderer.** Locating the density
  cliff requires either real captures or a renderer modelling display/sensor sampling. This
  is now the strongest argument for prioritising the capture harness over further simulator
  work, and it is a concrete instance of RISK-024 — a simulator kinder than reality producing
  a confident wrong conclusion.

---

## F15 — Screen localisation depended on payload data `[FACT]`

**Found:** 2026-08-02, chasing a discrepancy in the ffsim tracking A/B. The tracked run needed
64 frames; the per-frame-acquisition run needed **447**, with header success **0.33** and
**301 detection failures**. Identical geometry, identical channel — so something other than
geometry was deciding whether the screen could be found.

**Mechanism.** `ScreenDetector::Detect` tested `min_area_fraction` — documented as *"the screen
must occupy at least this fraction of the image"* — against the **count of lit pixels**. Those
are different quantities, and the lit-pixel count depends on what is being transmitted:

| Frame | Bright cells | Lit px ÷ image | Result |
|---|--:|--:|---|
| Real data | 12,048 / 24,000 | 0.321 | detected |
| Zero-padded | 1,075 / 24,000 | **0.0287** | **refused** (needs 0.030) |

The screen occupied ~64% of the image in *both* cases. Of 60 real transmitter frames, **54 were
refused, every one a systematic symbol** from the zero-padded tail — a 16 KB payload into a
176 KB block leaves most systematic symbols entirely zero.

**Why this was worse than the failure rate suggests.** It made screen localisation depend on
payload content, which is exactly what the persistent boundary ring and corner markers exist to
prevent. In the field it would have appeared as unreproducible flakiness on any file containing
a run of zeros, or on any transfer whose final block was padded — the sort of defect that is
near-impossible to diagnose on hardware and trivially visible in simulation.

**Fix.** Test quad area, which is what the check always meant. Costs nothing: the pixel scan
runs either way, and the early-out only skipped a homography solve and marker scoring. Spurious
quads from scattered noise still fail marker verification, which is the real gate.
**54/60 refused → 0/60.** Regression tests: `RealTransmitterFramesReproduceTheFfsimFailure`,
`DetectsAnAlmostEntirelyDarkFrame`.

**A correction to my own reasoning, recorded because the error was instructive.** Mid-investigation
the evidence looked like "tracking is more *reliable* than per-frame detection, because it freezes
the binarisation threshold instead of re-deriving it from changing content". That was a plausible
mechanism and it was wrong. With F15 fixed, both paths decode identically — 64 frames, H = 1.0,
zero failures. The entire reliability gap was this bug. **Tracking buys speed, not accuracy**, and
had the bug not been chased to the bottom the project would have acquired a confident,
wrong belief about why its central architectural bet works.

---

## F16 — Grid sweep: goodput scales with density, no cliff found `[HYP]`

**Run:** 2026-08-02, `tools/grid_sweep.sh` — a simulator dry run of EXP-001 through the full
image path. 512 KB payload, 1200×2000 capture, head-on, no impairments.

| Grid | Cells | px/cell | Geom err (cells) | Frames | `H` | Goodput | Verified |
|---|--:|--:|--:|--:|--:|--:|:--|
| 48×80 … 96×160 | — | — | — | — | — | — | header too big for grid |
| 108×180 | 19,440 | 8.89 | 0.153 | 256 | 1.0000 | 120 KB/s | yes |
| 120×200 | 24,000 | 8.00 | 0.168 | 192 | 1.0000 | 160 KB/s | yes |
| 132×220 | 29,040 | 7.27 | 0.187 | 192 | 1.0000 | 160 KB/s | yes |
| 144×240 | 34,560 | 6.67 | 0.207 | 192 | 1.0000 | 160 KB/s | yes |
| 156×260 | 40,560 | 6.15 | 0.162 | 128 | 1.0000 | 240 KB/s | yes |
| 168×280 | 47,040 | 5.71 | 0.239 | 128 | 1.0000 | 240 KB/s | yes |
| 192×320 | 61,440 | 5.00 | 0.279 | 128 | 1.0000 | 240 KB/s | yes |

Goodput rises monotonically 120 → 240 KB/s with no degradation anywhere: header success stays
at 1.0000 and worst geometric error stays under 0.28 cells throughout. Goodput is quantised
into steps because whole frames and whole blocks are quantised, not because of any channel
effect.

**The null result is the finding, and it must not be over-read.** See the F14 retraction: the
renderer models neither display subpixel structure, nor sensor MTF, nor moiré, nor diffraction
— exactly the mechanisms that produce a density cliff. Finding no cliff here is what a
simulator that omits the relevant physics is *guaranteed* to report.

**Two things that only showed up by running it:**

1. **The first attempt measured nothing.** With a 16 KB payload every grid finished in the same
   64 frames — one symbol per frame, block heavily under-filled — so goodput was pinned at
   `payload ÷ (64/Fd)` and every row read an identical 15.0 KB/s. A sweep can look perfectly
   healthy while varying nothing; the payload has to be large enough that per-frame capacity
   actually determines completion time.
2. **Small grids fail on the header, not the channel.** `FrameLayout::Create` validates geometry
   but cannot know how large the *coded* header is, so grids below ~108 columns build a
   valid-looking layout and then fail at render with a generic out-of-range error. `ffsim` now
   checks this where both facts are in scope and says so plainly; the sweep reports it as a
   configuration limit rather than letting it masquerade as a decode failure.

**Consequence for EXP-001:** it cannot be answered in simulation with the current renderer.
Locating the density cliff needs real captures, or a renderer that models display and sensor
sampling. This is the strongest argument yet for prioritising the capture harness.

---

## F17 — Replay is bit-identical to live decode `[FACT]`

**Built and proven:** 2026-08-02. The capture/replay harness (component C17) is the path every
real capture will arrive on, and its value depends entirely on one property: replaying a
recording must produce **exactly** what decoding those frames live produced. If it does not, a
bug reproducing in replay might be a harness artifact, and a fix validated against replayed
frames might not hold on device.

`ReplayIsBitIdenticalToLiveDecode` decodes rendered frames straight through `FramePipeline`,
then writes those same images to a bundle and replays them, requiring every cell sample to
match with `EXPECT_DOUBLE_EQ` — not a tolerance — and the pipeline diagnostics
(`frames_decoded`, `total_pixels_examined`) to match too. It passes.

**Establishing this before hardware exists is the point.** The first real capture lands on a
path already known to be faithful, so any surprise in that data is about the channel rather
than about our tooling.

**What made it possible:** extracting `FramePipeline` (component C06a). The simulator already
owned a detect→rectify→sample→normalise chain, and replay needed the same one. Rather than
duplicate it — which would have guaranteed eventual drift, and with it the quiet invalidation
of every recorded dataset — the chain became a single shared object that the simulator, the
harness and eventually the live receiver all drive.

**End-to-end today, with no hardware:** `ffsim --record DIR` writes a synthetic bundle,
`ffreplay DIR` decodes it through the production chain, and the ADR-0006 A/B runs on replayed
frames (831,518 vs 1,350,000 geometry pixels/frame, 1 acquisition + 108 refined). CI performs
the record→replay loop on every push.

---

## F18 — Intra-frame FEC was missing, and the channel does not work without it `[HYP]`

**Found and built:** 2026-08-02. ADR-0009 specifies intra-frame FEC **plus** cross-frame
fountain coding. The fountain half had worked since Phase 1; **the intra-frame half did not
exist.** A frame with a handful of unreadable cells failed its CRC and was discarded entirely,
throwing away thousands of good cells to lose a few bad ones, and the fountain then spent a
whole extra frame recovering what a few parity bytes could have fixed in place.

Worse, `ReedSolomon` had **no erasure decoding at all** — only blind error correction. Every
stage upstream exists to identify unreadable cells rather than guess at them (NaN samples,
`SoftSymbol::erased`, the photometric residual check of F8), and all of that information was
being discarded at the final step.

### Why erasures are worth double

An error is wrong at an *unknown* position: the code must spend budget finding both where and
what. An erasure has a *known* position, so only the value must be solved. Hence
`2·errors + erasures ≤ nsym` — the same parity corrects **nsym erasures but only nsym/2
errors**. Supplying positions doubles the correction power at zero cost in parity.

### The measurement

Identical impaired channel (noise 26, shot 0.9, crosstalk 0.20, occlusion 3%, frame drop 10%),
128 KB payload, seed 5:

| `nsym` | `Rfec` | Frames | Uncorrectable | Goodput | Verified |
|--:|--:|--:|--:|--:|:--|
| **0 (off)** | 1.0000 | hit the 200,000 cap | — | — | **NO — never completes** |
| 16 | 0.9373 | 117 | 11 | 65.6 KB/s | yes |
| **32** | **0.8745** | **89** | **0** | **86.3 KB/s** | yes |
| 48 | 0.8118 | 112 | 0 | 68.6 KB/s | yes |
| 64 | 0.7490 | 241 | 0 | 31.9 KB/s | yes |

**Without intra-frame FEC the transfer never completes on this channel.** With it, 89 frames.

The curve also shows the code-rate trade-off in the raw: too little parity (16) leaves 11
uncorrectable frames for the fountain to re-send; too much (64) wastes over a quarter of every
frame on parity and needs 2.7× the frames. The optimum is somewhere near 32 **for this channel**
— and since the optimum is channel-dependent, this is precisely the knob the adaptive link
controller must select at runtime rather than a constant to bake in.

### Interleaving is not optional

Optical damage is spatially clustered — a glare spot, a fingerprint, a rolling-shutter
transition band. Without interleaving a burst lands inside one codeword and exceeds its budget
while its neighbours sit idle. Payload bytes are therefore scattered across all 10 codewords,
so a 300-byte contiguous burst becomes ~30 isolated erasures in each — comfortably inside a
32-byte budget. Tested directly (`SurvivesASpatialBurstThatWouldKillOneCodeword`), and the
scatter property is asserted on its own so a broken interleave reports as a broken interleave
rather than a mysterious FEC regression.

### A bug worth recording

The first erasure implementation passed the headline test (16 erasures at nsym=16) and failed
the pure-erasure cases below it. The cause: Berlekamp-Massey was consuming the Forney syndromes
from index 0, but the key equation `Λ_err·T ≡ Ω (mod x^nsym)` has `deg Ω < f+e`, so the first
`f` coefficients belong to Ω and carry no information about the unknown errors — feeding them
in invents a spurious error locator. The symptom was diagnostic: it worked **only** at
`f == nsym`, where the BM loop does not execute and the bug cannot express itself.

### Known limitation, recorded not hidden

Reed-Solomon works on bytes, so one unreadable cell condemns its whole byte — eight cells'
worth of budget spent on one. That promotion is lossy and is the price of a byte-oriented code.
A bit-level or non-binary code would not pay it (OQ-025, EXP-011).

---

## F19 — The soft information was saturated, and M0 barely needs it anyway `[HYP]`

**Found:** 2026-08-02, testing whether erasure-marking from LLR magnitude buys anything.

The project charter states *"soft symbol confidence should be preserved for forward-error
correction"*, and the whole `SoftSymbol`/LLR path exists to honour it. That hypothesis had
never been measured. It has two halves and both now have answers, neither of them the expected
one.

### Half one: the LLR was saturated — a real bug

Measured `|llr|` distribution over 37,392 cells, swept across noise amplitudes 0 → 120:

| Noise amplitude | Share in the top band (112–127) |
|--:|--:|
| 0 | **1.000** |
| 30 | **1.000** |
| 60 | **1.000** |
| 90 | **1.000** |
| 120 | **1.000** |

**Every cell, at every noise level, in one band.** The magnitude carried no information
whatsoever — the "soft" path was a one-bit signal with an erasure flag wearing a costume.

Cause: `QuantiseLlr` used a fixed scale of 32, mapping only `d ∈ [0,4]` sigma onto the whole
int8 range. With 3×3 subsampling the per-cell SNR is far higher than that, so everything
clamped. Saturation at high SNR is not itself wrong — a certain cell *is* certain — but
collapsing every distinguishable confidence into one value discards exactly the gradation the
FEC layer is meant to exploit.

**Fixed** by scaling against the *measured* clean-cell distance, so `|llr|` means "fraction of
the way to an unambiguous decision" relative to this frame's own separation and noise. The
distribution now responds to the channel (top band 0.929 → 0.866 across the sweep) where
before it was pinned at 1.000.

### Half two: M0 decisions are essentially never marginal

Even with the quantisation fixed, the **symbol error rate is 0.00000 at every noise level
tested, up to amplitude 120** — nearly half the full 0–255 range.

The reason is structural: the sampler averages 3×3 subsamples, each a bilinear interpolation
over ~4 pixels, so roughly 36 pixels back every cell and noise falls by about 6×. Binary
luminance then sits ~6σ from the threshold. There is no marginal decision for soft information
to disambiguate.

**Erasure-marking sweeps confirm it.** Converting low-confidence cells to erasures (which cost
half what errors cost) never helped:

| `--erase-below` | 0 | 4 | 8 | 16 | 24 | 32 |
|---|--:|--:|--:|--:|--:|--:|
| Goodput (KB/s) | **13.8** | 13.3 | 10.8 | 2.7 | 2.7 | 2.7 |

Zero is optimal. On the image path the threshold had *no effect at all* at any setting.

### What this means

**For M0, soft-decision FEC has little to offer.** Frame failures are **structural** —
occlusion, tracking loss, photometric failure, cells outside the frame — not decisional. Those
already produce erasures through the NaN path, which is where the value has been all along
(F18: intra-frame FEC with erasure positions is the difference between completing and not).

This tempers a stated architectural assumption rather than refuting it. The soft path should
matter considerably more for **M2 four-level luminance**, where adjacent levels sit at one
third the spacing and effective noise is ~3× worse — which is precisely when the fixed-scale
saturation would have hidden the benefit. Fixing the quantisation now means M2 can be evaluated
on its merits instead of against a broken confidence signal.

⚠ Simulator result, uncalibrated (RISK-024). The renderer models neither cell-to-cell crosstalk
beyond a box blur, nor fixed-pattern sensor noise, nor display non-uniformity at cell scale —
all of which would push real decisions closer to the threshold. The *mechanism* (36-pixel
averaging cuts noise ~6×) is real and will hold on hardware; the resulting error rate is
optimistic.

---

## F20 — The whole code-rate ladder can be scored from one run. The answer cannot be delivered `[HYP]`

**Found and built:** 2026-08-03, implementing the adaptive link controller (component C14,
features ADP-01 and ADP-02). Registered as EXP-023 before running.

F18 established that the optimal `nsym` is channel-dependent and closed with "this is precisely
the knob the adaptive link controller must select at runtime". Building that controller produced
one good surprise and one hard structural limit.

### The good surprise: the counterfactual is free

`IntraFec` derives its codeword count from the **frame capacity alone**
(`codewords = frame_capacity / codeword_bytes`); `nsym` shrinks only the message. So the
interleave mapping, the coded frame length and therefore the per-codeword damage pattern are
**all invariant in `nsym`**. A codeword decodes at `nsym = n` exactly when the budget it needs,
`2·errors + erasures`, is `≤ n` — and neither term depends on `n`.

Therefore **the per-frame worst codeword budget is a sufficient statistic for the entire
ladder.** One 256-bin histogram, one increment per frame, no allocation, scores every candidate
rung counterfactually from a run made at a single rung. The receiver never has to try a rung to
learn what it would have done, and C14 needs **no new per-frame measurement** — only a
subtraction over numbers the decoder already produces (F23).

**Measured (EXP-023, four channels, ladder `nsym ∈ {8,16,24,32,40,48,64}`):** on all three
channels that complete a verified transfer, the controller — seeing only receiver telemetry from
a single run at `nsym = 32` — named the brute-force goodput optimum exactly. **Rung distance 0,
three for three.**

| Channel | Brute-force optimum | Controller's pick | Rung distance | Cost of the worst rung |
|---|---|---|--:|--:|
| clean | 8 @ 120.0 KB/s | **8** | 0 | 50.0% |
| mild | 8 @ 112.9 KB/s | **8** | 0 | 66.8% |
| F18-impaired | 24 @ 86.3 KB/s | **24** | 0 | 93.2% |
| severe | none — no rung verifies | 40 | — | not scoreable |

⚠ Simulator, uncalibrated (RISK-024). This measures whether the **decision rule** is sound. It
says nothing about which `nsym` real hardware will want.

### The hard limit: there is nowhere to send the answer

**`nsym` cannot be changed mid-session against this protocol.** `nsym` sets the FEC message
size, which sets the fountain symbol size, which `FileManifest` fixes for the whole transfer —
and it must, because an XOR-based erasure code cannot combine symbols of unequal length
(`FountainDecoder::out_` is `k · symbol_size`). Every other capacity-changing knob — modulation
profile, grid — has exactly the same consequence.

So **C14 as the component registry specifies it — a mid-session control loop over profile and
code rate — cannot be implemented against the current protocol.** What can be implemented, and
now is, is the decision function: an estimator and a policy that name the right rung. Code-rate
selection is a **session-start** decision here, and on a one-way link the transmitter cannot
hear the receiver's answer at all (OQ-013).

This is recorded rather than worked around. The controller reports its recommendation and
`ffsim --adapt` prints an explicit note that the figure is **not actuated**, because a
"+7% headroom" line with no delivery path is exactly the kind of number that becomes a claim.

### A second, duller reason the control loop would not have mattered much

Even with actuation, **most of these transfers are shorter than the controller's own dwell.**
The EXP-023 runs complete in 64–241 display states; the promote dwell is 96 frames on top of a
32-frame observation minimum. So the hysteresis walk never moves at all on the clean and
impaired channels — correctly, since a 64-frame session offers no time to converge.

Two consequences. First, for transfers of this length **only the session-start choice matters**,
which is a second independent argument for treating rate selection as a negotiation problem
rather than a control problem. Second, the dwell constants were chosen for a *sustained* link
and are the wrong order of magnitude for a 128 KB transfer; they should be re-derived once a
realistic transfer duration is known, and until then a controller tuned for a long session
should not be assumed to do anything on a short one. `ffsim --adapt` now says so explicitly when
the walk does not move, so the recommendation and the walk cannot be misread as contradicting
each other.

### What the cost of guessing tells us about OQ-013

OQ-013 has been "the least-designed part of the architecture" without a number attached. Now it
has one: a fixed `nsym` chosen for the wrong channel costs **50%, 67% and 93%** of the available
goodput on the three channels above. H4 was registered at ">30%" and is comfortably confirmed.

That is a large enough penalty to say plainly: **the value of ever closing the loop is high**,
and reverse optical control (ADP-04, currently Phase 9) is worth more than its position
suggests. It is not yet an argument for moving it — these are uncalibrated simulator channels,
and a real link may sit in a broad plateau where any middle rung is fine. It *is* an argument
for measuring the spread on real captures early.

### The cheaper fix that comes first

Decoupling the fountain symbol size from the frame capacity — a **fixed, small symbol size with
an integer number of whole symbols per frame** — would make code rate adaptable within a session
without touching the fountain code's mathematics. It would also address the **~27% block padding
waste** F4 recorded, which has the same root cause: symbol size chosen to fill the frame. Raised
as OQ-037; it is a protocol change and belongs in an ADR if it is taken.

---

## F21 — The FEC telemetry was truncated exactly on the frames that mattered `[FACT]`

**Found:** 2026-08-03, building C14's consumer of `IntraFec::Stats`.

`worst_erasures_in_codeword` was accumulated **inside** the decode loop, after each codeword's
`ReedSolomon::Decode` call. So an early return on the first uncorrectable codeword left the
statistic reflecting only the codewords already processed — and **never the failing one**.

The bias is precisely backwards. A frame that failed is the only evidence that a stronger code
was needed, and it reported whatever load the earlier, healthier codewords happened to carry. A
frame with 4 erasures in codeword 0 and 60 in codeword 7 reported **4**.

**Fix:** compute the worst load across all codewords *before* attempting any decode. The loads
are all known at that point, so completeness costs nothing. Regression test asserts the failing
codeword's own load survives the early return (`StatsReportTheFailingCodewordsOwnLoad`).

**Worth remembering:** this is telemetry that was correct on the success path and absent on the
failure path, which is the one that matters. It survived F18's own measurements — the
`budget_used` accessor was written *for* the adaptive controller and documented as reporting
headroom — and was only exposed by building the consumer. **A field's first real reader is its
first real test.**

---

## F22 — The hysteresis margin and the ladder spacing are not independent `[FACT]`

**Found:** 2026-08-03, when C14 refused to leave `nsym = 24` on a channel with no errors at all.

"Fall back fast, promote slowly" (MODULATION-SPEC) asks for an asymmetric margin, and 10% for
the promote direction looks like an entirely reasonable hysteresis figure. It is not, and the
reason is arithmetic: adjacent rungs of the default ladder differ by only **3.35%** of frame
capacity at the dense end (`247/239 − 1`). A 10% margin therefore exceeds the gain any single
promotion can offer.

The resulting behaviour: on a **perfectly clean channel** the controller promoted from 32 to 24,
found every further step worth less than its own margin, and **parked there permanently** —
leaving ~6% of every frame unused for the rest of the session.

**This is a worse failure mode than oscillation, because nothing about it looks wrong.** No
error, no thrash, no warning; just a session running at less capacity than it had. The
registry's C14 entry names oscillation and optimising-the-wrong-metric as the failure modes to
watch. This is a third one, and it is the quiet one.

**Fix, in two parts.** The default `promote_margin` drops to 0.02, below the smallest single-rung
gain. And `LinkController::Create` now **rejects** a margin no promotion on its ladder could
clear (`kDegenerateParameters`), rather than leaving the trap set for whoever next tunes either
field. `SmallestPromotionGain()` is public so the constraint is inspectable instead of
folkloric. Test: `RejectsAPromoteMarginThatWouldStrandTheLadder`.

**Generalisable:** a hysteresis band is meaningless in isolation — it is only ever large or small
*relative to the step it gates*. Any tunable pair like this should be validated jointly at
construction, because the failure is silent and the two values are usually chosen by different
people at different times.

---

## F23 — Erasures are not what the code spends. The budget is `[HYP]`

**Found:** 2026-08-03, when C14's first version confidently recommended a rung that measurement
showed to be 24% worse.

The first estimator binned the worst per-codeword **erasure load**, reasoning that erasures are
what the receiver knows about — the same reasoning that makes F18's erasure decoding valuable.
On F18's impaired channel it reported the erasure load as **9 at every rung** (F20's invariance,
confirmed end to end), predicted a **1.0000** frame success rate at `nsym = 16`, and recommended
it.

**The real decoder failed 11 of 117 frames at `nsym = 16`**, and its measured goodput there is
**65.6 KB/s against 86.3 KB/s at the optimum — a 24% loss, chosen deliberately by a controller
that was certain.**

### Why

The exact per-codeword predicate is `2·errors + erasures ≤ nsym`. **Undetected errors cost two
parity bytes each**, and the erasure count omits them entirely. On this channel the worst
erasure load was 9 while the worst *budget* was **30** (mean 14, p95 19) — the missing term
dominated.

### The receiver knew more than it thought

Errors look unobservable by definition: if the receiver knew where they were, they would be
erasures. But they are recoverable **after** the fact. `ReedSolomon::Decode` returns the number
of symbols it changed, and a verified correction accounts for exactly the declared erasures plus
the errors its locator found — so `errors = corrected − erasures`. No new instrumentation, no
extra pass, no change to the decoder.

With `worst_budget_used` as the statistic, the controller recommends **24** on that channel: the
brute-force optimum, rung distance 0 (F20's table).

### The limitation, stated rather than buried

**A frame that does not decode cannot have its budget measured** — decoding is how the budget
gets measured. Such frames yield only the bound "more than the rung it was tried at", are
recorded at that floor, and are **counted** (`censored_observations`). The consequence is
directional and worth memorising: **an estimate taken at a rung that loses frames is trustworthy
for weakening the code and optimistic for strengthening it.**

The severe channel of EXP-023 shows this working as designed and failing as documented:
**28,497 of 40,000 observations censored**, every rung crediting the censored frames with
success, and the controller's answer correspondingly unvalidatable — which `ffsim --adapt` says
out loud rather than printing a clean-looking recommendation.

### A note on F19's scope

F19 measured a symbol error rate of **0.00000** up to noise amplitude 120 and concluded that M0
decisions are essentially never marginal. That measurement stands, but it was taken on the
**image path** with additive pixel noise. This channel is the **cell-sample path with crosstalk
0.20**, and it produces undetected symbol errors in quantity — roughly 10 per codeword in the
worst frame. F19's conclusion is narrower than its wording suggests: *pixel noise* does not
produce marginal M0 decisions; *inter-cell crosstalk* does. No retraction is needed, but anyone
citing F19 for "M0 has no errors" should read this alongside it.

---

## What has NOT been validated

To be explicit, since a working simulator invites over-confidence.

> **Corrected 2026-08-03.** The first two entries in this list had gone stale and were
> understating what exists — the geometric pipeline landed with F9/F10/F13/F17 and payload FEC
> with F18, but this section still said neither existed. That is exactly the
> documentation-versus-implementation divergence RISK-020 tracks and F10 was caught by, so it is
> corrected here rather than quietly rewritten. The superseded text, quoted in full because
> **this working tree is not under version control** and there is no history to recover it from:
>
> > - No geometric pipeline exists yet — no marker detection, homography, rectification or
> >   subpixel sampling. `sim` hands cell samples straight to the demodulator. **The hardest
> >   computer-vision problems are untouched.**
> > - No payload FEC. `Rfec` = 1.0 throughout; EXP-011 has not run.
>
> Both were true when written and both were false by 2026-08-02 (F9/F10/F17, F18).

**What now exists** (so the list below is not read as broader than it is): the full CV path —
detection, homography, tracking, subpixel sampling, photometric field (C06/C06a/C08/C09);
intra-frame FEC with errors-and-erasures decoding (C11, F18); the capture/replay harness proven
bit-identical to live decode (C17, F17); and the C14 estimator and code-rate policy (F20–F23).

**What still is not validated:**

- **No device code at all.** Every platform claim remains `[FACT]`-from-documentation, not
  measured. This is unchanged and remains the single most important gap.
- **The channel model is uncalibrated** (RISK-024). Its impairments are plausible, not
  validated. Every simulator number in this document is `[HYP]` for that reason.
- `Pc`, `Fd` and the density cliff — the model's highest-leverage unknowns — remain entirely
  unmeasured. F16 established that the cliff **cannot** be found with the current renderer.
- **No payload code family selected.** Intra-frame FEC exists, but EXP-011 has not run, so RS
  is a working default rather than a justified choice (OQ-009).
- **The GPU sampling path does not exist**, nor does the mandatory CPU/GPU equivalence test
  (C08, EXP-016).
- **C14 cannot actuate** (F20). The decision function is validated; the delivery path is
  blocked by the protocol, and on a one-way link by physics (OQ-013, OQ-037).
- **No thermal policy** (ADP-05, OQ-036): the decode-cost measurements that would justify one
  come from EXP-011 and EXP-020, and neither has run.
