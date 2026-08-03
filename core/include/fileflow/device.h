// Device capability judgement (component C02) — the portable half.
//
// ADR-0014: the Android layer MARSHALS, this decides. `DeviceReport` is a flat record of what the
// platform claimed and what measurement showed; `Decide()` turns it into a `DeviceProfile`. No
// I/O, no clock, no camera handle, no Android headers — so recorded characteristic sets from real
// devices replay as fixtures, which is what C02's test strategy asks for.
//
// WHY THE JUDGEMENT IS HERE AND NOT IN KOTLIN. C02's registry entry says vendors misreport
// capabilities (RISK-011) and that "everything downstream trusts it". That makes this the most
// correctness-critical code in the receiver's startup path. Kotlin/Camera2 code cannot be tested
// without a device; this can be tested in two seconds. Putting it in Kotlin would have made the
// least verifiable component also the least tested.
//
// ⚠ EVERYTHING THIS FILE CONCLUDES IS `[HYP]` UNTIL EXP-007 RUNS ON REAL HARDWARE. The tier
// thresholds are reasoned from the goodput model and the platform documentation, not measured.
// The point of the design is to make that experiment cheap, not to substitute for it.
#pragma once

#include <fileflow/grid.h>
#include <fileflow/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fileflow {

// How a claim was established. The distinction is the entire reason this type exists: a device
// that ADVERTISES 120 fps and a device MEASURED delivering 120 distinct frames are different
// devices, and RISK-011 says the first lies often enough to matter.
enum class Evidence : std::uint8_t {
    kUnknown = 0,   // never looked
    kClaimed = 1,   // the platform said so; not checked
    kVerified = 2,  // we measured it and it held
    kRefuted = 3,   // we measured it and the platform was wrong
};

[[nodiscard]] std::string_view EvidenceName(Evidence e) noexcept;

// One camera output configuration the platform offered.
struct CameraMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double max_fps = 0.0;
    bool high_speed = false;      // came from getHighSpeedVideoSizes/Ranges
    bool cpu_readable = false;    // an ImageReader/YUV_420_888 path exists

    // A high-speed session excludes ImageReader `[FACT]`, so a mode claiming both is either a
    // platform bug or our own marshalling error. Either way it must not be trusted.
    [[nodiscard]] bool IsSelfContradictory() const noexcept { return high_speed && cpu_readable; }
};

// What the Android layer observed, claims and measurements kept separate.
struct DeviceReport {
    std::string model;              // display/telemetry only; never a behavioural switch
    std::string soc;

    // --- Display ---
    double max_refresh_hz = 0.0;
    std::uint32_t panel_width = 0;
    std::uint32_t panel_height = 0;
    // Distinct optical frames per second actually presented. Fd, not the refresh rate --
    // conflating them is the error PERFORMANCE-PHILOSOPHY exists to prevent.
    double measured_fd = 0.0;
    Evidence fd_evidence = Evidence::kUnknown;

    // --- Camera ---
    std::vector<CameraMode> camera_modes;
    int hardware_level = -1;        // INFO_SUPPORTED_HARDWARE_LEVEL, -1 = unread
    bool claims_manual_sensor = false;
    Evidence manual_sensor_evidence = Evidence::kUnknown;
    // SENSOR_INFO_TIMESTAMP_SOURCE. UNKNOWN removes the display/camera clock cross-check that
    // C07's phase classifier wants.
    bool timestamp_source_realtime = false;
    Evidence timestamp_evidence = Evidence::kUnknown;
    double rolling_shutter_skew_ns = -1.0;  // negative = not reported

    [[nodiscard]] Status Validate() const noexcept;
};

// Supported tiers. Deliberately coarse: a tier is a promise about which milestones are reachable,
// and a fine-grained ladder would imply confidence we do not have.
enum class DeviceTier : std::uint8_t {
    kUnsupported = 0,  // cannot run the protocol at all
    kBaseline = 1,     // 30 states/s class; correctness only, not a performance target
    kStandard = 2,     // 60 states/s with CPU capture — Phases 1-5, milestone 4's basis
    kHighRate = 3,     // >=120 states/s with a GPU path — the only tier where milestone 6 exists
};

[[nodiscard]] std::string_view DeviceTierName(DeviceTier t) noexcept;

struct DeviceProfile {
    DeviceTier tier = DeviceTier::kUnsupported;

    // Grids this device can drive with an INTEGER number of panel pixels per cell. Non-integer
    // pitch resamples the cell edges and is refused rather than approximated
    // (DEVICE-MATRIX.md: the grid is device-dependent and negotiated in the frame header).
    std::vector<GridGeometry> supported_grids;

    double usable_fd = 0.0;                     // the Fd we are willing to stand behind
    Evidence usable_fd_evidence = Evidence::kUnknown;
    int selected_camera_mode = -1;              // index into DeviceReport::camera_modes, -1 = none
    bool manual_controls_usable = false;
    bool clock_cross_check_available = false;

    // Human-readable reasons for the decision, in order. Populated on every path including
    // success, because "why did this device get Baseline" is the first question a bug report
    // asks and reconstructing it from a tier enum alone is impossible.
    std::vector<std::string> notes;

    [[nodiscard]] bool CanAttemptMilestone6() const noexcept {
        return tier == DeviceTier::kHighRate;
    }
};

// Thresholds, all `[HYP]`. Exposed so an experiment can sweep them rather than editing code.
struct TieringPolicy {
    double standard_min_fd = 55.0;   // a "60 Hz" panel measured at 58 is still Standard
    double high_rate_min_fd = 110.0;
    std::uint32_t min_cell_pixels = 8;   // integer panel pixels per cell, per axis
    std::uint32_t max_cell_pixels = 16;
    std::uint32_t min_grid_cols = 108;   // below this the coded header does not fit (F16)

    // Trust an unverified platform claim? Default NO: RISK-011 is rated High likelihood, and a
    // probe that trusts is not a probe. Settable so EXP-007 can measure what trusting costs.
    bool accept_claimed_evidence = false;

    [[nodiscard]] Status Validate() const noexcept;
};

// The whole component, as a pure function.
[[nodiscard]] Result<DeviceProfile> Decide(const DeviceReport& report, TieringPolicy policy = {});

// Grids whose cell pitch is an integer number of panel pixels on BOTH axes.
//
// Exposed separately because it is the one piece of C02 that is pure arithmetic over the panel
// geometry, and DEVICE-MATRIX.md's per-device grid table is exactly its output: 120x200 is
// integer on the Pixel 8, 144x240 on the S26 Ultra, and neither is integer on both.
[[nodiscard]] std::vector<GridGeometry> IntegerPitchGrids(std::uint32_t panel_width,
                                                          std::uint32_t panel_height,
                                                          const TieringPolicy& policy);

}  // namespace fileflow
