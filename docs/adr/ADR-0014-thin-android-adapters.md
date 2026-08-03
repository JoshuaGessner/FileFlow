# ADR-0014 — Thin Android adapters; all judgement in portable C++

> **Status:** Accepted
> **Date:** 2026-08-03
> **Owner:** Architecture
> **Related:** ADR-0003, ADR-0010, ADR-0013, C01, C02, C05, C17, RISK-011

## Context

The Android shell is starting. ADR-0003 chose "Kotlin shell + C++ core" and ADR-0010 requires
the decode chain to run off-device. Neither says where the **capability probe's judgement** or
the **camera recorder's bundle writing** live, and for both there is a real pull toward Kotlin:
`CameraCharacteristics` and `Display.Mode` are Java APIs, the camera delivers its bytes to a
Kotlin callback, and writing a file from Kotlin is trivial. The path of least resistance puts
both entirely in Kotlin.

Two facts make that the wrong choice, and the second is the decisive one.

**Vendors misreport capabilities** (RISK-011). C02's registry entry is explicit that the probe
must *verify by measurement* rather than read characteristics, and that "everything downstream
trusts it" — the probe is correctness-critical, not glue.

**Kotlin and Camera2 code cannot be tested without a device; portable C++ is covered by the
existing desktop suite.** There is no device attached to this project today, and even once
there is, on-device tests are slow, flaky and manual compared with 218 tests that run in two
seconds. Putting the probe's judgement in Kotlin would make the single most
correctness-critical, least-verifiable component also the **least tested code in the
repository**. That is exactly backwards.

## Decision

**The Android layer marshals data. It makes no decisions.**

| Concern | Lives in | Why |
|---|---|---|
| Reading `CameraCharacteristics`, `Display.Mode`, thermal status | Kotlin | Only Kotlin can |
| Flattening those readings into a plain, serialisable record | Kotlin → JNI, once at startup | Not per-frame; ADR-0003's boundary rule is untouched |
| Classifying a device into a tier; choosing grids, modes, exposure | **`core/`** | Testable off-device against recorded fixtures |
| Deciding whether a claimed capability was *verified* | **`core/`** | The judgement RISK-011 exists for |
| Delivering camera frames (Y plane + timestamps) | Kotlin/NDK → C++ | Only Kotlin/NDK can |
| Writing the capture bundle | **`harness::CaptureWriter`, unchanged** | See below |
| Session orchestration, permissions, UI, lifecycle | Kotlin | C01's actual job |

**The recorder reuses `harness::CaptureWriter` rather than reimplementing the format.**
`harness/` is already free of platform headers — it needs only `<filesystem>` and `<fstream>` —
so it links into the Android library as-is. This is not merely convenient: F17's proof that
replay is bit-identical to live decode is a proof about **that writer**. A Kotlin
reimplementation of `capture.meta` and the `.gray` layout would make F17's guarantee apply to
the wrong code, and the first real capture would arrive on an unproven path — losing the exact
property the harness was built early to establish.

**Corollary: the probe is a pure function.** `DeviceProfile Decide(const DeviceReport&)` takes a
flat record of what the platform claimed and what measurement showed, and returns a profile. No
I/O, no clock, no camera handle. That is what makes recorded characteristic sets from real
devices replayable as fixtures, which C02's test strategy already asks for.

## Alternatives

1. **Everything in Kotlin, C++ only for decode.** Rejected: it puts the correctness-critical
   probe in the untestable half, and duplicates the bundle format.
2. **Everything in C++ including Camera2 via NDK Camera.** Rejected *for now*. The NDK camera
   API is available and ADR-0013 already chose Camera2/NDK Camera over CameraX, but permissions,
   lifecycle and storage access must be Kotlin regardless, so a pure-C++ shell is not achievable
   and the split has to exist somewhere. Where the ≥120 fps GPU path (ADR-0005) needs it, the
   capture *service* may move to NDK Camera later; that does not change this ADR, which is about
   where **judgement** lives.
3. **A `DeviceProfile` computed in Kotlin and passed down for the core to trust.** Rejected: it
   is the same as (1) with an extra copy, and it makes the trust boundary invisible.

## Consequences

**Positive.** The probe's judgement is unit-testable today, with no device, against fixtures.
The recorder produces byte-compatible bundles by construction. The untestable surface shrinks to
"did we read the right field from the platform", which is the part a device test genuinely has
to cover — and it is a much smaller thing to review.

**Negative.** More JNI surface than a Kotlin-only probe: a struct must be marshalled across the
boundary. Mitigated by it being a **one-shot startup call**, not per-frame — ADR-0003's rule is
that no *per-frame* data crosses JNI, and this does not.

**Negative.** `harness/` becomes part of the shipped library, where it was previously a host-only
tool. It links as a **static** library into the single `libfileflow.so`, so ADR-0013's
one-`.so` rule is preserved. Worth stating explicitly because that rule is load-bearing: adding
a second `.so` requires switching the whole project to `c++_shared` in the same change.

**Negative.** `harness/` uses `<filesystem>`, which `core/` deliberately avoids. This is
acceptable in the recorder — it is I/O by definition — but `core/` must not acquire a
`<filesystem>` dependency by osmosis.

## Risks

| Risk | Mitigation |
|---|---|
| The Kotlin marshalling layer reads the wrong characteristic and the core faithfully decides on garbage | The `DeviceReport` records *what was claimed* and *what was measured* separately, so a mismatch is data the core can act on rather than an invisible substitution (RISK-011) |
| `harness/` grows a platform dependency and breaks the `core-only` preset | The existing `core-desktop` CI job plus the Android-header grep already cover `core/`; extend the grep to `harness/` |
| JNI struct drifts from the C++ definition | One marshalling site, reviewed as a unit; a golden round-trip test over the flat record |
| Doing this at all before any hardware exists produces a probe that is wrong in ways no test can see | Accepted and stated: nothing here is validated until EXP-007 runs on a real device. The code is structured so that experiment is cheap, not so that it can be skipped |

## Validation plan

1. `core/` cross-compiles for `arm64-v8a` under the NDK, unchanged — this is the first time
   ADR-0010's promise is actually cashed, and any friction amends ADR-0013 rather than becoming
   a workaround.
2. The probe's decision logic is exercised by desktop tests over synthetic `DeviceReport`s,
   including the adversarial cases RISK-011 describes (a device claiming `MANUAL_SENSOR` and
   ignoring it; an advertised 120 fps mode delivering duplicate frames).
3. The recorder writes a bundle that `ffreplay` decodes, asserted on desktop with a synthetic
   frame source before any camera is involved.
4. **Nothing here is `[FACT]` until it runs on a Pixel 8 or an S26 Ultra.** Until then every
   platform claim remains documentation-derived, and the probe's tiering is `[HYP]`.
