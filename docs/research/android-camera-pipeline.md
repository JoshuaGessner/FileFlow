# Research: the Android camera pipeline

> **Status:** Draft — contains one finding that constrains the architecture
> **Owner:** RX / capture subsystem
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0005, RISK-001, RISK-002, EXP-007, OQ-001, OQ-002

## Headline finding

**Android's constrained high-speed capture session — the only portable route to ≥120 fps
capture — cannot deliver frames to an `ImageReader`.** `[FACT]`

From the platform javadoc for `CameraDevice.createConstrainedHighSpeedCaptureSession`
(AOSP `frameworks/base`, `core/java/android/hardware/camera2/CameraDevice.java`):

> "a high speed capture session will only support up to 2 output Surfaces"

> "All Surfaces must be either video encoder surfaces (acquired by
> `MediaRecorder#getSurface` or `MediaCodec#createInputSurface`) or preview surfaces
> (obtained from `SurfaceView`, `SurfaceTexture` via `Surface#Surface(SurfaceTexture)`)."

> "The Surface sizes must be one of the sizes reported by
> `StreamConfigurationMap#getHighSpeedVideoSizes`. When multiple Surfaces are configured,
> their size must be same."

> "The FPS ranges being requested to this session must be selected from
> `StreamConfigurationMap#getHighSpeedVideoFpsRangesFor`."

> "An active high speed capture session only accepts request lists created via
> `CameraConstrainedHighSpeedCaptureSession#createHighSpeedRequestList`, and the request
> list can only be submitted to this session via `CameraCaptureSession#captureBurst` or
> `CameraCaptureSession#setRepeatingBurst`."

`ImageReader` is not in the permitted surface list. `YUV_420_888` CPU-accessible output
is therefore **not available in a high-speed session**.

### Why this matters to FileFlow

The project's stated architecture pairs "Camera2 or NDK Camera receiver" with "low-copy
YUV camera processing", and milestone 6 asks whether 120 Hz/120 fps devices can exceed
1 MB/s. Those two commitments are in direct tension on the ≥120 fps path:

- **At ≤60 fps** (standard `CameraCaptureSession`): `AImageReader` with `YUV_420_888` is
  available, and the planned low-copy CPU/SIMD path works as described. `[FACT]`
- **At ≥120 fps** (constrained high-speed session): the receiver must consume frames as a
  **`SurfaceTexture` / external GPU texture**, or via a `MediaCodec` encoder surface.
  There is no CPU YUV path. `[FACT]`

This does not block the project, but it forces a decision that was not in the original
architecture: the high-frame-rate receiver is a **GPU-resident decoder**. Cells must be
sampled in a fragment or compute shader from an `GL_TEXTURE_EXTERNAL_OES` (or Vulkan
external-format) image, with only the reduced soft-symbol output read back to the CPU.

That is arguably a *better* design — sampling 34,560 cells is embarrassingly parallel and
the readback is orders of magnitude smaller than the frame — but it is a different design,
with different memory ownership and different failure modes. It is captured in ADR-0005
and drives EXP-007.

A secondary consequence: the `MediaCodec` route (encode to H.264/HEVC, decode later)
is **not viable** for our purposes — lossy video compression destroys exactly the
high-spatial-frequency cell structure we are modulating. Recording for the offline
harness must use a lossless path or accept that the recording is not representative.
`[HYP]` — needs confirmation in EXP-007.

## Standard capture path (≤60 fps)

| Element | Notes |
|---|---|
| `CameraCaptureSession` | Normal session; `ImageReader`/`AImageReader` permitted. |
| `YUV_420_888` | Flexible YUV; plane strides and pixel strides vary by device and must be read from the `Image.Plane`, never assumed. |
| Y plane | For M0/M1/M2 we need **only luminance**. The Y plane alone is a full-resolution 8-bit greyscale image — chroma can be ignored entirely, which avoids all chroma-subsampling loss. `[FACT]` |
| Chroma | Only needed for M3 (colour). `YUV_420_888` chroma is subsampled 2×2, so colour cells must be at least 2×2 sensor pixels to avoid chroma bleed. This is a real constraint on M3 cell size. `[FACT]` |
| `AImageReader` (NDK) | `media/NdkImageReader.h`. Avoids JNI on the hot path. |
| `AHardwareBuffer` | `AImageReader_getWindowNativeHandle` / `AImage_getHardwareBuffer` enable zero-copy handoff to GPU. |

### Frame rate control
`CONTROL_AE_TARGET_FPS_RANGE` selected from `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES`.
Setting a fixed range like `[60,60]` prevents auto-exposure from dropping the frame rate
in low light — which it will otherwise do, silently halving `Fd`'s usable ceiling.
This is a mandatory setting for us, not an optimisation. `[FACT]`

Note that the maximum advertised range is device- and resolution-dependent, and
`StreamConfigurationMap.getOutputMinFrameDuration(format, size)` gives the real per-stream
floor. A device advertising 60 fps may only reach it at reduced resolution. `[FACT]`

## Manual controls

Required capability: `REQUEST_AVAILABLE_CAPABILITIES` containing
`MANUAL_SENSOR` (and `MANUAL_POST_PROCESSING` for white balance).

