// The receiver's image-to-symbols pipeline (components C06 + C08 + C09 in one place).
//
// WHY THIS EXISTS AS ITS OWN TYPE: three different sources feed the decoder -- the Android
// camera, the channel simulator, and the replay harness -- and all three must run the SAME
// geometry and photometry. If each assembled its own tracker/sampler/field, they would drift,
// and the moment they drift a replayed capture stops proving anything about live behaviour.
// That substitutability is the entire premise of ADR-0010, so the shared stage is a shared
// object, not a shared convention.
//
// Everything here is deterministic: identical pixels in, identical samples out. The replay
// harness depends on that for regression testing, and the simulator depends on it for the
// determinism gate in CI.
#pragma once

#include <fileflow/capture_source.h>
#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/photometric.h>
#include <fileflow/result.h>
#include <fileflow/sampler.h>
#include <fileflow/tracker.h>

#include <cstdint>

namespace fileflow {

struct PipelineConfig {
    TrackerConfig tracker;
    SamplerConfig sampler{.interior_margin = 0.3, .samples_per_axis = 3};
    PhotometricConfig photometric;

    // Force full-image acquisition every frame, disabling the tracker's fast path.
    // The A/B switch for ADR-0006.
    bool disable_tracking = false;
};

// Per-stage failure accounting. Kept separate rather than lumped into one "failures" counter
// because the stages fail for entirely different reasons and fixing the wrong one is easy:
// geometry failures mean the screen was not found, photometric failures mean it was found but
// could not be read.
struct PipelineDiagnostics {
    std::uint64_t frames_in = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t geometry_failures = 0;
    std::uint64_t photometric_failures = 0;
    std::uint64_t total_pixels_examined = 0;

    [[nodiscard]] std::uint64_t failures() const noexcept {
        return geometry_failures + photometric_failures;
    }

    // Photometric telemetry, summed over frames that reached the photometry stage.
    //
    // Kept here rather than left to the caller because a frame that localises and then erases
    // almost every cell is the hardest failure to attribute, and the numbers that attribute it are
    // discarded the moment the field goes out of scope. F21 is the same lesson from the FEC layer:
    // telemetry that exists only on the success path is missing where it matters.
    std::uint64_t photometric_frames = 0;
    double sum_bright_pilots = 0.0;
    double sum_dark_pilots = 0.0;
    double sum_separation = 0.0;
    double sum_residual = 0.0;
    double sum_bright_nonuniformity = 0.0;

    [[nodiscard]] double mean_bright_pilots() const noexcept {
        return photometric_frames ? sum_bright_pilots / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_dark_pilots() const noexcept {
        return photometric_frames ? sum_dark_pilots / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_separation() const noexcept {
        return photometric_frames ? sum_separation / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_residual() const noexcept {
        return photometric_frames ? sum_residual / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_bright_nonuniformity() const noexcept {
        return photometric_frames
                   ? sum_bright_nonuniformity / static_cast<double>(photometric_frames)
                   : 0.0;
    }
};

class FramePipeline {
  public:
    static Result<FramePipeline> Create(const FrameLayout& layout, PipelineConfig cfg = {});

    // Turn one image into per-cell samples.
    //
    // A frame that fails any stage still comes back as a CapturedFrame -- with all-NaN samples
    // and no ground truth -- rather than as an error. Losing a frame is normal operation on
    // this channel, and the fountain layer is built for it; the caller needs the index gap
    // recorded, not an exception. The reason is in diagnostics().
    [[nodiscard]] CapturedFrame Process(const ImageView8& img, std::uint64_t index,
                                        std::int64_t timestamp_ns);

    // Geometry from the most recent Process() call. Only meaningful when the last frame
    // decoded; check last_ok() first.
    [[nodiscard]] const Homography& last_homography() const noexcept { return last_h_; }
    [[nodiscard]] bool last_ok() const noexcept { return last_ok_; }
    [[nodiscard]] double last_marker_score() const noexcept { return last_score_; }

    [[nodiscard]] const PipelineDiagnostics& diagnostics() const noexcept { return diag_; }
    [[nodiscard]] const ScreenTracker& tracker() const noexcept { return tracker_; }
    [[nodiscard]] const FrameLayout& layout() const noexcept { return *layout_; }

    // Drop the geometry lock, e.g. across a session boundary.
    void Reset() noexcept { tracker_.Reset(); }

  private:
    FramePipeline(const FrameLayout& layout, PipelineConfig cfg, CellSampler sampler,
                  ScreenTracker tracker);

    const FrameLayout* layout_;
    PipelineConfig cfg_;
    CellSampler sampler_;
    ScreenTracker tracker_;

    Homography last_h_;
    bool last_ok_ = false;
    double last_score_ = 0.0;
    PipelineDiagnostics diag_;
};

}  // namespace fileflow
