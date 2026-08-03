#include <fileflow/geometry.h>
#include <fileflow/image.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

using namespace fileflow;

namespace {
constexpr std::array<Point2, 4> kUnitSquare{
    Point2{0, 0}, Point2{1, 0}, Point2{1, 1}, Point2{0, 1}};

std::span<const Point2, 4> Span4(const std::array<Point2, 4>& a) {
    return std::span<const Point2, 4>(a);
}
}  // namespace

// ---------------------------------------------------------------- linear solver

TEST(SolveLinearSystem, SolvesWellConditioned) {
    // 2x + y = 5 ; x - y = 1  ->  x = 2, y = 1
    std::array<double, 4> a{2, 1, 1, -1};
    std::array<double, 2> b{5, 1};
    ASSERT_TRUE(SolveLinearSystem(a, b, 2));
    EXPECT_NEAR(b[0], 2.0, 1e-12);
    EXPECT_NEAR(b[1], 1.0, 1e-12);
}

TEST(SolveLinearSystem, RequiresPivotingToSucceed) {
    // Leading pivot is exactly zero; only partial pivoting saves this.
    std::array<double, 4> a{0, 1, 1, 0};
    std::array<double, 2> b{3, 4};
    ASSERT_TRUE(SolveLinearSystem(a, b, 2));
    EXPECT_NEAR(b[0], 4.0, 1e-12);
    EXPECT_NEAR(b[1], 3.0, 1e-12);
}

TEST(SolveLinearSystem, RejectsSingularAndBadSizes) {
    std::array<double, 4> singular{1, 2, 2, 4};  // row2 = 2*row1
    std::array<double, 2> b{1, 2};
    EXPECT_FALSE(SolveLinearSystem(singular, b, 2));

    std::array<double, 4> a{1, 0, 0, 1};
    EXPECT_FALSE(SolveLinearSystem(a, b, 0));
    EXPECT_FALSE(SolveLinearSystem(a, b, 9));   // n > 8
    EXPECT_FALSE(SolveLinearSystem(a, b, 3));   // buffer too small for n=3
}

// ---------------------------------------------------------------- homography

TEST(Homography, IdentityMapsPointsToThemselves) {
    const auto h = Homography::Identity();
    const Point2 p = h.Apply({3.5, -2.25});
    EXPECT_NEAR(p.x, 3.5, 1e-12);
    EXPECT_NEAR(p.y, -2.25, 1e-12);
}

TEST(Homography, RecoversAKnownPerspectiveTransform) {
    // A genuinely projective quad -- not affine, so h6/h7 must be non-zero.
    const std::array<Point2, 4> dst{Point2{100, 50}, Point2{540, 90},
                                    Point2{500, 400}, Point2{130, 360}};
    auto h = Homography::FromCorrespondences(Span4(kUnitSquare), Span4(dst));
    ASSERT_TRUE(h.ok()) << ErrorName(h.error());

    // Must reproduce the correspondences to floating-point precision.
    EXPECT_LT(h.value().MaxReprojectionError(kUnitSquare, dst), 1e-9);

    // And it must actually be projective, otherwise the test proves nothing about the
    // hard case: an affine fit would also pass the four corner checks.
    const auto& m = h.value().m();
    EXPECT_GT(std::fabs(m[6]) + std::fabs(m[7]), 1e-9);
}

TEST(Homography, InverseRoundTrips) {
    const std::array<Point2, 4> dst{Point2{100, 50}, Point2{540, 90},
                                    Point2{500, 400}, Point2{130, 360}};
    auto h = Homography::FromCorrespondences(Span4(kUnitSquare), Span4(dst));
    ASSERT_TRUE(h.ok());
    auto inv = h.value().Inverse();
    ASSERT_TRUE(inv.ok());

    for (double gy = 0.0; gy <= 1.0; gy += 0.25) {
        for (double gx = 0.0; gx <= 1.0; gx += 0.25) {
            const Point2 img = h.value().Apply({gx, gy});
            const Point2 back = inv.value().Apply(img);
            EXPECT_NEAR(back.x, gx, 1e-9);
            EXPECT_NEAR(back.y, gy, 1e-9);
        }
    }
}

