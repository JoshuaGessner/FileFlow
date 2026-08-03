# ADR-0003 — Kotlin application shell with a C++ NDK performance core

> **Status:** Accepted
> **Date:** 2026-08-02
> **Owner:** Architecture
> **Related:** ADR-0010, ARCHITECTURE-OVERVIEW.md

## Context

The decode chain processes ~34,560 cells per frame at 60–120 frames per second — on the
order of 2–4 million cell samples per second, each requiring gather, normalise, threshold
and LLR computation, followed by iterative FEC decoding. This must run within a fraction
of a mobile core's budget, under thermal pressure, without allocating.

Separately, the simulator and replay harness must run the **same decoder** off-device
(ADR-0010), which rules out any Android-coupled implementation for the decode chain.

## Decision

**Kotlin** for the application shell: lifecycle, permissions, file selection, storage, UI,
session orchestration.
**C++ (NDK)** for the performance core: capture adaptation, tracking, sampling,
demodulation, FEC, fountain, reconstruction.
The **JNI boundary carries session control and progress only — never per-frame data.**

## Alternatives

1. **All Kotlin/JVM.** Rejected: GC pauses on a real-time capture path are unacceptable,
   no direct SIMD, and the decoder could not run off-device without a JVM. The last point
   alone is disqualifying given ADR-0010.
2. **All C++ with a minimal Java activity.** Rejected: fights the Android platform for
   lifecycle, permissions and storage with no benefit — those parts are not hot.
3. **Rust core instead of C++.** Genuinely attractive — memory safety matters a great deal
   here because we parse attacker-controlled input (G6). Rejected for now on NDK
   integration friction, GPU/graphics interop maturity, and team familiarity. **This is
   the alternative most worth revisiting**, and the decision is weaker than it looks: if
   the security review in Phase 4 finds the parser surface hard to make safe in C++, Rust
   for the protocol-parsing layer specifically would be a reasonable partial adoption.
4. **Kotlin Multiplatform for the core.** Rejected: performance characteristics and
   off-device story are both worse than C++.

## Consequences

**Positive.** Predictable performance, no GC on the hot path, NEON intrinsics, direct
buffer access, and a decode chain that compiles for desktop unchanged.

**Negative.** Two languages, two build systems, JNI boilerplate, and **manual memory
safety in exactly the code that parses untrusted input.** This is the real cost of the
decision and it is paid in review discipline and fuzzing
(see [INPUT-VALIDATION.md](../security/INPUT-VALIDATION.md)).

**Neutral.** The narrow JNI boundary is a design constraint that also happens to make
testing easier — the core is exercised without an emulator.

## Evidence

- Per-frame workload estimate above is arithmetic from the candidate grids, not a
  measurement. `[HYP]` — EXP-016 measures actual sampling cost.
- The off-device requirement is a direct consequence of ADR-0010, which is a process
  decision we hold with high confidence.
- No prototype was built to confirm that Kotlin/JVM would in fact be too slow. The
  argument rests on GC pause behaviour on a real-time path, which is well-established
  generally but unmeasured here. `[OPEN]`

## Risks

| Risk | Mitigation |
|---|---|
| Memory-safety defects in untrusted-input parsing | Strict bounds discipline, fuzzing from Phase 3, ASan/UBSan in CI, consider Rust for the parser |
| JNI boundary accidentally becomes hot | Architectural rule + a CI check that no per-frame call crosses it |
| Two-language build complexity | CMake + Gradle set up once, early, before there is much code |

## Validation plan

- EXP-016 measures cell-sampling throughput on ARM64 (CPU/SIMD versus GPU).
- If measured decode cost fits comfortably within budget, this decision is over-engineered
  but harmless. If it does not fit even in C++, the problem is the algorithm, not the
  language.
