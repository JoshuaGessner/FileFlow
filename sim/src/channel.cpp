#include <fileflow/sim/channel.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fileflow::sim {

Status ChannelConfig::Validate() const noexcept {
    auto in_unit = [](double v) { return v >= 0.0 && v <= 1.0; };
    if (gamma <= 0.0 || gamma > 8.0) return Error::kDegenerateParameters;
    if (exposure_gain <= 0.0) return Error::kDegenerateParameters;
    if (read_noise_sigma < 0.0 || shot_noise_scale < 0.0) return Error::kDegenerateParameters;
    if (!in_unit(crosstalk) || !in_unit(vignetting)) return Error::kDegenerateParameters;
    if (!in_unit(occlusion_fraction)) return Error::kDegenerateParameters;
    if (!in_unit(frame_drop_rate) || !in_unit(duplicate_rate) || !in_unit(mixed_rate))
        return Error::kDegenerateParameters;
    if (!in_unit(mixed_band_fraction)) return Error::kDegenerateParameters;
    return Status::Ok();
}

double Channel::Gaussian() {
    // Box-Muller, caching the spare so each pair costs one transform.
    if (have_spare_gaussian_) {
        have_spare_gaussian_ = false;
        return spare_gaussian_;
    }
    double u1 = rng_.NextDouble();
    const double u2 = rng_.NextDouble();
    if (u1 < 1e-12) u1 = 1e-12;
    const double mag = std::sqrt(-2.0 * std::log(u1));
    spare_gaussian_ = mag * std::sin(2.0 * M_PI * u2);
    have_spare_gaussian_ = true;
    return mag * std::cos(2.0 * M_PI * u2);
}

std::vector<double> Channel::Apply(const CellMatrix& tx) {
    const std::uint32_t cols = tx.cols();
    const std::uint32_t rows = tx.rows();
    std::vector<double> out(static_cast<std::size_t>(cols) * rows, 0.0);

    // --- 1. Spatial crosstalk: energy from neighbouring cells leaking into each sample.
    // This is the direct analogue of inter-symbol interference, and it is what ultimately
    // bounds grid density (the "density cliff" EXP-001 must locate).
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const double centre = tx.at(c, r);
            double neigh = 0.0;
            int n = 0;
            if (c > 0)          { neigh += tx.at(c - 1, r); ++n; }
            if (c + 1 < cols)   { neigh += tx.at(c + 1, r); ++n; }
            if (r > 0)          { neigh += tx.at(c, r - 1); ++n; }
            if (r + 1 < rows)   { neigh += tx.at(c, r + 1); ++n; }
            const double avg = n > 0 ? neigh / n : centre;
            out[static_cast<std::size_t>(r) * cols + c] =
                (1.0 - cfg_.crosstalk) * centre + cfg_.crosstalk * avg;
        }
    }

    // --- 2. Photometric: vignetting, gamma, exposure, black level ---
    exposure_ *= (1.0 + cfg_.exposure_drift_per_frame);
    const double cx = (cols - 1) / 2.0;
    const double cy = (rows - 1) / 2.0;
    const double max_r2 = cx * cx + cy * cy;

    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            double v = out[i] / 255.0;

            if (cfg_.gamma != 1.0) v = std::pow(std::max(v, 0.0), cfg_.gamma);

            if (cfg_.vignetting > 0.0 && max_r2 > 0.0) {
                const double dx = c - cx;
                const double dy = r - cy;
                const double falloff = 1.0 - cfg_.vignetting * ((dx * dx + dy * dy) / max_r2);
                v *= std::max(falloff, 0.0);
            }

            v = v * cfg_.exposure_gain * exposure_ * 255.0 + cfg_.black_level;
            out[i] = v;
        }
    }

    // --- 3. Glare: a local saturated blob. Saturation is UNRECOVERABLE -- both levels
    // clip to the same value, so the region must be detected and erased, not "corrected".
    if (cfg_.glare_strength > 0.0 && cfg_.glare_radius_cells > 0.0) {
        const double gx = rng_.NextDouble() * cols;
        const double gy = rng_.NextDouble() * rows;
        const double rad2 = cfg_.glare_radius_cells * cfg_.glare_radius_cells;
        for (std::uint32_t r = 0; r < rows; ++r) {
            for (std::uint32_t c = 0; c < cols; ++c) {
                const double dx = c - gx;
                const double dy = r - gy;
                const double d2 = dx * dx + dy * dy;
                if (d2 < rad2) {
                    const double w = 1.0 - std::sqrt(d2 / rad2);
                    const std::size_t i = static_cast<std::size_t>(r) * cols + c;
                    out[i] += cfg_.glare_strength * 255.0 * w;
                }
            }
        }
    }

    // --- 4. Sensor noise: shot noise is signal-dependent, read noise is not ---
    for (double& v : out) {
        if (cfg_.shot_noise_scale > 0.0) {
            v += Gaussian() * cfg_.shot_noise_scale * std::sqrt(std::max(v, 0.0));
        }
        if (cfg_.read_noise_sigma > 0.0) {
            v += Gaussian() * cfg_.read_noise_sigma;
        }
        v = std::clamp(v, 0.0, 255.0);  // sensor clipping
    }

    // --- 5. Occlusion: NaN, so the demodulator produces ERASURES rather than guesses ---
    if (cfg_.occlusion_fraction > 0.0) {
        const auto n_occ =
            static_cast<std::size_t>(cfg_.occlusion_fraction * static_cast<double>(out.size()));
        // A contiguous band, because real occlusions (a finger, a case edge) are clustered,
        // not scattered. Clustered damage is what the interleaver must survive.
        const std::size_t start = static_cast<std::size_t>(rng_.NextDouble() *
                                                           static_cast<double>(out.size()));
        for (std::size_t k = 0; k < n_occ; ++k) {
            out[(start + k) % out.size()] = std::numeric_limits<double>::quiet_NaN();
        }
    }

    return out;
}

