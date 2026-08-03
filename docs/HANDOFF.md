# Session handoff prompt

Copy everything below the line into a fresh agent session working in `/Users/josh/dev/fileflow`.

---

You are the lead systems architect and research engineer for **FileFlow**, a native
phone-to-phone optical data-transfer project. The working directory is
`/Users/josh/dev/fileflow`. Substantial work already exists — read this whole prompt before
touching anything.

## The project

Transfer arbitrary files from the **display of one Android phone** to the **camera of another**
using no Wi-Fi, cellular, Bluetooth, NFC, USB or accessory. The only channel is light. The goal
is **maximum verified application-layer goodput**, replacing rapidly-changing QR streams with a
purpose-built screen-to-camera protocol.

Read these first, in order:

1. `docs/INDEX.md` — the map of all 53 documents
2. `docs/vision/PERFORMANCE-PHILOSOPHY.md` — the metric discipline, non-negotiable
3. `docs/experiments/PHASE1-FINDINGS.md` — **23 findings; this is the project's memory**
4. `CONTRIBUTING.md` — evidence tags and the rules below
5. `docs/architecture/COMPONENT-REGISTRY.md` — component status, C01–C20

## Operating rules — these are not style preferences

- **Never report a target as an achievement.** 200 KB/s is a milestone, not a result.
- **Never delete an unfavourable result.** F14 is a retraction kept in full because the
  retraction is more instructive than the finding was. Do the same.
- **Always name the metric.** Six different rates get confused in this domain (display refresh,
  display state rate `Fd`, camera fps, raw symbol rate, corrected bit rate, payload goodput).
  An unlabelled number is a documentation defect.
- **Tag every claim**: `[FACT]` (primary source, cite it), `[LIT]` (published, cite + access
  note), `[HYP]` (unvalidated, needs a linked experiment), `[OPEN]`.
- **Every simulator number is `[HYP]`.** The simulator is uncalibrated — RISK-024, and the
  second-highest risk in the register.
- **Measure, don't assume.** Several findings below exist because a plausible hypothesis was
  wrong. Where you can run the experiment, run it.
- No design decision may live only in a chat log. It goes in an ADR, a finding, or a registry.

## Build and verify

```bash
cmake --preset desktop-release && cmake --build build/desktop-release -j8
./build/desktop-release/core/tests/fileflow_tests          # must show 215 passing

cmake --preset desktop-asan   && cmake --build build/desktop-asan -j8
./build/desktop-asan/core/tests/fileflow_tests             # ASan+UBSan, must be clean

cmake --preset core-only      && cmake --build build/core-only -j8   # enforces ADR-0010
python3 tools/check_links.py                               # every link must resolve
```

Presets: `desktop-debug`, `desktop-release`, `desktop-asan`, `desktop-fuzz`, `core-only`.
CI has 7 jobs. **Apple clang ships no libFuzzer**, so the 5 fuzz targets run on Linux CI only;
there are deterministic in-suite equivalents for macOS.

Tools:
- `build/desktop-release/sim/ffsim` — channel simulator, `--help` for flags
- `build/desktop-release/harness/ffreplay` — replay a recorded capture bundle
- `tools/grid_sweep.sh`, `tools/grid_fit.py`, `tools/adapt_sweep.sh`,
  `tools/perf_model/perf_model.py`, `tools/check_links.py`

## What exists

**Portable C++20 in `core/`** — no Android headers, enforced by the `core-only` preset:
GF(256) + Reed–Solomon **with errors-and-erasures decoding**, LT fountain code, frame/header
codec, M0 binary luminance modulation with soft symbols, intra-frame FEC (interleaved RS),
session manifest + SHA-256 verification, the full CV path: homography (DLT + Hartley
normalisation), screen detection, frame-to-frame tracking, subpixel cell sampling, photometric
field with pilot-residual confidence — and the C14 adaptive link controller (`link.cpp`:
receiver-side channel estimation plus goodput-optimal code-rate selection with hysteresis,
reporting only — see F20).

`FramePipeline` (`core/src/pipeline.cpp`) is the **single shared image→symbols stage**. The
simulator, the replay harness and eventually the live receiver all drive it. Do not duplicate
it — if they drift, recorded datasets stop proving anything about live behaviour.

**`sim/`** — 3D viewing geometry (yaw/pitch/roll/distance), impairments, `ffsim --image-path`
runs the real geometry chain, `--record DIR` writes capture bundles.

**`harness/`** — capture bundles (`capture.meta` + raw `.gray` frames), `ReplaySource`,
`ffreplay`. Proven **bit-identical to live decode**.

A file transfer verifies end-to-end through the complete chain under perspective, jitter, blur,
glare and falloff.

## Findings that will save you time

Full detail in `docs/experiments/PHASE1-FINDINGS.md`. The ones that change how you work:

- **F14 is RETRACTED.** A "~6 px/cell detection floor" was an artifact of the F15 bug, and the
  corrected boundary turned out to be a `min_area_fraction` config threshold, not optics.
  **Do not plan grid geometry around any detection floor.**
- **F16: EXP-001 cannot be answered in simulation.** The grid sweep found no density cliff down
  to 5.00 px/cell — because the renderer models neither display subpixel structure, sensor MTF,
  moiré nor diffraction, which are the mechanisms that *cause* a cliff. A simulator omitting
  that physics is guaranteed to report no cliff. **This needs real captures.**
