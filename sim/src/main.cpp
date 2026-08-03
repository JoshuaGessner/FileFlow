// ffsim — end-to-end simulation driver.
//
// Runs a complete transfer through the REAL decode chain under a configured channel and
// reports the metrics the experiment registry asks for. Every run is reproducible from
// its flags plus the commit; the seed is always echoed.
//
// ⚠ Outputs are [HYP] until the simulator is calibrated against real captures in Phase 2
// (SIM-03 / RISK-024). Nothing printed here is a measured result.
#include <fileflow/frame.h>
#include <fileflow/harness/capture.h>
#include <fileflow/intra_fec.h>
#include <fileflow/link.h>
#include <fileflow/modulation.h>
#include <fileflow/photometric.h>
#include <fileflow/sampler.h>
#include <fileflow/sim/channel.h>
#include <fileflow/sim/render.h>
#include <fileflow/tracker.h>
#include <fileflow/transfer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace fileflow;

struct Options {
    std::uint32_t cols = 120;
    std::uint32_t rows = 200;
    std::size_t payload_bytes = 64 * 1024;
    std::uint32_t symbol_size = 0;  // 0 => size it to fill the frame
    std::uint32_t block_symbols = 64;
    std::uint64_t max_frames = 200000;
    double fd = 60.0;  // assumed distinct display states per second
    sim::ChannelConfig channel;

    // --- Image path (component C06/C08/C09) ---
    // Off by default. When off, cell samples are synthesised directly, which ISOLATES coding
    // and modulation. When on, the decoder must earn its homography from rendered pixels, so
    // geometric error, detection failures and photometric estimation error enter the budget.
    // The two modes answer different questions; neither replaces the other.
    bool image_path = false;
    bool no_tracking = false;  // force full acquisition every frame (ADR-0006 A/B baseline)
    sim::OpticalRenderConfig render;

    // Write the rendered frames out as a capture bundle. Lets the replay harness be exercised
    // end to end before any real capture exists, and gives the Android recorder a reference
    // bundle to produce byte-compatible output against.
    std::string record_dir;

    // Intra-frame FEC (ADR-0009's first half). nsym=0 disables it, which is the A/B baseline
    // for measuring what the layer actually buys.
    std::size_t fec_nsym = 32;

    // Erase cells whose |llr| falls below this. 0 = off (hard decisions only).
    int erase_below = 0;

    // Run the adaptive link controller (C14) alongside the transfer and report which nsym the
    // receiver's own telemetry would have chosen. REPORTING ONLY -- see the note it prints.
    bool adapt = false;
};

double Arg(const char* v) { return std::strtod(v, nullptr); }

