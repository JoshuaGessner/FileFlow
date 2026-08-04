// Device capability judgement (C02), tested entirely off-device.
//
// This is the point of ADR-0014. The probe's judgement is the most correctness-critical code in
// the receiver's startup path -- everything downstream trusts it, and vendors misreport
// capabilities (RISK-011) -- so it lives in portable C++ where it can be held to adversarial
// cases in two seconds instead of requiring a phone.
//
// The strongest validation available without hardware is that the grid arithmetic independently
// reproduces DEVICE-MATRIX.md's per-device table. If the probe and the document disagree, one of
// them is wrong, and the test says so.
//
// ⚠ Nothing here validates the TIER THRESHOLDS. Those are `[HYP]` until EXP-007 and EXP-006 run
// on a Pixel 8 and an S26 Ultra. What is tested is the decision LOGIC: that claims are not
// trusted, that measurements override claims, and that unmeasured means unmeasured.
#include <fileflow/device.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace fileflow;

namespace {

bool HasGrid(const std::vector<GridGeometry>& grids, std::uint32_t cols, std::uint32_t rows) {
    return std::any_of(grids.begin(), grids.end(), [cols, rows](const GridGeometry& g) {
        return g.cols == cols && g.rows == rows;
    });
}

bool AnyNoteContains(const DeviceProfile& p, std::string_view needle) {
    return std::any_of(p.notes.begin(), p.notes.end(), [needle](const std::string& n) {
        return n.find(needle) != std::string::npos;
    });
}

// A device that measures well on every axis. Individual tests degrade one thing at a time, so
// each failure is attributable to the thing that was degraded.
DeviceReport HealthyPixel8() {
    DeviceReport r;
    r.model = "Pixel 8";
    r.soc = "Tensor G3";
    r.max_refresh_hz = 120.0;
    r.panel_width = 1080;
    r.panel_height = 2400;
    r.measured_fd = 60.0;
    r.fd_evidence = Evidence::kVerified;
    r.camera_modes = {
        {.width = 1920, .height = 1080, .max_fps = 60.0, .high_speed = false,
         .cpu_readable = true},
        {.width = 1280, .height = 720, .max_fps = 240.0, .high_speed = true,
         .cpu_readable = false},
    };
    r.hardware_level = 1;
    r.claims_manual_sensor = true;
    r.manual_sensor_evidence = Evidence::kVerified;
    r.timestamp_source_realtime = true;
    r.timestamp_evidence = Evidence::kVerified;
    r.rolling_shutter_skew_ns = 22000.0;
    return r;
}

}  // namespace

// ---------------------------------------------------------------- grid arithmetic

TEST(DeviceProbe, ReproducesTheDocumentedPixel8Grids) {
    // DEVICE-MATRIX.md: 1080x2400. Charter grid 120x200 at 9x12 px; 108x240 at a perfectly
    // square 10x10 px. Both must fall out of the arithmetic, not be hardcoded anywhere.
    const auto grids = IntegerPitchGrids(1080, 2400, {});
    EXPECT_TRUE(HasGrid(grids, 120, 200)) << "the charter grid for this panel";
    EXPECT_TRUE(HasGrid(grids, 108, 240)) << "the square-cell grid";

    // 144x240 is the S26 Ultra's grid and is NOT integer here: 1080/144 = 7.5.
    EXPECT_FALSE(HasGrid(grids, 144, 240))
        << "144 columns needs a 7.5 px pitch on a 1080-wide panel; that must be refused";
}

TEST(DeviceProbe, ReproducesTheDocumentedS26UltraGrids) {
    // DEVICE-MATRIX.md: 1440x3120. Charter grid 144x240 at 10x13 px; 120x260 at 12x12 px.
    const auto grids = IntegerPitchGrids(1440, 3120, {});
    EXPECT_TRUE(HasGrid(grids, 144, 240)) << "the charter grid for this panel";
    EXPECT_TRUE(HasGrid(grids, 120, 260)) << "the square-cell grid";
}

