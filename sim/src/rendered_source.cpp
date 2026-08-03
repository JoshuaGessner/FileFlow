#include <fileflow/sim/rendered_source.h>

#include <cmath>

namespace fileflow::sim {

RenderedSource::RenderedSource(const FrameLayout& layout, std::vector<CellMatrix> frames,
                               RenderedSourceConfig cfg, FramePipeline pipeline)
    : layout_(&layout),
      frames_(std::move(frames)),
      cfg_(cfg),
      pipeline_(std::move(pipeline)),
      rng_(cfg.jitter_seed) {}

Result<RenderedSource> RenderedSource::Create(const FrameLayout& layout,
                                              std::vector<CellMatrix> frames,
                                              RenderedSourceConfig cfg) {
    PipelineConfig pc;
    pc.tracker = cfg.tracker;
    pc.sampler = cfg.sampler;
    pc.photometric = cfg.photometric;
    pc.disable_tracking = cfg.disable_tracking;

    FF_ASSIGN_OR_RETURN(auto pipeline, FramePipeline::Create(layout, pc));
    return RenderedSource(layout, std::move(frames), cfg, std::move(pipeline));
}

std::optional<CapturedFrame> RenderedSource::Next() {
    if (next_ >= frames_.size()) return std::nullopt;

    const CellMatrix& tx = frames_[next_++];
    const GridGeometry g = layout_->geometry();

    // --- Per-frame viewing geometry, with optional handheld jitter ---
    OpticalRenderConfig rc = cfg_.render;
    if (cfg_.jitter_deg > 0.0 || cfg_.jitter_distance > 0.0) {
        const auto jitter = [&](double amp) { return (rng_.NextDouble() * 2.0 - 1.0) * amp; };
        rc.view.yaw_deg += jitter(cfg_.jitter_deg);
        rc.view.pitch_deg += jitter(cfg_.jitter_deg);
        rc.view.roll_deg += jitter(cfg_.jitter_deg);
        rc.view.distance += jitter(cfg_.jitter_distance);
    }
    rc.seed = cfg_.render.seed + index_;  // decorrelate noise between frames

    Homography truth;
    const Image8 img = RenderView(tx, g, rc, &truth);

    // The SHARED production pipeline -- the same object the replay harness and, later, the
    // live receiver drive. Reimplementing these stages here would let simulated and replayed
    // results diverge, and the moment they diverge a recorded dataset stops proving anything
    // about live behaviour (ADR-0010).
    const std::uint64_t idx = index_++;
    CapturedFrame out =
        pipeline_.Process(img.view(), idx, static_cast<std::int64_t>(idx) * 16'666'667);

    if (!pipeline_.last_ok()) return out;  // the pipeline's diagnostics recorded why

    // Geometric accuracy against ground truth, in CELLS. Only the simulator can measure this
    // -- a real capture has no truth to compare against -- which is exactly why the simulator
    // stays useful after the harness exists rather than being replaced by it.
    const Homography& est = pipeline_.last_homography();
    for (std::uint32_t r = 0; r < g.rows; r += 8) {
        for (std::uint32_t c = 0; c < g.cols; c += 8) {
            const Point2 gp{static_cast<double>(c) + 0.5, static_cast<double>(r) + 0.5};
            const Point2 a = truth.Apply(gp);
            const Point2 b = est.Apply(gp);
            if (!std::isfinite(a.x) || !std::isfinite(b.x)) continue;
            const Point2 a1 = truth.Apply({gp.x + 1.0, gp.y});
            const double px_per_cell = std::hypot(a1.x - a.x, a1.y - a.y);
            if (px_per_cell < 1e-6) continue;
            worst_geo_ = std::max(worst_geo_, std::hypot(a.x - b.x, a.y - b.y) / px_per_cell);
        }
    }

    out.ground_truth = tx;
    return out;
}

}  // namespace fileflow::sim
