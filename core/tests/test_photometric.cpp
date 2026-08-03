#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <gtest/gtest.h>

#include <cmath>

#include "render_test_util.h"

using namespace fileflow;
using namespace fileflow::test;

namespace {

constexpr GridGeometry kGrid{48, 80};
constexpr int kPx = 8;  // pixels per cell

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(kGrid, {.pilot_pitch = 8, .marker_size = 4, .header_rows = 4});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

// Fill payload cells with a deterministic pseudo-random binary pattern; markers, pilots,
// header and guards get their layout-defined values.
CellMatrix RenderFrame(const FrameLayout& layout, std::vector<bool>* truth) {
    CellMatrix m(kGrid.cols, kGrid.rows);
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    truth->assign(static_cast<std::size_t>(kGrid.cells()), false);

    for (std::uint32_t r = 0; r < kGrid.rows; ++r) {
        for (std::uint32_t c = 0; c < kGrid.cols; ++c) {
            const std::size_t idx = static_cast<std::size_t>(r) * kGrid.cols + c;
            std::uint8_t v = kLevelDark;
            switch (layout.role(c, r)) {
                case CellRole::kPilot:
                case CellRole::kColorPilot:
                    v = layout.PilotValue(c, r);
                    break;
                case CellRole::kMarker:
                    v = layout.MarkerValue(c, r);
                    break;
                case CellRole::kGuard:
                    v = kLevelDark;
                    break;
                default: {
                    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                    v = (rng & 1) ? kLevelBright : kLevelDark;
                    break;
                }
            }
            m.set(c, r, v);
            (*truth)[idx] = (v == kLevelBright);
        }
    }
    return m;
}

Homography AxisAligned() {
    const auto corners = GridCorners(kGrid);
    const std::array<Point2, 4> quad{
        Point2{0, 0}, Point2{static_cast<double>(kGrid.cols * kPx), 0},
        Point2{static_cast<double>(kGrid.cols * kPx), static_cast<double>(kGrid.rows * kPx)},
        Point2{0, static_cast<double>(kGrid.rows * kPx)}};
    auto h = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    EXPECT_TRUE(h.ok());
    return h.value();
}

// Count payload cells decoded wrongly when thresholding `values` at `threshold`.
int CountErrors(const FrameLayout& layout, std::span<const double> values,
                const std::vector<bool>& truth, double threshold) {
    int wrong = 0;
    for (const std::uint32_t idx : layout.payload_cells()) {
        const double v = values[idx];
        if (std::isnan(v)) continue;  // erasures are not errors -- they are honest
        if ((v > threshold) != truth[idx]) ++wrong;
    }
    return wrong;
}

}  // namespace

TEST(PhotometricField, RejectsBadInput) {
    const auto layout = MakeLayout();
    std::vector<double> wrong_size(10, 0.0);
    EXPECT_EQ(PhotometricField::Estimate(layout, wrong_size).error(), Error::kLengthMismatch);

    std::vector<double> ok(kGrid.cells(), 128.0);
    EXPECT_FALSE(PhotometricField::Estimate(layout, ok, {.search_radius = 0}).ok());
    EXPECT_FALSE(PhotometricField::Estimate(layout, ok, {.min_separation = -1}).ok());
}

TEST(PhotometricField, RefusesWhenAPilotClassIsMissing) {
    // All pilots read the same -> no scale exists. Fabricating one would produce confident
    // garbage over the whole frame, so refusing is the only correct behaviour.
    const auto layout = MakeLayout();
    const std::vector<double> flat(kGrid.cells(), 128.0);
    auto f = PhotometricField::Estimate(layout, flat);
    ASSERT_TRUE(f.ok());  // both classes present, just equal
    // Every cell must be erased because local separation is ~0.
    const auto norm = f.value().Normalise(flat);
    for (const std::uint32_t idx : layout.payload_cells()) {
        EXPECT_TRUE(std::isnan(norm[idx]));
    }

    // Now genuinely remove one class: NaN out every bright pilot.
    std::vector<double> missing(kGrid.cells(), 40.0);
    for (const std::uint32_t idx : layout.pilot_cells()) {
        const std::uint32_t c = idx % kGrid.cols;
        const std::uint32_t r = idx / kGrid.cols;
        missing[idx] = (layout.PilotValue(c, r) == kLevelBright) ? std::nan("") : 40.0;
    }
    EXPECT_EQ(PhotometricField::Estimate(layout, missing).error(),
              Error::kDegenerateParameters);
}

