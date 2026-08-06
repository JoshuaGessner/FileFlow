#include <fileflow/photometric.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fileflow {
namespace {

struct PilotObs {
    double lc = 0.0;  // lattice coordinates (fractional)
    double lr = 0.0;
    double value = 0.0;
};

}  // namespace

Result<PhotometricField> PhotometricField::Estimate(const FrameLayout& layout,
                                                    std::span<const double> samples,
                                                    PhotometricConfig cfg) {
    const GridGeometry& g = layout.geometry();
    if (samples.size() != static_cast<std::size_t>(g.cells())) return Error::kLengthMismatch;
    if (cfg.search_radius < 1 || cfg.search_radius > 8) return Error::kValueOutOfRange;
    if (!(cfg.min_separation > 0.0) || !(cfg.idw_power > 0.0)) return Error::kValueOutOfRange;

    PhotometricField f;
    f.g_ = g;
    f.cfg_ = cfg;
    f.pitch_ = layout.config().pilot_pitch == 0 ? 16 : layout.config().pilot_pitch;
    f.lat_cols_ = (g.cols + f.pitch_ - 1) / f.pitch_;
    f.lat_rows_ = (g.rows + f.pitch_ - 1) / f.pitch_;
    if (f.lat_cols_ == 0 || f.lat_rows_ == 0) return Error::kDegenerateParameters;

    // --- Gather pilot observations, split by expected level ---
    std::vector<PilotObs> bright;
    std::vector<PilotObs> dark;
    double sum_bright = 0.0;
    double sum_dark = 0.0;

    for (const std::uint32_t idx : layout.pilot_cells()) {
        const std::uint32_t c = idx % g.cols;
        const std::uint32_t r = idx / g.cols;
        const double v = samples[idx];
        if (std::isnan(v)) continue;  // occluded pilot contributes nothing

        const PilotObs obs{static_cast<double>(c) / static_cast<double>(f.pitch_),
                           static_cast<double>(r) / static_cast<double>(f.pitch_), v};
        if (layout.PilotValue(c, r) == kLevelBright) {
            bright.push_back(obs);
            sum_bright += v;
        } else {
            dark.push_back(obs);
            sum_dark += v;
        }
    }

    // Without at least one of each level there is no scale and no offset. Refusing here is
    // correct: a fabricated reference would produce confident garbage across the frame.
    if (bright.empty() || dark.empty()) return Error::kDegenerateParameters;

    f.n_bright_ = bright.size();
    f.n_dark_ = dark.size();
    f.global_.bright = sum_bright / static_cast<double>(bright.size());
    f.global_.dark = sum_dark / static_cast<double>(dark.size());

    // --- Interpolate each level onto the lattice by inverse-distance weighting ---
    const auto nodes = static_cast<std::size_t>(f.lat_cols_) * f.lat_rows_;
    f.bright_.assign(nodes, f.global_.bright);
    f.dark_.assign(nodes, f.global_.dark);

    f.residual_.assign(nodes, 0.0);

    const double rad = static_cast<double>(cfg.search_radius);
    // Interpolates one level onto the lattice and accumulates the weighted spread of the
    // contributing pilots about that estimate. The spread is the model-fit residual: when
    // pilots that should agree do not, the region is damaged and its cells must be erased
    // rather than decoded against a confidently-wrong reference.
    const auto fill = [&](const std::vector<PilotObs>& obs, std::vector<double>& field,
                          double fallback) {
        for (std::uint32_t j = 0; j < f.lat_rows_; ++j) {
            for (std::uint32_t i = 0; i < f.lat_cols_; ++i) {
                double wsum = 0.0;
                double vsum = 0.0;
                bool exact = false;
                for (const auto& o : obs) {
                    const double dx = o.lc - static_cast<double>(i);
                    const double dy = o.lr - static_cast<double>(j);
                    // Square window keeps the cost bounded and local; the ring of fallback
                    // matters where markers/header punch holes in the lattice.
                    if (std::fabs(dx) > rad || std::fabs(dy) > rad) continue;
                    const double d2 = dx * dx + dy * dy;
                    // Exact hit: use it alone rather than dividing by ~zero.
                    if (d2 < 1e-9) {
                        wsum = 1.0;
                        vsum = o.value;
                        exact = true;
                        break;
                    }
                    const double w = 1.0 / std::pow(d2, cfg.idw_power * 0.5);
                    wsum += w;
                    vsum += w * o.value;
                }

                const std::size_t node = static_cast<std::size_t>(j) * f.lat_cols_ + i;
                const double est = wsum > 0.0 ? vsum / wsum : fallback;
                field[node] = est;

                // Second pass for the residual. Skipped on an exact hit, where a single
                // pilot defines the estimate and the spread is meaningless.
                if (exact || wsum <= 0.0) continue;
                double rw = 0.0;
                double rv = 0.0;
                for (const auto& o : obs) {
                    const double dx = o.lc - static_cast<double>(i);
                    const double dy = o.lr - static_cast<double>(j);
                    if (std::fabs(dx) > rad || std::fabs(dy) > rad) continue;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < 1e-9) continue;
                    const double w = 1.0 / std::pow(d2, cfg.idw_power * 0.5);
                    rw += w;
                    rv += w * std::fabs(o.value - est);
                }
                if (rw > 0.0) {
                    // Both levels contribute to the same node residual; take the worst.
                    f.residual_[node] = std::max(f.residual_[node], rv / rw);
                }
            }
        }
    };

    fill(bright, f.bright_, f.global_.bright);
    fill(dark, f.dark_, f.global_.dark);

    // Noise estimate: residual spread of pilots about their interpolated level. This is a
    // real per-frame measurement, not a constant -- it drives LLR magnitude, so a noisy
    // frame yields appropriately less confident soft decisions.
    double resid = 0.0;
    std::size_t n = 0;
    for (const auto& o : bright) {
        const auto ref = f.RefAt(static_cast<std::uint32_t>(o.lc * f.pitch_),
                                 static_cast<std::uint32_t>(o.lr * f.pitch_));
        resid += (o.value - ref.bright) * (o.value - ref.bright);
        ++n;
    }
    for (const auto& o : dark) {
        const auto ref = f.RefAt(static_cast<std::uint32_t>(o.lc * f.pitch_),
                                 static_cast<std::uint32_t>(o.lr * f.pitch_));
        resid += (o.value - ref.dark) * (o.value - ref.dark);
        ++n;
    }
    f.global_.noise_sigma =
        n > 1 ? std::max(1.0, std::sqrt(resid / static_cast<double>(n))) : 8.0;

    return f;
}

