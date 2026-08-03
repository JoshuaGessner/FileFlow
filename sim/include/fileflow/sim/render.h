// Optical rendering: a CellMatrix as a camera would actually see it (component C16).
//
// This is the GEOMETRIC half of the simulator, the part channel.h deferred until the CV path
// existed. It matters because it exercises the real receiver chain -- detect, rectify, sample
// -- rather than handing cell samples straight to the demodulator. Geometric error is a real
// term in the error budget and the only honest way to include it is to make the decoder earn
// its homography from pixels.
//
// ⚠ Same warning as channel.h: NOT calibrated against real captures (RISK-024). Every output
// is [HYP] until Phase 2 compares it with recorded frames.
#pragma once

#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>

#include <array>
#include <cstdint>

namespace fileflow::sim {

// Where the camera is relative to the screen. Drives EXP-018 (viewing angle) and EXP-017
// (distance) directly, rather than making experimenters hand-write corner coordinates.
struct ViewConfig {
    int image_width = 960;
    int image_height = 1280;

    // 1.0 places the screen filling ~80% of image height head-on. Larger is further away.
    double distance = 1.0;

    double yaw_deg = 0.0;    // rotation about the screen's vertical axis
    double pitch_deg = 0.0;  // about the horizontal axis
    double roll_deg = 0.0;   // about the optical axis

    // Off-centre placement, in fractions of image size.
    double offset_x = 0.0;
    double offset_y = 0.0;
};

struct OpticalRenderConfig {
    ViewConfig view;

    // Multiplicative luminance at the frame corners relative to the centre, about the GRID
    // centre -- i.e. a transmitter-side effect (RISK-025), not lens vignetting. It does not
    // follow the image centre under an off-axis view, which is exactly what makes it
    // different from vignetting and why the photometric field must not assume a cause.
    double corner_falloff = 1.0;

    // Uniform additive black-level lift.
    double black_lift = 0.0;

    // Directional glare: additive lift strongest at the left edge of the GRID, zero at the
    // right. Finding F7: this, not falloff, is what defeats a global threshold, because it
    // squeezes the level ranges from the opposite side until they overlap.
    double glare_lift = 0.0;

    int blur_radius = 0;          // box blur, px. Approximates defocus.
    double noise_amplitude = 0.0; // deterministic additive noise, peak in 8-bit levels
    std::uint64_t seed = 12345;

    std::uint8_t background = 0;  // surround level; locked exposure underexposes it
};

// Screen corners in image space (TL, TR, BR, BL) for a viewing configuration.
// Returns a degenerate quad if the screen would be behind the camera.
[[nodiscard]] std::array<Point2, 4> QuadFromView(const ViewConfig& v, const GridGeometry& g);

// Grid corners in grid space, in the canonical order used everywhere: TL, TR, BR, BL.
[[nodiscard]] std::array<Point2, 4> GridCorners(const GridGeometry& g);

// Render `cells` warped by an explicit transform.
[[nodiscard]] Image8 RenderOptical(const CellMatrix& cells, const Homography& grid_to_image,
                                   const OpticalRenderConfig& cfg);

// Render using the viewing geometry, and report the transform that was used so tests and
// experiments can measure estimated-vs-true geometric error.
[[nodiscard]] Image8 RenderView(const CellMatrix& cells, const GridGeometry& g,
                                const OpticalRenderConfig& cfg, Homography* used_transform);

}  // namespace fileflow::sim
