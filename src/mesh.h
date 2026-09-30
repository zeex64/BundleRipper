#pragma once
#include "unity.h"
#include <memory>
#include <string>
#include <vector>

namespace xl {

// A decoded mesh in Unity's coordinate system (left-handed, Y up, clockwise front faces).
struct MeshData {
    std::string name;
    size_t vertex_count = 0;
    std::vector<float> pos;    // xyz
    std::vector<float> nrm;    // xyz, or empty
    std::vector<float> col;    // rgba, or empty
    std::vector<float> uv[4];  // uv sets 0..3, each 2 per vertex, or empty
    struct Sub {
        std::vector<uint32_t> idx;  // triangle list, absolute vertex indices
    };
    std::vector<Sub> subs;
};

std::shared_ptr<MeshData> decode_mesh(const Database& db, ObjRef ref);
// Unity's built-in primitives from "unity default resources" (Cube, Plane, ...), or null.
std::shared_ptr<MeshData> builtin_mesh(int64_t path_id);

// Fills missing normals with area-weighted face normals.
void ensure_normals(MeshData& m);

} // namespace xl
