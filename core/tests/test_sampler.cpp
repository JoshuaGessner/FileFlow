#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <gtest/gtest.h>

#include <cmath>

#include "render_test_util.h"

using namespace fileflow;
using namespace fileflow::test;

namespace {

// A checkerboard is the worst case for crosstalk: every cell's neighbours are its opposite,
// so any sampling error shows up immediately as a mid-grey reading.
CellMatrix Checkerboard(const GridGeometry& g) {
    CellMatrix m(g.cols, g.rows);
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            m.set(c, r, ((c + r) % 2 == 0) ? kLevelBright : kLevelDark);
        }
    }
    return m;
}

Homography FitGrid(const GridGeometry& g, const std::array<Point2, 4>& quad) {
    const auto corners = GridCorners(g);
    auto h = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    EXPECT_TRUE(h.ok());
    return h.value();
}

}  // namespace

TEST(CellSampler, RejectsInvalidConfig) {
    const GridGeometry g{24, 40};
    EXPECT_FALSE(CellSampler::Create(g, {.interior_margin = 0.5}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.interior_margin = -0.1}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.samples_per_axis = 0}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.samples_per_axis = 99}).ok());
    EXPECT_TRUE(CellSampler::Create(g, {}).ok());
}

TEST(CellSampler, RecoversCheckerboardUnderAxisAlignedMapping) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    // 8 px per cell, no perspective.
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 192, 320);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);
    ASSERT_EQ(samples.size(), g.cells());

    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            const double v = samples[static_cast<std::size_t>(r) * g.cols + c];
            ASSERT_FALSE(std::isnan(v)) << "cell " << c << "," << r;
            const double expected = ((c + r) % 2 == 0) ? 255.0 : 0.0;
            EXPECT_NEAR(v, expected, 1.0) << "cell " << c << "," << r;
        }
    }
}

TEST(CellSampler, RecoversCheckerboardUnderPerspective) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    // A genuinely projective quad: the grid is viewed off-axis.
    const std::array<Point2, 4> quad{Point2{30, 20}, Point2{300, 55},
                                     Point2{280, 380}, Point2{55, 350}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 340, 420);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);

    int wrong = 0;
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            const double v = samples[static_cast<std::size_t>(r) * g.cols + c];
            if (std::isnan(v)) {
                ++wrong;
                continue;
            }
            const bool bright_expected = ((c + r) % 2 == 0);
            if ((v > 127.5) != bright_expected) ++wrong;
        }
    }
    // Under perspective the far edge is compressed to ~4 px/cell; a handful of boundary
    // cells may be marginal, but the bulk must decode.
    EXPECT_LT(wrong, static_cast<int>(g.cells()) / 100) << wrong << " of " << g.cells();
}

TEST(CellSampler, InteriorMarginRejectsNeighbourCrosstalk) {
    // THE point of interior-margin sampling. Under blur, a full-cell sample averages in the
    // neighbours; an interior sample does not. On a checkerboard that difference is the
    // difference between decoding and reading uniform grey.
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 192, 320, {.blur_radius = 2});

    const auto contrast = [&](double margin) {
        auto s = CellSampler::Create(g, {.interior_margin = margin, .samples_per_axis = 3});
        EXPECT_TRUE(s.ok());
        const auto v = s.value().Sample(img.view(), h);
        double bright = 0;
        double dark = 0;
        int nb = 0;
        int nd = 0;
        // Interior cells only, so frame-edge effects do not confound the comparison.
        for (std::uint32_t r = 2; r < g.rows - 2; ++r) {
            for (std::uint32_t c = 2; c < g.cols - 2; ++c) {
                const double x = v[static_cast<std::size_t>(r) * g.cols + c];
                if (std::isnan(x)) continue;
                if ((c + r) % 2 == 0) { bright += x; ++nb; } else { dark += x; ++nd; }
            }
        }
        EXPECT_GT(nb, 0);
        EXPECT_GT(nd, 0);
        return bright / nb - dark / nd;
    };

    const double tight = contrast(0.35);
    const double loose = contrast(0.02);
    EXPECT_GT(tight, loose) << "interior margin did not improve contrast: tight=" << tight
                            << " loose=" << loose;
}

TEST(CellSampler, OutOfFrameCellsBecomeErasuresNotGuesses) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    // Image covers only the top-left quadrant of the grid.
    const Image8 img = Render(cells, h, 96, 160);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);

    std::size_t nan_count = 0;
    for (const double v : samples) {
        if (std::isnan(v)) ++nan_count;
    }
    EXPECT_GT(nan_count, 0u) << "cells outside the image must be erasures";
    EXPECT_LT(nan_count, samples.size()) << "the visible quadrant must still decode";

    // Specifically: a cell well outside the image must be NaN, never a fabricated value.
    EXPECT_TRUE(std::isnan(samples[static_cast<std::size_t>(g.rows - 1) * g.cols + g.cols - 1]));
}

TEST(CellSampler, EmptyImageYieldsAllErasures) {
    const GridGeometry g{8, 8};
    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(ImageView8{}, Homography::Identity());
    ASSERT_EQ(samples.size(), g.cells());
    for (const double v : samples) EXPECT_TRUE(std::isnan(v));
}
