#include <fileflow/sim/render.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fileflow::sim {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Head-on, the screen height covers this fraction of the image at distance 1.
constexpr double kFillFraction = 0.8;

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

Vec3 Rotate(Vec3 p, double yaw, double pitch, double roll) {
    // Rz(roll) * Ry(yaw) * Rx(pitch)
    const double cx = std::cos(pitch), sx = std::sin(pitch);
    Vec3 a{p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx};

    const double cy = std::cos(yaw), sy = std::sin(yaw);
    Vec3 b{a.x * cy + a.z * sy, a.y, -a.x * sy + a.z * cy};

    const double cz = std::cos(roll), sz = std::sin(roll);
    return {b.x * cz - b.y * sz, b.x * sz + b.y * cz, b.z};
}

}  // namespace

std::array<Point2, 4> GridCorners(const GridGeometry& g) {
    return {Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
            Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
            Point2{0, static_cast<double>(g.rows)}};
}

std::array<Point2, 4> QuadFromView(const ViewConfig& v, const GridGeometry& g) {
    // Screen as a rectangle of unit height in the z=0 plane, aspect from the grid.
    const double h = 1.0;
    const double w = (g.rows > 0) ? h * static_cast<double>(g.cols) / static_cast<double>(g.rows)
                                  : h;

    const std::array<Vec3, 4> corners{
        Vec3{-w / 2, -h / 2, 0}, Vec3{w / 2, -h / 2, 0},
        Vec3{w / 2, h / 2, 0}, Vec3{-w / 2, h / 2, 0}};

    const double yaw = v.yaw_deg * kPi / 180.0;
    const double pitch = v.pitch_deg * kPi / 180.0;
    const double roll = v.roll_deg * kPi / 180.0;

    // Focal length fixed so distance actually changes apparent size.
    const double f = kFillFraction * static_cast<double>(v.image_height);
    const double cx = 0.5 * static_cast<double>(v.image_width) +
                      v.offset_x * static_cast<double>(v.image_width);
    const double cy = 0.5 * static_cast<double>(v.image_height) +
                      v.offset_y * static_cast<double>(v.image_height);

    std::array<Point2, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        Vec3 p = Rotate(corners[i], yaw, pitch, roll);
        p.z += v.distance;
        if (p.z <= 1e-6) return {Point2{0, 0}, Point2{0, 0}, Point2{0, 0}, Point2{0, 0}};
        out[i] = {f * p.x / p.z + cx, f * p.y / p.z + cy};
    }
    return out;
}

Image8 RenderOptical(const CellMatrix& cells, const Homography& grid_to_image,
                     const OpticalRenderConfig& cfg) {
    Image8 img(cfg.view.image_width, cfg.view.image_height, cfg.background);
    auto inv_r = grid_to_image.Inverse();
    if (!inv_r.ok()) return img;
    const Homography inv = inv_r.value();

    const double gcx = 0.5 * static_cast<double>(cells.cols());
    const double gcy = 0.5 * static_cast<double>(cells.rows());
    const double max_r = std::hypot(gcx, gcy);

    std::uint64_t rng = cfg.seed | 1ULL;
    const auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const Point2 g = inv.Apply({static_cast<double>(x), static_cast<double>(y)});
            if (!std::isfinite(g.x) || !std::isfinite(g.y)) continue;
            if (g.x < 0 || g.y < 0) continue;
            const auto c = static_cast<std::int64_t>(g.x);
            const auto r = static_cast<std::int64_t>(g.y);
            if (c >= cells.cols() || r >= cells.rows()) continue;

            double v = cells.at(static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(r));

            if (cfg.corner_falloff != 1.0 && max_r > 0.0) {
                const double rad = std::hypot(g.x - gcx, g.y - gcy) / max_r;
                v *= 1.0 + (cfg.corner_falloff - 1.0) * rad * rad;
            }
            v += cfg.black_lift;
            if (cfg.glare_lift != 0.0 && cells.cols() > 0) {
                const double t =
                    1.0 - std::clamp(g.x / static_cast<double>(cells.cols()), 0.0, 1.0);
                v += cfg.glare_lift * t;
            }
            if (cfg.noise_amplitude > 0.0) {
                const double u = static_cast<double>(next() % 2001) / 1000.0 - 1.0;
                v += u * cfg.noise_amplitude;
            }

            img.set(x, y, static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0)));
        }
    }

    if (cfg.blur_radius > 0) {
        Image8 blurred(img.width(), img.height(), cfg.background);
        const int k = cfg.blur_radius;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                int sum = 0;
                int n = 0;
                for (int dy = -k; dy <= k; ++dy) {
                    for (int dx = -k; dx <= k; ++dx) {
                        const int sx = x + dx;
                        const int sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= img.width() || sy >= img.height()) {
                            continue;
                        }
                        sum += img.at(sx, sy);
                        ++n;
                    }
                }
                blurred.set(x, y, static_cast<std::uint8_t>(n > 0 ? sum / n : 0));
            }
        }
        return blurred;
    }
    return img;
}

Image8 RenderView(const CellMatrix& cells, const GridGeometry& g,
                  const OpticalRenderConfig& cfg, Homography* used_transform) {
    const auto quad = QuadFromView(cfg.view, g);
    const auto corners = GridCorners(g);
    auto h = Homography::FromCorrespondences(std::span<const Point2, 4>(corners),
                                             std::span<const Point2, 4>(quad));
    if (!h.ok()) {
        if (used_transform != nullptr) *used_transform = Homography::Identity();
        return Image8(cfg.view.image_width, cfg.view.image_height, cfg.background);
    }
    if (used_transform != nullptr) *used_transform = h.value();
    return RenderOptical(cells, h.value(), cfg);
}

}  // namespace fileflow::sim
