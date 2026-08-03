# Contributing to FileFlow

FileFlow is a research-heavy systems project. Most of the risk is not in writing code —
it is in fooling ourselves about what the optical channel can actually do. These rules
exist to make self-deception hard.

## The non-negotiable rules

1. **Never report a target as an achievement.** "200 KB/s" is a milestone. Until a
   benchmark run produces it under a named methodology, it is written as a target.
2. **Never delete an unfavourable experimental result.** Negative results are the most
   valuable output of Phases 1–3. Mark them superseded; never remove them.
3. **Always name the metric.** Never write "we got 3 Mbps" — write "raw encoded bit
   rate 3 Mbps; corrected 1.9 Mbps; payload goodput 180 KB/s". See
   [docs/vision/PERFORMANCE-PHILOSOPHY.md](docs/vision/PERFORMANCE-PHILOSOPHY.md).
4. **No design decision lives only in chat or a commit message.** If it changes the
   architecture, it becomes an ADR. If it is uncertain, it becomes an open question
   and an experiment.
5. **Preserve raw benchmark data.** `data/experiments/<EXP-ID>/raw/` is append-only.
6. **All benchmark calculations must be reproducible.** Ship the script, not the number.

## Document discipline

Whenever you add or materially change a document:

- Update [docs/INDEX.md](docs/INDEX.md) and [docs/DOCUMENT-MAP.md](docs/DOCUMENT-MAP.md).
- Update the front-matter block (`Status`, `Last reviewed`, `Owner`).
- If it introduces an uncertainty, add it to
  [docs/experiments/OPEN-QUESTIONS.md](docs/experiments/OPEN-QUESTIONS.md).
- If it proposes a measurement, add it to
  [docs/experiments/EXPERIMENT-REGISTRY.md](docs/experiments/EXPERIMENT-REGISTRY.md).
- If it introduces a subsystem, add it to
  [docs/architecture/COMPONENT-REGISTRY.md](docs/architecture/COMPONENT-REGISTRY.md).
- If it introduces user-visible or engineering capability, add it to
  [docs/planning/FEATURE-REGISTRY.md](docs/planning/FEATURE-REGISTRY.md).

Every document carries this header:

```markdown
> **Status:** Draft | Proposed | Accepted | Superseded
> **Owner:** <subsystem>
> **Last reviewed:** YYYY-MM-DD
> **Related:** ADR-00NN, EXP-NNN, FEAT-XXX-NN
```

## Evidence discipline

Claims in this repository are tagged, and the tag is mandatory:

| Tag | Meaning |
|---|---|
| `[FACT]` | Verified against a primary source (platform docs, AOSP source, RFC, standard). Cite it. |
| `[LIT]` | Reported by published literature. Cite paper, venue, year. Note if only the abstract was available. |
| `[HYP]` | Our design hypothesis. Unvalidated. Must have a linked experiment. |
| `[OPEN]` | Unresolved question. Must appear in the open-question registry. |

Do not upgrade a tag without evidence. A `[HYP]` becomes a `[FACT]` only when an
experiment in `data/experiments/` supports it, and the experiment ID is cited.

## Citation format

```
Author(s). "Title." Venue, Year. URL (accessed YYYY-MM-DD).
Access note: full text | abstract only | secondary description
```

Distinguish peer-reviewed work from demonstrations, blog posts and news coverage.
Summarise in original language — do not copy substantial copyrighted text.

## Code rules

- **Kotlin** for the app shell, lifecycle, permissions, UI, storage.
- **C++ (NDK)** for the performance core: capture, tracking, demodulation, FEC.
- The C++ core must build and run **off-device** so the simulator and the recorded-frame
  harness exercise exactly the same decoder as the live receiver. This is a hard
  architectural constraint, not a convenience — see
  [ADR-0010](docs/adr/ADR-0010-simulator-before-optimization.md).
- Throwaway probes go in `tools/` with a header comment beginning `THROWAWAY:` and a
  one-line statement of the planning question they answer.
- No `malloc`/`new` in the per-frame decode path. Buffers are pooled and pre-sized.

## Untrusted input

Every byte arriving from the camera is attacker-controlled. Before writing any parser:
read [docs/security/INPUT-VALIDATION.md](docs/security/INPUT-VALIDATION.md). Bounds are
checked before allocation, never after.

## Experiment workflow

1. Add the entry to the experiment registry with a stable `EXP-NNN` ID, hypothesis and
   success threshold **before** running it.
2. Record the software commit and full device/environment metadata.
3. Write raw output to `data/experiments/EXP-NNN/raw/`.
4. Write processed output and the conclusion to `data/experiments/EXP-NNN/`.
5. Update the registry with the conclusion and a confidence level.
6. If the result contradicts an ADR, the ADR is amended or superseded — not ignored.