- **F15: localisation must never depend on payload content.** It did once (an area test on
  lit-pixel count), and zero-padded frames became undetectable. Geometry reads the boundary
  ring and markers only.
- **F18: intra-frame FEC is load-bearing, not an optimisation.** Without it the transfer does
  not complete on an impaired channel at all. Erasure decoding is where the value is — RS
  corrects `nsym` erasures but only `nsym/2` errors, and the receiver already knows which cells
  it could not read.
- **F19: soft-decision input buys nothing for M0.** Symbol error rate is 0.00000 up to noise
  amplitude 120; failures are structural (occlusion, tracking, photometric), not decisional.
  Expect this to change for M2, where levels sit at one third the spacing.
- **F13: the tracking saving is a property of the search region, not of tracking.** A
  bounding-box window saved nothing when the screen filled the frame; a boundary annulus fixed
  it. Measured 1.93× decoder throughput at identical decode quality.
- **F20: the code-rate ladder is free to evaluate, and impossible to act on.** `IntraFec`'s
  codeword count depends on frame capacity alone, so per-codeword damage is invariant in `nsym`
  and one histogram scores every rung counterfactually. But `nsym` fixes the fountain symbol
  size, which the manifest fixes for the session — so **rate adaptation is a session-start
  decision, not a control loop** (OQ-037 proposes the fix). Do not plan a mid-session profile
  loop until that is resolved.
- **F23: erasures are not what the code spends — the budget `2·errors + erasures` is.** An
  erasure-only estimator predicted a 100% frame success rate at `nsym = 16` on a channel where
  the decoder failed 11 of 117 frames, and cost 24% of the goodput. Errors are recoverable after
  the fact as `corrected − erasures`. Also read this next to F19: *pixel noise* does not produce
  marginal M0 decisions, but *crosstalk* does.
- **F22: a hysteresis margin is meaningless without its ladder.** A 10% promote margin over
  rungs 3.35% apart does not slow adaptation, it stops it permanently and silently. Validate the
  pair, never either alone.

## Reference hardware

**Google Pixel 8** and **Samsung Galaxy S26 Ultra** (`docs/planning/DEVICE-MATRIX.md`).

- Cell pitch must be an **integer number of panel pixels**, and each charter grid is integer on
  exactly one of the two devices: 120×200 suits the Pixel 8 (9×12 px), 144×240 suits the S26
  Ultra (10×13 px). The grid is therefore device-dependent and negotiated in the frame header.
- **RISK-025**: the S26 Ultra has a hardware Privacy Display (Black Matrix). It must be off, and
  press reporting says viewing angles are reduced even when disabled. At close range this can
  produce a *transmitter-side* radial luminance falloff that mimics vignetting.
- Samsung may restrict ≥60 fps capture for third-party apps (OQ-035, unverified). If true, the
  Pixel 8 is the only viable high-frame-rate receiver and milestone 6 rests on it.

## The single most important constraint

**No Android code exists yet.** Everything is portable C++ running on desktop. The project has
reached the point where progress is gated on **real captures**, not on more simulation.

The shortest path to real data is: (1) a Gradle/NDK project skeleton, (2) a **capability probe**
that verifies rather than trusts Camera2 characteristics, (3) a **camera recorder** writing the
capture-bundle format `ffreplay` already consumes. The moment (3) works, every analysis tool
built so far applies to real data unchanged — no transmitter, UI or live link required.

**Building the Android project is a large structural addition (Gradle, NDK toolchain, manifest,
permissions) and the user has not yet given a go-ahead. Ask before starting it.**

## Suggested next work, in value order

1. ~~**Adaptive link controller (C14).**~~ **DONE 2026-08-03** — `core/src/link.cpp`,
   `ChannelEstimator` + `LinkController`, EXP-023, findings F20–F23. It picks the brute-force
   optimum exactly on every simulated channel that completes. **It cannot actuate**, and that is
   the finding, not a gap in the implementation: read F20 before touching adaptation again.
2. **OQ-037 — decouple the fountain symbol size from frame capacity.** This is now the gating
   item for all of adaptation, and it also fixes F4's ~27% block-padding waste. A fixed small
   symbol size carrying an integer number of whole symbols per frame. Touches `transfer.cpp`,
   the frame header and `ffsim`; it is a protocol change, so it needs an ADR and a version bump.
   **Highest value of the remaining simulator work**, because two separate findings point at it.
3. **M1 differential modulation.** ADR-0008 is the project's lowest-confidence decision and
   EXP-010 runs entirely in the simulator — it can be validated or killed cheaply.
4. **M2 four-level luminance.** F19 predicts this is where soft information starts to matter,
   and F23 sharpens the prediction: crosstalk already produces real symbol errors on the
   cell-sample path, and M2's levels sit at one third the spacing.
5. **Android shell** — ask first. Still the only path to the measurements that matter
   (`Pc`, `Fd`, the density cliff), and still gated on a go-ahead.

## What to tell the user

Report honestly, including negative results and retractions. If you find that something
previously recorded is wrong, correct it *and* say so plainly — that has happened twice already
(F14 retracted, and a "tracking is more reliable" conclusion in F15 that turned out to be a bug
artifact). Distinguish measured results from model outputs every time.

37 open questions are tracked in `docs/experiments/OPEN-QUESTIONS.md`. Add to it rather than
resolving uncertainty by assumption.
