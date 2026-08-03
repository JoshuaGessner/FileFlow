// Projective geometry for screen localisation (component C08).
//
// THE central piece of receiver maths. Visual MIMO's capacity analysis
// ([VISUALMIMO-CISS11], docs/research/BIBLIOGRAPHY.md) identifies PERSPECTIVE DISTORTION as
// the dominant limit on multiplexing gain -- it plays the role multipath fading plays in RF
// MIMO. A homography that is slightly wrong shifts every sample point toward its neighbours,
// which raises spatial crosstalk uniformly across the frame and pushes the whole grid off
// the density cliff at once. Sub-pixel accuracy here is worth more than any decoder tuning.
//
// Convention: GRID SPACE has cell (c,r) spanning [c,c+1] x [r,r+1], so the full grid
// occupies [0,cols] x [0,rows]. A Homography maps grid space -> image pixel space.
#pragma once

#include <fileflow/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fileflow {

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

// 3x3 projective transform, row-major, normalised so m[8] == 1 where possible.
class Homography {
  public:
    Homography() : m_{1, 0, 0, 0, 1, 0, 0, 0, 1} {}
    explicit Homography(const std::array<double, 9>& m) : m_(m) {}

    static Homography Identity() { return {}; }

    // Solve the transform taking `src` to `dst` from exactly four correspondences.
    //
    // Uses Hartley normalisation (translate centroid to origin, scale mean radius to
    // sqrt(2)) before the DLT solve. Skipping normalisation is the classic way to get a
    // homography that looks fine on synthetic data and degrades badly on real captures,
    // because raw pixel coordinates in the hundreds produce a system whose condition number
    // is enormous. Cheap to do, expensive to omit.
    //
    // Fails with kDegenerateHomography when points are collinear or coincident.
    static Result<Homography> FromCorrespondences(std::span<const Point2, 4> src,
                                                  std::span<const Point2, 4> dst);

    [[nodiscard]] Point2 Apply(Point2 p) const noexcept;

    // Fails with kDegenerateHomography if the matrix is singular.
    [[nodiscard]] Result<Homography> Inverse() const;

    [[nodiscard]] Homography Compose(const Homography& then) const noexcept;

    [[nodiscard]] const std::array<double, 9>& m() const noexcept { return m_; }

    // Largest reprojection error over a correspondence set, in destination units.
    // The honest way to report geometric accuracy: a mean hides the one bad corner that
    // ruins a whole frame edge.
    [[nodiscard]] double MaxReprojectionError(std::span<const Point2> src,
                                              std::span<const Point2> dst) const noexcept;

  private:
    std::array<double, 9> m_;
};

// Solve a small dense linear system in place by Gauss-Jordan with partial pivoting.
// Exposed for testing; `n` must be <= 8. Returns false if the matrix is singular.
bool SolveLinearSystem(std::span<double> a, std::span<double> b, int n) noexcept;

// --- Convex-quad scanline support, used by the tracker's boundary-annulus search ---

// The horizontal extent of a convex quad at a given row. `empty` when the row misses it.
struct RowSpan {
    double x0 = 0.0;
    double x1 = 0.0;
    bool empty = true;
};

// Intersect a convex quad with the horizontal line y = `y`.
//
// Correct for ROTATED quads, which is the whole reason it exists: an axis-aligned bounding
// box scaled about its centre does not track a rotated quad's boundary, so a bbox-based
// annulus would exclude parts of the boundary ring exactly when the receiver is held at an
// angle -- the common case.
[[nodiscard]] RowSpan QuadRowSpan(const std::array<Point2, 4>& quad, double y) noexcept;

// Scale a quad about its own centroid. factor > 1 dilates, < 1 erodes.
[[nodiscard]] std::array<Point2, 4> ScaleQuad(const std::array<Point2, 4>& quad,
                                              double factor) noexcept;

[[nodiscard]] Point2 QuadCentroid(const std::array<Point2, 4>& quad) noexcept;

// Streaming corner extraction: the extremes of a point set along the two diagonal
// directions, which for a convex quad ARE its corners.
//
// STREAMING BY NECESSITY, not by taste. The obvious version collects candidate points into a
// vector first, but that allocation is sized by ATTACKER-CONTROLLED pixel content -- measured
// at 48 MB per frame at 12 MP and 762 MB at 200 MP, ~1.85 GB/s at 4K60 (finding F12,
// THREAT-MODEL T2). Four extremes need no storage, so this keeps O(1) state and never
// allocates. Shared by the full-image detector and the tracker's annulus scan.
struct CornerExtremes {
    Point2 tl{}, tr{}, br{}, bl{};
    std::uint64_t count = 0;

    // NOTE the seeds: a point at (0,0) has both sum and diff equal to zero, so seeding these
    // at 0 would silently refuse to record it and shift the detected corner inward on any
    // screen flush with the image origin.
    double min_sum = 1e300, max_sum = -1e300, min_diff = 1e300, max_diff = -1e300;

    void Add(double x, double y) noexcept {
        const double s = x + y;
        const double d = x - y;
        if (s < min_sum) { min_sum = s; tl = {x, y}; }
        if (s > max_sum) { max_sum = s; br = {x, y}; }
        if (d > max_diff) { max_diff = d; tr = {x, y}; }
        if (d < min_diff) { min_diff = d; bl = {x, y}; }
        ++count;
    }

    [[nodiscard]] bool valid() const noexcept { return count >= 4; }
    [[nodiscard]] std::array<Point2, 4> quad() const noexcept { return {tl, tr, br, bl}; }
};

}  // namespace fileflow
