#include <fileflow/pipeline.h>

#include <cmath>

namespace fileflow {

FramePipeline::FramePipeline(const FrameLayout& layout, PipelineConfig cfg, CellSampler sampler,
                             ScreenTracker tracker)
    : layout_(&layout),
      cfg_(cfg),
      sampler_(std::move(sampler)),
      tracker_(std::move(tracker)) {}

Result<FramePipeline> FramePipeline::Create(const FrameLayout& layout, PipelineConfig cfg) {
    FF_ASSIGN_OR_RETURN(auto sampler, CellSampler::Create(layout.geometry(), cfg.sampler));
    FF_ASSIGN_OR_RETURN(auto tracker, ScreenTracker::Create(layout, cfg.tracker));
    return FramePipeline(layout, cfg, std::move(sampler), std::move(tracker));
}

CapturedFrame FramePipeline::Process(const ImageView8& img, std::uint64_t index,
                                     std::int64_t timestamp_ns) {
    const GridGeometry g = layout_->geometry();

    CapturedFrame out;
    out.index = index;
    out.timestamp_ns = timestamp_ns;
    out.phase = FramePhase::kUnknown;
    out.cell_samples.assign(static_cast<std::size_t>(g.cells()), std::nan(""));

    ++diag_.frames_in;
    last_ok_ = false;
    last_score_ = 0.0;

    // --- Geometry ---
    if (cfg_.disable_tracking) tracker_.Reset();
    const TrackResult tr = tracker_.Track(img);
    diag_.total_pixels_examined += tracker_.last_pixels_examined();

    if (!tr.ok) {
        ++diag_.geometry_failures;
        return out;
    }
    last_h_ = tr.grid_to_image;
    last_score_ = tr.marker_score;

    // --- Rectify and sample ---
    const auto raw = sampler_.Sample(img, tr.grid_to_image);

    // --- Photometry ---
    auto field = PhotometricField::Estimate(*layout_, raw, cfg_.photometric);
    if (!field.ok()) {
        // The screen was located but cannot be read -- a different failure from not finding
        // it, and worth separating: this one points at exposure or lighting, not tracking.
        ++diag_.photometric_failures;
        return out;
    }

    const PhotometricField& pf = field.value();
    ++diag_.photometric_frames;
    diag_.sum_bright_pilots += static_cast<double>(pf.bright_pilots_used());
    diag_.sum_dark_pilots += static_cast<double>(pf.dark_pilots_used());
    diag_.sum_separation += pf.mean_separation();
    diag_.sum_residual += pf.mean_residual();
    diag_.sum_bright_nonuniformity += pf.bright_nonuniformity();

    out.cell_samples = pf.Normalise(raw);
    out.phase = FramePhase::kClean;  // temporal classification is component C07, not here
    last_ok_ = true;
    ++diag_.frames_decoded;
    return out;
}

}  // namespace fileflow