// ---------------------------------------------------------------- source

SimulatedSource::SimulatedSource(GridGeometry g, std::vector<CellMatrix> frames,
                                 ChannelConfig cfg)
    : g_(g),
      frames_(std::move(frames)),
      channel_(cfg),
      sched_rng_(cfg.seed ^ 0xA5A5A5A5ULL) {}

std::optional<CapturedFrame> SimulatedSource::Next() {
    const ChannelConfig& cfg = channel_.config();

    while (next_ < frames_.size()) {
        // Frame drop: the display state was presented but never captured. Counted, never
        // silently ignored -- drops are a Pc term and must reach telemetry.
        if (cfg.frame_drop_rate > 0.0 && sched_rng_.NextDouble() < cfg.frame_drop_rate) {
            ++next_;
            ++dropped_;
            ++index_;
            continue;
        }
        break;
    }
    if (next_ >= frames_.size()) return std::nullopt;

    CapturedFrame f;
    f.index = index_++;
    f.timestamp_ns = static_cast<std::int64_t>(f.index) * 16'666'667;  // ~60 fps
    f.phase = FramePhase::kClean;

    const CellMatrix& tx = frames_[next_];
    f.cell_samples = channel_.Apply(tx);

    // Mixed frame: a rolling-shutter mixture of this state and the next. The transition
    // band is erased rather than guessed -- recovering it is M4's job (Phase 7), and the
    // Pm term in the goodput model is exactly the fraction we eventually recover here.
    if (cfg.mixed_rate > 0.0 && next_ + 1 < frames_.size() &&
        sched_rng_.NextDouble() < cfg.mixed_rate) {
        f.phase = FramePhase::kMixed;
        const CellMatrix& nxt = frames_[next_ + 1];
        std::vector<double> other = channel_.Apply(nxt);

        const auto boundary =
            static_cast<std::uint32_t>(sched_rng_.NextDouble() * static_cast<double>(g_.rows));
        const auto band = static_cast<std::uint32_t>(
            std::max(1.0, cfg.mixed_band_fraction * static_cast<double>(g_.rows)));

        for (std::uint32_t r = 0; r < g_.rows; ++r) {
            for (std::uint32_t c = 0; c < g_.cols; ++c) {
                const std::size_t i = static_cast<std::size_t>(r) * g_.cols + c;
                if (r > boundary + band) {
                    f.cell_samples[i] = other[i];              // below the band: next state
                } else if (r >= boundary) {
                    f.cell_samples[i] = std::numeric_limits<double>::quiet_NaN();  // erased
                }
            }
        }
    }

    if (attach_gt_) f.ground_truth = tx;

    // Duplicate: the same display state captured twice. Cheap to detect and drop, and
    // doing so early is one of the least expensive wins available to the receiver.
    if (cfg.duplicate_rate > 0.0 && sched_rng_.NextDouble() < cfg.duplicate_rate) {
        // Leave next_ where it is so the same state is emitted again next call.
    } else {
        ++next_;
    }

    ++emitted_;
    return f;
}

}  // namespace fileflow::sim
