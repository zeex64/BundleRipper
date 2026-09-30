#pragma once
#include "scene.h"
#include <filesystem>

namespace xl {

// Writes the scene's grind splines as OBJ NURBS curves, which Blender's OBJ importer turns into
// curve objects: poly lines as degree 1, Bezier paths as degree 3 with Bezier knots (exact).
// Coordinates are world space on the same axes as the .glb (Y up, X mirrored from Unity), so
// importing with Blender's default OBJ axes lines the curves up with the imported map.
// Returns the number of curves written.
size_t write_curves_obj(const std::vector<OutCurve>& curves, const std::filesystem::path& out);

} // namespace xl
