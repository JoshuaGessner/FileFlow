# Engineering practices and toolchain

> **Status:** Draft — proposal, pending decisions marked ⬥
> **Owner:** Architecture
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0003, ADR-0010, ADR-0013, INPUT-VALIDATION.md

Research-backed proposal for how this project is actually built. Every recommendation
names its evidence; open decisions are marked ⬥ and listed at the end.

---

## 1. Toolchain baseline

| Choice | Value | Evidence |
|---|---|---|
| NDK | **r29** (29.0.14206865, Oct 2025) or latest stable | Current stable; updated LLVM, fixed lldb/ndk-stack, libc++ fixes `[FACT]` |
| C++ standard | **C++20** | Supported by NDK libc++. **C++23 is not reliably supported** — CMake compiler checks fail `[LIT — NDK issue tracker]`. Do not target it |
| Min SDK | **33** (Android 13) | FrameTimeline APIs are API 33 `[FACT]` — ADR-0004 |
| Target/compile SDK | Latest stable | Standard practice |
| ABI | **arm64-v8a only** initially | Reference-device-first (ADR-0011). HWASan is ARM64-only anyway. Adding `x86_64` for emulator work is cheap if wanted |
| CMake | **3.22+** | CMakePresets support (3.20+) with margin. AGP defaults to 3.10.2 if unset — **we must set it explicitly** |

### The STL decision — a real trap

NDK documentation is explicit `[FACT]`: using `c++_static` across **multiple** shared
libraries violates the One Definition Rule and causes *memory corruption* (allocate in one
`.so`, free in another), uncaught exceptions crossing library boundaries, and duplicated
STL code.

| Scenario | Correct choice |
|---|---|
| One shared library | `c++_static` — smallest, fastest |
| More than one shared library | `libc++_shared.so` — mandatory |

**Recommendation: ship exactly one native library, `libfileflow.so`, and use `c++_static`.**

This becomes an architectural rule, not a build setting: **adding a second `.so` requires
switching the whole project to `c++_shared` in the same change.** Recorded in ADR-0013 so
nobody adds a second native library casually two years from now.

---

## 2. Repository layout

Designed around the hard constraint from ADR-0010: **the decode chain must build and run
off-device**, so the simulator and replay harness exercise the production decoder.

```
fileflow/
├── core/                        # C++20. NO Android headers. Builds standalone.
│   ├── CMakeLists.txt
│   ├── include/fileflow/        # public headers
│   ├── src/
│   │   ├── frame/               # C04 optical frame generator + parser
│   │   ├── cv/                  # C06 tracker, C08 sampler, C09 photometric
│   │   ├── phase/               # C07 frame-phase classifier
│   │   ├── modulation/          # C10 soft demodulator
│   │   ├── fec/                 # C11
│   │   ├── fountain/            # C12
│   │   ├── protocol/            # session/file layers — THE UNTRUSTED PARSER SURFACE
│   │   ├── file/                # C13 reconstruction + verification
│   │   ├── adapt/               # C14 link controller
│   │   └── telemetry/           # C15
│   └── tests/                   # GoogleTest, runs on desktop AND device
│
├── platform/android/            # thin Android adapters ONLY
│   ├── capture/                 # C05 Camera2/NDK camera → CaptureSource
│   ├── render/                  # C03 GLES renderer
│   └── jni/                     # narrow session-control boundary
│
├── sim/                         # C16 simulator — desktop only
├── harness/                     # C17 replay harness — desktop + device
├── app/                         # Kotlin application shell (C01)
├── tools/
│   ├── perf_model/              # existing
│   └── vectors/                 # C18 test-vector generator
├── fuzz/                        # libFuzzer targets
├── data/experiments/            # append-only raw results
├── docs/
├── CMakeLists.txt               # desktop entry point
└── CMakePresets.json            # desktop-debug, desktop-asan, android-arm64, …
```

> **Status 2026-08-03: the tree above is the PROPOSAL. `core/src/` is actually FLAT.**
> Recorded because a layout diagram that quietly disagrees with the repository is the same
> documentation-versus-implementation divergence RISK-020 tracks, and F10 was caught by. None of
> the `frame/`, `cv/`, `phase/`, `modulation/`, `fec/`, `fountain/`, `protocol/`, `file/`,
> `adapt/` or `telemetry/` subdirectories exist. What exists is one file per component:
>
> ```
> core/src/   detect.cpp fountain.cpp frame.cpp geometry.cpp gf256.cpp grid.cpp hash.cpp
>             intra_fec.cpp link.cpp modulation.cpp photometric.cpp pipeline.cpp
>             result.cpp sampler.cpp tracker.cpp transfer.cpp        (16 files, 0 subdirs)
> ```
>
> Sixteen files is not enough to need directories, and the flat layout has held fine through
> the whole CV path and C14. **The proposal is not withdrawn** — it is a reasonable target if
> `core/src/` grows past ~25 files — but until then the tree above should be read as intent, not
> as a map. In particular `adapt/` was never created: C14 lives in `core/src/link.cpp`. See the
> matching note under D1 about `protocol/`, which was the first instance of this.

