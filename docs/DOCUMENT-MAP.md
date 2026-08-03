# Document map

> **Status:** Current
> **Owner:** C20 / documentation
> **Last reviewed:** 2026-08-03

Purpose, owner, status, review date and relationships for every document. Update this
whenever a document is added or materially changed (CONTRIBUTING.md).

**Status values:** `Draft` (written, not reviewed) · `Current` (reviewed, accurate) ·
`Stale` (needs review) · `Superseded`.

---

| Document | Purpose | Owner / subsystem | Status | Last reviewed | Related decisions | Related experiments | Implementation path |
|---|---|---|---|---|---|---|---|
| `README.md` | Entry point, milestones, layout | Project lead | Draft | 2026-08-02 | — | — | — |
| `CONTRIBUTING.md` | Working rules, evidence tags | Project lead | Draft | 2026-08-02 | — | — | — |
| `docs/INDEX.md` | Index of all documents | C20 | Current | 2026-08-02 | — | — | — |
| `docs/DOCUMENT-MAP.md` | This map | C20 | Current | 2026-08-02 | — | — | — |
| **Vision** | | | | | | | |
| `vision/PROJECT-VISION.md` | Problem, thesis, plausibility | Project lead | Draft | 2026-08-02 | ADR-0001, 0006, 0012 | — | — |
| `vision/GOALS-AND-NON-GOALS.md` | G1–G7, NG1–NG10 | Project lead | Draft | 2026-08-02 | ADR-0001, 0002, 0011 | — | — |
| `vision/TERMINOLOGY.md` | Shared vocabulary | Project lead | Draft | 2026-08-02 | ADR-0012 | — | all |
| `vision/PERFORMANCE-PHILOSOPHY.md` | Goodput discipline | Project lead | Draft | 2026-08-02 | ADR-0012 | all benchmarks | C15 |
| **Research** | | | | | | | |
| `research/RESEARCH-PLAN.md` | Method, gaps, RT tasks | Research | Draft | 2026-08-02 | — | RT-01…RT-08 | — |
| `research/BIBLIOGRAPHY.md` | Annotated sources | Research | Draft | 2026-08-02 | ADR-0006, 0009, 0012 | — | — |
| `research/PRIOR-ART-MATRIX.md` | Prior-system comparison | Research | Draft — gaps marked | 2026-08-02 | ADR-0006 | EXP-002 | — |
| `research/android-display-pipeline.md` | Display platform facts | TX | Draft | 2026-08-02 | ADR-0004 | EXP-006 | C03 |
| `research/android-camera-pipeline.md` | Camera platform facts | RX | Draft | 2026-08-02 | **ADR-0005** | EXP-007 | C05 |
| `research/coding-theory.md` | Channel and coding framing | FEC | Draft | 2026-08-02 | ADR-0009 | EXP-011, 012 | C11, C12 |
| `research/fec-library-evaluation.md` | Library candidates | FEC | Draft | 2026-08-02 | ADR-0009 | EXP-011, 012 | C11, C12 |
| `research/computer-vision.md` | CV methods | CV | Draft | 2026-08-02 | ADR-0006 | EXP-015, 016, 017 | C06, C08, C09 |
| `research/PAPER-ACCESS.md` | How to obtain primary sources legally; what ACM/IEEE are | Research | Draft | 2026-08-02 | — | — | — |
| **Architecture** | | | | | | | |
| `architecture/ARCHITECTURE-OVERVIEW.md` | System shape | Architecture | Draft | 2026-08-02 | ADR-0003, 0004, 0005, 0010 | — | all |
| `architecture/DATA-FLOW.md` | Flow and erasure propagation | Architecture | Draft | 2026-08-02 | ADR-0009, 0010 | — | all |
| `architecture/COMPONENT-REGISTRY.md` | C01–C20 specs (plus C06a) | Architecture | Draft | 2026-08-03 | all ADRs | all | all |
| `architecture/ENGINEERING-PRACTICES.md` | Toolchain, layout, testing, CI, decisions D1–D5 | Architecture | Draft | 2026-08-02 | ADR-0013 | — | all |
| **ADRs** | | | | | | | |
| `adr/README.md` | ADR index | Architecture | Current | 2026-08-02 | — | — | — |
| `adr/ADR-0001` | Android-first | Project lead | **Accepted** | 2026-08-02 | NG1 | — | C01 |
| `adr/ADR-0002` | Native not browser | Project lead | **Accepted** | 2026-08-02 | NG2 | — | C01 |
| `adr/ADR-0003` | Kotlin + C++ | Architecture | **Accepted** | 2026-08-02 | ADR-0010 | EXP-016 | all |
| `adr/ADR-0004` | GPU transmitter | TX | **Accepted** | 2026-08-02 | ADR-0001 | EXP-006 | C03 |
| `adr/ADR-0005` | Dual receive paths | RX | **Proposed** | 2026-08-02 | ADR-0003 | EXP-007 | C05, C08 |
| `adr/ADR-0006` | Grid not QR | Architecture | **Proposed** | 2026-08-02 | ADR-0007, 0008 | EXP-001, 002, 017 | C04, C06 |
| `adr/ADR-0007` | Binary baseline | Modulation | **Accepted** | 2026-08-02 | ADR-0008 | EXP-003 | C10 |
| `adr/ADR-0008` | Differential path | Modulation | **Proposed — low confidence** | 2026-08-02 | ADR-0007 | EXP-010 | C10 |
| `adr/ADR-0009` | FEC + fountain | FEC | **Proposed** | 2026-08-02 | — | EXP-011, 012 | C11, C12 |
| `adr/ADR-0010` | Simulator first | Testing | **Accepted** | 2026-08-02 | ADR-0003 | — | C16, C17 |
| `adr/ADR-0011` | Reference devices | Project lead | **Accepted** | 2026-08-02 | ADR-0001 | — | C02 |
| `adr/ADR-0012` | Goodput metric | Project lead | **Accepted** | 2026-08-02 | — | all benchmarks | C15 |
| `adr/ADR-0013` | Toolchain and build system | Architecture | **Proposed** | 2026-08-02 | ADR-0003, 0005, 0010 | — | `core/`, `platform/` |
| **Specifications** | | | | | | | |
| `specifications/OPTICAL-FRAME-CANDIDATES.md` | Three layouts, no winner | Frame design | Draft | 2026-08-02 | ADR-0006 | EXP-001, 013 | C04, C08 |
| `specifications/MODULATION-SPEC.md` | M0–M4 | Modulation | Draft | 2026-08-02 | ADR-0007, 0008 | EXP-010, 013, 014 | C10 |
| `specifications/PROTOCOL-SPEC.md` | Six-layer stack | Protocol | Draft | 2026-08-02 | ADR-0009 | — | C04, C11, C12, C13 |
| `specifications/PERFORMANCE-MODEL.md` | First-order model | Architecture | Draft | 2026-08-02 | ADR-0012 | all | `tools/perf_model/` |
| **Experiments** | | | | | | | |
| `experiments/EXPERIMENT-REGISTRY.md` | EXP-001–023 | Research | Draft — EXP-023 run; EXP-001 answered only as a null result (F16) | 2026-08-03 | all ADRs | — | `data/experiments/` |
| `experiments/OPEN-QUESTIONS.md` | OQ-001–037 | Project lead | Draft | 2026-08-03 | all ADRs | all | — |
| `experiments/PHASE1-FINDINGS.md` | **F1–F25 — the project's memory.** Design defects, one retraction (F14), every simulated rate, plus what the first CI run and the first SDK contact found | Architecture | Current | 2026-08-03 | ADR-0006, 0009, 0013 | EXP-001, 023 | `core/`, `sim/`, `harness/` |
| `experiments/FIRST-THREE-EXPERIMENTS.md` | Immediate work | Project lead | Draft | 2026-08-02 | ADR-0005, 0004, 0008 | EXP-007, 006, 010 | — |
| **Testing** | | | | | | | |
| `testing/SIMULATOR-PLAN.md` | Simulator design | Testing | Draft | 2026-08-02 | ADR-0010 | EXP-010, 011, 012 | C16 |
| `testing/CAPTURE-HARNESS.md` | Recording and replay | Testing | Draft | 2026-08-02 | ADR-0010 | all device experiments | C17 |
| `testing/BENCHMARK-METHODOLOGY.md` | How we measure | Benchmarking | Draft | 2026-08-02 | ADR-0012 | all | C15 |
| **Planning** | | | | | | | |
| `planning/ROADMAP.md` | Phases 0–10 | Project lead | Draft | 2026-08-02 | all ADRs | all | — |
| `planning/FEATURE-REGISTRY.md` | Feature registry | Project lead | Draft | 2026-08-02 | all ADRs | all | all |
| `planning/RISK-REGISTER.md` | RISK-001–025 | Project lead | Draft | 2026-08-02 | all ADRs | all | — |
| `planning/DEVICE-MATRIX.md` | Pixel 8 + Galaxy S26 Ultra: specs, per-device grids, capabilities to verify | Project lead | Draft | 2026-08-02 | ADR-0011 | EXP-003, 007, 018 | C02 |
| **Security** | | | | | | | |
| `security/THREAT-MODEL.md` | Threats T1–T10 | Security | Draft | 2026-08-02 | — | — | C13, protocol parsers |
| `security/INPUT-VALIDATION.md` | Validation rules | Security | Draft | 2026-08-02 | ADR-0003 | — | all parsers |

