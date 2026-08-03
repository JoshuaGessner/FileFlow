# ADR-0013 — Toolchain, build system and repository layout

> **Status:** Accepted — D1–D5 resolved 2026-08-02; validated by Phase 1 scaffolding
> **Date:** 2026-08-02
> **Owner:** Architecture
> **Related:** ADR-0003, ADR-0005, ADR-0010, ENGINEERING-PRACTICES.md

## Context

ADR-0003 chose Kotlin + C++ and ADR-0010 requires the decode chain to build off-device.
Neither specified the concrete toolchain, build system or repository structure. Those
choices are cheap now and expensive after Phase 1 has code in it.

## Decision

| Element | Choice |
|---|---|
| NDK | r29 (or latest stable) |
| C++ standard | **C++20** (not C++23 — unreliable in NDK libc++) |
| Min SDK | 33 (FrameTimeline requirement, ADR-0004) |
| ABI | arm64-v8a only initially |
| Build system | CMake 3.22+ with `CMakePresets.json`; Gradle `externalNativeBuild` consumes the same `CMakeLists.txt` |
| Dependencies | CMake `FetchContent`, pinned by commit |
| Native libraries | **Exactly one `.so` (`libfileflow.so`) with `c++_static`** |
| Camera API | **Camera2 / NDK Camera. CameraX is disqualified** |
| Unit tests | GoogleTest, running on desktop *and* device |
| Memory safety | ASan + UBSan on desktop CI; **HWASan** on device |
| Fuzzing | libFuzzer targets, continuous in CI |
| Repo layout | `core/` (no Android headers) + `platform/android/` adapters + `sim/`, `harness/`, `app/` |

## Alternatives

1. **ndk-build instead of CMake.** Rejected — CMake is the documented path, supports
   desktop builds natively, and `CMakePresets.json` handles the multi-target case cleanly.
2. **C++23.** Rejected — CMake compiler-support checks fail against NDK libc++
   `[LIT — NDK issue tracker]`. `std::expected` would have been welcome; we use a small
   in-house `Result<T, E>` instead.
3. **Multiple native libraries.** Rejected, with prejudice — see consequences.
4. **vcpkg or Conan.** Deferred. Better at scale; unnecessary for our small dependency set
   and adds a moving part to the NDK build.
5. **CameraX.** Rejected on evidence — see below.
6. **Rust core.** Open decision D1; see ENGINEERING-PRACTICES §10.

## Consequences

**Positive.** One build system across desktop and Android. `CMakePresets` makes the
sanitiser and fuzzing configurations first-class rather than ad-hoc. The `core/` boundary
is structurally enforced by a CI job that builds it with no Android toolchain present —
ADR-0010's requirement becomes a build failure rather than a convention.

**Negative.** Min SDK 33 excludes older devices (accepted under ADR-0011). arm64-only means
no emulator testing of native code initially — acceptable, since the emulator has neither
a real camera nor a real display pipeline and is useless for our actual problem.

**The single-`.so` rule is load-bearing.** NDK documentation is explicit that `c++_static`
across multiple shared libraries violates the ODR and produces memory corruption when
memory is allocated in one library and freed in another, uncaught exceptions across library
boundaries, and duplicated STL code `[FACT]`. Our chosen configuration is safe *only*
while there is one native library.

> **Rule:** adding a second `.so` requires switching the entire project to `c++_shared`
> **in the same change**. This is not a preference.

## Evidence

- NDK r29 (29.0.14206865) released October 2025; current stable. `[FACT]`
- C++20 supported by NDK libc++; **C++23 fails CMake compiler-support checks**.
  `[LIT — NDK issue tracker discussion]`
- `ANDROID_STL` defaults to `c++_static`; NDK docs caution this "is not appropriate for all
  applications" and detail the multi-library corruption modes. `[FACT]`
- AGP defaults to CMake 3.10.2 when unset — we must pin a version explicitly. `[FACT]`
- **ASan has been unsupported on Android since 2023; HWASan is the recommended
  replacement.** ARM64-only; Android 14 (API 34)+ on stock devices, or Pixel on API 29+
  with a special system image; ~15% RAM overhead. `[FACT]`
- **CameraX disqualification:** no constrained-high-speed-session support (blocking our
  ≥120 fps path); `Camera2Interop` manual-control changes close and reopen the camera
  `[LIT — camerax-developers group]`, which would drop frames mid-transfer and directly
  reduce `Pc`.

## Risks

| Risk | Mitigation |
|---|---|
| Someone adds a second `.so` without switching STL → memory corruption | This ADR; a CI check on the number of built shared libraries |
| Determinism lost to floating point, breaking golden vectors | Decide fixed-point vs documented tolerance per stage, early |
| HWASan device requirements (ARM64, API 34+) not met by reference hardware | Confirm during device selection (OQ-033); desktop ASan covers most cases regardless |
| `core/` accidentally acquires an Android dependency | The `core-desktop` CI job fails immediately |
| C++20 feature gaps in NDK libc++ discovered late | Verify the specific features we rely on during Phase 1 scaffolding |

## Validation plan

Phase 1 scaffolding validates this ADR directly — if `core/` builds and tests clean on
desktop with no Android toolchain, and the same sources build under the NDK, the structure
works. Any friction discovered there should amend this ADR rather than accumulate as
workarounds.

## Decisions resolved 2026-08-02

| ⬥ | Resolution |
|---|---|
| **D1 — core language** | **C++20, no special hedge.** The hardened-parser variant (protocol layer free of pointer arithmetic as a Rust-rewrite candidate) was offered and **not** taken. Consequence: sanitisers and fuzzing carry the FULL weight on the attacker-facing surface, so that tooling is present from the first commit rather than retrofitted — ASan/UBSan in CI, four libFuzzer targets, `bugprone-*`/`cert-*`/`cppcoreguidelines-*` in clang-tidy, and a bounds-checked `ByteReader` that all parsers must go through. |
| **D2 — simulator harness** | **Pure C++.** `sim/` links the production `core/` decode chain directly; no Python/nanobind binding layer. Revisit only if exploratory analysis in Phase 2 proves painful. |
| **D3 — one `.so` + `c++_static`** | Confirmed. |
| **D4 — licence** | **Apache-2.0.** Patent grant included, compatible with the permissive libraries under consideration. Closes OQ-021. Makes the RaptorQ patent question (RISK-016) worth resolving properly, since distribution is public. |
| **D5 — hosting/CI** | GitHub + Actions. |

## Validation status (Phase 1 scaffolding, 2026-08-02)

The scaffolding exercised this ADR and it held:

- `core/` builds standalone with no Android toolchain; a CI job plus a grep gate enforce it.
- C++20 sufficed; no C++23 feature was missed except `std::expected`, replaced by the
  in-house `Result<T, E>` as planned.
- ASan + UBSan run clean over the full suite (92 tests).
- **Deviation found:** Apple clang does **not** ship `libclang_rt.fuzzer`, so libFuzzer
  targets cannot be built with the macOS system toolchain. They are opt-in
  (`FILEFLOW_BUILD_FUZZ`, default OFF) and run on Linux in CI. Local fuzzing on macOS needs
  Homebrew LLVM. This is a real constraint on the D1 decision's mitigation story and is
  worth knowing before relying on local fuzz runs.