TEST(DeviceProbe, TheTwoReferenceDevicesShareNoGrid) {
    // The load-bearing consequence in DEVICE-MATRIX.md: each charter grid is integer on exactly
    // ONE of the two devices, which is why the grid is negotiated in the frame header rather
    // than fixed by the protocol. If this ever passes, that rationale needs revisiting.
    const auto pixel = IntegerPitchGrids(1080, 2400, {});
    const auto sam = IntegerPitchGrids(1440, 3120, {});

    EXPECT_TRUE(HasGrid(pixel, 120, 200));
    EXPECT_FALSE(HasGrid(sam, 120, 200));
    EXPECT_TRUE(HasGrid(sam, 144, 240));
    EXPECT_FALSE(HasGrid(pixel, 144, 240));
}

TEST(DeviceProbe, GridsAreDensestFirstAndFreeOfDuplicates) {
    const auto grids = IntegerPitchGrids(1080, 2400, {});
    ASSERT_FALSE(grids.empty());
    for (std::size_t i = 1; i < grids.size(); ++i) {
        EXPECT_GE(grids[i - 1].cells(), grids[i].cells()) << "not sorted densest-first";
        EXPECT_FALSE(grids[i - 1].cols == grids[i].cols && grids[i - 1].rows == grids[i].rows)
            << "duplicate grid at index " << i;
    }
}

TEST(DeviceProbe, APanelWithNoIntegerPitchYieldsNoGrids) {
    // 1001 is prime-ish in the relevant range: no pitch in [8,16] divides it.
    EXPECT_TRUE(IntegerPitchGrids(1001, 2400, {}).empty());
    EXPECT_TRUE(IntegerPitchGrids(0, 0, {}).empty());
}

// ---------------------------------------------------------------- the trust rules

TEST(DeviceProbe, AVerifiedHealthyDeviceReachesHighRate) {
    DeviceReport r = HealthyPixel8();
    r.measured_fd = 120.0;  // verified at 120 distinct states/s, with a high-speed mode
    auto p = Decide(r);
    ASSERT_TRUE(p.ok()) << ErrorName(p.error());
    EXPECT_EQ(p.value().tier, DeviceTier::kHighRate);
    EXPECT_TRUE(p.value().CanAttemptMilestone6());
    EXPECT_EQ(p.value().selected_camera_mode, 1) << "should select the high-speed mode";
    EXPECT_DOUBLE_EQ(p.value().usable_fd, 120.0);
}

TEST(DeviceProbe, RefusesToInferFdFromTheRefreshRate) {
    // The conflation ADR-0012 exists to prevent, and RISK-003's whole point: a 120 Hz panel is
    // not a 120-states/s transmitter until measured. An unmeasured device must NOT be quoted a
    // rate, so it cannot reach Standard however good its camera is.
    DeviceReport r = HealthyPixel8();
    r.max_refresh_hz = 120.0;
    r.measured_fd = 0.0;
    r.fd_evidence = Evidence::kUnknown;

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tier, DeviceTier::kUnsupported);
    EXPECT_DOUBLE_EQ(p.value().usable_fd, 0.0);
    EXPECT_TRUE(AnyNoteContains(p.value(), "NOT MEASURED"));
    EXPECT_TRUE(AnyNoteContains(p.value(), "not a substitute"));
}

TEST(DeviceProbe, AClaimedButUnverifiedRateIsNotTrustedByDefault) {
    // A probe that trusts is not a probe (RISK-011, rated High likelihood).
    DeviceReport r = HealthyPixel8();
    r.measured_fd = 120.0;
    r.fd_evidence = Evidence::kClaimed;

    auto strict = Decide(r);
    ASSERT_TRUE(strict.ok());
    EXPECT_EQ(strict.value().tier, DeviceTier::kUnsupported)
        << "an unverified claim must not earn a tier";

    // Settable so EXP-007 can measure what trusting would have cost.
    auto lenient = Decide(r, {.accept_claimed_evidence = true});
    ASSERT_TRUE(lenient.ok());
    EXPECT_EQ(lenient.value().tier, DeviceTier::kHighRate);
}

