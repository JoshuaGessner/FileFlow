// Why does independent per-frame detection fail on frames the tracker handles fine?
//
// Observed in ffsim --image-path --no-tracking: 301 of 447 frames refused by full-image
// acquisition, while tracking decoded all of them. That is a large effect and it bears
// directly on ADR-0006, so this measures the mechanism rather than assuming one.
#include <fileflow/detect.h>
#include <fileflow/fountain.h>
#include <fileflow/frame.h>
#include <fileflow/modulation.h>
#include <fileflow/sim/render.h>
#include <fileflow/transfer.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

using namespace fileflow;

namespace {

constexpr GridGeometry kGrid{120, 200};

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(kGrid, LayoutConfig{});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

// Frames differing ONLY in payload content, exactly as a real transfer produces.
CellMatrix FrameWithPayload(const FrameLayout& layout, std::uint64_t seed) {
    const M0Modulator mod(layout);
    SplitMix64 rng(seed);
    std::vector<std::uint8_t> hdr(mod.header_capacity_bytes());
    std::vector<std::uint8_t> pay(mod.payload_capacity_bytes());
    for (auto& b : hdr) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    for (auto& b : pay) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    CellMatrix m(kGrid.cols, kGrid.rows);
    EXPECT_TRUE(mod.Render(hdr, pay, &m).ok());
    return m;
}

}  // namespace

TEST(DetectStability, IndependentDetectionVariesWithPayloadContent) {
    // Same geometry, same lighting, same everything -- only the PAYLOAD BITS differ between
    // frames. If independent detection were content-independent, every frame would score
    // identically. Any spread is the payload leaking into the geometry stage.
    const auto layout = MakeLayout();
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());

    sim::OpticalRenderConfig rc;
    rc.view.image_width = 1200;
    rc.view.image_height = 2000;

    int refused = 0;
    double min_score = 1.0, max_score = 0.0;
    double min_margin = 1.0, max_margin = 0.0;
    std::uint32_t min_thr = 255, max_thr = 0;

    constexpr int kFrames = 40;
    for (int i = 0; i < kFrames; ++i) {
        Homography truth;
        const Image8 img =
            sim::RenderView(FrameWithPayload(layout, 1000 + static_cast<std::uint64_t>(i)),
                            kGrid, rc, &truth);

        const std::uint32_t thr = ScreenDetector::OtsuThreshold(img.view(), 2);
        min_thr = std::min(min_thr, thr);
        max_thr = std::max(max_thr, thr);

        auto d = det.value().Detect(img.view());
        if (!d.ok()) {
            ++refused;
            continue;
        }
        min_score = std::min(min_score, d.value().marker_score);
        max_score = std::max(max_score, d.value().marker_score);
        const double margin = d.value().marker_score - d.value().runner_up_score;
        min_margin = std::min(min_margin, margin);
        max_margin = std::max(max_margin, margin);
    }

    std::printf("[detect-stability] over %d frames differing only in payload:\n", kFrames);
    std::printf("   refused        %d / %d\n", refused, kFrames);
    std::printf("   Otsu threshold %u .. %u\n", min_thr, max_thr);
    if (refused < kFrames) {
        std::printf("   marker score   %.3f .. %.3f  (min required %.2f)\n", min_score,
                    max_score, DetectionConfig{}.min_marker_score);
        std::printf("   rotation margin %.3f .. %.3f (min required %.2f)\n", min_margin,
                    max_margin, DetectionConfig{}.min_rotation_margin);
    }

    // Localisation must be COMPLETELY independent of payload content. These frames differ
    // only in their payload bits, so every measured quantity should be identical across all
    // of them -- not merely "close". Any spread at all means data is reaching the geometry
    // stage, which is the class of defect F15 turned out to be.
    EXPECT_EQ(refused, 0);
    EXPECT_EQ(min_thr, max_thr) << "binarisation threshold varied with payload content";
    EXPECT_DOUBLE_EQ(min_score, max_score) << "marker score varied with payload content";
    EXPECT_DOUBLE_EQ(min_margin, max_margin) << "rotation margin varied with payload content";
    EXPECT_GT(min_score, DetectionConfig{}.min_marker_score);
}