**The rule that makes this work:** `core/` has no `#include <android/...>` and no JNI. A CI
job builds `core/` for desktop with `-Werror` and no Android toolchain at all. If that job
passes, the constraint holds. If someone breaks it, CI says so the same day.

`CaptureSource` is implemented three times — Android camera, simulator, replay harness —
and `core/` depends only on the interface.

---

## 3. Build system

**CMake + CMakePresets.json** as the single source of truth; Gradle's
`externalNativeBuild` points at the same `CMakeLists.txt`.

Presets to define:

| Preset | Purpose |
|---|---|
| `desktop-debug` | Fast iteration, assertions on |
| `desktop-asan` | ASan + UBSan — the primary memory-safety gate |
| `desktop-release` | Simulator sweeps (needs to be fast) |
| `desktop-fuzz` | `-fsanitize=fuzzer,address,undefined` |
| `android-arm64-debug` / `-release` | Device builds |

Dependencies via **CMake `FetchContent`** (GoogleTest, and any small library we adopt).
Rationale: no extra package manager to install, pinned by commit hash, works identically
on desktop and in the NDK build. vcpkg/Conan are better at scale but add a moving part we
do not yet need.

---

## 4. Testing strategy

Four layers, each catching what the others cannot:

| Layer | Tool | Runs where | Gate |
|---|---|---|---|
| **Unit** | GoogleTest | Desktop + device | Every commit |
| **Golden vectors** | GoogleTest + committed vectors (C18) | Desktop + device, **must match byte-for-byte** | Every commit |
| **Simulator regression** | `sim` sweeps with fixed seeds | Desktop | Every commit (fast subset), nightly (full sweep) |
| **Replay regression** | `harness` over recorded captures | Desktop | Every commit once captures exist |

### Determinism is a hard requirement
Same seed + same commit ⇒ byte-identical output, on desktop *and* device. This is what
makes simulator and replay regression testing meaningful at all.

Consequence: **floating point in the decode path is a determinism hazard.** Where FP
affects vector output, either use fixed-point or document an explicit tolerance. Decide
this per stage, early — retrofitting determinism is painful.

### Cross-implementation equivalence
ADR-0005 gives us two cell samplers (CPU/NEON and GPU shader). A test asserting their
outputs match within tolerance is **mandatory and non-negotiable** — it is the only thing
preventing the two paths from silently diverging.

---

## 5. Memory safety

We are parsing attacker-controlled input in a non-memory-safe language (G6, RISK-009). The
tooling must be aggressive.

| Tool | Where | Notes |
|---|---|---|
| **ASan + UBSan** | Desktop CI, every commit | Cheap, catches most of it, no device needed |
| **HWASan** | On-device testing | **ASan on Android has been unsupported since 2023; HWASan is the recommended replacement** `[FACT]`. ARM64-only. Android 14 (API 34)+ on stock devices, or Pixel on API 29+ with a special system image. ~15% RAM overhead — much lighter than ASan |
| **libFuzzer** | `fuzz/` targets, CI | Continuous, not one-off. Corpus cached between runs |
| **Static analysis** | clang-tidy | `bugprone-*`, `cert-*`, `cppcoreguidelines-*` |

**Fuzz targets, in priority order** (all reachable from untrusted optical input):
frame header parser → session header parser → TLV extension parser → FEC parameter
handling → fountain metadata → file reassembly.

Malformed-input vectors from C18 seed the fuzzing corpus, so the two efforts compound.

**ClusterFuzzLite** is the standard pattern for continuous fuzzing in CI if we want more
than time-boxed runs.

---

## 6. Camera API — CameraX is disqualified

Worth recording explicitly, because CameraX is the default recommendation for Android
camera work and someone will propose it:

1. **CameraX has no constrained-high-speed-session support.** Our ≥120 fps path
   (ADR-0005, milestone 6) is unreachable through it.
2. **`Camera2Interop` for manual controls closes and reopens the camera** `[LIT —
   camerax-developers group]`. Mid-transfer exposure or focus changes would drop frames,
   which directly costs `Pc`.
3. CameraX's use-case abstraction sits between us and the low-copy buffer access we need.

**Camera2 / NDK Camera is confirmed**, not merely preferred. This strengthens ADR-0005.

---

## 7. Kotlin/JNI boundary

The architectural rule (ARCHITECTURE-OVERVIEW): **no per-frame data crosses JNI.**

Enforcement proposal:
- All JNI entry points in one file, `platform/android/jni/`, reviewed as a unit.
- A CI check (or a documented review item) that the JNI surface stays small — if the number
  of entry points grows, something is leaking across the boundary.
