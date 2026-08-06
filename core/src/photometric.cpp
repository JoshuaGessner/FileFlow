#include <fileflow/photometric.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fileflow {
namespace {

// Weight floor, in squared lattice units, so a pilot on top of a node cannot produce an infinite
// weight. A quarter of a lattice step: near enough to "on the node" to dominate the fit, far enough
// to keep the normal equations conditioned.
constexpr double kMinDistanceSq = 0.25;
// Below this many pilots in the window there is no plane worth fitting.
constexpr std::size_t kMinPlanePilots = 4;
// Collinearity guard, relative to the scale of the normal equations.
constexpr double kMinConditioning = 1e-6;

// Determinant of a 3x3 given in row-major order.
constexpr double Det3(double a, double b, double c,
                      double d, double e, double f,
                      double g, double h, double i) noexcept {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

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
    // contributing pilots ABOUT A LOCAL PLANE. The spread is the model-fit residual: when pilots
    // that should agree do not, the region is damaged and its cells must be erased rather than
    // decoded against a confidently-wrong reference.
    //
    // ## Why a plane and not a weighted mean
    //
    // This fitted a locally CONSTANT level (plain inverse-distance weighting) and measured the
    // residual as the spread about it. That makes the residual a function of the field's SLOPE:
    // across a window of radius r, a gradient g produces a spread of about g*r even when every
    // pilot is perfectly healthy and agrees exactly with its neighbours. The estimator reports
    // "these pilots disagree" when the truth is "this region is shaded".
    //
    // It is not a small effect on real optics. Measured on the first above-cliff two-device
    // capture: bright nonuniformity 3.83 across the frame, mean residual 44.5 against a budget of
    // 16.7, and 76.7% of lattice nodes erased on residual while 0% failed on separation. Nothing
    // was damaged; the screen was simply much brighter in the middle than at the edges, which is
    // what an OLED panel looks like from 13.5 cm where the edges are viewed far off-axis.
    //
    // The pathology is that it got WORSE as the rig improved. Moving closer to clear the density
    // cliff steepened the gradient, so the same fix that made the frame resolvable made it
    // unreadable: headers went from 14 to 0 between the two captures (F45).
    //
    // A first-order fit absorbs the ramp exactly, leaving the residual to measure what it was
    // always meant to measure -- occlusion, glare, and a sampling grid sitting off its cells.
    // Degenerate cases (too few pilots, or collinear ones, which happens where markers and the
    // header punch holes in the lattice) fall back to the weighted mean, which is the old
    // behaviour and is safe.
    const auto fill = [&](const std::vector<PilotObs>& obs, std::vector<double>& field,
                          double fallback) {
        for (std::uint32_t j = 0; j < f.lat_rows_; ++j) {
            for (std::uint32_t i = 0; i < f.lat_cols_; ++i) {
                // Weighted moments of the basis {1, dx, dy} against the observations.
                double S = 0.0, Sx = 0.0, Sy = 0.0;
                double Sxx = 0.0, Sxy = 0.0, Syy = 0.0;
                double Sz = 0.0, Sxz = 0.0, Syz = 0.0;
                double lo = 0.0, hi = 0.0;
                std::size_t n = 0;
                for (const auto& o : obs) {
                    const double dx = o.lc - static_cast<double>(i);
                    const double dy = o.lr - static_cast<double>(j);
                    // Square window keeps the cost bounded and local; the ring of fallback
                    // matters where markers/header punch holes in the lattice.
                    if (std::fabs(dx) > rad || std::fabs(dy) > rad) continue;
                    // Floored so a pilot sitting exactly on the node gets a large but FINITE
                    // weight. The old code special-cased the exact hit to avoid dividing by zero
                    // and, as a side effect, reported residual 0 for that node however damaged its
                    // neighbourhood was -- a false clean bill of health on the very nodes a pilot
                    // lands on.
                    const double d2 = std::max(dx * dx + dy * dy, kMinDistanceSq);
                    const double w = 1.0 / std::pow(d2, cfg.idw_power * 0.5);
                    if (n == 0) { lo = hi = o.value; }
                    else { lo = std::min(lo, o.value); hi = std::max(hi, o.value); }
                    S += w;   Sx += w * dx;        Sy += w * dy;
                    Sxx += w * dx * dx;  Sxy += w * dx * dy;  Syy += w * dy * dy;
                    Sz += w * o.value;   Sxz += w * dx * o.value;  Syz += w * dy * o.value;
                    ++n;
                }

                const std::size_t node = static_cast<std::size_t>(j) * f.lat_cols_ + i;
                if (S <= 0.0) {
                    field[node] = fallback;
                    continue;
                }

                // Weighted mean: the locally-constant estimate, and the fallback.
                double a = Sz / S;
                double b = 0.0;
                double c = 0.0;

                if (n >= kMinPlanePilots) {
                    const double det = Det3(S, Sx, Sy, Sx, Sxx, Sxy, Sy, Sxy, Syy);
                    // The Gram matrix of {1, dx, dy} is positive semi-definite, so its determinant
                    // vanishes exactly when the pilots are collinear -- no plane is determined and
                    // solving anyway would amplify noise into a steep bogus ramp. Compared against
                    // the scale of the system so the test is dimensionally sound.
                    const double ref = S * Sxx * Syy;
                    if (ref > 0.0 && det > kMinConditioning * ref) {
                        a = Det3(Sz, Sx, Sy, Sxz, Sxx, Sxy, Syz, Sxy, Syy) / det;
                        b = Det3(S, Sz, Sy, Sx, Sxz, Sxy, Sy, Syz, Syy) / det;
                        c = Det3(S, Sx, Sz, Sx, Sxx, Sxz, Sy, Sxy, Syz) / det;
                        // Never extrapolate outside what was actually observed. At the frame edge
                        // the window is one-sided, and an unclamped plane can shoot past every
                        // pilot that informed it -- a confident reference nothing supports.
                        if (a < lo || a > hi) {
                            a = std::min(std::max(a, lo), hi);
                            b = 0.0;
                            c = 0.0;
                        }
                    }
                }

                field[node] = a;

                // Second pass: spread of the pilots about the fitted surface.
                double rw = 0.0;
                double rv = 0.0;
                for (const auto& o : obs) {
                    const double dx = o.lc - static_cast<double>(i);
                    const double dy = o.lr - static_cast<double>(j);
                    if (std::fabs(dx) > rad || std::fabs(dy) > rad) continue;
                    const double d2 = std::max(dx * dx + dy * dy, kMinDistanceSq);
                    const double w = 1.0 / std::pow(d2, cfg.idw_power * 0.5);
                    rw += w;
                    rv += w * std::fabs(o.value - (a + b * dx + c * dy));
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

std::size_t PhotometricField::nodes_below_separation(double min_sep) const noexcept {
    if (bright_.size() != dark_.size()) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < bright_.size(); ++i) {
        if (bright_[i] - dark_[i] < min_sep) ++n;
    }
    return n;
}

std::size_t PhotometricField::nodes_above_residual(double ratio) const noexcept {
    if (residual_.size() != bright_.size() || residual_.size() != dark_.size()) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < residual_.size(); ++i) {
        // Judged against the node's OWN separation, as the erasure rule does: a residual of 20 is
        // negligible where the levels are 200 apart and fatal where they are 30 apart.
        if (residual_[i] > ratio * (bright_[i] - dark_[i])) ++n;
    }
    return n;
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
