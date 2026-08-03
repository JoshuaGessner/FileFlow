# FileFlow — a native screen-to-camera optical modem

FileFlow transfers arbitrary files from the **display of one Android phone** to the
**camera of another**, using no Wi-Fi, cellular, Bluetooth, NFC, USB, or external
accessory. The only channel is light: one phone renders a modulated optical frame
sequence, the other decodes it through its camera.

The goal is **maximum verified application-layer goodput** on hardware people already
own — replacing rapidly-changing QR-code streams with a purpose-built screen-to-camera
communication protocol.

> **Project status: Phase 1 — portable core implemented, simulated only.**
> *Updated 2026-08-03. The previous "Phase 0, there is no implementation yet" was left
> unchanged through the entire Phase 1 build and had become the most misleading sentence in
> the repository.*
>
> **What exists:** a complete portable C++20 decode chain (`core/`) that builds and runs with
> no Android toolchain — GF(256)/Reed–Solomon with errors-and-erasures decoding, LT fountain
> coding, intra-frame FEC, M0 binary luminance with soft symbols, the full CV path (screen
> detection, homography, tracking, subpixel sampling, photometric field), session manifest with
> SHA-256 verification, and an adaptive-code-rate controller. Plus a channel simulator
> (`sim/`), a capture/replay harness proven bit-identical to live decode (`harness/`),
> libFuzzer targets (`fuzz/`), 215 tests, and CI with 7 jobs.
>
> **What does not exist: any Android code, or any measurement on real hardware.**
> A file transfer verifies end-to-end under simulated perspective, jitter, blur, glare and
> falloff — through the real decode chain, but against a channel model that is **uncalibrated**
> (RISK-024). **No performance number anywhere in this repository is a measured result.** Every
> figure is a simulator output, a model output, or a citation, and is labelled as such.
> Progress is now gated on real captures, not on more simulation.

## The one metric that matters

```
goodput = verified original file payload bits / total transfer time
```

Seven different rates get confused in this problem space. FileFlow keeps them separate,
always, in every document and every log line:

| Metric | What it means |
|---|---|
| Display refresh rate | Hz the panel scans out |
| **Display state rate `Fd`** | **distinct optical frames actually presented per second — not the refresh rate** |
| Camera capture rate | fps the sensor delivers |
| Raw optical symbol rate | cells × distinct display states per second |
| Raw encoded bit rate | symbol rate × bits per symbol |
| Corrected decoder bit rate | after FEC overhead, losses and erasures |
| **Payload goodput** | **verified file bytes ÷ wall-clock transfer time** |

`Fd` is the one most easily conflated with the refresh rate, and it is bounded by the
*receiver*, not the transmitter. It has never been measured on hardware (OQ-029, EXP-006);
every goodput figure in this repository assumes a value for it and says so.

Any performance claim that does not name which of these it refers to is treated as a
documentation defect. See [docs/vision/PERFORMANCE-PHILOSOPHY.md](docs/vision/PERFORMANCE-PHILOSOPHY.md).

## Milestones

Every milestone is defined **on real hardware**. Simulation cannot satisfy any of them, so
nothing below is complete; the middle column says how far the simulated equivalent has got.

| # | Target | Simulated equivalent | On hardware |
|---|---|---|---|
| 1 | Reliable static-pattern detection and rectification | **Working** — worst cell-centre error < 0.25 cells, survives 20° yaw + 12° pitch | **Not started** |
| 2 | Reliable live binary packet transmission | **Working** — through the real decode chain | **Not started** |
| 3 | Reliable arbitrary-file transfer | **Working** — SHA-256 verified under noise, occlusion, glare, falloff and 10% frame loss | **Not started** |
| 4 | Sustained verified goodput > 200 KB/s on reference phones | — | **Not started** |
| 5 | Sustained verified goodput > 500 KB/s, controlled stationary setup | — | **Not started** |
| 6 | Investigate whether 120 Hz/120 fps devices can exceed 1 MB/s | — | Research target |

⚠ "Working" above means *in an uncalibrated simulator* (RISK-024), which models neither display
subpixel structure, sensor MTF, moiré nor diffraction. Finding F16 shows this matters: the
simulator reports **no density cliff** at any grid tested, which is exactly what a model
omitting those mechanisms is guaranteed to report — not evidence that none exists.

Milestones 4–6 are **research targets, not guarantees.** The first-order model in
[docs/specifications/PERFORMANCE-MODEL.md](docs/specifications/PERFORMANCE-MODEL.md)
indicates milestone 4 is *not* reachable with binary luminance at 60 display states/s
on any candidate grid — it requires multilevel modulation or a higher state rate.
That is a modelled conclusion awaiting measurement, not a finding.