| Control | Key | Purpose for FileFlow |
|---|---|---|
| Exposure time | `SENSOR_EXPOSURE_TIME` | Short exposure reduces motion blur and temporal mixing across display states. Central to `Pc`. |
| Sensitivity | `SENSOR_SENSITIVITY` | ISO. Trades noise against the shorter exposure we want. |
| AE off | `CONTROL_AE_MODE = OFF` | Prevents exposure hunting from changing the photometric channel mid-transfer. |
| Focus | `CONTROL_AF_MODE = OFF` + `LENS_FOCUS_DISTANCE` | Fixed focus at the known screen distance; prevents AF hunting mid-transfer. |
| AWB off | `CONTROL_AWB_MODE = OFF` + `COLOR_CORRECTION_GAINS` | Essential for M3; white-balance drift is a colour-channel disturbance. |
| Stabilisation off | `LENS_OPTICAL_STABILIZATION_MODE`, `CONTROL_VIDEO_STABILIZATION_MODE = OFF` | OIS/EIS warp the geometry unpredictably and break homography tracking. Must be off. |
| Noise reduction / edge | `NOISE_REDUCTION_MODE`, `EDGE_MODE = OFF` | Vendor NR and sharpening are nonlinear spatial filters — exactly the wrong thing applied to a dense symbol grid. |
| Tonemap | `TONEMAP_MODE = CONTRAST_CURVE` with a linear curve | Removes the vendor tone curve, linearising the luminance response. Important for M2's four-level slicing. |

The `EDGE_MODE`/`NOISE_REDUCTION_MODE`/`TONEMAP_MODE` group is under-discussed in the
screen-camera literature and we expect it to matter substantially for dense grids. `[HYP]`

**Hardware level** (`INFO_SUPPORTED_HARDWARE_LEVEL`): `LEGACY` devices cannot do manual
control at all; `FULL` or `LEVEL_3` is what we need. `LIMITED` is case-by-case. The
capability probe must classify this and refuse or degrade accordingly.

## Timestamps

`SENSOR_INFO_TIMESTAMP_SOURCE` is either `REALTIME` (same base as
`SystemClock.elapsedRealtimeNanos()`) or `UNKNOWN` (an arbitrary monotonic base).

This matters because correlating camera frames with display presentation timestamps
requires a shared clock. On `REALTIME` devices the correlation is direct. On `UNKNOWN`
devices only *relative* timing is usable, and the frame-phase classifier must estimate
the offset from the optical signal itself (timing tracks and phase indicators) rather
than from clocks. The frame layouts therefore **must not depend on clock correlation
being available**. This is a design constraint on the optical frame, recorded in
[OPTICAL-FRAME-CANDIDATES.md](../specifications/OPTICAL-FRAME-CANDIDATES.md). `[FACT]`

## Rolling shutter

Nearly all smartphone CMOS sensors are rolling shutter: rows are exposed sequentially.
A camera frame therefore samples the display at slightly different times down the image.
When the display changes state during that scan, the frame contains a **mixture**: the
top rows show state *k*, the bottom rows state *k+1*, with a transition band between.

`SENSOR_ROLLING_SHUTTER_SKEW` reports the time delta between first and last row exposure
where available. `[FACT]` This is the key parameter for the mixed-frame model and for
M4. The transition band's position moves frame to frame as the two clocks drift, which is
also what makes it *exploitable*: it sweeps, so over many frames every screen region is
eventually sampled cleanly.

## Open questions

| ID | Question |
|---|---|
| OQ-001 | On our reference devices, what is the maximum frame rate at which CPU-accessible `YUV_420_888` is actually delivered without drops, at a resolution sufficient to resolve our densest grid? |
| OQ-002 | Does the GPU-texture path in a high-speed session deliver genuinely distinct frames at 120 fps, or does the vendor pipeline duplicate/interpolate? |
| OQ-016 | Do vendor `EDGE_MODE`/`NOISE_REDUCTION_MODE` settings actually take effect, or are they silently ignored? |
| OQ-017 | Is `SENSOR_ROLLING_SHUTTER_SKEW` reported, and is it accurate? |

## Sources

- Android Open Source Project, `frameworks/base`, `core/java/android/hardware/camera2/CameraDevice.java` — javadoc for `createConstrainedHighSpeedCaptureSession`. https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/master/core/java/android/hardware/camera2/CameraDevice.java (accessed 2026-08-02). Access note: full source.
- Android Developers, "Use multiple camera streams simultaneously." https://developer.android.com/media/camera/camera2/multiple-camera-streams-simultaneously (accessed 2026-08-02).
- Android NDK reference, `AImageReader` / `NdkImageReader.h`. https://developer.android.com/ndk/reference/group/media (accessed 2026-08-02).
- Microsoft Learn mirror of `StreamConfigurationMap.GetHighSpeedVideoSizes` javadoc. https://learn.microsoft.com/en-us/dotnet/api/android.hardware.camera2.params.streamconfigurationmap.gethighspeedvideosizes (accessed 2026-08-02). Access note: secondary mirror of platform javadoc; used only for corroboration.