TEST(DeviceProbe, ARefutedClaimUsesTheMeasurementAndSaysSo) {
    // The case measuring exists for: the panel advertises 120 Hz and delivers 60 distinct
    // states. The measurement wins, and the discrepancy appears in the notes because a bug
    // report needs it.
    DeviceReport r = HealthyPixel8();
    r.max_refresh_hz = 120.0;
    r.measured_fd = 60.0;
    r.fd_evidence = Evidence::kRefuted;

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_DOUBLE_EQ(p.value().usable_fd, 60.0);
    EXPECT_EQ(p.value().usable_fd_evidence, Evidence::kRefuted);
    EXPECT_EQ(p.value().tier, DeviceTier::kStandard) << "60 states/s with a CPU path";
    EXPECT_FALSE(p.value().CanAttemptMilestone6());
    EXPECT_TRUE(AnyNoteContains(p.value(), "REFUTED"));
}

TEST(DeviceProbe, DropsACameraModeClaimingBothHighSpeedAndCpuReadability) {
    // A high-speed session excludes ImageReader `[FACT]`. A mode claiming both cannot exist, so
    // it is a vendor bug or our own marshalling error -- and acting on it would build a capture
    // path that does not exist.
    DeviceReport r = HealthyPixel8();
    r.measured_fd = 120.0;
    r.camera_modes = {
        {.width = 1920, .height = 1080, .max_fps = 240.0, .high_speed = true,
         .cpu_readable = true},  // impossible
        {.width = 1920, .height = 1080, .max_fps = 60.0, .high_speed = false,
         .cpu_readable = true},
    };

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().selected_camera_mode, 1) << "must not select the contradictory mode";
    EXPECT_EQ(p.value().tier, DeviceTier::kStandard)
        << "no usable high-speed mode remains, so high-rate is unreachable";
    EXPECT_TRUE(AnyNoteContains(p.value(), "cannot exist"));
}

TEST(DeviceProbe, ARefutedManualSensorClaimDisablesManualControl) {
    // RISK-011's named example: a device advertising MANUAL_SENSOR that ignores the settings.
    DeviceReport r = HealthyPixel8();
    r.claims_manual_sensor = true;
    r.manual_sensor_evidence = Evidence::kRefuted;

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_FALSE(p.value().manual_controls_usable);
    EXPECT_TRUE(AnyNoteContains(p.value(), "ignored the settings"));
}

TEST(DeviceProbe, AnUnknownTimestampSourceRemovesTheClockCrossCheck) {
    DeviceReport r = HealthyPixel8();
    r.timestamp_source_realtime = false;
    r.timestamp_evidence = Evidence::kVerified;

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_FALSE(p.value().clock_cross_check_available);
    EXPECT_TRUE(AnyNoteContains(p.value(), "optics alone"));
    EXPECT_TRUE(AnyNoteContains(p.value(), "not REALTIME"));
}

TEST(DeviceProbe, AClaimedButUnverifiedClockIsNotDescribedAsMissing) {
    // Regression for F26, found by reading the first real hardware report: the device claimed a
    // REALTIME source, and the verdict said there was no REALTIME source. Both statements
    // appeared on one screen. The claim was true and the refusal was correct -- an unverified
    // claim earns nothing (RISK-011) -- but the wording contradicted the claims block above it,
    // in a project whose entire discipline is separating claims from evidence.
    DeviceReport r = HealthyPixel8();
    r.timestamp_source_realtime = true;
    r.timestamp_evidence = Evidence::kClaimed;

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    // The refusal itself must stand.
    EXPECT_FALSE(p.value().clock_cross_check_available);
    EXPECT_TRUE(AnyNoteContains(p.value(), "claimed but unverified"));
    // And it must NOT deny the claim the device actually made.
    EXPECT_FALSE(AnyNoteContains(p.value(), "not REALTIME"));
    EXPECT_FALSE(AnyNoteContains(p.value(), "UNKNOWN"));
}

TEST(DeviceProbe, HighFdWithoutAHighSpeedModeStaysStandard) {
    // Fd is bounded by the RECEIVER, not the transmitter (PERFORMANCE-PHILOSOPHY). A 120-states/s
    // display with no ≥120 fps capture path cannot deliver milestone 6.
    DeviceReport r = HealthyPixel8();
    r.measured_fd = 120.0;
    r.camera_modes = {{.width = 1920, .height = 1080, .max_fps = 60.0, .high_speed = false,
                       .cpu_readable = true}};

    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tier, DeviceTier::kStandard);
    EXPECT_TRUE(AnyNoteContains(p.value(), "receiver bounds Fd"));
}