PhotometricRef PhotometricField::RefAt(std::uint32_t col, std::uint32_t row) const noexcept {
    PhotometricRef ref = global_;
    if (lat_cols_ == 0 || lat_rows_ == 0) return ref;

    // Bilinear interpolation over the lattice-resolution fields.
    const double lc = static_cast<double>(col) / static_cast<double>(pitch_);
    const double lr = static_cast<double>(row) / static_cast<double>(pitch_);

    const double cx = std::clamp(lc, 0.0, static_cast<double>(lat_cols_ - 1));
    const double cy = std::clamp(lr, 0.0, static_cast<double>(lat_rows_ - 1));
    const auto i0 = static_cast<std::uint32_t>(cx);
    const auto j0 = static_cast<std::uint32_t>(cy);
    const std::uint32_t i1 = std::min(i0 + 1, lat_cols_ - 1);
    const std::uint32_t j1 = std::min(j0 + 1, lat_rows_ - 1);
    const double fx = cx - static_cast<double>(i0);
    const double fy = cy - static_cast<double>(j0);

    const auto lerp2 = [&](const std::vector<double>& fld) {
        const double a = fld[static_cast<std::size_t>(j0) * lat_cols_ + i0];
        const double b = fld[static_cast<std::size_t>(j0) * lat_cols_ + i1];
        const double c = fld[static_cast<std::size_t>(j1) * lat_cols_ + i0];
        const double d = fld[static_cast<std::size_t>(j1) * lat_cols_ + i1];
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    };

    ref.bright = lerp2(bright_);
    ref.dark = lerp2(dark_);
    return ref;
}

double PhotometricField::ResidualAt(std::uint32_t col, std::uint32_t row) const noexcept {
    if (residual_.empty() || lat_cols_ == 0 || lat_rows_ == 0) return 0.0;
    const double lc = static_cast<double>(col) / static_cast<double>(pitch_);
    const double lr = static_cast<double>(row) / static_cast<double>(pitch_);
    const auto i = static_cast<std::uint32_t>(
        std::clamp(lc, 0.0, static_cast<double>(lat_cols_ - 1)));
    const auto j = static_cast<std::uint32_t>(
        std::clamp(lr, 0.0, static_cast<double>(lat_rows_ - 1)));
    return residual_[static_cast<std::size_t>(j) * lat_cols_ + i];
}

std::vector<double> PhotometricField::Normalise(std::span<const double> samples) const {
    std::vector<double> out(samples.size(), std::nan(""));
    if (samples.size() != static_cast<std::size_t>(g_.cells())) return out;

    for (std::uint32_t r = 0; r < g_.rows; ++r) {
        for (std::uint32_t c = 0; c < g_.cols; ++c) {
            const std::size_t idx = static_cast<std::size_t>(r) * g_.cols + c;
            const double v = samples[idx];
            if (std::isnan(v)) continue;

            const PhotometricRef ref = RefAt(c, r);
            const double sep = ref.bright - ref.dark;
            // Local contrast collapsed: erase rather than amplify noise into a confident bit.
            if (!(sep >= cfg_.min_separation)) continue;

            // The pilots here disagree with their own fitted level, so the reference cannot
            // be trusted even though it looks numerically healthy. Erase.
            if (ResidualAt(c, r) > cfg_.max_pilot_residual_ratio * sep) continue;

            out[idx] = (v - ref.dark) / sep * 255.0;
        }
    }
    return out;
}

double PhotometricField::mean_residual() const noexcept {
    if (residual_.empty()) return 0.0;
    double s = 0.0;
    for (const double r : residual_) s += r;
    return s / static_cast<double>(residual_.size());
}

double PhotometricField::mean_separation() const noexcept {
    if (bright_.empty() || bright_.size() != dark_.size()) return 0.0;
    double s = 0.0;
    for (std::size_t i = 0; i < bright_.size(); ++i) s += bright_[i] - dark_[i];
    return s / static_cast<double>(bright_.size());
}

double PhotometricField::bright_nonuniformity() const noexcept {
    if (bright_.empty()) return 1.0;
    double lo = bright_[0];
    double hi = bright_[0];
    for (const double v : bright_) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!(std::fabs(lo) > 1e-9)) return hi > 0.0 ? std::numeric_limits<double>::infinity() : 1.0;
    return hi / lo;
}

}  // namespace fileflow
