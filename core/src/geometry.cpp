#include <fileflow/geometry.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fileflow {
namespace {

constexpr double kSingularTolerance = 1e-12;

struct Normalisation {
    double scale = 1.0;
    double cx = 0.0;
    double cy = 0.0;
    bool ok = false;
};

// Hartley normalisation: centroid to origin, mean distance from origin to sqrt(2).
Normalisation ComputeNormalisation(std::span<const Point2, 4> pts) noexcept {
    Normalisation n;
    for (const auto& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return n;
        n.cx += p.x;
        n.cy += p.y;
    }
    n.cx /= 4.0;
    n.cy /= 4.0;

    double mean_dist = 0.0;
    for (const auto& p : pts) {
        mean_dist += std::hypot(p.x - n.cx, p.y - n.cy);
    }
    mean_dist /= 4.0;

    // All four points coincident -- no scale can be recovered.
    if (mean_dist < kSingularTolerance) return n;

    n.scale = std::sqrt(2.0) / mean_dist;
    n.ok = true;
    return n;
}

Point2 ApplyNormalisation(const Normalisation& n, Point2 p) noexcept {
    return {(p.x - n.cx) * n.scale, (p.y - n.cy) * n.scale};
}

std::array<double, 9> Multiply(const std::array<double, 9>& a,
                               const std::array<double, 9>& b) noexcept {
    std::array<double, 9> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) {
                s += a[static_cast<std::size_t>(i) * 3 + static_cast<std::size_t>(k)] *
                     b[static_cast<std::size_t>(k) * 3 + static_cast<std::size_t>(j)];
            }
            r[static_cast<std::size_t>(i) * 3 + static_cast<std::size_t>(j)] = s;
        }
    }
    return r;
}

}  // namespace

bool SolveLinearSystem(std::span<double> a, std::span<double> b, int n) noexcept {
    if (n <= 0 || n > 8) return false;
    const auto sn = static_cast<std::size_t>(n);
    if (a.size() < sn * sn || b.size() < sn) return false;

    for (std::size_t col = 0; col < sn; ++col) {
        // Partial pivoting: without it, a zero (or tiny) pivot silently destroys accuracy.
        std::size_t pivot = col;
        double best = std::fabs(a[col * sn + col]);
        for (std::size_t r = col + 1; r < sn; ++r) {
            const double v = std::fabs(a[r * sn + col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < kSingularTolerance) return false;

        if (pivot != col) {
            for (std::size_t c = 0; c < sn; ++c) {
                std::swap(a[col * sn + c], a[pivot * sn + c]);
            }
            std::swap(b[col], b[pivot]);
        }

        const double inv = 1.0 / a[col * sn + col];
        for (std::size_t c = 0; c < sn; ++c) a[col * sn + c] *= inv;
        b[col] *= inv;

        for (std::size_t r = 0; r < sn; ++r) {
            if (r == col) continue;
            const double f = a[r * sn + col];
            if (f == 0.0) continue;
            for (std::size_t c = 0; c < sn; ++c) a[r * sn + c] -= f * a[col * sn + c];
            b[r] -= f * b[col];
        }
    }
    return true;
}

Result<Homography> Homography::FromCorrespondences(std::span<const Point2, 4> src,
                                                   std::span<const Point2, 4> dst) {
    const Normalisation ns = ComputeNormalisation(src);
    const Normalisation nd = ComputeNormalisation(dst);
    if (!ns.ok || !nd.ok) return Error::kDegenerateHomography;

    std::array<Point2, 4> s{};
    std::array<Point2, 4> d{};
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = ApplyNormalisation(ns, src[i]);
        d[i] = ApplyNormalisation(nd, dst[i]);
    }

    // Two rows per correspondence:
    //   h0*x + h1*y + h2 - h6*x*u - h7*y*u = u
    //   h3*x + h4*y + h5 - h6*x*v - h7*y*v = v
    std::array<double, 64> a{};
    std::array<double, 8> b{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = s[i].x;
        const double y = s[i].y;
        const double u = d[i].x;
        const double v = d[i].y;

        double* r0 = &a[(i * 2) * 8];
        r0[0] = x;  r0[1] = y;  r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0;
        r0[6] = -x * u; r0[7] = -y * u;
        b[i * 2] = u;

        double* r1 = &a[(i * 2 + 1) * 8];
        r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = x; r1[4] = y; r1[5] = 1;
        r1[6] = -x * v; r1[7] = -y * v;
        b[i * 2 + 1] = v;
    }

    if (!SolveLinearSystem(a, b, 8)) return Error::kDegenerateHomography;

    const std::array<double, 9> hn{b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], 1.0};

    // Denormalise: H = Td^-1 * Hn * Ts, where T = [[s,0,-s*cx],[0,s,-s*cy],[0,0,1]].
    const std::array<double, 9> ts{ns.scale, 0, -ns.scale * ns.cx,
                                   0, ns.scale, -ns.scale * ns.cy,
                                   0, 0, 1};
    const double inv_sd = 1.0 / nd.scale;
    const std::array<double, 9> td_inv{inv_sd, 0, nd.cx,
                                       0, inv_sd, nd.cy,
                                       0, 0, 1};

    std::array<double, 9> m = Multiply(td_inv, Multiply(hn, ts));

    if (std::fabs(m[8]) < kSingularTolerance) return Error::kDegenerateHomography;
    const double inv = 1.0 / m[8];
    for (auto& v : m) v *= inv;
    for (const auto& v : m) {
        if (!std::isfinite(v)) return Error::kDegenerateHomography;
    }

    // The matrix must be INVERTIBLE, and this check was missing.
    //
    // A projective transform is invertible by definition; a singular matrix is a degenerate
    // fit, not a valid homography. Normalising m[8] to 1 and checking finiteness -- which is
    // all this did before -- does not imply a non-zero determinant, so `FromCorrespondences`
    // could succeed while `Inverse()` on the very same matrix failed. `ScreenDetector` then
    // returned a Detection whose homography could not be inverted, and the tracker and every
    // other consumer of the inverse inherited a broken contract.
    //
    // Found by `fuzz_screen_detect` (GitHub Actions run 30858226556, 2026-08-03) on the FIRST
    // run the fuzz targets ever had: 5,022 executions, a 3x2-pixel image against a 24x40 grid,
    // tripping the harness's invertibility invariant. It reproduces only where floating-point
    // rounding puts the determinant on the far side of the tolerance -- Linux, not macOS --
    // which is why enforcing the contract here beats chasing the arithmetic (finding F24).
    //
    // Checked with the same tolerance `Inverse()` uses, so the two cannot disagree.
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::fabs(det) < kSingularTolerance) {
        return Error::kDegenerateHomography;
    }

    return Homography(m);
}

