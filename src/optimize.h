// Mesh clean-up before writing: exact-duplicate vertex welding, error-bounded simplification
// and GPU vertex-cache/fetch ordering.
#pragma once
#include "scene.h"

namespace xl {

struct OptimizeOptions {
    bool enabled = true;          // weld, drop degenerate/duplicate triangles, reorder (lossless)
    double max_error = 0.001;     // simplification limit in world metres; 0 = no simplification
    bool keep_colors = false;     // vertex colours are written (--vertex-colors), so they must survive
};

struct OptimizeStats {
    size_t meshes = 0, simplified = 0, kept_whole = 0;
    size_t vertices_before = 0, vertices_after = 0;    // stored (unique meshes)
    size_t triangles_before = 0, triangles_after = 0;  // stored (unique meshes)
    size_t drawn_before = 0, drawn_after = 0;          // triangles over every placed copy
};

void optimize_meshes(Scene& scene, const OptimizeOptions& opt, OptimizeStats& stats);

} // namespace xl