- Session control and progress only. Progress updates at UI rate (~10 Hz), never per frame.
- Native code owns its memory; Kotlin holds an opaque handle.

---

## 8. CI

| Job | Trigger | Purpose |
|---|---|---|
| `core-desktop` | every push | Build `core/` **with no Android toolchain**. Enforces ADR-0010 structurally |
| `core-asan` | every push | ASan + UBSan unit tests |
| `vectors` | every push | Golden vectors match byte-for-byte |
| `sim-smoke` | every push | Fast simulator sweep, fixed seeds |
| `android-build` | every push | NDK build + Gradle assemble |
| `clang-tidy` | every push | Static analysis |
| `fuzz-smoke` | every push | Time-boxed libFuzzer run, cached corpus |
| `link-check` | every push | Documentation links resolve (DOC-04) |
| `sim-full` | nightly | Full parameter sweeps → `data/experiments/` |
| `replay` | every push (once captures exist) | Recorded-capture regression |

GitHub Actions with Gradle caching and NDK pinning is the conventional setup. Device
testing needs either self-hosted runners with real hardware or manual runs — **the emulator
is useless to us**, since it has neither a real camera nor a real display pipeline.

---

## 9. Code conventions

- **No allocation in the per-frame path.** Pools sized at session start. Enforceable in
  debug builds with an allocation-tracking hook that asserts on the decode thread.
- **Checked arithmetic** for all size/offset computation (INPUT-VALIDATION.md).
- **No VLA, no `alloca`** on any path reachable from parsed input.
- **`[[nodiscard]]`** on every function returning a status or error.
- Error handling: `std::expected`-style result types (C++23's `std::expected` is
  unavailable — use a small in-house `Result<T, E>`). **No exceptions across the JNI
  boundary**, ever.
- `clang-format`, enforced in CI.
- Throwaway code lives in `tools/` with a `THROWAWAY:` header comment (CONTRIBUTING).

---

## 10. Open decisions ⬥

| ⬥ | Decision | Recommendation | Why it matters now |
|---|---|---|---|
| D1 | Core language: **C++20** or **Rust** | See below | Effectively irreversible after Phase 1 |
| D2 | Simulator: pure C++, or C++ core + Python/NumPy harness | C++ core + thin Python harness via **nanobind** | Sweep ergonomics vs. one language |
| D3 | Single `.so` + `c++_static`, or plan for multiple + `c++_shared` | **Single `.so`, `c++_static`** | Corruption risk if got wrong later |
| D4 | Licence | Apache-2.0 unless there is a reason otherwise | Gates which FEC libraries we may use |
| D5 | Repo hosting / CI provider | GitHub + Actions | Conventional |

### D1 — the language question, honestly

ADR-0003 chose C++ and flagged Rust as "the alternative most worth revisiting." Now is the
only cheap moment to revisit it.

**For Rust:** the entire protocol-parsing surface is attacker-controlled (T1, RISK-009).
Memory safety there is worth a great deal, and it would eliminate most of the tooling in
§5. Cargo is a better build experience than CMake+FetchContent.

**For C++:** NDK integration is first-class and documented; GPU/graphics interop (GLES,
Vulkan, `AHardwareBuffer`, external OES textures) is C-API-shaped and painful to wrap;
NEON intrinsics are directly available; the Android camera NDK is a C API. Rust-in-NDK
works but adds a cross-compilation layer and JNI story we would own.

**My recommendation: C++20 for the core, with a specific hedge** — keep the protocol-parsing
surface free of pointer arithmetic, drive it entirely through a bounds-checked span/reader
abstraction, and treat it as the candidate for a later Rust rewrite if fuzzing finds the
surface hard to secure. This gets most of the safety benefit where it matters without paying
the interop cost across the whole codebase.

> **Status 2026-08-02: honoured.** The parsing surface is `core/src/frame.cpp` (frame header)
> and `core/src/transfer.cpp` (session manifest, filename sanitisation), both driven entirely
> through the bounds-checked reader in `core/include/fileflow/bytes.h`. There is no
> `core/src/protocol/` directory — an earlier draft of this paragraph named one that was never
> created. Verified: no pointer arithmetic and no `memcpy` in either file.

I would not fight a decision to go Rust-first — it is defensible — but the GPU and camera
interop cost is real and lands exactly on the highest-performance path.

### D2 — simulator harness language

The impairment pipeline is image processing over parameter sweeps, which is Python/NumPy's
home ground. But ADR-0010 requires the **decode chain** to be production C++.

Recommended split: impairments and decode in C++ (fast, one implementation, no drift);
**sweep orchestration, analysis and plotting in Python**, calling the C++ core through
**nanobind** (~4× faster compiles, ~10× lower call overhead than pybind11, first-class
ndarray support without depending on NumPy `[LIT]`).

Pure C++ is also fine and simpler; it just makes exploratory analysis clumsier, and there
will be a lot of exploratory analysis in Phases 1–2.
