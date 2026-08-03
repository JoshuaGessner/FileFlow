// Screen acquisition and orientation (component C06) — image to homography.
//
// STRATEGY, and why it is not "detect four fiducials":
//
// Localisation and orientation are different problems with different best signals, so this
// splits them rather than asking one mechanism to do both.
//
//   1. LOCALISE from the persistent boundary ring. It is the largest, highest-contrast,
//      always-present structure in the frame. Four long straight edges can be fitted as
//      LINES and intersected; every pixel along an edge constrains the fit, so the corners
//      come out far more accurately than any small-fiducial centroid -- and small fiducials
//      are exactly what degrades first with distance and defocus.
//
//   2. ORIENT from the asymmetric corner markers, by scoring all four rotations of the
//      candidate quad and taking the best. Resolving a 4-way ambiguity is what asymmetric
//      fiducials are genuinely good at, and it needs far less resolution than localisation.
//
// The verification step is not optional. A detector that returns a confident WRONG homography
// is worse than one that returns nothing: every downstream cell is then sampled from the
// wrong place, the photometric field fits garbage, and the payload decodes to confident wrong
// bits. So a detection must clear an absolute score AND beat the runner-up by a margin,
// otherwise it reports kMarkersNotFound.
#pragma once

#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/result.h>

#include <array>
#include <cstdint>

namespace fileflow {

struct DetectionConfig {
    // Screen must occupy at least this fraction of the image, measured as the area of the
    // detected QUAD. Rejects small bright objects (a lamp, a reflection) without needing a
    // model of what they are.
    //
    // Deliberately NOT a test on the lit-pixel count: that is payload-dependent, and testing
    // it there made localisation fail on frames carrying mostly zeros (finding F15).
    double min_area_fraction = 0.03;

    // Fraction of marker cells that must match the expected pattern.
    double min_marker_score = 0.70;

    // The best rotation must beat the runner-up by this much. Guards against a frame whose
    // markers are damaged enough that two rotations look equally plausible -- reporting
    // failure there is correct, because a 90-degree error decodes to pure garbage.
    double min_rotation_margin = 0.12;

    // Cap on binarisation search cost; the image is subsampled to at most this many pixels
    // when computing the global threshold.
    std::uint32_t histogram_stride = 2;
};

struct Detection {
    Homography grid_to_image;
    std::array<Point2, 4> quad{};  // screen corners in image space: TL, TR, BR, BL
    int rotation = 0;              // quarter-turns applied to align the quad with the grid
    double marker_score = 0.0;     // winning rotation's score
    double runner_up_score = 0.0;  // for telemetry: how close the call was
    std::uint32_t threshold = 0;   // binarisation level actually used
};

class ScreenDetector {
  public:
    static Result<ScreenDetector> Create(const FrameLayout& layout, DetectionConfig cfg = {});

    // Locate the screen and resolve orientation. The returned homography maps GRID space
    // ([0,cols] x [0,rows]) to image pixels, ready for CellSampler.
    [[nodiscard]] Result<Detection> Detect(const ImageView8& img) const;

    // Otsu's method: the threshold minimising intra-class variance. Exposed for testing.
    [[nodiscard]] static std::uint32_t OtsuThreshold(const ImageView8& img,
                                                     std::uint32_t stride);

    // Fraction of marker cells matching expectation, for one candidate homography.
    //
    // PUBLIC so ScreenTracker can verify a locally-refined geometry without redoing
    // acquisition. Verification is the non-negotiable half of detection -- a tracker that
    // refines geometry but never checks it against the markers would drift silently, which
    // is the precise failure this whole design refuses to allow.
    [[nodiscard]] double ScoreMarkers(const ImageView8& img, const Homography& h,
                                      std::uint32_t threshold) const;

    [[nodiscard]] const DetectionConfig& config() const noexcept { return cfg_; }

  private:
    ScreenDetector(const FrameLayout& layout, DetectionConfig cfg)
        : layout_(&layout), cfg_(cfg) {}

    const FrameLayout* layout_;
    DetectionConfig cfg_;
};

}  // namespace fileflow
