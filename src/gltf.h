#pragma once
#include "scene.h"
#include <filesystem>

namespace xl {

struct GltfResult {
    uint64_t bytes = 0;
    size_t nodes = 0, meshes = 0, materials = 0, images = 0;
};

struct GltfOptions {
    // PNGs go into a "<name>_textures" folder beside the .glb instead of inside it.
    bool external_textures = false;
    // Write COLOR_0. Blender multiplies it into base colour through extra nodes.
    bool vertex_colors = false;
    // Cut-outs as glTF MASK (Blender builds "Alpha Clip" math nodes). Otherwise they are
    // written as BLEND, which Blender wires straight into the BSDF alpha, and tagged
    // sk8_material.alpha = mask (+ alpha_cutoff) so ReSkate Studio still treats them as cut-outs.
    bool standard_alpha = false;
    // Link occlusion (the ORM texture's red channel). Blender puts it in a side node group
    // that ReSkate Studio doesn't read, so it is off unless asked for.
    bool occlusion = false;
};

// Writes the scene as a binary glTF. Unity's left-handed Y-up space becomes glTF's
// right-handed Y-up space by mirroring X. Texture tiling is baked into the UVs so no
// texture transforms are needed.
GltfResult write_glb(const Scene& scene, const std::filesystem::path& out, const GltfOptions& options);

} // namespace xl