TEST(DetectStability, RealTransmitterFramesReproduceTheFfsimFailure) {
    // The previous test uses RANDOM payload bytes and detection is perfectly stable. ffsim
    // uses real FileTransmitter output and fails ~67% of frames. So the difference is in the
    // frame CONTENT, and this pins down which frames break it.
    const auto layout = MakeLayout();
    const M0Modulator mod(layout);
    const HeaderCodec hdr_codec;
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());

    std::vector<std::uint8_t> payload(16384);
    SplitMix64 prng(0xF11EF10ULL);
    for (auto& b : payload) b = static_cast<std::uint8_t>(prng.Next() & 0xFF);

    const auto symbol_size = static_cast<std::uint32_t>(mod.payload_capacity_bytes());
    auto tx = FileTransmitter::Create(payload, "sim.bin", 0x51533101, symbol_size, 64);
    ASSERT_TRUE(tx.ok()) << ErrorName(tx.error());

    sim::OpticalRenderConfig rc;
    rc.view.image_width = 1200;
    rc.view.image_height = 2000;

    int refused = 0;
    int systematic_refused = 0;
    int repair_refused = 0;
    constexpr int kFrames = 60;

    for (int i = 0; i < kFrames; ++i) {
        auto fp = tx.value().NextFrame();
        auto hc = hdr_codec.Encode(fp.header);
        ASSERT_TRUE(hc.ok());
        CellMatrix frame(kGrid.cols, kGrid.rows);
        ASSERT_TRUE(mod.Render(hc.value(), fp.data, &frame).ok());

        Homography truth;
        const Image8 img = sim::RenderView(frame, kGrid, rc, &truth);

        // Diagnostics: how much of the SCREEN is lit, versus how much of the IMAGE the
        // screen occupies. If detection tracks the former, geometry depends on payload data.
        if (i < 4 || (refused > 0 && refused < 3)) {
            const std::uint32_t thr = ScreenDetector::OtsuThreshold(img.view(), 2);
            std::uint64_t lit = 0;
            std::uint64_t bright_cells = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (img.at(x, y) > thr) ++lit;
                }
            }
            for (std::uint32_t ci = 0; ci < kGrid.cells(); ++ci) {
                if (frame.flat(ci) == kLevelBright) ++bright_cells;
            }
            const double img_area = static_cast<double>(img.width()) * img.height();
            std::printf("   frame %2d esi=%u: bright cells %llu/%u, lit px %llu "
                        "(%.4f of image; threshold needs %.4f)\n",
                        i, fp.header.esi, static_cast<unsigned long long>(bright_cells),
                        kGrid.cells(), static_cast<unsigned long long>(lit),
                        static_cast<double>(lit) / img_area,
                        DetectionConfig{}.min_area_fraction);
        }

        if (!det.value().Detect(img.view()).ok()) {
            ++refused;
            // Systematic symbols carry source data; repair symbols are XOR combinations and
            // have different statistics.
            if (fp.header.esi < tx.value().manifest().block_symbols) {
                ++systematic_refused;
            } else {
                ++repair_refused;
            }
        }
    }

    std::printf("[detect-stability] real transmitter frames: %d/%d refused "
                "(%d systematic, %d repair)\n",
                refused, kFrames, systematic_refused, repair_refused);

    // REGRESSION for finding F15. Localisation must not depend on payload data at all: the
    // persistent boundary ring and the corner markers exist precisely so the screen is found
    // the same way whatever is being transmitted. Zero-padded systematic symbols are the
    // natural counterexample and are entirely normal -- any file whose final block is padded,
    // or which simply contains a run of zeros, produces them.
    EXPECT_EQ(refused, 0) << refused << " of " << kFrames
                          << " frames refused; localisation is payload-dependent again";
}

TEST(DetectStability, DetectsAnAlmostEntirelyDarkFrame) {
    // The extreme case, stated directly rather than via the transmitter: a frame whose payload
    // is ALL ZEROS. Only the boundary ring, markers, pilots, timing tracks and header are lit
    // -- about 4.5% of cells. The screen still occupies most of the image, so it must still be
    // found. This is the frame the old lit-pixel-count check rejected.
    const auto layout = MakeLayout();
    const M0Modulator mod(layout);
    const HeaderCodec hdr_codec;
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());

    FrameHeader h{};
    h.session_id = 0x1234;
    h.block_id = 0;
    h.esi = 0;
    h.payload_bytes = static_cast<std::uint32_t>(mod.payload_capacity_bytes());
    auto hc = hdr_codec.Encode(h);
    ASSERT_TRUE(hc.ok());

    const std::vector<std::uint8_t> zeros(mod.payload_capacity_bytes(), 0);
    CellMatrix frame(kGrid.cols, kGrid.rows);
    ASSERT_TRUE(mod.Render(hc.value(), zeros, &frame).ok());

    std::uint64_t bright = 0;
    for (std::uint32_t i = 0; i < kGrid.cells(); ++i) {
        if (frame.flat(i) == kLevelBright) ++bright;
    }
    // Confirm the frame really is mostly dark, so the test cannot pass vacuously.
    EXPECT_LT(static_cast<double>(bright) / kGrid.cells(), 0.15)
        << "frame was not dark enough to exercise the case";

    sim::OpticalRenderConfig rc;
    rc.view.image_width = 1200;
    rc.view.image_height = 2000;
    Homography truth;
    const Image8 img = sim::RenderView(frame, kGrid, rc, &truth);

    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok()) << "an all-zero payload made the screen undetectable: "
                        << ErrorName(d.error());
    EXPECT_GT(d.value().marker_score, 0.9);
}
