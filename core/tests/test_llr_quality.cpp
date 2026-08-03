// Is the soft information actually soft, and is it actually informative?
//
// The architecture rests on "soft symbol confidence should be preserved for forward-error
// correction" (project charter, ADR-0009). That hypothesis has two halves, and neither had ever
// been measured:
//
//   1. GRADATION -- does |llr| take a useful range of values, or does it saturate to a
//      one-bit-plus-erasure signal that only pretends to be soft?
//   2. INFORMATIVENESS -- do low-|llr| cells actually decode wrong more often than high-|llr|
//      cells? If not, the magnitude is decoration and erasure-marking from it cannot work.
//
// These print distributions rather than asserting thresholds, because the numbers are the
// finding. Only gross regressions are asserted.
#include <fileflow/fountain.h>
#include <fileflow/modulation.h>
#include <fileflow/photometric.h>
#include <fileflow/sampler.h>
#include <fileflow/sim/render.h>
#include <fileflow/tracker.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace fileflow;

namespace {

constexpr GridGeometry kGrid{48, 80};

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(
        kGrid, {.pilot_pitch = 8, .marker_size = 6, .header_rows = 4, .guard_width = 1,
                .boundary_width = 1});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

}  // namespace

namespace {

// Runs one noise level and reports the |llr| distribution plus the per-band error rate.
void SurveyAtNoise(double noise_amplitude) {
    const auto layout = MakeLayout();
    const M0Modulator mod(layout);

    sim::OpticalRenderConfig rc;
    rc.view.image_width = 700;
    rc.view.image_height = 1100;
    rc.view.yaw_deg = 14.0;
    rc.corner_falloff = 0.45;
    rc.glare_lift = 70.0;
    rc.blur_radius = 1;
    rc.noise_amplitude = noise_amplitude;

    auto tracker = ScreenTracker::Create(layout, {});
    EXPECT_TRUE(tracker.ok());
    auto sampler = CellSampler::Create(kGrid, {.interior_margin = 0.3, .samples_per_axis = 3});
    EXPECT_TRUE(sampler.ok());

    // |llr| bucketed into 8 bands of 16, plus the error rate within each band.
    std::array<std::uint64_t, 8> band_count{};
    std::array<std::uint64_t, 8> band_wrong{};
    std::uint64_t erased = 0;
    std::uint64_t total = 0;

    SplitMix64 rng(31337);
    for (int frame = 0; frame < 12; ++frame) {
        std::vector<std::uint8_t> hdr(mod.header_capacity_bytes());
        std::vector<std::uint8_t> pay(mod.payload_capacity_bytes());
        for (auto& b : hdr) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        for (auto& b : pay) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);

        CellMatrix tx(kGrid.cols, kGrid.rows);
        EXPECT_TRUE(mod.Render(hdr, pay, &tx).ok());

        rc.seed = 900 + static_cast<std::uint64_t>(frame);
        Homography truth;
        const Image8 img = sim::RenderView(tx, kGrid, rc, &truth);

        const TrackResult tr = tracker.value().Track(img.view());
        if (!tr.ok) continue;
        const auto raw = sampler.value().Sample(img.view(), tr.grid_to_image);
        auto field = PhotometricField::Estimate(layout, raw);
        if (!field.ok()) continue;
        const auto norm = field.value().Normalise(raw);

        const PhotometricRef ref = mod.EstimateReference(norm);
        SoftSymbolBuffer soft;
        mod.DemodulatePayload(norm, ref, &soft);

        // Ground truth for each payload cell, straight from what was transmitted.
        const auto& pay_cells = layout.payload_cells();
        for (std::size_t i = 0; i < soft.size() && i < pay_cells.size(); ++i) {
            ++total;
            const SoftSymbol& s = soft.symbols[i];
            if (s.erased || s.llr == kLlrErasure) {
                ++erased;
                continue;
            }
            const bool decoded_dark = s.llr > 0;
            const bool truth_dark = tx.flat(pay_cells[i]) != kLevelBright;

            const int mag = s.llr < 0 ? -static_cast<int>(s.llr) : static_cast<int>(s.llr);
            const auto band = static_cast<std::size_t>(std::min(mag / 16, 7));
            ++band_count[band];
            if (decoded_dark != truth_dark) ++band_wrong[band];
        }
    }

    EXPECT_GT(total, 1000u);

    std::printf("[llr] noise=%.0f: %llu cells, %llu erased (%.1f%%)\n",
                noise_amplitude, static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(erased),
                100.0 * static_cast<double>(erased) / static_cast<double>(total));
    std::printf("[llr] |llr| band    count        error rate\n");
    std::uint64_t decided = 0;
    std::uint64_t wrong = 0;
    for (std::size_t b = 0; b < 8; ++b) {
        decided += band_count[b];
        wrong += band_wrong[b];
        const double er = band_count[b] ? static_cast<double>(band_wrong[b]) /
                                              static_cast<double>(band_count[b])
                                        : 0.0;
        std::printf("       %3zu-%3zu     %10llu     %.4f\n", b * 16, b * 16 + 15,
                    static_cast<unsigned long long>(band_count[b]), er);
    }
    std::printf("[llr] overall symbol error rate %.5f\n",
                decided ? static_cast<double>(wrong) / static_cast<double>(decided) : 0.0);

    // GRADATION: if nearly everything lands in the top band the signal is one-bit plus an
    // erasure flag, and calling it "soft" is a fiction.
    const double top_band_share =
        decided ? static_cast<double>(band_count[7]) / static_cast<double>(decided) : 1.0;
    std::printf("[llr] share in the saturated top band: %.3f\n", top_band_share);

    EXPECT_GT(decided, 0u);
}

}  // namespace

TEST(LlrQuality, SaturatesUntilTheChannelIsGenuinelyMarginal) {
    // Sweeps noise to answer whether the LLR CAN gradate, or whether the quantisation is
    // simply broken. If the top-band share falls and the low bands acquire a higher error rate
    // as noise rises, the mechanism works and clean channels are genuinely unambiguous. If the
    // distribution never moves, the scale factor is wrong and the "soft" path is a fiction.
    for (const double noise : {0.0, 30.0, 60.0, 90.0, 120.0}) {
        SurveyAtNoise(noise);
        std::printf("\n");
    }
}
