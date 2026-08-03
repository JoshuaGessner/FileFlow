// Capture bundle round-trip and replay fidelity (component C17).
//
// THE LOAD-BEARING TEST is ReplayIsBitIdenticalToLiveDecode. A recorded dataset is only worth
// keeping if replaying it produces exactly what the original decode produced -- otherwise a
// bug that reproduces in replay might be an artifact of the harness, and a fix validated
// against replayed frames might not hold live. Proving that BEFORE any real hardware exists
// means the first real capture arrives on a path already known to be faithful.
#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/harness/capture.h>
#include <fileflow/modulation.h>
#include <fileflow/sim/render.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fileflow;
using namespace fileflow::harness;

namespace {

namespace fs = std::filesystem;

constexpr GridGeometry kGrid{48, 80};

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(
        kGrid, {.pilot_pitch = 8, .marker_size = 6, .header_rows = 4, .guard_width = 1,
                .boundary_width = 1});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

std::string TempBundle(const char* name) {
    const auto dir = fs::temp_directory_path() / (std::string("ff-cap-") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir.string();
}

CaptureMetadata GoodMetadata(int w, int h) {
    CaptureMetadata m;
    m.sender_model = "Pixel 8";
    m.receiver_model = "Galaxy S26 Ultra";
    m.os_build = "test";
    m.app_commit = "deadbeef";
    m.grid_cols = kGrid.cols;
    m.grid_rows = kGrid.rows;
    m.modulation_profile = "M0";
    m.width = static_cast<std::uint32_t>(w);
    m.height = static_cast<std::uint32_t>(h);
    m.fps = 60.0;
    m.distance_cm = 30.0;
    m.angle_deg = 0.0;
    m.motion_condition = "rigid";
    m.source_payload_sha256 = std::string(64, 'a');
    m.source_payload_bytes = 4096;
    return m;
}

std::vector<Image8> RenderFrames(const FrameLayout& layout, int n, int w, int h) {
    const M0Modulator mod(layout);
    SplitMix64 rng(2024);
    std::vector<Image8> out;
    sim::OpticalRenderConfig rc;
    rc.view.image_width = w;
    rc.view.image_height = h;

    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> hdr(mod.header_capacity_bytes());
        std::vector<std::uint8_t> pay(mod.payload_capacity_bytes());
        for (auto& b : hdr) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        for (auto& b : pay) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        CellMatrix m(kGrid.cols, kGrid.rows);
        EXPECT_TRUE(mod.Render(hdr, pay, &m).ok());
        Homography truth;
        out.push_back(sim::RenderView(m, kGrid, rc, &truth));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- metadata

TEST(CaptureMetadata, RoundTripsThroughText) {
    const auto m = GoodMetadata(640, 900);
    auto parsed = CaptureMetadata::Parse(m.Serialise());
    ASSERT_TRUE(parsed.ok()) << ErrorName(parsed.error());
    const auto& p = parsed.value();

    EXPECT_EQ(p.sender_model, m.sender_model);
    EXPECT_EQ(p.receiver_model, m.receiver_model);
    EXPECT_EQ(p.app_commit, m.app_commit);
    EXPECT_EQ(p.grid_cols, m.grid_cols);
    EXPECT_EQ(p.grid_rows, m.grid_rows);
    EXPECT_EQ(p.width, m.width);
    EXPECT_EQ(p.height, m.height);
    EXPECT_DOUBLE_EQ(p.fps, m.fps);
    EXPECT_DOUBLE_EQ(p.distance_cm, m.distance_cm);
    EXPECT_EQ(p.motion_condition, m.motion_condition);
    EXPECT_EQ(p.source_payload_sha256, m.source_payload_sha256);
}

TEST(CaptureMetadata, UnrecordedFieldsStayVisiblyUnset) {
    // A capture that never recorded its distance must NOT come back claiming a plausible one.
    // Negative sentinels survive the round trip so an incomplete dataset stays obviously
    // incomplete rather than quietly acquiring defaults.
    CaptureMetadata m;
    m.width = 320;
    m.height = 480;
    auto parsed = CaptureMetadata::Parse(m.Serialise());
    ASSERT_TRUE(parsed.ok());
    EXPECT_LT(parsed.value().distance_cm, 0.0);
    EXPECT_LT(parsed.value().exposure_ns, 0.0);
    EXPECT_LT(parsed.value().iso, 0.0);
    EXPECT_TRUE(parsed.value().sender_model.empty());
}

TEST(CaptureMetadata, ReportsWhatIsMissing) {
    CaptureMetadata m;
    m.width = 320;
    m.height = 480;
    const auto missing = m.MissingRequiredFields();
    EXPECT_FALSE(missing.empty());
    // The point is naming them, so a half-labelled dataset cannot quietly become evidence.
    EXPECT_NE(std::find(missing.begin(), missing.end(), "distance_cm"), missing.end());
    EXPECT_NE(std::find(missing.begin(), missing.end(), "app_commit"), missing.end());

    EXPECT_TRUE(GoodMetadata(640, 900).MissingRequiredFields().empty());
}

TEST(CaptureMetadata, RejectsHostileDimensions) {
    CaptureMetadata m;
    m.width = 0;
    m.height = 100;
    EXPECT_FALSE(m.Validate().ok());

    m.width = 100000;
    m.height = 100000;  // 10 GP -- would be a 10 GB allocation per frame
    EXPECT_FALSE(m.Validate().ok());
}

TEST(CaptureMetadata, SurvivesArbitraryGarbage) {
    // The parser reads files off disk. Ours today, someone else's eventually.
    SplitMix64 rng(99);
    for (int trial = 0; trial < 2000; ++trial) {
        std::string junk;
        const std::size_t n = rng.Next() % 300;
        for (std::size_t i = 0; i < n; ++i) {
            junk.push_back(static_cast<char>(rng.Next() & 0xFF));
        }
        auto r = CaptureMetadata::Parse(junk);
        (void)r;  // must not crash, hang, or over-allocate
    }
}

// ---------------------------------------------------------------- bundle round trip

TEST(CaptureBundle, WriteThenReplayRecoversEveryFrame) {
    const auto layout = MakeLayout();
    const std::string dir = TempBundle("roundtrip");
    constexpr int kW = 640;
    constexpr int kH = 900;
    const auto frames = RenderFrames(layout, 5, kW, kH);

    auto w = CaptureWriter::Create(dir, GoodMetadata(kW, kH));
    ASSERT_TRUE(w.ok()) << ErrorName(w.error());
    for (const auto& f : frames) ASSERT_TRUE(w.value().WriteFrame(f.view()).ok());
    ASSERT_TRUE(w.value().Finish().ok());

    auto src = ReplaySource::Create(dir, layout);
    ASSERT_TRUE(src.ok()) << ErrorName(src.error());
    EXPECT_EQ(src.value().metadata().frame_count, 5u);

    int seen = 0;
    while (auto f = src.value().Next()) {
        EXPECT_EQ(f->index, static_cast<std::uint64_t>(seen));
        ++seen;
    }
    EXPECT_EQ(seen, 5);
    EXPECT_EQ(src.value().frames_emitted(), 5u);
    EXPECT_EQ(src.value().frames_dropped(), 0u);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CaptureBundle, ReplayIsBitIdenticalToLiveDecode) {
    // THE test. Decode the frames directly through FramePipeline, then write the identical
    // images to a bundle and replay them. Every cell sample must match EXACTLY -- not
    // approximately -- because anything less means replayed results are not evidence about
    // live behaviour, and every recorded dataset would carry an unknown error term.
    const auto layout = MakeLayout();
    const std::string dir = TempBundle("fidelity");
    constexpr int kW = 640;
    constexpr int kH = 900;
    const auto frames = RenderFrames(layout, 6, kW, kH);

    // Path A: straight through the pipeline, as a live receiver would.
    auto direct = FramePipeline::Create(layout);
    ASSERT_TRUE(direct.ok());
    std::vector<std::vector<double>> live;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        live.push_back(
            direct.value()
                .Process(frames[i].view(), i, static_cast<std::int64_t>(i) * 16'666'667)
                .cell_samples);
    }

    // Path B: through a written and replayed bundle.
    auto w = CaptureWriter::Create(dir, GoodMetadata(kW, kH));
    ASSERT_TRUE(w.ok());
    for (const auto& f : frames) ASSERT_TRUE(w.value().WriteFrame(f.view()).ok());
    ASSERT_TRUE(w.value().Finish().ok());

    auto src = ReplaySource::Create(dir, layout);
    ASSERT_TRUE(src.ok());

    std::size_t idx = 0;
    while (auto f = src.value().Next()) {
        ASSERT_LT(idx, live.size());
        ASSERT_EQ(f->cell_samples.size(), live[idx].size());
        for (std::size_t c = 0; c < live[idx].size(); ++c) {
            const double a = live[idx][c];
            const double b = f->cell_samples[c];
            if (std::isnan(a)) {
                EXPECT_TRUE(std::isnan(b)) << "frame " << idx << " cell " << c
                                           << ": live erased, replay did not";
            } else {
                // Bit-identical, not near. Both paths run the same code on the same bytes.
                EXPECT_DOUBLE_EQ(a, b) << "frame " << idx << " cell " << c;
            }
        }
        ++idx;
    }
    EXPECT_EQ(idx, live.size());

    // And the diagnostics must agree too, not just the samples.
    EXPECT_EQ(src.value().pipeline().diagnostics().frames_decoded,
              direct.value().diagnostics().frames_decoded);
    EXPECT_EQ(src.value().pipeline().diagnostics().total_pixels_examined,
              direct.value().diagnostics().total_pixels_examined);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CaptureBundle, RewindReplaysIdentically) {
    // Re-running one dataset under different configurations is the reason to keep it. That
    // only works if a rewind is a true reset -- including the tracker's lock.
    const auto layout = MakeLayout();
    const std::string dir = TempBundle("rewind");
    constexpr int kW = 640;
    constexpr int kH = 900;
    const auto frames = RenderFrames(layout, 4, kW, kH);

    auto w = CaptureWriter::Create(dir, GoodMetadata(kW, kH));
    ASSERT_TRUE(w.ok());
    for (const auto& f : frames) ASSERT_TRUE(w.value().WriteFrame(f.view()).ok());
    ASSERT_TRUE(w.value().Finish().ok());

    auto src = ReplaySource::Create(dir, layout);
    ASSERT_TRUE(src.ok());

    std::vector<std::vector<double>> pass1;
    while (auto f = src.value().Next()) pass1.push_back(f->cell_samples);

    src.value().Rewind();
    std::size_t i = 0;
    while (auto f = src.value().Next()) {
        ASSERT_LT(i, pass1.size());
        for (std::size_t c = 0; c < f->cell_samples.size(); ++c) {
            const double a = pass1[i][c];
            const double b = f->cell_samples[c];
            if (std::isnan(a)) EXPECT_TRUE(std::isnan(b));
            else EXPECT_DOUBLE_EQ(a, b) << "rewind changed frame " << i << " cell " << c;
        }
        ++i;
    }
    EXPECT_EQ(i, pass1.size());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CaptureBundle, RefusesAGridMismatch) {
    // A bundle recorded at one grid decoded against another yields garbage that looks like a
    // channel result. Catch it at open.
    const std::string dir = TempBundle("mismatch");
    constexpr int kW = 640;
    constexpr int kH = 900;
    auto meta = GoodMetadata(kW, kH);
    meta.grid_cols = 96;  // not what the layout below uses
    meta.grid_rows = 160;

    auto w = CaptureWriter::Create(dir, meta);
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w.value().Finish().ok());

    const auto layout = MakeLayout();  // 48x80
    auto src = ReplaySource::Create(dir, layout);
    EXPECT_FALSE(src.ok());
    EXPECT_EQ(src.error(), Error::kGridMismatch);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CaptureBundle, MissingFrameIsADropNotATruncation) {
    // A corrupt or absent frame file is a gap in the recording, not the end of it. Truncating
    // would silently discard the rest of an otherwise good dataset.
    const auto layout = MakeLayout();
    const std::string dir = TempBundle("gap");
    constexpr int kW = 640;
    constexpr int kH = 900;
    const auto frames = RenderFrames(layout, 5, kW, kH);

    auto w = CaptureWriter::Create(dir, GoodMetadata(kW, kH));
    ASSERT_TRUE(w.ok());
    for (const auto& f : frames) ASSERT_TRUE(w.value().WriteFrame(f.view()).ok());
    ASSERT_TRUE(w.value().Finish().ok());

    std::error_code ec;
    fs::remove(fs::path(dir) / "frames" / "000002.gray", ec);

    auto src = ReplaySource::Create(dir, layout);
    ASSERT_TRUE(src.ok());
    int seen = 0;
    int all_nan = 0;
    while (auto f = src.value().Next()) {
        ++seen;
        if (std::all_of(f->cell_samples.begin(), f->cell_samples.end(),
                        [](double v) { return std::isnan(v); })) {
            ++all_nan;
        }
    }
    EXPECT_EQ(seen, 5) << "a missing frame truncated the replay";
    EXPECT_EQ(all_nan, 1) << "the missing frame should be one fully-erased frame";

    fs::remove_all(dir, ec);
}

TEST(ReadGrayFrame, RejectsWrongSizedFiles) {
    const std::string dir = TempBundle("sizes");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (fs::path(dir) / "f.gray").string();

    {
        std::ofstream f(path, std::ios::binary);
        const std::vector<char> bytes(100, 0);
        f.write(bytes.data(), 100);
    }

    std::vector<std::uint8_t> buf;
    // Truncated and over-long files must both be refused: accepting either shifts every
    // subsequent pixel and produces a plausible-looking wrong image.
    EXPECT_EQ(ReadGrayFrame(path, 20, 6, &buf).error(), Error::kLengthMismatch);   // wants 120
    EXPECT_EQ(ReadGrayFrame(path, 10, 8, &buf).error(), Error::kLengthMismatch);   // wants 80
    EXPECT_TRUE(ReadGrayFrame(path, 10, 10, &buf).ok());                           // wants 100
    EXPECT_EQ(buf.size(), 100u);

    EXPECT_FALSE(ReadGrayFrame(dir + "/nope.gray", 10, 10, &buf).ok());
    fs::remove_all(dir, ec);
}
