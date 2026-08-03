#include <fileflow/grid.h>

#include <gtest/gtest.h>

#include <map>

using namespace fileflow;

TEST(GridGeometry, RejectsDegenerateAndOversized) {
    EXPECT_EQ((GridGeometry{0, 100}).Validate().error(), Error::kValueOutOfRange);
    EXPECT_EQ((GridGeometry{100, 0}).Validate().error(), Error::kValueOutOfRange);
    EXPECT_EQ((GridGeometry{9999, 100}).Validate().error(), Error::kValueOutOfRange);
    EXPECT_TRUE((GridGeometry{120, 200}).Validate().ok());
}

TEST(FrameLayout, BuildsForAllCandidateGrids) {
    // The three candidate grids from the charter. Real limits come from EXP-001.
    for (auto [c, r] : {std::pair{96u, 160u}, std::pair{120u, 200u}, std::pair{144u, 240u}}) {
        auto l = FrameLayout::Create(GridGeometry{c, r}, LayoutConfig{});
        ASSERT_TRUE(l.ok()) << c << "x" << r << ": " << ErrorName(l.error());
        EXPECT_GT(l.value().payload_cells().size(), 0u);
        EXPECT_GT(l.value().header_cells().size(), 0u);
        EXPECT_GT(l.value().pilot_cells().size(), 0u);
    }
}

TEST(FrameLayout, RejectsGridTooSmallForItsStructure) {
    // Markers plus guards must leave a usable interior, or the layout is meaningless.
    auto l = FrameLayout::Create(GridGeometry{16, 16}, LayoutConfig{});
    EXPECT_EQ(l.error(), Error::kValueOutOfRange);
}

TEST(FrameLayout, EveryCellHasExactlyOneRole) {
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    ASSERT_TRUE(l.ok());
    const auto& layout = l.value();

    std::map<CellRole, std::size_t> counts;
    for (std::uint32_t r = 0; r < 200; ++r) {
        for (std::uint32_t c = 0; c < 120; ++c) counts[layout.role(c, r)]++;
    }

    std::size_t total = 0;
    for (auto [role, n] : counts) total += n;
    EXPECT_EQ(total, 120u * 200u);

    EXPECT_GT(counts[CellRole::kMarker], 0u);
    EXPECT_GT(counts[CellRole::kPilot], 0u);
    EXPECT_GT(counts[CellRole::kColorPilot], 0u);  // reserved for M3 from day one
    EXPECT_GT(counts[CellRole::kHeader], 0u);
    EXPECT_GT(counts[CellRole::kPayload], 0u);
    EXPECT_GT(counts[CellRole::kTimingTrack], 0u);
    EXPECT_GT(counts[CellRole::kBoundary], 0u);
}

TEST(FrameLayout, ThePerimeterIsEntirelyBoundaryOrMarker) {
    // The boundary ring is what screen localisation depends on (ADR-0006, findings F10/F15),
    // so it must be UNBROKEN. A single perimeter cell carrying payload would put data in the
    // one structure that has to stay constant across frames -- and would only show up as
    // occasional, content-dependent detection failures.
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    ASSERT_TRUE(l.ok());
    const auto& layout = l.value();

    for (std::uint32_t r = 0; r < 200; ++r) {
        for (std::uint32_t c = 0; c < 120; ++c) {
            const bool on_perimeter = (c == 0 || c == 119 || r == 0 || r == 199);
            if (!on_perimeter) continue;
            const CellRole role = layout.role(c, r);
            EXPECT_TRUE(role == CellRole::kBoundary || role == CellRole::kMarker)
                << "perimeter cell (" << c << "," << r << ") has role "
                << static_cast<int>(role);
        }
    }
}

TEST(FrameLayout, IndexListsMatchRoles) {
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    ASSERT_TRUE(l.ok());
    const auto& layout = l.value();
    const std::uint32_t cols = layout.geometry().cols;

    for (std::uint32_t idx : layout.payload_cells()) {
        EXPECT_EQ(layout.role(idx % cols, idx / cols), CellRole::kPayload);
    }
    for (std::uint32_t idx : layout.header_cells()) {
        EXPECT_EQ(layout.role(idx % cols, idx / cols), CellRole::kHeader);
    }
    for (std::uint32_t idx : layout.pilot_cells()) {
        EXPECT_EQ(layout.role(idx % cols, idx / cols), CellRole::kPilot);
    }
}

TEST(FrameLayout, OverheadIsInTheModelledRange) {
    // The performance model assumes O between 0.12 and 0.20 for the candidate layouts
    // (docs/specifications/PERFORMANCE-MODEL.md). If the real layout drifts far outside
    // that, the model's scenarios need regenerating -- so this test guards the assumption.
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    ASSERT_TRUE(l.ok());
    const double o = l.value().overhead_fraction();
    EXPECT_GT(o, 0.0);
    EXPECT_LT(o, 0.35) << "layout overhead O=" << o << " is far above the modelled range";
}

TEST(FrameLayout, PilotsAreDistributedNotClustered) {
    // The defining property of Candidate B: pilots spread through the payload area so the
    // illumination field is INTERPOLATED rather than extrapolated from a border.
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    ASSERT_TRUE(l.ok());
    const auto& layout = l.value();
    const std::uint32_t cols = layout.geometry().cols;
    const std::uint32_t rows = layout.geometry().rows;

    bool in_top = false, in_bottom = false, in_left = false, in_right = false, in_middle = false;
    for (std::uint32_t idx : layout.pilot_cells()) {
        const std::uint32_t c = idx % cols;
        const std::uint32_t r = idx / cols;
        if (r < rows / 3) in_top = true;
        if (r > 2 * rows / 3) in_bottom = true;
        if (c < cols / 3) in_left = true;
        if (c > 2 * cols / 3) in_right = true;
        if (r > rows / 3 && r < 2 * rows / 3 && c > cols / 3 && c < 2 * cols / 3) in_middle = true;
    }
    EXPECT_TRUE(in_top && in_bottom && in_left && in_right && in_middle)
        << "pilots must cover the whole grid, including the centre";
}

TEST(CellMatrix, StoresAndRetrieves) {
    CellMatrix m(10, 20);
    EXPECT_EQ(m.cols(), 10u);
    EXPECT_EQ(m.rows(), 20u);
    m.set(3, 4, 200);
    EXPECT_EQ(m.at(3, 4), 200);
    EXPECT_EQ(m.flat(4 * 10 + 3), 200);
}
