# Reference device matrix

> **Status:** Draft — specs from vendor/press sources; **nothing here is probe-verified yet**
> **Owner:** RX / capability probe
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0011, EXP-006, EXP-007, OQ-033 (now closed)

Closes **OQ-033**. Two reference devices, selected by availability rather than by the
capability axes in ADR-0011 — which is fine, and they happen to span the axes reasonably.

⚠ **Every figure below is from vendor or press material.** ADR-0011 and RISK-011 both say
vendors misreport capabilities; the capability probe (CAP-01/CAP-03) must **verify by
measurement** before any of this is treated as fact.

## The pair

| | **Google Pixel 8** | **Samsung Galaxy S26 Ultra** |
|---|---|---|
| Released | Oct 2023 | early 2026 |
| SoC | Tensor G3 | Snapdragon 8 Elite Gen 5 |
| Panel | 6.2" OLED | 6.9" Dynamic AMOLED |
| Native resolution | **1080 × 2400** | **1440 × 3120** |
| Refresh | 60–120 Hz (Smooth Display; **not** LTPO — that is the Pro) | 120 Hz |
| Peak brightness | ~2000 nits (claimed) | high (claimed) |
| Notable | Reference AOSP behaviour, predictable Camera2 | **Privacy Display / Black Matrix** — see below |

**Coverage against the ADR-0011 axes:** both OLED (no LCD comparison — EXP-021 cannot run
as specified); both 120 Hz-capable; two very different SoC and camera stacks; two different
vendor Android skins. Good enough to catch device-specific assumptions, and the OLED-only
limitation should be recorded rather than glossed over.

---

## ⚠ RISK-025 — the S26 Ultra's Privacy Display attacks our channel directly

The S26 Ultra ships **Privacy Display**, a hardware feature using "Black Matrix" /
Flex Magic Pixel: the panel's subpixels are split into **Narrow Pixels** (emitting
forward) and **Wide Pixels** (emitting broadly), and a filter is applied to half the
pixels. With the feature on, the screen is reported unreadable from as little as **30° off
axis**. `[LIT — press coverage, not vendor spec]`

Three consequences, in descending order of how much they worry me:

1. **Privacy Display must be OFF for any transfer.** With it on, the receiving camera is a
   bystander by design. The app should detect or at minimum instruct.

2. **Press reporting says the Black Matrix reduces viewing angles versus older panels
   *even when the feature is off*.** `[LIT — unverified]` If true, the S26 Ultra has a
   permanently narrower angular emission cone than a typical phone, which directly shrinks
   our angle-tolerance envelope (EXP-018) on this device.

3. **The subtle one — angle-dependent luminance falloff across a single frame.** At 30 cm a
   6.9" screen subtends a wide angle: the receiver views the screen's corners
   substantially further off-axis than its centre. On a panel with restricted angular
   emission, that produces a **radial luminance falloff that is a property of the
   transmitter, not the lens**. It will look like vignetting to the receiver but it is not
   the camera's vignetting, it gets *worse* at closer range — exactly where we want to
   operate for maximum grid density — and it will not be corrected by lens-based
   assumptions.

   Our distributed pilot lattice (layout Candidate B) should absorb this, because it
   estimates the illumination field by interpolation from pilots spread across the frame
   without caring what caused the falloff. **That is a genuine, unplanned argument in
   favour of Candidate B over Candidate A**, whose border-only pilots would have to
   extrapolate the falloff into the centre.

**Action:** add an explicit Privacy-Display-on/off arm to EXP-018 and EXP-003. This is
cheap to test and potentially decisive for whether the S26 Ultra is usable as a transmitter.

---

## Grid selection — cell pitch must be an integer number of panel pixels

Non-integer cell pitch puts cell boundaries on fractional pixels, which the transmitter
cannot render crisply and which raises spatial crosstalk for no gain. Computed by
`tools/grid_fit.py`:

### Charter grids against these panels

| Grid | Pixel 8 (1080×2400) | S26 Ultra (1440×3120) |
|---|---|---|
| 96 × 160 | 11.25 × 15.00 — **fractional** | 15.00 × 19.50 — **fractional** |
| 120 × 200 | **9 × 12 — integer** | 12.00 × 15.60 — **fractional** |
| 144 × 240 | 7.50 × 10.00 — **fractional** | **10 × 13 — integer** |

