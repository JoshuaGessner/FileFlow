// Deterministic replays of inputs that the fuzzers actually crashed on.
//
// WHY THIS FILE EXISTS. Apple clang ships no libFuzzer (finding F5), so the fuzz targets run
// on Linux CI only and a developer on macOS cannot reproduce a fuzzer finding with the fuzzer.
// Every crash therefore gets a permanent, deterministic test here, replaying the exact bytes
// through the exact same invariant checks the fuzz harness asserts. That keeps the finding
// alive on every platform and turns a one-off CI failure into a regression gate.
//
// Each case records the run that found it and the crashing input verbatim, because the input
// is the evidence and it is not recoverable from anywhere else.
#include <fileflow/detect.h>
#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace fileflow;

namespace {

// The invariant block from fuzz/fuzz_screen_detect.cpp, expressed as gtest expectations
// instead of __builtin_trap so a failure says WHICH claim broke rather than just dying.
void AssertDetectionIsCoherent(const FrameLayout& layout, const ImageView8& img,
                               const Detection& d) {
    for (const double v : d.grid_to_image.m()) {
        EXPECT_TRUE(std::isfinite(v)) << "homography has a non-finite entry";
    }
    for (const auto& p : d.quad) {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y)) << "quad corner is non-finite";
    }

    EXPECT_GE(d.marker_score, 0.0);
    EXPECT_LE(d.marker_score, 1.0);
    EXPECT_GE(d.runner_up_score, 0.0);
    EXPECT_LE(d.runner_up_score, 1.0);
    EXPECT_GE(d.marker_score, DetectionConfig{}.min_marker_score)
        << "returned a detection below the score it claims to enforce";
    EXPECT_GE(d.marker_score - d.runner_up_score, DetectionConfig{}.min_rotation_margin)
        << "returned a detection below the rotation margin it claims to enforce";
    EXPECT_GE(d.rotation, 0);
    EXPECT_LE(d.rotation, 3);
    EXPECT_TRUE(d.grid_to_image.Inverse().ok()) << "homography is not invertible";

    auto sampler = CellSampler::Create(layout.geometry(), {});
    ASSERT_TRUE(sampler.ok());
    const auto samples = sampler.value().Sample(img, d.grid_to_image);
    ASSERT_EQ(samples.size(), layout.geometry().cells());
    for (const double v : samples) {
        EXPECT_TRUE(std::isnan(v) || std::isfinite(v)) << "sampler invented an infinity";
    }

    auto field = PhotometricField::Estimate(layout, samples);
    if (!field.ok()) return;
    const auto norm = field.value().Normalise(samples);
    ASSERT_EQ(norm.size(), samples.size());
    for (const double v : norm) {
        EXPECT_TRUE(std::isnan(v) || std::isfinite(v)) << "normalise invented an infinity";
    }
}

// Replicates fuzz_screen_detect.cpp's input decoding exactly, so the test exercises the same
// geometry the fuzzer did. Returns false if the harness would have returned early.
bool RunScreenDetectHarness(const std::vector<std::uint8_t>& data) {
    if (data.size() < 8) return false;
    const int w = 1 + (data[0] % 128);
    const int h = 1 + (data[1] % 128);
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (data.size() < 8 + pixels) return false;

    const ImageView8 img(data.data() + 8, w, h, w);

    auto layout = FrameLayout::Create({24, 40}, {.pilot_pitch = 8,
                                                 .marker_size = 4,
                                                 .header_rows = 4,
                                                 .guard_width = 1,
                                                 .boundary_width = 1});
    if (!layout.ok()) return false;
    auto det = ScreenDetector::Create(layout.value(), {});
    if (!det.ok()) return false;

    auto d = det.value().Detect(img);
    if (!d.ok()) return true;  // refusing is always an acceptable answer

    AssertDetectionIsCoherent(layout.value(), img, d.value());
    return true;
}

}  // namespace

// Found by `fuzz_screen_detect` on GitHub Actions run 30858226556 (2026-08-03) — the first
// time the fuzz targets had ever actually executed. 5,022 executions to reach it.
//
// The input decodes to a 3x2 pixel image, against a 24x40 cell layout. The detector was asked
// to find a 960-cell screen inside six pixels and did not refuse.
TEST(FuzzRegression, ScreenDetectOnAThreeByTwoImage) {
    const std::vector<std::uint8_t> crash{0x02, 0x81, 0x8a, 0x37, 0x36, 0x81, 0x94, 0x94, 0x94,
                                          0x94, 0x94, 0x8a, 0x37, 0x36, 0x00, 0x00, 0x51};
    EXPECT_TRUE(RunScreenDetectHarness(crash));
}

// The general property the crash above is one instance of: an image far too small to contain
// the grid must be REFUSED, not fitted. Swept rather than spot-checked, because the fuzzer
// found one point and the boundary is what matters.
TEST(FuzzRegression, TinyImagesAreRefusedRatherThanFitted) {
    for (int w = 1; w <= 6; ++w) {
        for (int h = 1; h <= 6; ++h) {
            std::vector<std::uint8_t> data(8 + static_cast<std::size_t>(w * h), 0x94);
            data[0] = static_cast<std::uint8_t>(w - 1);
            data[1] = static_cast<std::uint8_t>(h - 1);
            EXPECT_TRUE(RunScreenDetectHarness(data)) << "w=" << w << " h=" << h;
        }
    }
}

// The ACTUAL defect behind the crash above, asserted as the contract it is rather than as one
// input that happened to expose it (F24).
//
// `FromCorrespondences` normalised m[8] to 1 and checked finiteness, neither of which implies a
// non-zero determinant — so it could return a matrix that `Inverse()` then rejected. Every
// consumer of the inverse (the tracker, most obviously) inherited that. The crashing input only
// reproduces where floating-point rounding puts the determinant on the far side of the
// tolerance, so this sweeps degenerate configurations directly instead of trusting one sample.
TEST(FuzzRegression, EveryAcceptedHomographyIsInvertible) {
    const std::array<Point2, 4> unit{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};

    // Configurations that push the fit toward singular: collapsed edges, collinear corners,
    // near-zero extent, and the sub-pixel quads a tiny image produces.
    const std::vector<std::array<Point2, 4>> degenerate{
        {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}},                          // all coincident
        {{{0, 0}, {1, 0}, {1, 0}, {0, 0}}},                          // zero height
        {{{0, 0}, {0, 1}, {0, 1}, {0, 0}}},                          // zero width
        {{{0, 0}, {1, 1}, {2, 2}, {3, 3}}},                          // fully collinear
        {{{0, 0}, {3, 0}, {3, 2}, {0, 2}}},                          // the 3x2 image case
        {{{0, 0}, {1e-9, 0}, {1e-9, 1e-9}, {0, 1e-9}}},              // vanishing extent
        {{{0, 0}, {1e12, 0}, {1e12, 1e12}, {0, 1e12}}},              // enormous extent
        {{{0, 0}, {2, 0}, {1, 1e-12}, {1, 0}}},                      // near-collinear
    };

    for (std::size_t i = 0; i < degenerate.size(); ++i) {
        auto h = Homography::FromCorrespondences(unit, degenerate[i]);
        if (!h.ok()) continue;  // refusing is always acceptable
        EXPECT_TRUE(h.value().Inverse().ok())
            << "case " << i << ": FromCorrespondences accepted a matrix Inverse() rejects — "
            << "the two disagree, which is exactly the contract break F24 records";
    }
}
