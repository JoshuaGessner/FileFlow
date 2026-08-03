// Offline channel simulator (component C16, ADR-0010).
//
// ⚠ THE SIMULATOR IS NOT A PREDICTOR OF FIELD PERFORMANCE until it is calibrated against
// recorded real captures in Phase 2 (SIM-03). Until then every output is tagged [HYP].
// This is RISK-024, the second-highest risk in the register: a simulator that models a
// KINDER channel than reality produces confident wrong conclusions, which is worse than
// having no simulator at all.
//
// Scope note: this first cut implements the impairments that matter for the coding and
// modulation experiments (EXP-010/011/012) -- noise, crosstalk, photometric drift,
// erasures, frame loss, duplicates and mixed frames. The geometric impairments
// (perspective, lens distortion, defocus, motion blur) slot in ahead of cell sampling once
// the CV path lands; see docs/testing/SIMULATOR-PLAN.md for the full 21-impairment list.
#pragma once

#include <fileflow/capture_source.h>
#include <fileflow/fountain.h>
#include <fileflow/grid.h>
#include <fileflow/modulation.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace fileflow::sim {

struct ChannelConfig {
    std::uint64_t seed = 20260802;

    // --- Photometric ---
    double gamma = 1.0;            // camera response non-linearity
    double exposure_gain = 1.0;    // multiplicative
    double exposure_drift_per_frame = 0.0;
    double black_level = 0.0;      // additive offset
    double vignetting = 0.0;       // 0 = none, 1 = corners fully dark

    // --- Noise ---
    double read_noise_sigma = 2.0;   // Gaussian, constant
    double shot_noise_scale = 0.0;   // signal-dependent: sigma = scale * sqrt(signal)

    // --- Spatial ---
    double crosstalk = 0.0;          // fraction of each cell's value bleeding from neighbours
    double glare_strength = 0.0;     // local saturation
    double glare_radius_cells = 0.0;
    double occlusion_fraction = 0.0; // fraction of the grid occluded (NaN samples)

    // --- Temporal ---
    double frame_drop_rate = 0.0;    // whole display states never captured
    double duplicate_rate = 0.0;     // a state captured twice
    double mixed_rate = 0.0;         // rolling-shutter mixture of two states
    double mixed_band_fraction = 0.1; // rows in the transition band, erased

    [[nodiscard]] Status Validate() const noexcept;
};

// Applies the impairment pipeline to a rendered CellMatrix, producing cell samples.
// Order approximates physical reality: spatial coupling, then photometric response, then
// sensor noise. Applying noise before blur models a different sensor than the reverse.
class Channel {
  public:
    explicit Channel(ChannelConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

    // Returns one sample per cell, flat row-major. NaN marks a cell with no usable sample.
    [[nodiscard]] std::vector<double> Apply(const CellMatrix& tx);

    [[nodiscard]] const ChannelConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] SplitMix64& rng() noexcept { return rng_; }

  private:
    [[nodiscard]] double Gaussian();

    ChannelConfig cfg_;
    SplitMix64 rng_;
    double exposure_ = 1.0;
    bool have_spare_gaussian_ = false;
    double spare_gaussian_ = 0.0;
};

// A CaptureSource that renders frames from a transmitter and pushes them through Channel.
// This is what lets EXP-010/011/012 run with EXACT ground truth and no hardware.
class SimulatedSource final : public CaptureSource {
  public:
    // `frames` is the rendered display-state sequence to transmit.
    SimulatedSource(GridGeometry g, std::vector<CellMatrix> frames, ChannelConfig cfg);

    [[nodiscard]] GridGeometry geometry() const override { return g_; }
    [[nodiscard]] std::optional<CapturedFrame> Next() override;
    [[nodiscard]] std::uint64_t frames_emitted() const override { return emitted_; }
    [[nodiscard]] std::uint64_t frames_dropped() const override { return dropped_; }

    // Ground truth is attached to every frame -- the thing a real camera can never provide.
    void set_attach_ground_truth(bool v) noexcept { attach_gt_ = v; }

  private:
    GridGeometry g_;
    std::vector<CellMatrix> frames_;
    Channel channel_;
    SplitMix64 sched_rng_;
    std::size_t next_ = 0;
    std::uint64_t emitted_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t index_ = 0;
    bool attach_gt_ = true;
};

}  // namespace fileflow::sim