void Usage() {
    std::puts(
        "ffsim — FileFlow offline channel simulator\n"
        "\n"
        "  --cols N --rows N            grid geometry (default 120x200)\n"
        "  --payload N                  payload bytes (default 65536)\n"
        "  --symbol-size N              fountain symbol size (default: fill the frame)\n"
        "  --fd RATE                    assumed display states/sec for goodput (default 60)\n"
        "  --block-symbols N            source symbols per block (default 64)\n"
        "  --seed N                     RNG seed (default 20260802)\n"
        "  --noise SIGMA                Gaussian read noise (default 2.0)\n"
        "  --shot SCALE                 signal-dependent shot noise (default 0)\n"
        "  --crosstalk F                spatial crosstalk 0..1 (default 0)\n"
        "  --gamma G                    camera response gamma (default 1.0)\n"
        "  --vignetting F               0..1 (default 0)\n"
        "  --exposure G                 exposure gain (default 1.0)\n"
        "  --glare S --glare-radius R   local saturation\n"
        "  --occlusion F                occluded grid fraction 0..1\n"
        "  --drop F                     frame drop rate 0..1\n"
        "  --duplicate F                duplicate frame rate 0..1\n"
        "  --mixed F                    rolling-shutter mixed frame rate 0..1\n"
        "  --max-frames N               safety bound on the run\n"
        "\n"
        "IMAGE PATH (renders real pixels; decoder must earn its homography)\n"
        "  --image-path                 enable render -> detect/track -> sample -> normalise\n"
        "  --no-tracking                full acquisition every frame (ADR-0006 A/B baseline)\n"
        "  --image-size W H             capture resolution (default 900x1500)\n"
        "  --distance D                 1.0 fills ~80%% of frame height (default 1.0)\n"
        "  --yaw D --pitch D --roll D   viewing angles in degrees\n"
        "  --falloff F                  corner luminance vs centre, 1.0 = flat\n"
        "  --img-glare L                directional black-level lift (finding F7)\n"
        "  --blur R                     defocus box-blur radius in pixels\n"
        "  --img-noise A                additive pixel noise amplitude\n"
        "  --record DIR                 write frames as a replayable capture bundle\n"
        "\n"
        "INTRA-FRAME FEC (ADR-0009)\n"
        "  --fec-nsym N                 RS parity bytes per 255-byte codeword (0 = off,\n"
        "                               default 32). Corrects N erasures or N/2 errors.\n"
        "  --erase-below L              erase cells with |llr| < L (0 = off). Trades a\n"
        "                               cheap certain erasure for a costly probable error.\n"
        "\n"
        "ADAPTIVE LINK CONTROLLER (C14)\n"
        "  --adapt                      score every nsym rung from this run's receiver\n"
        "                               telemetry and report the one that maximises expected\n"
        "                               payload. REPORTING ONLY: nsym fixes the fountain\n"
        "                               symbol size, which the manifest fixes for the whole\n"
        "                               session, so it cannot be changed mid-transfer.\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options o;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };

        if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (a == "--cols") o.cols = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--rows") o.rows = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--payload") o.payload_bytes = static_cast<std::size_t>(Arg(next()));
        else if (a == "--symbol-size") o.symbol_size = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--block-symbols") o.block_symbols = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--seed") o.channel.seed = static_cast<std::uint64_t>(Arg(next()));
        else if (a == "--noise") o.channel.read_noise_sigma = Arg(next());
        else if (a == "--shot") o.channel.shot_noise_scale = Arg(next());
        else if (a == "--crosstalk") o.channel.crosstalk = Arg(next());
        else if (a == "--gamma") o.channel.gamma = Arg(next());
        else if (a == "--vignetting") o.channel.vignetting = Arg(next());
        else if (a == "--exposure") o.channel.exposure_gain = Arg(next());
        else if (a == "--glare") o.channel.glare_strength = Arg(next());
        else if (a == "--glare-radius") o.channel.glare_radius_cells = Arg(next());
        else if (a == "--occlusion") o.channel.occlusion_fraction = Arg(next());
        else if (a == "--drop") o.channel.frame_drop_rate = Arg(next());
        else if (a == "--duplicate") o.channel.duplicate_rate = Arg(next());
        else if (a == "--mixed") o.channel.mixed_rate = Arg(next());
        else if (a == "--max-frames") o.max_frames = static_cast<std::uint64_t>(Arg(next()));
        else if (a == "--fd") o.fd = Arg(next());
        else if (a == "--image-path") o.image_path = true;
        else if (a == "--no-tracking") o.no_tracking = true;
        else if (a == "--image-size") {
            o.render.view.image_width = static_cast<int>(Arg(next()));
            o.render.view.image_height = static_cast<int>(Arg(next()));
        }
        else if (a == "--distance") o.render.view.distance = Arg(next());
        else if (a == "--yaw") o.render.view.yaw_deg = Arg(next());
        else if (a == "--pitch") o.render.view.pitch_deg = Arg(next());
        else if (a == "--roll") o.render.view.roll_deg = Arg(next());
        else if (a == "--falloff") o.render.corner_falloff = Arg(next());
        else if (a == "--img-glare") o.render.glare_lift = Arg(next());
        else if (a == "--blur") o.render.blur_radius = static_cast<int>(Arg(next()));
        else if (a == "--img-noise") o.render.noise_amplitude = Arg(next());
        else if (a == "--record") o.record_dir = next();
        else if (a == "--fec-nsym") o.fec_nsym = static_cast<std::size_t>(Arg(next()));
        else if (a == "--erase-below") o.erase_below = static_cast<int>(Arg(next()));
        else if (a == "--adapt") o.adapt = true;
        else { std::fprintf(stderr, "unknown flag: %s\n", a.c_str()); Usage(); return 2; }
    }

    if (auto s = o.channel.Validate(); !s.ok()) {
        std::fprintf(stderr, "bad channel config: %s\n", ErrorName(s.error()).data());
        return 2;
    }

    // Deterministic payload from a fixed seed: reproducible without storing the file.
    std::vector<std::uint8_t> payload(o.payload_bytes);
    SplitMix64 prng(0xF11EF10ULL ^ o.channel.seed);
    for (auto& b : payload) b = static_cast<std::uint8_t>(prng.Next() & 0xFF);

    GridGeometry g{o.cols, o.rows};
    auto layout_r = FrameLayout::Create(g, LayoutConfig{});
    if (!layout_r.ok()) {
        std::fprintf(stderr, "layout: %s\n", ErrorName(layout_r.error()).data());
        return 1;
    }
    const FrameLayout layout = std::move(layout_r).value();
    const M0Modulator mod(layout);
    const HeaderCodec hdr_codec;

    // FrameLayout::Create validates geometry but cannot know how large the CODED header is --
    // that belongs to HeaderCodec, a layer it does not depend on. So a grid too small for its
    // own header builds a valid-looking layout and then fails at Render with a generic
    // out-of-range error. Check it here, where both facts are in scope, and say so plainly.
    if (mod.header_capacity_bytes() < hdr_codec.coded_size()) {
        std::fprintf(stderr,
                     "grid %ux%u is too small for the coded header: band holds %zu bytes, "
                     "header needs %zu.\nReduce --marker-size/--header-rows via the layout, "
                     "or use a larger grid.\n",
                     g.cols, g.rows, mod.header_capacity_bytes(), hdr_codec.coded_size());
        return 2;
    }

    // Size symbols to fill the frame. Leaving payload cells unused wastes channel capacity
    // outright and would make every reported rate meaningless.
    std::optional<IntraFec> fec;
    if (o.fec_nsym > 0) {
        IntraFecParams fp;
        fp.nsym = o.fec_nsym;
        auto f = IntraFec::Create(mod.payload_capacity_bytes(), fp);
        if (!f.ok()) {
            std::fprintf(stderr, "intra-frame FEC: %s\n", ErrorName(f.error()).data());
            return 2;
        }
        fec.emplace(std::move(f).value());
    }

    // The fountain symbol must fit the FEC layer's MESSAGE capacity, not the raw frame
    // capacity -- parity occupies the difference. Getting this wrong would silently truncate
    // every symbol.
    if (fec.has_value()) {
        // With FEC on, the symbol size is DETERMINED by the code's message capacity -- parity
        // occupies the rest of the frame. An explicit --symbol-size that disagrees would fail
        // later inside the encoder with a bare length_mismatch, so override it and say why.
        const auto required = static_cast<std::uint32_t>(fec->message_bytes());
        if (o.symbol_size != 0 && o.symbol_size != required) {
            std::fprintf(stderr,
                         "note: --symbol-size %u ignored; intra-frame FEC fixes it at %u "
                         "(frame capacity %zu minus parity)\n",
                         o.symbol_size, required, mod.payload_capacity_bytes());
        }
        o.symbol_size = required;
        if (o.symbol_size > FountainParams::kMaxSymbolSize) {
            std::fprintf(stderr, "grid too large: FEC message %u exceeds max symbol size %u\n",
                         o.symbol_size, FountainParams::kMaxSymbolSize);
            return 2;
        }
    } else if (o.symbol_size == 0) {
        o.symbol_size = static_cast<std::uint32_t>(mod.payload_capacity_bytes());
        if (o.symbol_size > FountainParams::kMaxSymbolSize) {
            o.symbol_size = FountainParams::kMaxSymbolSize;
        }
    }

    auto tx_r = FileTransmitter::Create(payload, "sim.bin", 0x51533101, o.symbol_size,
                                        o.block_symbols);
    if (!tx_r.ok()) {
        std::fprintf(stderr, "transmitter: %s\n", ErrorName(tx_r.error()).data());
        return 1;
    }
    FileTransmitter tx = std::move(tx_r).value();

    auto rx_r = FileReceiver::Create(tx.manifest());
    if (!rx_r.ok()) {
        std::fprintf(stderr, "receiver: %s\n", ErrorName(rx_r.error()).data());
        return 1;
    }
    FileReceiver rx = std::move(rx_r).value();

    // Adaptive link controller (C14). Built from the SAME IntraFec the decoder runs, because
    // every score it computes is arithmetic over that code's geometry.
    std::optional<ChannelEstimator> estimator;
    std::optional<LinkController> controller;
    if (o.adapt) {
        if (!fec.has_value()) {
            std::fputs("--adapt requires intra-frame FEC: with --fec-nsym 0 there is no code "
                       "rate to select and no erasure-load telemetry to select it from\n",
                       stderr);
            return 2;
        }
        auto e = ChannelEstimator::Create(fec->codewords(), fec->params().codeword_bytes);
        if (!e.ok()) {
            std::fprintf(stderr, "channel estimator: %s\n", ErrorName(e.error()).data());
            return 2;
        }
        estimator.emplace(std::move(e).value());

        LinkControllerConfig lcfg;
        // The running nsym must be a rung, or the controller has nowhere to step from. Insert
        // it rather than refusing: an operator sweeping --fec-nsym should not have to keep the
        // ladder in sync by hand.
        if (std::find(lcfg.ladder.begin(), lcfg.ladder.end(), o.fec_nsym) ==
            lcfg.ladder.end()) {
            lcfg.ladder.push_back(o.fec_nsym);
            std::sort(lcfg.ladder.begin(), lcfg.ladder.end());
        }
        auto c = LinkController::Create(fec->codewords(), fec->params().codeword_bytes,
                                        LinkProfile{.nsym = o.fec_nsym}, lcfg);
        if (!c.ok()) {
            std::fprintf(stderr,
                         "link controller: %s (a --fec-nsym inserted into the ladder can sit "
                         "too close to a neighbour for the promote margin to clear)\n",
                         ErrorName(c.error()).data());
            return 2;
        }
        controller.emplace(std::move(c).value());
    }

    sim::Channel channel(o.channel);

    // Image-path machinery. Constructed only when needed so the default cell-sample mode
    // stays exactly as fast and as isolated as it was.
    std::optional<ScreenTracker> tracker;
    std::optional<CellSampler> sampler;
    if (o.image_path) {
        if (o.render.view.image_width <= 0) o.render.view.image_width = 900;
        if (o.render.view.image_height <= 0) o.render.view.image_height = 1500;

        auto tr = ScreenTracker::Create(layout, TrackerConfig{});
        if (!tr.ok()) {
            std::fprintf(stderr, "tracker: %s\n", ErrorName(tr.error()).data());
            return 1;
        }
        auto sm = CellSampler::Create(g, SamplerConfig{.interior_margin = 0.3,
                                                       .samples_per_axis = 3});
        if (!sm.ok()) {
            std::fprintf(stderr, "sampler: %s\n", ErrorName(sm.error()).data());
            return 1;
        }
        tracker.emplace(std::move(tr).value());
        sampler.emplace(std::move(sm).value());
    }

    std::optional<harness::CaptureWriter> recorder;
    if (!o.record_dir.empty()) {
        if (!o.image_path) {
            std::fputs("--record requires --image-path: there are no frames to record "
                       "without it\n", stderr);
            return 2;
        }
        harness::CaptureMetadata meta;
        meta.sender_model = "ffsim (synthetic)";
        meta.receiver_model = "ffsim (synthetic)";
        meta.app_commit = "simulated";
        meta.notes = "SYNTHETIC capture from the channel simulator -- NOT a real measurement";
        meta.grid_cols = g.cols;
        meta.grid_rows = g.rows;
        meta.modulation_profile = "M0";
        meta.width = static_cast<std::uint32_t>(o.render.view.image_width);
        meta.height = static_cast<std::uint32_t>(o.render.view.image_height);
        meta.fps = o.fd;
        meta.angle_deg = o.render.view.yaw_deg;
        meta.motion_condition = "synthetic";
        meta.source_payload_bytes = o.payload_bytes;
        meta.source_payload_sha256 = ToHex(Sha256::Of(payload));
        auto w = harness::CaptureWriter::Create(o.record_dir, meta);
        if (!w.ok()) {
            std::fprintf(stderr, "cannot open bundle %s: %s\n", o.record_dir.c_str(),
                         ErrorName(w.error()).data());
            return 1;
        }
        recorder.emplace(std::move(w).value());
    }

    const auto t0 = std::chrono::steady_clock::now();

    std::uint64_t presented = 0, captured = 0, header_ok = 0, header_fail = 0;
    std::uint64_t erasure_cells = 0, payload_cells_total = 0;
    std::uint64_t detect_fail = 0, photo_fail = 0, geo_px = 0;
    std::uint64_t fec_corrected = 0, fec_uncorrectable = 0;
    std::size_t fec_worst_load = 0;
    double worst_geo_cells = 0.0;
    double render_seconds = 0.0;

    while (!rx.complete() && presented < o.max_frames) {
        auto fp = tx.NextFrame();
        auto hdr_coded = hdr_codec.Encode(fp.header);
        if (!hdr_coded.ok()) { std::fputs("header encode failed\n", stderr); return 1; }

        std::vector<std::uint8_t> payload_coded;
        if (fec.has_value()) {
            auto e = fec->Encode(fp.data);
            if (!e.ok()) {
                std::fprintf(stderr, "fec encode: %s\n", ErrorName(e.error()).data());
                return 1;
            }
            payload_coded = std::move(e).value();
        } else {
            payload_coded.assign(fp.data.begin(), fp.data.end());
        }

        CellMatrix frame(g.cols, g.rows);
        if (auto s = mod.Render(hdr_coded.value(), payload_coded, &frame); !s.ok()) {
            std::fprintf(stderr, "render: %s\n", ErrorName(s.error()).data());
            return 1;
        }
        ++presented;

        // Frame drop: presented but never captured. A Pc term.
        if (o.channel.frame_drop_rate > 0.0 &&
            channel.rng().NextDouble() < o.channel.frame_drop_rate) {
            // A drop is a pre-FEC loss: parity cannot recover a frame the camera never saw.
            // The receiver observes it as a sequence-number gap, so this is telemetry it really
            // does have, not simulator omniscience.
            if (estimator.has_value()) estimator->ObservePreFecLoss();
            continue;
        }
        ++captured;

        std::vector<double> samples;
        if (o.image_path) {
            // Render real pixels and make the decoder find the screen for itself.
            Homography truth;
            sim::OpticalRenderConfig rc = o.render;
            rc.seed = o.channel.seed + presented;  // decorrelate noise between frames

            // Rendering is TRANSMITTER-SIDE simulation, not receiver work, and it dominates
            // wall time in this mode. Timing it separately keeps "decoder throughput" honest
            // -- folding it in would understate the decoder by several times and produce
            // exactly the sort of conflated metric ADR-0012 exists to prevent.
            const auto r0 = std::chrono::steady_clock::now();
            const Image8 img = sim::RenderView(frame, g, rc, &truth);
            render_seconds += std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - r0)
                                  .count();

            if (recorder.has_value()) {
                if (auto s = recorder->WriteFrame(img.view()); !s.ok()) {
                    std::fprintf(stderr, "record: %s\n", ErrorName(s.error()).data());
                    return 1;
                }
            }

            if (o.no_tracking) tracker->Reset();
            const TrackResult tr = tracker->Track(img.view());
            geo_px += tracker->last_pixels_examined();
            if (!tr.ok) {
                ++detect_fail;
                if (estimator.has_value()) estimator->ObservePreFecLoss();
                continue;
            }

            // Geometric error against ground truth, in CELLS -- only the simulator can
            // measure this, and it is the term that gates everything downstream.
            for (std::uint32_t r = 0; r < g.rows; r += 8) {
                for (std::uint32_t c = 0; c < g.cols; c += 8) {
                    const Point2 gp{static_cast<double>(c) + 0.5, static_cast<double>(r) + 0.5};
                    const Point2 a = truth.Apply(gp);
                    const Point2 b = tr.grid_to_image.Apply(gp);
                    if (!std::isfinite(a.x) || !std::isfinite(b.x)) continue;
                    const Point2 a1 = truth.Apply({gp.x + 1.0, gp.y});
                    const double px_per_cell = std::hypot(a1.x - a.x, a1.y - a.y);
                    if (px_per_cell < 1e-6) continue;
                    const double err = std::hypot(a.x - b.x, a.y - b.y) / px_per_cell;
                    if (err > worst_geo_cells) worst_geo_cells = err;
                }
            }

            const auto raw = sampler->Sample(img.view(), tr.grid_to_image);
            auto field = PhotometricField::Estimate(layout, raw, PhotometricConfig{});
            if (!field.ok()) {
                ++photo_fail;
                if (estimator.has_value()) estimator->ObservePreFecLoss();
                continue;
            }
            samples = field.value().Normalise(raw);
        } else {
            samples = channel.Apply(frame);
        }

        const PhotometricRef ref = mod.EstimateReference(samples);

        auto hbytes = mod.DemodulateHeader(samples, ref, hdr_codec.coded_size());
        if (!hbytes.ok()) {
            ++header_fail;
            if (estimator.has_value()) estimator->ObservePreFecLoss();
            continue;
        }

        std::vector<std::uint8_t> hbuf = hbytes.value();
        auto hdr = hdr_codec.Decode(hbuf);
        if (!hdr.ok()) {
            ++header_fail;
            if (estimator.has_value()) estimator->ObservePreFecLoss();
            continue;
        }
        ++header_ok;

        SoftSymbolBuffer soft;
        mod.DemodulatePayload(samples, ref, &soft);
        payload_cells_total += soft.size();

        const std::size_t coded_len =
            fec.has_value() ? fec->coded_bytes() : hdr.value().payload_bytes;
        HardDecision hd = HardDecide(soft, coded_len, static_cast<Llr>(o.erase_below));
        erasure_cells += hd.erasures;

        if (fec.has_value()) {
            // Erasure POSITIONS, not just the count: RS corrects nsym erasures but only
            // nsym/2 errors, so supplying them doubles the correction budget for free.
            IntraFec::Stats fst;
            auto dec = fec->Decode(hd.bytes, hd.erased_bytes, &fst);
            fec_worst_load = std::max(fec_worst_load, fst.worst_erasures_in_codeword);

            // Observe the load whether or not the frame decoded. The failures are the
            // informative half: a frame that exceeded its budget is the only evidence that a
            // stronger code was needed, and dropping it here would bias the estimate toward
            // exactly the rung already in use.
            if (estimator.has_value()) {
                estimator->ObserveFrame(fst.worst_budget_used, fst.budget_censored);
                controller->Select(*estimator, presented);
            }

            if (!dec.ok()) {
                // An uncorrectable frame is a normal event on this channel. Drop it and let
                // the fountain layer recover -- never ingest partially-corrected data.
                ++fec_uncorrectable;
                continue;
            }
            ++fec_corrected;
            rx.Ingest(hdr.value(), dec.value());
        } else {
            rx.Ingest(hdr.value(), hd.bytes);
        }
    }

    if (recorder.has_value()) {
        if (auto s = recorder->Finish(); !s.ok()) {
            std::fprintf(stderr, "record finish: %s\n", ErrorName(s.error()).data());
            return 1;
        }
        std::printf("recorded %u frames to %s\n", recorder->frames_written(),
                    o.record_dir.c_str());
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    auto out = rx.Finish();
    const bool verified = out.ok();

    // Metric names follow docs/vision/TERMINOLOGY.md exactly. An unlabelled rate is a
    // documentation defect (ADR-0012).
    std::printf("--- ffsim (simulated; [HYP] until Phase 2 calibration) ---\n");
    std::printf("seed                     %llu\n", static_cast<unsigned long long>(o.channel.seed));
    std::printf("grid                     %ux%u  (%u cells)\n", g.cols, g.rows, g.cells());
    std::printf("payload cells / frame    %zu\n", layout.payload_cells().size());
    std::printf("frame overhead O         %.4f\n", layout.overhead_fraction());
    std::printf("payload bytes            %zu\n", o.payload_bytes);
    std::printf("display states presented %llu\n", static_cast<unsigned long long>(presented));
    std::printf("frames captured          %llu\n", static_cast<unsigned long long>(captured));
    std::printf("header success H         %.4f\n",
                captured ? static_cast<double>(header_ok) / static_cast<double>(captured) : 0.0);
    std::printf("header failures          %llu\n", static_cast<unsigned long long>(header_fail));
    std::printf("cell erasure rate        %.4f\n",
                payload_cells_total
                    ? static_cast<double>(erasure_cells) / static_cast<double>(payload_cells_total)
                    : 0.0);
    if (o.image_path) {
        std::printf("\n--- geometry (image path) ---\n");
        std::printf("capture resolution       %dx%d\n", o.render.view.image_width,
                    o.render.view.image_height);
        std::printf("view                     distance %.2f, yaw %.1f, pitch %.1f, roll %.1f\n",
                    o.render.view.distance, o.render.view.yaw_deg, o.render.view.pitch_deg,
                    o.render.view.roll_deg);
        std::printf("tracking                 %s\n",
                    o.no_tracking ? "DISABLED (full acquisition per frame)" : "enabled");
        std::printf("full acquisitions        %llu\n",
                    static_cast<unsigned long long>(tracker->full_acquisitions()));
        std::printf("tracking losses          %llu\n",
                    static_cast<unsigned long long>(tracker->losses()));
        std::printf("detection failures       %llu\n",
                    static_cast<unsigned long long>(detect_fail));
        std::printf("photometric failures     %llu\n",
                    static_cast<unsigned long long>(photo_fail));
        // Worst, not mean: a mean hides the one bad frame that ruins an edge.
        std::printf("worst geometric error    %.4f cells\n", worst_geo_cells);
        const double px_per_frame = captured ? static_cast<double>(geo_px) /
                                                   static_cast<double>(captured)
                                             : 0.0;
        const double full_frame_px = static_cast<double>(o.render.view.image_width) *
                                     static_cast<double>(o.render.view.image_height);
        std::printf("geometry pixels/frame    %.0f  (%.3f of a full-image scan)\n",
                    px_per_frame, full_frame_px > 0 ? px_per_frame / full_frame_px : 0.0);
        std::printf("  compare --no-tracking to get the ADR-0006 A/B\n");
        std::printf("\n");
    }

    if (fec.has_value()) {
        std::printf("\n--- intra-frame FEC (ADR-0009) ---\n");
        std::printf("RS parity per codeword   %zu bytes (corrects %zu erasures / %zu errors)\n",
                    fec->params().nsym, fec->params().nsym, fec->params().nsym / 2);
        std::printf("codewords per frame      %zu (interleaved)\n", fec->codewords());
        std::printf("FEC code rate Rfec       %.4f\n", fec->params().code_rate());
        std::printf("frames FEC-recovered     %llu\n",
                    static_cast<unsigned long long>(fec_corrected));
        std::printf("frames uncorrectable     %llu\n",
                    static_cast<unsigned long long>(fec_uncorrectable));
        // HEADROOM, not just success. A run that peaked at 31 of 32 decoded fine and was one
        // bad cell from failing -- the adaptive controller needs to see that difference.
        std::printf("worst codeword load      %zu / %zu erasures (%.0f%% of budget)\n",
                    fec_worst_load, fec->params().nsym,
                    100.0 * static_cast<double>(fec_worst_load) /
                        static_cast<double>(fec->params().nsym));
        std::printf("\n");
    }

    if (estimator.has_value()) {
        const ChannelEstimator& est = *estimator;
        std::printf("\n--- adaptive link controller (C14; ADP-01 estimate, ADP-02 policy) ---\n");
        std::printf("frames into estimator    %llu  (%llu pre-FEC losses)\n",
                    static_cast<unsigned long long>(est.frames_observed()),
                    static_cast<unsigned long long>(est.pre_fec_losses()));
        std::printf("P(frame reaches FEC)     %.4f\n", est.pre_fec_success());
        std::printf("codeword budget used     mean %.1f, median %zu, p95 %zu, worst %zu bytes"
                    "  (2*errors + erasures)\n",
                    est.mean_budget(), est.BudgetQuantile(0.5), est.BudgetQuantile(0.95),
                    est.worst_budget_seen());
        if (est.censored_observations() > 0) {
            // Not a footnote: these frames were bounded, not measured, and the bound is
            // optimistic for every rung above the running one.
            std::printf("censored observations    %llu of %llu  <-- frames that did not decode, "
                        "so their budget is a FLOOR.\n"
                        "                         Predictions for rungs ABOVE %zu are "
                        "optimistic by up to this share.\n",
                        static_cast<unsigned long long>(est.censored_observations()),
                        static_cast<unsigned long long>(est.frames_observed()), o.fec_nsym);
        }

        // The full response surface, not just the winner: a sharp peak and a broad plateau call
        // for different decisions, and only the table shows which one this is (EXP-013's rule,
        // applied here for the same reason).
        std::printf("\n  nsym    Rfec   msg bytes   P(decode)   expected bytes / display state\n");
        for (const RungScore& r : controller->ScoreLadder(est)) {
            std::printf("  %4zu  %.4f      %6.0f      %.4f   %10.1f%s\n", r.nsym, r.code_rate,
                        r.message_bytes_per_frame, r.predicted_frame_success, r.score,
                        r.nsym == o.fec_nsym ? "   <- running" : "");
        }

        const RungScore best = controller->BestRung(est);
        const double running = [&] {
            for (const RungScore& r : controller->ScoreLadder(est)) {
                if (r.nsym == o.fec_nsym) return r.score;
            }
            return 0.0;
        }();
        std::printf("\nrecommended nsym         %zu   (from receiver telemetry alone)\n",
                    best.nsym);
        std::printf("running nsym             %zu\n", o.fec_nsym);
        if (running > 0.0) {
            std::printf("predicted headroom       %+.1f%% expected bytes/display state vs "
                        "the running rung\n",
                        100.0 * (best.score / running - 1.0));
        }
        std::printf("hysteresis walk ended at %zu after %llu profile change(s)\n",
                    controller->current().nsym,
                    static_cast<unsigned long long>(controller->changes()));
        // The recommendation is the unconstrained argmax; the walk is what the policy would
        // actually have done in the frames available. On a short transfer those differ for a
        // mundane reason worth stating, or the two lines look contradictory.
        if (controller->changes() == 0 && best.nsym != o.fec_nsym) {
            const auto dwell = controller->config().promote_dwell_frames;
            if (presented < dwell + controller->config().min_observations) {
                std::printf("  the walk did not move because this transfer (%llu display "
                            "states) is shorter than\n  the promote dwell (%llu frames) plus "
                            "the observation minimum -- for a session this\n  short only the "
                            "session-START choice matters, which is F20's point exactly.\n",
                            static_cast<unsigned long long>(presented),
                            static_cast<unsigned long long>(dwell));
            }
        }

        // Without this note the number above reads as an achievable gain. It is not one yet.
        std::printf(
            "\n  NOTE: reported, NOT actuated. nsym sets the FEC message size, which sets the\n"
            "  fountain symbol size, which the manifest fixes for the whole session -- so this\n"
            "  is a session-START decision, not a mid-session control loop. And on a one-way\n"
            "  link the transmitter cannot hear it at all (OQ-013). See finding F20.\n");
        std::printf("\n");
    }

    std::printf("fountain overhead        %.4f\n", rx.overhead());
    std::printf("blocks complete          %u / %u\n", rx.blocks_complete(),
                tx.manifest().block_count);
    std::printf("VERIFIED                 %s\n", verified ? "yes (SHA-256 match)" : "NO");
    if (!verified) std::printf("failure                  %s\n", ErrorName(out.error()).data());

    // ---- Rates. ADR-0012: name the metric, always. These are DIFFERENT quantities and
    // conflating them is the single failure mode this project exists to avoid.
    const double payload_bits_per_frame =
        static_cast<double>(layout.payload_cells().size());  // M0 = 1 bit per payload cell
    const double raw_bit_rate = payload_bits_per_frame * o.fd;
    const double optical_seconds = static_cast<double>(presented) / o.fd;
    const double goodput = static_cast<double>(o.payload_bytes) / optical_seconds;

    std::printf("\n--- rates (see docs/vision/TERMINOLOGY.md) ---\n");
    std::printf("assumed display rate Fd  %.1f states/s   [ASSUMPTION, not measured]\n", o.fd);
    std::printf("raw optical bit rate     %.2f Mb/s\n", raw_bit_rate / 1e6);
    std::printf("optical transfer time    %.3f s  (%llu states / Fd)\n", optical_seconds,
                static_cast<unsigned long long>(presented));
    if (verified) {
        std::printf("PAYLOAD GOODPUT          %.1f KB/s   <-- the metric (ADR-0012)\n",
                    goodput / 1024.0);
    }
    // Reported separately and NEVER as goodput: this is how fast THIS DESKTOP chews through
    // frames, which says nothing about the optical channel. It matters only as a check that
    // the decoder could keep up in real time (RISK-006).
    // Simulator rendering is transmitter-side and must not be charged to the decoder.
    const double decode_secs = secs - render_seconds;
    const double decode_fps =
        decode_secs > 0.0 ? static_cast<double>(captured) / decode_secs : 0.0;
    std::printf("decoder throughput       %.0f frames/s on this host (NOT a channel rate)\n",
                decode_fps);
    std::printf("  real-time headroom     %.1fx vs Fd=%.0f\n", decode_fps / o.fd, o.fd);
    if (o.image_path) {
        std::printf("  (simulator rendering    %.1f s of %.1f s wall, excluded above)\n",
                    render_seconds, secs);
    }
    return verified ? 0 : 1;
}