## Architecture at a glance

```
Kotlin app shell  ──►  C++ NDK performance core
                        ├── TX: OpenGL ES / Vulkan full-screen symbol matrix
                        └── RX: Camera2 / NDK Camera, low-copy YUV or GPU texture
                              → screen tracker → rectifier → soft demodulator
                              → intra-frame FEC → fountain decoder → file verify
```

Full description: [docs/architecture/ARCHITECTURE-OVERVIEW.md](docs/architecture/ARCHITECTURE-OVERVIEW.md).

## Where to start reading

1. [docs/INDEX.md](docs/INDEX.md) — the map of every document
2. [docs/vision/PROJECT-VISION.md](docs/vision/PROJECT-VISION.md) — what this is and why
3. [docs/vision/TERMINOLOGY.md](docs/vision/TERMINOLOGY.md) — the vocabulary, read this early
4. [docs/architecture/ARCHITECTURE-OVERVIEW.md](docs/architecture/ARCHITECTURE-OVERVIEW.md)
5. [docs/planning/ROADMAP.md](docs/planning/ROADMAP.md) — what happens in what order
6. [docs/experiments/FIRST-THREE-EXPERIMENTS.md](docs/experiments/FIRST-THREE-EXPERIMENTS.md) — the immediate work

## Repository layout

```
core/            portable C++20 decode chain. NO Android headers — enforced by CI
  include/fileflow/  public headers
  src/               one file per component (flat; see ENGINEERING-PRACTICES §2)
  tests/             GoogleTest — runs on desktop, no device needed
sim/             channel simulator + optical renderer (ffsim)
harness/         capture-bundle writer and replay source (ffreplay)
fuzz/            libFuzzer targets — Linux CI only (Apple clang ships no libFuzzer, F5)
docs/            all design documentation (see docs/INDEX.md)
  vision/        objective, goals/non-goals, terminology, performance philosophy
  research/      literature review, platform research, prior-art matrix, bibliography
  architecture/  system overview, data flow, component registry
  adr/           architecture decision records
  specifications/ optical frame, modulation, protocol, performance model
  experiments/   experiment registry, open questions, PHASE1-FINDINGS (F1–F23)
  testing/       simulator plan, capture harness, benchmark methodology
  planning/      roadmap, feature registry, risk register, device matrix
  security/      threat model, input validation rules
tools/           grid_sweep.sh, adapt_sweep.sh, grid_fit.py, check_links.py
  perf_model/    reproducible first-order goodput calculator
data/
  experiments/   raw experimental results (never deleted, never rewritten)
```

There is **no** `platform/android/` or `app/` directory yet — no Android code exists.

## Building and running

```bash
cmake --preset desktop-release && cmake --build build/desktop-release -j8
./build/desktop-release/core/tests/fileflow_tests     # 215 tests

cmake --preset core-only && cmake --build build/core-only -j8   # enforces ADR-0010
python3 tools/check_links.py                                    # every doc link must resolve
```

Presets: `desktop-debug`, `desktop-release`, `desktop-asan`, `desktop-fuzz`, `core-only`.

```bash
# End-to-end transfer through the real decode chain, rendering real pixels
./build/desktop-release/sim/ffsim --payload 65536 --image-path --image-size 1200 2000

# Record a capture bundle, then replay it through the production decoder
./build/desktop-release/sim/ffsim --payload 16384 --image-path --record /tmp/bundle
./build/desktop-release/harness/ffreplay /tmp/bundle

./build/desktop-release/sim/ffsim --help
```

## Reproducing the numbers

```bash
python3 tools/perf_model/perf_model.py          # markdown tables
python3 tools/perf_model/perf_model.py --json   # machine-readable
tools/grid_sweep.sh                             # grid-density sweep (EXP-001 dry run, F16)
tools/adapt_sweep.sh                            # code-rate ladder + controller (EXP-023)
```

Every number in the performance model document is generated by that script. If you
change a parameter, regenerate the tables rather than editing them by hand. Experiment
results ship with the script that produced them (CONTRIBUTING rule 6).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The rules that matter most: never report a
target as an achievement, never delete an unfavourable result, and never let a design
decision live only in a chat log or a commit message.

## Licence

**Apache-2.0** — see [LICENSE](LICENSE). Decided in
[ADR-0013](docs/adr/ADR-0013-toolchain-and-build-system.md) (D4); OQ-021 is closed.
