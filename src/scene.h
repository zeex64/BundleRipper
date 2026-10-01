// Converts a loaded Unity scene into a format-neutral scene for the glTF writer.
#pragma once
#include "json.h"
#include "math.h"
#include "mesh.h"
#include "unity.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xl {

struct Edits;

// One PNG to produce. Sources are decoded, combined per role, then encoded.
struct ImageJob {
    enum Role { Color, Normal, MaskHDRP, MetallicStd, Emissive, Raw, PackORM };
    Role role = Color;
    // Color/Normal/Emissive/Raw: src[0]. MaskHDRP: mask. MetallicStd: metallic, occlusion, albedo.
    // PackORM: occlusion, roughness (or smoothness), metallic greyscale maps.
    ObjRef src[3];
    float params[8] = {};
    std::string name;
    // results
    std::vector<uint8_t> png;
    int wrap_u = 0, wrap_v = 0, filter = 1;
    bool ok = false;
    bool cutout = false;  // Color: alpha looks like a cut-out mask (leaves, fences)
    // The scene view wants the pixels, not a PNG: RGBA rows bottom-up (as Unity stores them).
    bool keep_pixels = false;
    int px_w = 0, px_h = 0;
    std::vector<uint8_t> pixels;
};

struct TexRef {
    int image = -1;  // index into Scene::images
    int uv = 0;
};

struct OutMaterial {
    std::string name;
    std::string key;  // stable identity across rips (source file + object id), for edits
    std::string shader;
    float base[4] = {1, 1, 1, 1};
    TexRef base_tex, normal_tex, orm_tex, emissive_tex;
    bool orm_has_occlusion = false;
    float normal_scale = 1;
    float metallic = 0, roughness = 1;
    float emissive[3] = {0, 0, 0};
    float emissive_strength = 1;
    int alpha_mode = 0;  // 0 opaque, 1 mask, 2 blend
    float alpha_cutoff = 0.5f;
    bool auto_alpha = false;  // shader hides its alpha test: decide from the base texture
    bool double_sided = false;
    bool unlit = false;
    bool uv_transform = false;
    float uv_scale[2] = {1, 1}, uv_offset[2] = {0, 0};  // Unity convention (v up)
    Json extras = Json::object();
};

// A grind spline as a curve in Unity world space: a poly line through `points`, or cubic Bezier
// segments with handles `left`/`right` at each point. Written to <map>_splines.obj.
struct OutCurve {
    std::string name;
    std::string key;  // stable identity (the spline object, or the object an auto spline was found on + index)
    bool bezier = false;
    bool closed = false;
    std::vector<V3> points, left, right;
};

struct OutMesh {
    std::string name;
    std::shared_ptr<MeshData> data;
    std::vector<int> materials;  // per submesh; -1 = default material
    bool lines = false;          // submesh 0 is a line list
};

struct OutLight {
    std::string name;
    int type = 0;  // 0 point, 1 spot, 2 directional
    float color[3] = {1, 1, 1};
    float intensity = 1;
    float range = 0;
    float inner = 0, outer = 0.785f;
    bool area = false;            // a Unity area light (rectangle or disc), exported as an area light
    float area_size[2] = {1, 1};  // metres
};

struct OutNode {
    std::string name;
    std::string key;  // stable identity across rips (source file + object id), for edits
    V3 t;
    Quat r;
    V3 s{1, 1, 1};
    int mesh = -1;
    int light = -1;
    std::vector<int> children;
    Json extras = Json::object();
};

struct Scene {
    std::vector<OutNode> nodes;
    std::vector<int> roots;
    std::vector<OutMesh> meshes;
    std::vector<OutMaterial> materials;
    std::vector<ImageJob> images;
    std::vector<OutLight> lights;
    std::vector<OutCurve> curves;       // the map's own grind splines
    std::vector<OutCurve> auto_curves;  // grind lines found on rails, copings and ledges
};

enum class DecalMode { Project, Quad, None };

struct Options {
    bool include_inactive = false;
    bool all_lods = false;
    int lod = 0;        // LOD level to export from each LODGroup (the last one if it has fewer)
    int tree_lod = -1;  // the same for terrain-painted tree/grass prefabs; -1 = as `lod`
    std::map<std::string, int> plant_lod;  // per painted prefab name: its own LOD level (99 = lowest)
    // Scene view: keep every LOD level, each renderer tagged with xl_lod_group / xl_lod_levels
    // (bit mask) / xl_lod_count (and xl_plant), so the view can switch levels without a reload.
    bool tag_lods = false;
    const Edits* edits = nullptr;  // map edits to bake in (edits.h)
    bool colliders = true;   // collider-only geometry as <name>_col objects
    bool flatten = true;     // every object at the root with its world transform
    bool triggers = false;
    bool splines = true;
    bool spline_meshes = false;  // also put the splines in the .glb as line meshes
    int autosplines = 2;         // 0 never, 1 when the map has no splines of its own, 2 always
    bool autospline_all = false; // every collidable object, not just grindable-looking ones
    bool lights = true;
    bool textures = true;
    DecalMode decals = DecalMode::Project;
    int max_texture_size = 0;
    int terrain_resolution = 1024;  // most grid squares per terrain side
    bool trees = true;              // place terrain-painted trees
};

struct Stats {
    size_t game_objects = 0, skipped_inactive = 0, skipped_lods = 0, renderers = 0, batched = 0, triangles = 0;
    size_t colliders_handed = 0, colliders_unmatched = 0;
    size_t colliders = 0, colliders_on_render = 0, colliders_exported = 0, decals = 0, empty_decals = 0,
           decal_triangles = 0, splines = 0, lights = 0, flattened = 0, baked_skew = 0;
    size_t autospline_objects = 0, autosplines = 0, trees = 0;
    size_t builtin_meshes = 0, missing_meshes = 0, empty_meshes = 0, terrains = 0, projected = 0;
};

Scene build_scene(const Database& db, const Options& opt, Stats& stats);

// After the images are made: materials whose shader hides its alpha test become
// alpha-tested when their base texture has a cut-out alpha.
void resolve_texture_alpha(Scene& scene);

} // namespace xl
