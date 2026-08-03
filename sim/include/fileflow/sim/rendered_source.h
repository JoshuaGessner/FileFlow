// A CaptureSource that goes all the way through the IMAGE path (component C16 + C06/C08/C09).
//
// SimulatedSource hands cell samples straight to the demodulator, which is right for coding
// and modulation experiments -- it isolates those variables. RenderedSource instead renders a
// real image and makes the decoder EARN its homography: detect the screen, rectify, sample,
// normalise. That means simulator results include geometric error, detection failures and
// photometric estimation error, which are real terms in the budget and are invisible to the
// cell-sample path.
//
// Both implement the same interface, so an experiment can switch between them and attribute
// the difference to the geometry stack. That comparison is the point of having both.
#pragma once

#include <fileflow/capture_source.h>
#include <fileflow/detect.h>
#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/photometric.h>
#include <fileflow/pipeline.h>
#include <fileflow/sampler.h>
#include <fileflow/sim/render.h>
#include <fileflow/tracker.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace fileflow::sim {

struct RenderedSourceConfig {
    OpticalRenderConfig render;
    SamplerConfig sampler;
    TrackerConfig tracker;
    PhotometricConfig photometric;

    // Force a full-image acquisition on every frame, disabling tracking.
    //
    // This is the A/B switch for ADR-0006: run a sequence both ways and the difference is the
    // tracker's contribution, measured end-to-end rather than argued.
    bool disable_tracking = false;

    // Per-frame jitter of the viewing geometry, in degrees and distance units. Models a
    // handheld receiver: the homography is never the same twice, so a decoder that silently
    // depends on a fixed geometry will fail here and not in the static case.
    double jitter_deg = 0.0;
    double jitter_distance = 0.0;
    std::uint64_t jitter_seed = 777;
};

class RenderedSource final : public CaptureSource {
  public:
    // Fails if the layout or configs are unusable -- e.g. a layout with no boundary ring,
    // which the detector requires.
    static Result<RenderedSource> Create(const FrameLayout& layout,
                                         std::vector<CellMatrix> frames,
                                         RenderedSourceConfig cfg);

    [[nodiscard]] GridGeometry geometry() const override { return layout_->geometry(); }
    [[nodiscard]] std::optional<CapturedFrame> Next() override;
    [[nodiscard]] std::uint64_t frames_emitted() const override {
        return pipeline_.diagnostics().frames_decoded;
    }
    [[nodiscard]] std::uint64_t frames_dropped() const override {
        return pipeline_.diagnostics().failures();
    }

    // --- Diagnostics unique to the image path. These are the whole reason it exists. ---

    // Frames where the detector refused. Counted as dropped, never as a bad decode: a
    // refusal is the detector working correctly.
    [[nodiscard]] std::uint64_t detection_failures() const noexcept {
        return pipeline_.diagnostics().geometry_failures;
    }

    // Worst cell-centre displacement between the ESTIMATED and TRUE homography, in cells,
    // over all frames so far. The honest geometric-accuracy metric -- a mean would hide the
    // one bad frame that matters.
    [[nodiscard]] double worst_geometric_error_cells() const noexcept { return worst_geo_; }

    [[nodiscard]] std::uint64_t photometric_failures() const noexcept {
        return pipeline_.diagnostics().photometric_failures;
    }

    // Total pixels the geometry stage examined across the run. With tracking enabled this is
    // the ADR-0006 number; with `disable_tracking` it is the baseline to compare against.
    [[nodiscard]] std::uint64_t total_pixels_examined() const noexcept {
        return pipeline_.diagnostics().total_pixels_examined;
    }
    [[nodiscard]] std::uint64_t full_acquisitions() const noexcept {
        return pipeline_.tracker().full_acquisitions();
    }
    [[nodiscard]] const FramePipeline& pipeline() const noexcept { return pipeline_; }

  private:
    RenderedSource(const FrameLayout& layout, std::vector<CellMatrix> frames,
                   RenderedSourceConfig cfg, FramePipeline pipeline);

    const FrameLayout* layout_;
    std::vector<CellMatrix> frames_;
    RenderedSourceConfig cfg_;
    FramePipeline pipeline_;
    SplitMix64 rng_;

    std::size_t next_ = 0;
    std::uint64_t index_ = 0;
    double worst_geo_ = 0.0;
};

}  // namespace fileflow::sim