---

## Documents proposed but not yet written

| Document | Purpose | Blocked on |
|---|---|---|
| ~~`planning/DEVICE-MATRIX.md`~~ | **WRITTEN 2026-08-02** — Pixel 8 + Galaxy S26 Ultra | ~~OQ-033~~ closed; probe verification still pending |
| `specifications/FRAME-FORMAT-V1.md` | The *selected* frame layout, once EXP-013 decides | EXP-001, EXP-013 |
| `specifications/LINK-PROFILES.md` | The registry of concrete modulation profiles | Phase 5 |
| ~~`research/visual-mimo-capacity.md`~~ | **Not needed** — RT-03 closed by reading the three Visual MIMO papers directly into BIBLIOGRAPHY.md | — |
| `testing/CI-PLAN.md` | CI jobs: desktop build, sanitisers, vectors, link check, replay | Phase 1 |
| `security/SECURITY-REVIEW-P4.md` | Phase 4 security review record | Phase 4 |

## Maintenance

- **Every document** carries a status header with owner and last-reviewed date.
- **No design decision** may exist only in chat or a commit message.
- Documents contradicted by an experiment are **amended, not ignored** — and the ADR they
  support is amended or superseded.
- Automated link checking **exists**: `python3 tools/check_links.py` verifies every relative
  link in the docs tree and runs in CI (the `docs` job). Run it after moving a file or adding a
  document. It verifies that links **resolve**, not that content is **current** — the divergences
  found on 2026-08-03 all had perfectly valid links (RISK-020).