**Each charter grid is integer on exactly one of the two devices and fractional on the
other.** There is no single charter grid that suits both.

### Recommended per-device grids

| Device | Grid | Cell pitch | Cells | Note |
|---|---|---|---|---|
| Pixel 8 | **120 × 200** | 9 × 12 px | 24,000 | Charter grid, integer. Start here |
| Pixel 8 | 108 × 240 | **10 × 10 px** | 25,920 | **Perfectly square cells** — isotropic crosstalk, simpler sampler |
| S26 Ultra | **144 × 240** | 10 × 13 px | 34,560 | Charter grid, integer |
| S26 Ultra | 120 × 260 | **12 × 12 px** | 31,200 | **Perfectly square cells** |

The square-cell options are worth taking seriously: isotropic cells make the crosstalk
model isotropic and remove a free parameter from the sampler. They should be arms in
EXP-001 and EXP-013.

**Protocol implication:** the grid is **device-dependent**, so it must be negotiated rather
than assumed. It already is — the frame header carries the grid and profile, and the
receiver reads it rather than presuming. No protocol change needed, but this confirms that
was the right call.

---

## Capabilities to verify before trusting anything (EXP-007 / EXP-006)

| Question | Pixel 8 | S26 Ultra | Why it matters |
|---|---|---|---|
| `INFO_SUPPORTED_HARDWARE_LEVEL` | ? | ? | Need FULL or LEVEL_3 for manual control |
| Max fps with CPU-accessible `YUV_420_888` | ? | ? | **OQ-001** — decides CPU vs GPU receive path |
| Constrained high-speed session available? | ? | ? | **OQ-002** — gates milestone 6 entirely |
| `getHighSpeedVideoSizes` contents | ? | ? | RISK-002 — may be below our grid requirement |
| Distinct frames at 120 fps via `SurfaceTexture`? | ? | ? | If duplicated, milestone 6 is dead regardless |
| Manual exposure / ISO / focus / AWB honoured? | ? | ? | Verify in returned `CaptureResult`, not the request |
| `EDGE_MODE` / `NOISE_REDUCTION_MODE` / `TONEMAP_MODE` honoured? | ? | ? | **OQ-016** — vendor sharpening ruins dense grids |
| `SENSOR_INFO_TIMESTAMP_SOURCE` | ? | ? | REALTIME enables clock correlation; UNKNOWN does not |
| `SENSOR_ROLLING_SHUTTER_SKEW` reported and accurate? | ? | ? | **OQ-017** — the M4 mixed-frame parameter |
| Actual presented display-state rate at 60 / 120 | ? | ? | **`Fd` is assumed, never measured** (EXP-006) |
| Lossless capture path for the harness? | ? | ? | **OQ-023** — else recordings misrepresent the channel |
| HWASan supported (ARM64 + API 34+)? | ? | ? | Our on-device memory-safety tool (ADR-0013) |

### Specific concern: Samsung and third-party high frame rates

Developer-forum reports suggest Samsung restricts ≥60 fps recording for third-party apps on
some devices, citing thermal and power management. `[LIT — forum reports, unverified,
possibly outdated]` If that holds on the S26 Ultra, the **Pixel 8 may be the only viable
high-frame-rate receiver of the pair**, and milestone 6 rests entirely on it.

This is exactly the kind of vendor-behaviour claim ADR-0011 says to verify rather than
trust, in either direction — it may well be stale.

---

## What this pairing cannot test

Recorded so the gaps are not forgotten:

- **No LCD panel.** EXP-021 (OLED vs LCD) cannot run as written. Either acquire an LCD
  device or rescope the experiment to OLED subpixel-layout differences between these two
  panels, which is still worth measuring given the Black Matrix.
- **No low-end device.** Both are flagships. Thermal and decoder-throughput results
  (EXP-020, RISK-015) will be optimistic relative to the broader Android population.
- **Only one pairing direction each.** With two devices there are four TX/RX combinations,
  which is a usable cross-device matrix (benchmark category 4) but a small one.
