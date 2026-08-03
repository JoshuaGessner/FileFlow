#include <fileflow/device.h>

#include <algorithm>
#include <string>

namespace fileflow {

std::string_view EvidenceName(Evidence e) noexcept {
    switch (e) {
        case Evidence::kUnknown: return "unknown";
        case Evidence::kClaimed: return "claimed";
        case Evidence::kVerified: return "verified";
        case Evidence::kRefuted: return "refuted";
    }
    return "invalid";
}

std::string_view DeviceTierName(DeviceTier t) noexcept {
    switch (t) {
        case DeviceTier::kUnsupported: return "unsupported";
        case DeviceTier::kBaseline: return "baseline";
        case DeviceTier::kStandard: return "standard";
        case DeviceTier::kHighRate: return "high-rate";
    }
    return "invalid";
}

Status DeviceReport::Validate() const noexcept {
    // Bounds before use. This record crosses JNI from a layer we are treating as untrusted for
    // the same reason we treat the camera as untrusted: a marshalling bug and a hostile input
    // are indistinguishable from here (INPUT-VALIDATION).
    if (max_refresh_hz < 0.0 || max_refresh_hz > 1000.0) return Error::kValueOutOfRange;
    if (measured_fd < 0.0 || measured_fd > 1000.0) return Error::kValueOutOfRange;
    if (panel_width > 65535 || panel_height > 65535) return Error::kValueOutOfRange;
    if (camera_modes.size() > 512) return Error::kValueOutOfRange;
    for (const CameraMode& m : camera_modes) {
        if (m.width > 65535 || m.height > 65535) return Error::kValueOutOfRange;
        if (m.max_fps < 0.0 || m.max_fps > 10000.0) return Error::kValueOutOfRange;
    }
    if (model.size() > 128 || soc.size() > 128) return Error::kValueOutOfRange;
    return Status::Ok();
}

Status TieringPolicy::Validate() const noexcept {
    if (standard_min_fd <= 0.0 || high_rate_min_fd <= standard_min_fd) {
        return Error::kValueOutOfRange;
    }
    if (min_cell_pixels == 0 || max_cell_pixels < min_cell_pixels) return Error::kValueOutOfRange;
    if (min_grid_cols == 0) return Error::kValueOutOfRange;
    return Status::Ok();
}

std::vector<GridGeometry> IntegerPitchGrids(std::uint32_t panel_width, std::uint32_t panel_height,
                                            const TieringPolicy& policy) {
    std::vector<GridGeometry> out;
    if (panel_width == 0 || panel_height == 0) return out;

    // Cell pitch must be an integer number of panel pixels on BOTH axes (DEVICE-MATRIX.md). A
    // fractional pitch puts cell boundaries mid-pixel, so the transmitter resamples its own
    // structure before the channel even starts -- which is a self-inflicted version of exactly
    // the crosstalk the receiver is fighting. Refuse rather than approximate.
    //
    // Note the pitch may differ between axes: panels are not square and forcing a square cell
    // would discard usable columns or rows for no benefit.
    for (std::uint32_t px = policy.min_cell_pixels; px <= policy.max_cell_pixels; ++px) {
        if (panel_width % px != 0) continue;
        for (std::uint32_t py = policy.min_cell_pixels; py <= policy.max_cell_pixels; ++py) {
            if (panel_height % py != 0) continue;
            const std::uint32_t cols = panel_width / px;
            const std::uint32_t rows = panel_height / py;
            if (cols < policy.min_grid_cols) continue;

            // Reject geometries the layout itself would refuse, so a caller never receives a
            // grid that fails later at render (the failure mode F16 recorded).
            if (!FrameLayout::Create({cols, rows}, LayoutConfig{}).ok()) continue;
            out.push_back(GridGeometry{cols, rows});
        }
    }

    // Densest first: more cells is more capacity, and the caller wants the best it can drive.
    std::sort(out.begin(), out.end(), [](const GridGeometry& a, const GridGeometry& b) {
        return a.cells() > b.cells();
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const GridGeometry& a, const GridGeometry& b) {
                              return a.cols == b.cols && a.rows == b.rows;
                          }),
              out.end());
    return out;
}

namespace {

// Does this evidence support acting on the claim?
bool Trustworthy(Evidence e, const TieringPolicy& policy) noexcept {
    if (e == Evidence::kVerified) return true;
    if (e == Evidence::kRefuted) return false;
    // kUnknown and kClaimed are both "the platform said so, we did not check".
    return policy.accept_claimed_evidence && e == Evidence::kClaimed;
}

}  // namespace