TEST(PhotometricField, PilotLatticeIsBalancedAcrossBothLevels) {
    // REGRESSION for the layout defect found on 2026-08-02: the colour-pilot reservation
    // rule used to take exclusively from the BRIGHT parity, leaving the white level
    // estimated from half as many pilots as the black level. The white level is precisely
    // the one degraded by angular luminance falloff (RISK-025), so it needs at least as
    // much support as the black level, not half.
    const auto layout = MakeLayout();
    std::size_t bright = 0;
    std::size_t dark = 0;
    for (const std::uint32_t idx : layout.pilot_cells()) {
        const std::uint32_t c = idx % kGrid.cols;
        const std::uint32_t r = idx / kGrid.cols;
        if (layout.PilotValue(c, r) == kLevelBright) ++bright; else ++dark;
    }
    ASSERT_GT(bright, 0u);
    ASSERT_GT(dark, 0u);
    const double ratio = static_cast<double>(dark) / static_cast<double>(bright);
    EXPECT_LT(ratio, 1.35) << "bright pilots " << bright << " vs dark " << dark;
    EXPECT_GT(ratio, 0.74) << "bright pilots " << bright << " vs dark " << dark;
}

TEST(PhotometricField, FlatFieldMatchesGlobalEstimate) {
    const auto layout = MakeLayout();
    std::vector<bool> truth;
    const auto cells = RenderFrame(layout, &truth);
    const Homography h = AxisAligned();
    const Image8 img = Render(cells, h, kGrid.cols * kPx, kGrid.rows * kPx);

    auto s = CellSampler::Create(kGrid, {});
    ASSERT_TRUE(s.ok());
    const auto raw = s.value().Sample(img.view(), h);

    auto f = PhotometricField::Estimate(layout, raw);
    ASSERT_TRUE(f.ok()) << ErrorName(f.error());
    EXPECT_NEAR(f.value().bright_nonuniformity(), 1.0, 0.05);

    const auto norm = f.value().Normalise(raw);
    EXPECT_EQ(CountErrors(layout, norm, truth, 127.5), 0);
}

TEST(PhotometricField, LocalFieldBeatsGlobalUnderRadialFalloff) {
    // THE RISK-025 TEST. Radial luminance falloff (the S26 Ultra's Privacy Display dimming
    // the corners) PLUS directional glare lifting the black level on one side.
    //
    // Both impairments are needed, and finding that out was itself informative: pure
    // multiplicative falloff does NOT defeat a global threshold, because with a near-zero
    // black level the bright level must fall by more than ~50% before it crosses the global
    // midpoint. Glare squeezes from the other side, so the dim region's bright level and the
    // glared region's dark level overlap and NO single threshold separates both. That is the
    // regime where a confident wrong bit gets produced, and it costs the FEC layer roughly
    // twice what an erasure costs.
    const auto layout = MakeLayout();
    std::vector<bool> truth;
    const auto cells = RenderFrame(layout, &truth);
    const Homography h = AxisAligned();
    const Image8 img = Render(cells, h, kGrid.cols * kPx, kGrid.rows * kPx,
                              {.corner_falloff = 0.35, .glare_lift = 150.0});

    auto s = CellSampler::Create(kGrid, {});
    ASSERT_TRUE(s.ok());
    const auto raw = s.value().Sample(img.view(), h);

    auto f = PhotometricField::Estimate(layout, raw);
    ASSERT_TRUE(f.ok()) << ErrorName(f.error());

    // The field must actually SEE the non-uniformity, otherwise this test is vacuous.
    EXPECT_GT(f.value().bright_nonuniformity(), 1.2)
        << "renderer did not produce a non-uniform field";

    // Baseline: what a single global reference would have done.
    const PhotometricRef global = f.value().global_ref();
    const int global_errors = CountErrors(layout, raw, truth, global.threshold());

    // The interpolated field.
    const auto norm = f.value().Normalise(raw);
    const int field_errors = CountErrors(layout, norm, truth, 127.5);

    // A floor, not just >0: if the impairment ever stops actually breaking the global
    // threshold this test would silently become vacuous and "prove" the field works.
    //
    // MEASURED 2026-08-02 at these settings: global thresholding produces 39 payload errors,
    // the interpolated field produces 0. 39 is ~1.3% raw BER -- not catastrophic, but a real
    // burden on the FEC layer, and it is 39 CONFIDENT WRONG bits rather than erasures. The
    // floor is set below the observed value rather than at some round number chosen first;
    // inflating the impairment until it hit a nicer figure would be fitting the experiment
    // to the desired answer.
    EXPECT_GT(global_errors, static_cast<int>(layout.payload_cells().size()) / 200)
        << "impairment too weak to challenge the global threshold: " << global_errors;
    EXPECT_LT(field_errors, global_errors / 10)
        << "field=" << field_errors << " global=" << global_errors;
    EXPECT_EQ(field_errors, 0) << field_errors << " payload cells decoded wrongly";
}

