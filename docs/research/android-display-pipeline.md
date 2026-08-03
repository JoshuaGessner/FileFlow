# Research: the Android display pipeline

> **Status:** Draft
> **Owner:** TX / renderer subsystem
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0004, RISK-003, EXP-006, OQ-004, OQ-005

## The core problem

`Fd` — distinct display states actually presented per second — is a **transmitter-side
quantity we do not directly control**. We can request a frame rate and submit buffers on
time, but SurfaceFlinger, the display driver and the panel decide what is actually
scanned out. A frame we rendered but that was never presented is a display state the
receiver can never decode, and it corrupts our sequence numbering if we assume otherwise.

So the transmitter has two jobs: present distinct states as fast as the panel allows, and
**know which states were actually presented**.

## Presentation control and verification

### Choreographer and FrameTimeline

The NDK Choreographer surface (`android/choreographer.h`):

| Function | API level |
|---|---|
| `AChoreographer_getInstance` | 24 |
| `AChoreographer_postFrameCallback64` | 29 |
| `AChoreographer_registerRefreshRateCallback` | 30 |
| `AChoreographer_postVsyncCallback` | **33** |
| `AChoreographerFrameCallbackData_getFrameTimelinesLength` | **33** |
| `AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos` | **33** |
| `AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos` | **33** |
| `AChoreographerFrameCallbackData_getFrameTimelineVsyncId` | **33** |
| `AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex` | **33** |

`[FACT]` — Android NDK reference.

The API 33 FrameTimeline model gives the app *several candidate timelines* per callback,
each with an expected presentation time, a deadline, and a vsync ID. The app picks the
timeline whose deadline it can meet and tells the platform which one it chose. This is
precisely the mechanism FileFlow needs: it converts "I hope this frame lands on time"
into "I have declared which vsync this frame targets."

**Implication:** minimum API level 33 (Android 13) for the deterministic-pacing path.
Given the Android-first, reference-device-first stance (ADR-0001, ADR-0011) this is an
acceptable floor. Devices below 33 can be supported later at lower `Fd` confidence.
Recorded as a decision input in ADR-0004.

### Verifying what was actually presented

Candidate mechanisms, in decreasing order of directness:

1. **`ASurfaceTransaction` completion callbacks** —
   `ASurfaceTransaction_setOnCompleteCallback` provides a present fence, giving the actual
   presentation time per transaction. This is the most direct signal available to an app.
   `[HYP]` — needs verification that the fence resolves per-frame at 60/120 Hz.
2. **`FrameTimeline` / `SurfaceControl.getFrameTimelines`** — expected vs. actual.
3. **`Display.getRefreshRate` / mode change callbacks** — coarse; tells us the panel mode,
   not per-frame outcomes.
4. **Perfetto / `atrace` with the `gfx` and `SurfaceFlinger` categories** — offline ground
   truth for characterisation runs, not usable in production. This is the right tool for
   EXP-006 and for confirming whatever in-app mechanism we adopt.

**Critical design consequence:** because presentation verification may be imperfect or
unavailable, the optical frame itself **must carry its own sequence number and frame-phase
indicator**. The receiver must never rely on the transmitter's intended schedule to know
which state it is looking at. This is a hard requirement on the frame layout and is
recorded in [OPTICAL-FRAME-CANDIDATES.md](../specifications/OPTICAL-FRAME-CANDIDATES.md).

### Requesting a frame rate

`Surface.setFrameRate(fps, compatibility, changeFrameRateStrategy)` (API 30; strategy
parameter API 31). Key facts:

- It is a **hint, not a guarantee**. The platform may refuse: a higher-priority surface
  may want a different rate, or the device may be in battery-saver mode. `[FACT]`
- `CHANGE_FRAME_RATE_ALWAYS` permits a mode switch that may be visually disruptive; the
  documented pattern is to call it, wait for `onDisplayChanged`, and allow **up to ~2
  seconds** for the switch to complete. `[FACT]`
- It does not throttle the app, but it *does* change Choreographer callback timing and
  buffer release intervals. `[FACT]`

