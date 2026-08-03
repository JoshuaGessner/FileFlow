// Fuzz the screen detector against arbitrary image bytes.
//
// Why this surface matters (THREAT-MODEL T1): the camera is an ATTACKER-CONTROLLED input.
// Anyone who can put pixels in front of the lens -- a hostile screen, a printed pattern, a
// projector -- chooses this function's input entirely. The detector runs BEFORE any CRC,
// signature or hash exists to reject anything, so it is the true front door of the receiver.
//
// The invariants below are the ones a wrong answer would violate. A detector that returns a
// plausible-looking but WRONG homography is the dangerous case, not a crash: every later
// stage then samples the wrong place, the photometric field fits noise, and the payload
// decodes to confident wrong bits.
#include <fileflow/detect.h>
#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace fileflow;

    // First bytes steer the image geometry so the fuzzer can reach different shapes,
    // including degenerate ones (zero width, single row, stride > width).
    if (size < 8) return 0;
    const int w = 1 + (data[0] % 128);
    const int h = 1 + (data[1] % 128);
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (size < 8 + pixels) return 0;

    const ImageView8 img(data + 8, w, h, w);

    auto layout = FrameLayout::Create({24, 40}, {.pilot_pitch = 8,
                                                 .marker_size = 4,
                                                 .header_rows = 4,
                                                 .guard_width = 1,
                                                 .boundary_width = 1});
    if (!layout.ok()) return 0;

    auto det = ScreenDetector::Create(layout.value(), {});
    if (!det.ok()) return 0;

    auto d = det.value().Detect(img);
    if (!d.ok()) return 0;  // refusing is always an acceptable answer

    // --- If it claims a detection, the claim must be internally coherent. ---

    // A non-finite homography would produce NaN sample coordinates everywhere downstream.
    for (const double v : d.value().grid_to_image.m()) {
        if (!std::isfinite(v)) __builtin_trap();
    }
    for (const auto& p : d.value().quad) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) __builtin_trap();
    }

    // The reported scores must be real fractions, and the verification thresholds it claims
    // to have passed must actually hold.
    const double s = d.value().marker_score;
    const double r = d.value().runner_up_score;
    if (!(s >= 0.0 && s <= 1.0)) __builtin_trap();
    if (!(r >= 0.0 && r <= 1.0)) __builtin_trap();
    if (s < DetectionConfig{}.min_marker_score) __builtin_trap();
    if (s - r < DetectionConfig{}.min_rotation_margin) __builtin_trap();
    if (d.value().rotation < 0 || d.value().rotation > 3) __builtin_trap();

    // The homography must be invertible -- the sampler and any tracker rely on it.
    if (!d.value().grid_to_image.Inverse().ok()) __builtin_trap();

    // --- The downstream chain must survive whatever geometry was just accepted. ---
    auto sampler = CellSampler::Create(layout.value().geometry(), {});
    if (!sampler.ok()) return 0;
    const auto samples = sampler.value().Sample(img, d.value().grid_to_image);
    if (samples.size() != layout.value().geometry().cells()) __builtin_trap();

    // Samples are either NaN (erasure) or a real luminance. An infinity here would mean the
    // sampler invented a value, which must never happen.
    for (const double v : samples) {
        if (!std::isnan(v) && !std::isfinite(v)) __builtin_trap();
    }

    auto field = PhotometricField::Estimate(layout.value(), samples);
    if (!field.ok()) return 0;

    const auto norm = field.value().Normalise(samples);
    if (norm.size() != samples.size()) __builtin_trap();
    for (const double v : norm) {
        if (!std::isnan(v) && !std::isfinite(v)) __builtin_trap();
    }
    return 0;
}