Result<DeviceProfile> Decide(const DeviceReport& report, TieringPolicy policy) {
    FF_TRY(policy.Validate());
    FF_TRY(report.Validate());

    DeviceProfile p;
    auto note = [&p](std::string s) { p.notes.push_back(std::move(s)); };

    // --- Display: which Fd are we willing to stand behind? ---
    //
    // A REFUTED measurement is the interesting case: the panel claims 120 Hz and delivers
    // duplicates. Falling back to the measured value rather than the claim is the whole point of
    // measuring, and it is why `usable_fd` is separate from `max_refresh_hz`.
    if (Trustworthy(report.fd_evidence, policy)) {
        p.usable_fd = report.measured_fd;
        p.usable_fd_evidence = report.fd_evidence;
        note("Fd " + std::to_string(report.measured_fd) + " states/s (" +
             std::string(EvidenceName(report.fd_evidence)) + ")");
    } else if (report.fd_evidence == Evidence::kRefuted) {
        p.usable_fd = report.measured_fd;
        p.usable_fd_evidence = Evidence::kRefuted;
        note("display REFUTED its own claim: advertised " + std::to_string(report.max_refresh_hz) +
             " Hz, measured " + std::to_string(report.measured_fd) +
             " distinct states/s. Using the measurement.");
    } else {
        // Unmeasured. Refuse to infer Fd from the refresh rate -- that is precisely the
        // conflation ADR-0012 and PERFORMANCE-PHILOSOPHY forbid, and RISK-003 says presentation
        // is not deterministic anyway.
        p.usable_fd = 0.0;
        p.usable_fd_evidence = report.fd_evidence;
        note("Fd NOT MEASURED (" + std::string(EvidenceName(report.fd_evidence)) +
             "). The refresh rate is not a substitute; EXP-006 must run.");
    }

    // --- Grids ---
    p.supported_grids = IntegerPitchGrids(report.panel_width, report.panel_height, policy);
    if (p.supported_grids.empty()) {
        note("no grid has an integer cell pitch on both axes for a " +
             std::to_string(report.panel_width) + "x" + std::to_string(report.panel_height) +
             " panel within the configured pitch range");
    } else {
        note(std::to_string(p.supported_grids.size()) + " integer-pitch grid(s), densest " +
             std::to_string(p.supported_grids.front().cols) + "x" +
             std::to_string(p.supported_grids.front().rows));
    }

    // --- Camera mode selection ---
    //
    // Self-contradictory modes are dropped BEFORE selection. A mode claiming both a high-speed
    // session and CPU readability contradicts a documented platform constraint `[FACT]`, so it
    // is either a vendor bug or our marshalling error; acting on it would produce a capture
    // path that cannot exist.
    double best_cpu_fps = 0.0;
    double best_gpu_fps = 0.0;
    int best_cpu = -1, best_gpu = -1;
    std::size_t contradictory = 0;

    for (std::size_t i = 0; i < report.camera_modes.size(); ++i) {
        const CameraMode& m = report.camera_modes[i];
        if (m.IsSelfContradictory()) {
            ++contradictory;
            continue;
        }
        if (m.width == 0 || m.height == 0 || m.max_fps <= 0.0) continue;

        if (m.cpu_readable && m.max_fps > best_cpu_fps) {
            best_cpu_fps = m.max_fps;
            best_cpu = static_cast<int>(i);
        }
        if (m.high_speed && m.max_fps > best_gpu_fps) {
            best_gpu_fps = m.max_fps;
            best_gpu = static_cast<int>(i);
        }
    }
    if (contradictory > 0) {
        note("dropped " + std::to_string(contradictory) +
             " camera mode(s) claiming BOTH a high-speed session and CPU readability — a "
             "high-speed session excludes ImageReader, so such a mode cannot exist");
    }

    p.manual_controls_usable = Trustworthy(report.manual_sensor_evidence, policy) &&
                               report.claims_manual_sensor;
    if (report.manual_sensor_evidence == Evidence::kRefuted) {
        note("device advertises MANUAL_SENSOR but ignored the settings when tested — treating "
             "manual control as unavailable (RISK-011)");
    } else if (report.claims_manual_sensor && !p.manual_controls_usable) {
        note("MANUAL_SENSOR claimed but unverified; not relied upon");
    }

    p.clock_cross_check_available =
        Trustworthy(report.timestamp_evidence, policy) && report.timestamp_source_realtime;
    if (!p.clock_cross_check_available) {
        note("no trustworthy REALTIME timestamp source: the display/camera clock cross-check is "
             "unavailable, so frame-phase classification rests on optics alone (C07, OQ-017)");
    }

    // --- Tier ---
    //
    // Ordered from the most demanding down, and every step states its reason. `usable_fd` is 0
    // when Fd was never measured, so an unmeasured device cannot reach Standard -- deliberately.
    // A device we have not measured is not a device we can quote a rate for.
    const bool have_grid = !p.supported_grids.empty();

    if (!have_grid) {
        p.tier = DeviceTier::kUnsupported;
        note("UNSUPPORTED: no usable grid geometry");
    } else if (p.usable_fd >= policy.high_rate_min_fd && best_gpu >= 0) {
        p.tier = DeviceTier::kHighRate;
        p.selected_camera_mode = best_gpu;
        note("HIGH-RATE: Fd >= " + std::to_string(policy.high_rate_min_fd) +
             " with a high-speed camera mode. This is the only tier where milestone 6 is "
             "testable at all.");
    } else if (p.usable_fd >= policy.standard_min_fd && best_cpu >= 0) {
        p.tier = DeviceTier::kStandard;
        p.selected_camera_mode = best_cpu;
        note("STANDARD: Fd >= " + std::to_string(policy.standard_min_fd) +
             " with a CPU-readable camera mode");
        if (p.usable_fd >= policy.high_rate_min_fd) {
            note("Fd would support high-rate but no high-speed camera mode was offered — the "
                 "receiver bounds Fd, not the transmitter");
        }
    } else if (best_cpu >= 0 && p.usable_fd > 0.0) {
        p.tier = DeviceTier::kBaseline;
        p.selected_camera_mode = best_cpu;
        note("BASELINE: a CPU capture path exists but measured Fd (" +
             std::to_string(p.usable_fd) + ") is below the standard threshold. Correctness "
             "only; this tier is not a performance target.");
    } else if (best_cpu < 0 && best_gpu < 0) {
        p.tier = DeviceTier::kUnsupported;
        note("UNSUPPORTED: no usable camera mode");
    } else {
        p.tier = DeviceTier::kUnsupported;
        note("UNSUPPORTED: Fd unmeasured or zero. Refusing to infer it from the refresh rate.");
    }

    return p;
}

}  // namespace fileflow