Point2 Homography::Apply(Point2 p) const noexcept {
    const double w = m_[6] * p.x + m_[7] * p.y + m_[8];
    if (std::fabs(w) < kSingularTolerance) {
        return {std::nan(""), std::nan("")};  // point on the horizon line
    }
    const double inv = 1.0 / w;
    return {(m_[0] * p.x + m_[1] * p.y + m_[2]) * inv,
            (m_[3] * p.x + m_[4] * p.y + m_[5]) * inv};
}

Result<Homography> Homography::Inverse() const {
    const double c00 = m_[4] * m_[8] - m_[5] * m_[7];
    const double c01 = m_[5] * m_[6] - m_[3] * m_[8];
    const double c02 = m_[3] * m_[7] - m_[4] * m_[6];
    const double det = m_[0] * c00 + m_[1] * c01 + m_[2] * c02;
    if (std::fabs(det) < kSingularTolerance) return Error::kDegenerateHomography;

    const double inv = 1.0 / det;
    std::array<double, 9> r{
        c00 * inv,
        (m_[2] * m_[7] - m_[1] * m_[8]) * inv,
        (m_[1] * m_[5] - m_[2] * m_[4]) * inv,
        c01 * inv,
        (m_[0] * m_[8] - m_[2] * m_[6]) * inv,
        (m_[2] * m_[3] - m_[0] * m_[5]) * inv,
        c02 * inv,
        (m_[1] * m_[6] - m_[0] * m_[7]) * inv,
        (m_[0] * m_[4] - m_[1] * m_[3]) * inv,
    };

    if (std::fabs(r[8]) > kSingularTolerance) {
        const double s = 1.0 / r[8];
        for (auto& v : r) v *= s;
    }
    for (const auto& v : r) {
        if (!std::isfinite(v)) return Error::kDegenerateHomography;
    }
    return Homography(r);
}

Homography Homography::Compose(const Homography& then) const noexcept {
    return Homography(Multiply(then.m_, m_));
}

Point2 QuadCentroid(const std::array<Point2, 4>& quad) noexcept {
    Point2 c{};
    for (const auto& p : quad) {
        c.x += p.x;
        c.y += p.y;
    }
    return {c.x * 0.25, c.y * 0.25};
}

std::array<Point2, 4> ScaleQuad(const std::array<Point2, 4>& quad, double factor) noexcept {
    const Point2 c = QuadCentroid(quad);
    std::array<Point2, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = {c.x + (quad[i].x - c.x) * factor, c.y + (quad[i].y - c.y) * factor};
    }
    return out;
}

RowSpan QuadRowSpan(const std::array<Point2, 4>& quad, double y) noexcept {
    RowSpan span;
    double lo = 1e300;
    double hi = -1e300;

    for (std::size_t i = 0; i < 4; ++i) {
        const Point2& a = quad[i];
        const Point2& b = quad[(i + 1) % 4];
        const double dy = b.y - a.y;

        if (std::fabs(dy) < 1e-12) {
            // Edge parallel to the scanline. It contributes only when the line lies ON it,
            // in which case both endpoints are valid crossings.
            if (std::fabs(a.y - y) < 1e-9) {
                lo = std::min({lo, a.x, b.x});
                hi = std::max({hi, a.x, b.x});
            }
            continue;
        }

        // Half-open in t so a vertex shared by two edges is not counted twice.
        const double t = (y - a.y) / dy;
        if (t < 0.0 || t >= 1.0) continue;
        const double x = a.x + (b.x - a.x) * t;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }

    if (lo > hi) return span;  // row misses the quad
    span.x0 = lo;
    span.x1 = hi;
    span.empty = false;
    return span;
}

double Homography::MaxReprojectionError(std::span<const Point2> src,
                                        std::span<const Point2> dst) const noexcept {
    const std::size_t n = src.size() < dst.size() ? src.size() : dst.size();
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Point2 p = Apply(src[i]);
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return std::nan("");
        const double e = std::hypot(p.x - dst[i].x, p.y - dst[i].y);
        if (e > worst) worst = e;
    }
    return worst;
}

}  // namespace fileflow