TEST(Homography, ComposeMatchesSequentialApplication) {
    const std::array<Point2, 4> mid{Point2{2, 1}, Point2{9, 0}, Point2{10, 7}, Point2{1, 8}};
    const std::array<Point2, 4> dst{Point2{100, 50}, Point2{540, 90},
                                    Point2{500, 400}, Point2{130, 360}};
    auto a = Homography::FromCorrespondences(Span4(kUnitSquare), Span4(mid));
    auto b = Homography::FromCorrespondences(Span4(mid), Span4(dst));
    ASSERT_TRUE(a.ok() && b.ok());

    const Homography ab = a.value().Compose(b.value());
    const Point2 direct = b.value().Apply(a.value().Apply({0.3, 0.7}));
    const Point2 composed = ab.Apply({0.3, 0.7});
    EXPECT_NEAR(composed.x, direct.x, 1e-9);
    EXPECT_NEAR(composed.y, direct.y, 1e-9);
}

TEST(Homography, RejectsDegenerateCorrespondences) {
    // Collinear destination points -- no valid homography exists.
    const std::array<Point2, 4> collinear{Point2{0, 0}, Point2{1, 1},
                                          Point2{2, 2}, Point2{3, 3}};
    auto h = Homography::FromCorrespondences(Span4(kUnitSquare), Span4(collinear));
    EXPECT_FALSE(h.ok());
    EXPECT_EQ(h.error(), Error::kDegenerateHomography);

    // All four coincident.
    const std::array<Point2, 4> same{Point2{5, 5}, Point2{5, 5}, Point2{5, 5}, Point2{5, 5}};
    EXPECT_FALSE(Homography::FromCorrespondences(Span4(kUnitSquare), Span4(same)).ok());
    EXPECT_FALSE(Homography::FromCorrespondences(Span4(same), Span4(kUnitSquare)).ok());
}

TEST(Homography, RejectsNonFiniteInput) {
    const std::array<Point2, 4> bad{Point2{0, 0}, Point2{1, 0},
                                    Point2{std::nan(""), 1}, Point2{0, 1}};
    EXPECT_FALSE(Homography::FromCorrespondences(Span4(kUnitSquare), Span4(bad)).ok());

    const std::array<Point2, 4> inf{Point2{0, 0}, Point2{1, 0},
                                    Point2{std::numeric_limits<double>::infinity(), 1},
                                    Point2{0, 1}};
    EXPECT_FALSE(Homography::FromCorrespondences(Span4(kUnitSquare), Span4(inf)).ok());
}

TEST(Homography, NormalisationSurvivesLargeCoordinates) {
    // The case Hartley normalisation exists for: coordinates far from the origin, where an
    // unnormalised DLT loses precision badly.
    const double k = 1.0e5;
    const std::array<Point2, 4> big{Point2{k, k}, Point2{k + 400, k + 30},
                                    Point2{k + 380, k + 300}, Point2{k + 20, k + 280}};
    auto h = Homography::FromCorrespondences(Span4(kUnitSquare), Span4(big));
    ASSERT_TRUE(h.ok());
    // Error must stay tiny RELATIVE to the coordinate magnitude.
    EXPECT_LT(h.value().MaxReprojectionError(kUnitSquare, big), 1e-4);
}

// ---------------------------------------------------------------- image sampling

TEST(ImageView8, BilinearInterpolatesAndRespectsStride) {
    // 2x2 image embedded in a 4-wide buffer: proves stride != width is handled.
    std::vector<std::uint8_t> buf{0, 100, 9, 9,
                                  200, 255, 9, 9};
    const ImageView8 v(buf.data(), 2, 2, 4);

    EXPECT_DOUBLE_EQ(v.SampleBilinear(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(v.SampleBilinear(1, 0), 100.0);
    EXPECT_DOUBLE_EQ(v.SampleBilinear(0, 1), 200.0);
    EXPECT_DOUBLE_EQ(v.SampleBilinear(1, 1), 255.0);
    EXPECT_DOUBLE_EQ(v.SampleBilinear(0.5, 0.0), 50.0);
    EXPECT_NEAR(v.SampleBilinear(0.5, 0.5), (0 + 100 + 200 + 255) / 4.0, 1e-9);
}

TEST(ImageView8, OutsideIsNaNNotClamped) {
    // Clamping would fabricate plausible data at the frame edge; NaN becomes an erasure.
    std::vector<std::uint8_t> buf{10, 20, 30, 40};
    const ImageView8 v(buf.data(), 2, 2, 2);

    EXPECT_TRUE(std::isnan(v.SampleBilinear(-0.01, 0)));
    EXPECT_TRUE(std::isnan(v.SampleBilinear(0, -0.01)));
    EXPECT_TRUE(std::isnan(v.SampleBilinear(1.01, 0)));
    EXPECT_TRUE(std::isnan(v.SampleBilinear(0, 1.01)));
    EXPECT_TRUE(std::isnan(v.SampleBilinear(std::nan(""), 0)));
    EXPECT_TRUE(std::isnan(ImageView8{}.SampleBilinear(0, 0)));
}