TEST(DeviceProbe, NoUsableCameraModeIsUnsupported) {
    DeviceReport r = HealthyPixel8();
    r.camera_modes.clear();
    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tier, DeviceTier::kUnsupported);
    EXPECT_EQ(p.value().selected_camera_mode, -1);
}

TEST(DeviceProbe, AnUnusablePanelIsUnsupportedRegardlessOfCamera) {
    DeviceReport r = HealthyPixel8();
    r.panel_width = 1001;  // no integer pitch
    auto p = Decide(r);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tier, DeviceTier::kUnsupported);
    EXPECT_TRUE(p.value().supported_grids.empty());
}

TEST(DeviceProbe, EveryDecisionCarriesItsReasoning) {
    // "Why did this device get Baseline" is the first question a bug report asks, and a tier
    // enum alone cannot answer it.
    for (const Evidence e : {Evidence::kUnknown, Evidence::kClaimed, Evidence::kVerified,
                             Evidence::kRefuted}) {
        DeviceReport r = HealthyPixel8();
        r.fd_evidence = e;
        auto p = Decide(r);
        ASSERT_TRUE(p.ok());
        EXPECT_FALSE(p.value().notes.empty()) << "evidence=" << EvidenceName(e);
    }
}

// ---------------------------------------------------------------- untrusted input

TEST(DeviceProbe, RejectsAnOutOfRangeReport) {
    // The report crosses JNI from a layer treated as untrusted for the same reason the camera is:
    // a marshalling bug and a hostile input are indistinguishable from here.
    {
        DeviceReport r = HealthyPixel8();
        r.measured_fd = -1.0;
        EXPECT_EQ(Decide(r).error(), Error::kValueOutOfRange);
    }
    {
        DeviceReport r = HealthyPixel8();
        r.max_refresh_hz = 1e9;
        EXPECT_EQ(Decide(r).error(), Error::kValueOutOfRange);
    }
    {
        DeviceReport r = HealthyPixel8();
        r.camera_modes.assign(1000, CameraMode{});
        EXPECT_EQ(Decide(r).error(), Error::kValueOutOfRange);
    }
    {
        DeviceReport r = HealthyPixel8();
        r.model.assign(500, 'x');
        EXPECT_EQ(Decide(r).error(), Error::kValueOutOfRange);
    }
    {
        DeviceReport r = HealthyPixel8();
        r.camera_modes[0].max_fps = 1e9;
        EXPECT_EQ(Decide(r).error(), Error::kValueOutOfRange);
    }
}

TEST(DeviceProbe, RejectsAnIncoherentPolicy) {
    const DeviceReport r = HealthyPixel8();
    EXPECT_EQ(Decide(r, {.standard_min_fd = 0.0}).error(), Error::kValueOutOfRange);
    // high_rate must exceed standard, or the tiers are not ordered.
    EXPECT_EQ(Decide(r, {.standard_min_fd = 100.0, .high_rate_min_fd = 50.0}).error(),
              Error::kValueOutOfRange);
    EXPECT_EQ(Decide(r, {.min_cell_pixels = 0}).error(), Error::kValueOutOfRange);
    EXPECT_EQ(Decide(r, {.min_cell_pixels = 12, .max_cell_pixels = 8}).error(),
              Error::kValueOutOfRange);
}

TEST(DeviceProbe, IsAPureFunction) {
    // ADR-0014's corollary: same input, same output, no hidden state. What makes recorded
    // characteristic sets replayable as fixtures.
    const DeviceReport r = HealthyPixel8();
    auto a = Decide(r);
    auto b = Decide(r);
    ASSERT_TRUE(a.ok() && b.ok());
    EXPECT_EQ(a.value().tier, b.value().tier);
    EXPECT_EQ(a.value().notes, b.value().notes);
    EXPECT_EQ(a.value().selected_camera_mode, b.value().selected_camera_mode);
    EXPECT_EQ(a.value().supported_grids.size(), b.value().supported_grids.size());
}