**Implication:** the transmitter must request its display mode and then *wait and verify*
before starting a transfer. A transfer that begins during a mode switch will see `Fd`
change underneath it. The session layer therefore has an explicit "display mode settled"
precondition.

### Variable refresh rate

Modern panels run VRR/LTPO and may idle down to 10–30 Hz to save power when content is
static — and our content is *not* static, but the platform's heuristics may not classify a
dense flickering grid the way we expect. Worse, an LTPO panel switching modes mid-transfer
changes `Fd` silently.

The mitigation is to request a fixed high mode, verify it took effect, and monitor for
mode changes during the transfer, aborting or re-negotiating if one occurs. `[HYP]`

## Rendering path

| Option | Assessment |
|---|---|
| **OpenGL ES 3.x on `SurfaceView`** | Baseline choice. Ubiquitous, simple, adequate — we are drawing a grid of flat quads, which is trivial GPU work. |
| **Vulkan** | Better explicit control over presentation and swapchain timing (`VK_GOOGLE_display_timing`). Higher complexity. Justified only if OpenGL ES pacing proves insufficient. |
| `SurfaceView` vs `TextureView` | **`SurfaceView`** — it gets its own layer and can be presented without a composition round-trip. `TextureView` is composited with the view hierarchy and adds latency and potential resampling. `TextureView` is disqualified. |

**Resampling is the silent killer.** If anything in the pipeline scales our image, cell
boundaries land on fractional pixels and crosstalk rises. The transmitter must render at
exactly the panel's native resolution, with the surface buffer size set to match, and no
scaling anywhere in the path. Verified with `SurfaceHolder.setFixedSize` matching the
display mode's physical resolution.

Related: **do not let the grid be an integer-ratio pattern that resonates with the panel's
subpixel layout.** OLED panels commonly use non-RGB-stripe subpixel arrangements
(e.g. diamond PenTile), so a "white" cell and a "grey" cell may have different spatial
subpixel structure. This interacts with camera sampling to produce moiré and colour
fringing. `[HYP]` — EXP-021 compares OLED and LCD.

## Buffering and latency

`SurfaceFlinger` triple-buffers by default. For a video player this hides jitter; for us
it adds latency between "we drew state *k*" and "state *k* is on the panel". Latency alone
is tolerable (it is a constant offset), but *variable* latency corrupts our model of which
state is displayed when. Since the optical frame carries its own sequence number, this is
survivable — another argument for self-describing frames.

## Brightness

Screen brightness is a channel parameter, not a UI preference. The transmitter should set
and hold maximum brightness for the transfer duration (`WindowManager.LayoutParams.
screenBrightness = 1.0f`), and must restore it afterwards. Note that this interacts with
thermal throttling (RISK-010) and battery drain (RISK-014), and that sustained maximum
brightness on OLED raises the burn-in concern in RISK-012.

## Open questions

| ID | Question |
|---|---|
| OQ-004 | Which presentation-verification mechanism is reliable per-frame at 60 and 120 Hz on the reference devices? |
| OQ-005 | Does `setFrameRate` reliably hold a 120 Hz mode for a multi-minute transfer, or does thermal/power management drop it? |
| OQ-018 | Does the panel's subpixel structure measurably affect binary luminance separation at our cell pitches? |

## Sources

- Android NDK reference, "Choreographer." https://developer.android.com/ndk/reference/group/choreographer (accessed 2026-08-02).
- Android Developers, "Frame rate." https://developer.android.com/media/optimize/performance/frame-rate (accessed 2026-08-02).
- Android Developers, "Optimize refresh rates." https://developer.android.com/games/optimize/display-refresh-rate-change (accessed 2026-08-02).
- Android Developers Blog, "High refresh rate rendering on Android," April 2020. https://android-developers.googleblog.com/2020/04/high-refresh-rate-rendering-on-android.html (accessed 2026-08-02). Access note: official vendor blog, not peer-reviewed.
- AOSP, `platform_frameworks_base`, `core/java/android/view/Choreographer.java`. https://github.com/aosp-mirror/platform_frameworks_base (accessed 2026-08-02).
