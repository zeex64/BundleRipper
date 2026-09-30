#pragma once
#include "math.h"
#include <cstdint>
#include <vector>

namespace xl {

// Finds the lines a skater would grind along on a mesh, in world space (Unity axes, Y up):
// convex top creases (ledges, boxes, square rails, ridges) and the top line of smooth round
// tubes up to 30 cm radius (rails, copings, pipes). Returns point runs, simplified.
std::vector<std::vector<V3>> find_grind_lines(const std::vector<V3>& positions, const std::vector<uint32_t>& triangles);

} // namespace xl
