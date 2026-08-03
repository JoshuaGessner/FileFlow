// Thin test adapter over the SIMULATOR's renderer (sim/render.h).
//
// There is deliberately only ONE renderer in the project. An imitation living in the test
// tree would drift from the simulator, and then tests would pass against a channel the
// simulator does not model -- which is RISK-024 wearing a different hat.
#pragma once

#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/sim/render.h>

#include <array>
#include <span>

namespace fileflow::test {

using RenderOptions = sim::OpticalRenderConfig;

inline std::span<const Point2, 4> Span4(const std::array<Point2, 4>& a) {
    return std::span<const Point2, 4>(a);
}

inline std::array<Point2, 4> GridCorners(const GridGeometry& g) { return sim::GridCorners(g); }

// Render into an explicitly sized image with an explicit transform.
inline Image8 Render(const CellMatrix& cells, const Homography& grid_to_image, int width,
                     int height, RenderOptions opt = {}) {
    opt.view.image_width = width;
    opt.view.image_height = height;
    return sim::RenderOptical(cells, grid_to_image, opt);
}

}  // namespace fileflow::test