TEST(PhotometricField, SurvivesFalloffPlusBlurAndNoise) {
    const auto layout = MakeLayout();
    std::vector<bool> truth;
    const auto cells = RenderFrame(layout, &truth);
    const Homography h = AxisAligned();
    const Image8 img = Render(cells, h, kGrid.cols * kPx, kGrid.rows * kPx,
                              {.corner_falloff = 0.55,
                               .black_lift = 10.0,
                               .blur_radius = 1,
                               .noise_amplitude = 8.0});

    auto s = CellSampler::Create(kGrid, {.interior_margin = 0.3, .samples_per_axis = 3});
    ASSERT_TRUE(s.ok());
    const auto raw = s.value().Sample(img.view(), h);

    auto f = PhotometricField::Estimate(layout, raw);
    ASSERT_TRUE(f.ok());
    // Noise must be MEASURED, not assumed -- it sets LLR magnitude downstream.
    EXPECT_GT(f.value().global_ref().noise_sigma, 1.0);

    const auto norm = f.value().Normalise(raw);
    const int errors = CountErrors(layout, norm, truth, 127.5);
    const auto payload_n = static_cast<int>(layout.payload_cells().size());
    EXPECT_LT(errors, payload_n / 100) << errors << " of " << payload_n;
}

TEST(PhotometricField, OccludedRegionErasesRatherThanGuesses) {
    const auto layout = MakeLayout();
    std::vector<bool> truth;
    const auto cells = RenderFrame(layout, &truth);
    const Homography h = AxisAligned();
    const Image8 img = Render(cells, h, kGrid.cols * kPx, kGrid.rows * kPx);

    auto s = CellSampler::Create(kGrid, {});
    ASSERT_TRUE(s.ok());
    auto raw = s.value().Sample(img.view(), h);

    // Occlude a block: sensor reads a flat mid-grey there, so local contrast collapses.
    for (std::uint32_t r = 20; r < 40; ++r) {
        for (std::uint32_t c = 10; c < 30; ++c) {
            raw[static_cast<std::size_t>(r) * kGrid.cols + c] = 120.0;
        }
    }

    auto f = PhotometricField::Estimate(layout, raw);
    ASSERT_TRUE(f.ok());
    const auto norm = f.value().Normalise(raw);

    // Cells deep inside the occlusion must be erased, not decoded confidently.
    int erased = 0;
    int total = 0;
    for (std::uint32_t r = 26; r < 34; ++r) {
        for (std::uint32_t c = 16; c < 24; ++c) {
            const std::size_t idx = static_cast<std::size_t>(r) * kGrid.cols + c;
            if (layout.role(c, r) != CellRole::kPayload) continue;
            ++total;
            if (std::isnan(norm[idx])) ++erased;
        }
    }
    ASSERT_GT(total, 0);
    EXPECT_GT(erased, total / 2) << erased << " of " << total << " erased";
}
