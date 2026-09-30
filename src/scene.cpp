#include "scene.h"
#include "grindlines.h"
#include "log.h"
#include "texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace xl {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSkipSubmesh = -2;  // submesh with no material: Unity does not draw it

float srgb_to_linear(double c) {
    c = std::max(0.0, c);
    return (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

bool icontains(const std::string& hay, const char* needle) {
    std::string h = lower(hay), n = lower(needle);
    return h.find(n) != std::string::npos;
}

// Approximate linear RGB of a black body (Tanner Helland's fit).
V3 blackbody(double kelvin) {
    double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0, r, g, b;
    if (t <= 66) {
        r = 255;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
    } else {
        r = 329.698727446 * std::pow(t - 60, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60, -0.0755148492);
        b = 255;
    }
    return {srgb_to_linear(std::clamp(r, 0.0, 255.0) / 255), srgb_to_linear(std::clamp(g, 0.0, 255.0) / 255),
            srgb_to_linear(std::clamp(b, 0.0, 255.0) / 255)};
}

Quat as_quat(const Value& v) {
    if (v.is_null()) return {};
    return normalize(Quat{v["x"].num(), v["y"].num(), v["z"].num(), v["w"].num(1)});
}
V3 as_v3(const Value& v, V3 fallback = {}) {
    if (v.is_null()) return fallback;
    return {v["x"].num(), v["y"].num(), v["z"].num()};
}

void mesh_bounds(const MeshData& m, V3& lo, V3& hi) {
    lo = {1e300, 1e300, 1e300};
    hi = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i < m.vertex_count; ++i) {
        V3 p{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]};
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
}

void world_bounds(const M4& w, const V3& lo, const V3& hi, V3& wlo, V3& whi) {
    wlo = {1e300, 1e300, 1e300};
    whi = {-1e300, -1e300, -1e300};
    for (int c = 0; c < 8; ++c) {
        V3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
        V3 q = xform_point(w, p);
        wlo = vmin(wlo, q);
        whi = vmax(whi, q);
    }
}

// Applies an affine transform to a mesh copy (positions and normals).
std::shared_ptr<MeshData> transformed(const MeshData& src, const M4& m, bool triangles = true) {
    auto out = std::make_shared<MeshData>(src);
    M4 nm = normal_matrix(m);
    for (size_t i = 0; i < out->vertex_count; ++i) {
        V3 p = xform_point(m, {out->pos[i * 3], out->pos[i * 3 + 1], out->pos[i * 3 + 2]});
        out->pos[i * 3] = (float)p.x;
        out->pos[i * 3 + 1] = (float)p.y;
        out->pos[i * 3 + 2] = (float)p.z;
        if (!out->nrm.empty()) {
            V3 n = normalize(xform_dir(nm, {out->nrm[i * 3], out->nrm[i * 3 + 1], out->nrm[i * 3 + 2]}));
            out->nrm[i * 3] = (float)n.x;
            out->nrm[i * 3 + 1] = (float)n.y;
            out->nrm[i * 3 + 2] = (float)n.z;
        }
    }
    if (det3(m) < 0 && triangles)
        for (auto& s : out->subs)
            for (size_t t = 0; t + 2 < s.idx.size(); t += 3) std::swap(s.idx[t + 1], s.idx[t + 2]);
    return out;
}

// Submeshes [first, first+count) of a mesh with only the vertices they use.
std::shared_ptr<MeshData> subset(const MeshData& src, size_t first, size_t count) {
    auto out = std::make_shared<MeshData>();
    out->name = src.name;
    std::unordered_map<uint32_t, uint32_t> remap;
    auto take = [&](uint32_t v) -> uint32_t {
        auto it = remap.find(v);
        if (it != remap.end()) return it->second;
        uint32_t n = (uint32_t)out->vertex_count++;
        remap[v] = n;
        out->pos.insert(out->pos.end(), &src.pos[v * 3], &src.pos[v * 3] + 3);
        if (!src.nrm.empty()) out->nrm.insert(out->nrm.end(), &src.nrm[v * 3], &src.nrm[v * 3] + 3);
        if (!src.col.empty()) out->col.insert(out->col.end(), &src.col[v * 4], &src.col[v * 4] + 4);
        for (int k = 0; k < 4; ++k)
            if (!src.uv[k].empty()) out->uv[k].insert(out->uv[k].end(), &src.uv[k][v * 2], &src.uv[k][v * 2] + 2);
        return n;
    };
    for (size_t s = first; s < first + count && s < src.subs.size(); ++s) {
        MeshData::Sub sub;
        for (uint32_t v : src.subs[s].idx) sub.idx.push_back(take(v));
        out->subs.push_back(std::move(sub));
    }
    return out;
}

std::shared_ptr<MeshData> capsule_mesh(double radius, double height, int direction) {
    double half = std::max(0.0, height * 0.5 - radius);
    // Unit capsule built along Y, then scaled/rotated onto the collider's axis.
    MeshData m;
    const int segs = 16, rings = 8;
    std::vector<std::vector<uint32_t>> grid;
    auto ring = [&](double phi, double yoff) {
        std::vector<uint32_t> row;
        for (int s = 0; s <= segs; ++s) {
            double th = 2 * kPi * s / segs;
            V3 n{std::cos(th) * std::sin(phi), std::cos(phi), std::sin(th) * std::sin(phi)};
            V3 p{n.x * radius, n.y * radius + yoff, n.z * radius};
            if (direction == 0) { p = {p.y, -p.x, p.z}; n = {n.y, -n.x, n.z}; }
            else if (direction == 2) { p = {p.x, -p.z, p.y}; n = {n.x, -n.z, n.y}; }
            m.pos.insert(m.pos.end(), {(float)p.x, (float)p.y, (float)p.z});
            m.nrm.insert(m.nrm.end(), {(float)n.x, (float)n.y, (float)n.z});
            row.push_back((uint32_t)m.vertex_count++);
        }
        grid.push_back(row);
    };
    for (int i = 0; i <= rings / 2; ++i) ring(kPi * i / rings, half);
    for (int i = rings / 2; i <= rings; ++i) ring(kPi * i / rings, -half);
    MeshData::Sub sub;
    auto P = [&](uint32_t i) { return V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}; };
    auto N = [&](uint32_t i) { return V3{m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]}; };
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        V3 f = cross(P(b) - P(a), P(c) - P(a));
        if (length(f) < 1e-12) return;
        if (dot(f, N(a) + N(b) + N(c)) < 0) std::swap(b, c);
        sub.idx.insert(sub.idx.end(), {a, b, c});
    };
    for (size_t i = 0; i + 1 < grid.size(); ++i)
        for (int s = 0; s < segs; ++s) {
            tri(grid[i][s], grid[i][s + 1], grid[i + 1][s + 1]);
            tri(grid[i][s], grid[i + 1][s + 1], grid[i + 1][s]);
        }
    m.subs.push_back(std::move(sub));
    m.name = "Capsule";
    return std::make_shared<MeshData>(std::move(m));
}

struct TexSlot {
    ObjRef tex;
    Vec2 scale{1, 1}, offset{0, 0};
};

// Gives each triangle of `tris` to the layer with the largest weight (`vw`: `L` weights per
// vertex, linear across a triangle). Where the strongest layer changes inside a triangle it is
// cut along the lines where one layer's weight overtakes another's, so layer borders run
// smoothly instead of stepping along the triangles. Cut vertices are appended to `m` with all
// attributes interpolated, and shared between triangles through `cuts`.
using CutKey = std::vector<std::pair<uint32_t, int64_t>>;
void split_by_weights(MeshData& m, const std::vector<uint32_t>& tris, const std::vector<float>& vw, size_t L,
                      std::vector<MeshData::Sub>& parts, std::map<CutKey, uint32_t>& cuts) {
    using Bary = std::array<double, 3>;
    parts.resize(L);
    auto vertex_for = [&](const std::array<uint32_t, 3>& tri, const Bary& b) -> uint32_t {
        CutKey key;
        for (int k = 0; k < 3; ++k)
            if (b[k] > 1e-7) key.push_back({tri[k], std::llround(b[k] * 1e6)});
        if (key.size() == 1) return key[0].first;
        std::sort(key.begin(), key.end());
        auto it = cuts.find(key);
        if (it != cuts.end()) return it->second;
        auto blend = [&](std::vector<float>& a, int n) {
            if (a.empty()) return;
            float out[4] = {0, 0, 0, 0};
            for (int k = 0; k < 3; ++k)
                for (int c = 0; c < n; ++c) out[c] += (float)b[k] * a[(size_t)tri[k] * n + c];
            a.insert(a.end(), out, out + n);
        };
        blend(m.pos, 3);
        blend(m.nrm, 3);
        if (!m.nrm.empty()) {
            float* nn = &m.nrm[m.nrm.size() - 3];
            V3 v = normalize(V3{nn[0], nn[1], nn[2]});
            nn[0] = (float)v.x;
            nn[1] = (float)v.y;
            nn[2] = (float)v.z;
        }
        blend(m.col, 4);
        for (auto& uv : m.uv) blend(uv, 2);
        uint32_t id = (uint32_t)m.vertex_count++;
        cuts[key] = id;
        return id;
    };
    auto P = [&](uint32_t i) { return V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}; };
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        std::array<uint32_t, 3> tri{tris[t], tris[t + 1], tris[t + 2]};
        size_t top[3];
        for (int k = 0; k < 3; ++k) {
            const float* w = &vw[(size_t)tri[k] * L];
            top[k] = (size_t)(std::max_element(w, w + L) - w);
        }
        if (top[0] == top[1] && top[1] == top[2]) {
            parts[top[0]].idx.insert(parts[top[0]].idx.end(), tri.begin(), tri.end());
            continue;
        }
        std::vector<size_t> cand;
        for (size_t l = 0; l < L; ++l) {
            bool used = false;
            for (int k = 0; k < 3; ++k) used = used || top[k] == l || vw[(size_t)tri[k] * L + l] > 0.05f;
            if (used) cand.push_back(l);
        }
        for (size_t l : cand) {
            std::vector<Bary> poly = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, next;
            for (size_t o : cand) {
                if (o == l || poly.size() < 3) continue;
                double g[3];
                for (int k = 0; k < 3; ++k) g[k] = vw[(size_t)tri[k] * L + l] - vw[(size_t)tri[k] * L + o];
                auto val = [&](const Bary& p) { return p[0] * g[0] + p[1] * g[1] + p[2] * g[2]; };
                next.clear();
                for (size_t i = 0; i < poly.size(); ++i) {
                    const Bary& cur = poly[i];
                    const Bary& prv = poly[(i + poly.size() - 1) % poly.size()];
                    double vc = val(cur), vp = val(prv);
                    if ((vc >= 0) != (vp >= 0)) {
                        double s = vp / (vp - vc);
                        next.push_back({prv[0] + (cur[0] - prv[0]) * s, prv[1] + (cur[1] - prv[1]) * s,
                                        prv[2] + (cur[2] - prv[2]) * s});
                    }
                    if (vc >= 0) next.push_back(cur);
                }
                poly.swap(next);
            }
            if (poly.size() < 3) continue;
            std::vector<uint32_t> ids;
            for (auto& p : poly) ids.push_back(vertex_for(tri, p));
            for (size_t i = 1; i + 1 < ids.size(); ++i) {
                uint32_t a = ids[0], b = ids[i], c = ids[i + 1];
                if (a == b || b == c || a == c) continue;
                if (length(cross(P(b) - P(a), P(c) - P(a))) < 1e-9) continue;
                parts[l].idx.insert(parts[l].idx.end(), {a, b, c});
            }
        }
    }
}

bool has_any(const std::string& s, std::initializer_list<const char*> words) {
    for (auto w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

bool ends_with_any(const std::string& s, std::initializer_list<const char*> suffixes) {
    for (auto w : suffixes) {
        size_t n = std::strlen(w);
        if (s.size() >= n && s.compare(s.size() - n, n, w) == 0) return true;
    }
    return false;
}

std::string trimmed_lower(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : lower(s.substr(a, b - a + 1));
}

// How much a (slot name, texture name) pair looks like a colour map; <= 0 = not one.
int base_texture_score(const std::string& slot, const std::string& texture) {
    std::string s = trimmed_lower(slot), t = trimmed_lower(texture);
    for (const std::string* x : {&s, &t})
        if (has_any(*x, {"normal", "nrm", "_norm", "bump", "mask", "metal", "rough", "smooth", "gloss", "occlu", "ambient",
                         "height", "disp", "spec", "emiss", "shadow", "overlay", "detail", "noise", "flow", "opacity",
                         "cubemap", "reflect", "ramp", "lut"}))
            return 0;
    if (ends_with_any(t, {"_n", "_nm", "_r", "_s", "_m", "_h", "_ao", "_mt", "_e"})) return 0;
    int score = 1;  // anything left could be a colour map
    for (const std::string* x : {&s, &t})
        if (has_any(*x, {"albedo", "basecolor", "base_color", "basecolour", "base_colour", "diffuse", "maintex", "color",
                         "colour", "_alb", "_diff"}))
            score += 10;
    if (ends_with_any(t, {"_d", "_c", "_col", "_a"})) score += 5;
    return score;
}

int normal_texture_score(const std::string& slot, const std::string& texture) {
    std::string s = trimmed_lower(slot), t = trimmed_lower(texture);
    if (has_any(s, {"detail", "bent", "coat", "flow"})) return 0;
    if (has_any(s, {"normal", "bump", "nrm"}) || has_any(t, {"normal", "nrm", "_norm", "bump"}) ||
        ends_with_any(t, {"_n", "_nm", "_nor"}))
        return 1;
    return 0;
}

struct Xf {
    ObjRef go, father;
    std::vector<ObjRef> children;
    V3 t, s{1, 1, 1};
    Quat r;
    M4 world;
    int state = 0;  // 0 not computed, 1 computing, 2 done
};

struct Go {
    std::string name;
    int layer = 0, tag = 0;
    bool active = true;
    ObjRef transform;
    std::vector<ObjRef> comps;
};

class Builder {
public:
    Builder(const Database& db, const Options& opt, Stats& st) : db_(db), opt_(opt), st_(st) {}
    Scene run();

private:
    const Database& db_;
    const Options& opt_;
    Stats& st_;
    Scene sc_;

    std::unordered_map<ObjRef, Xf, ObjRefHash> xfs_;
    std::unordered_map<ObjRef, Go, ObjRefHash> gos_;
    std::unordered_set<ObjRef, ObjRefHash> lod_skip_;
    std::unordered_map<ObjRef, std::shared_ptr<MeshData>, ObjRefHash> meshes_;
    std::unordered_map<ObjRef, int, ObjRefHash> materials_;
    std::unordered_map<ObjRef, std::string, ObjRefHash> shader_names_, script_names_;
    std::map<std::string, int> image_keys_, mesh_keys_;
    std::vector<bool> receives_decals_;  // per material
    // HDRP planar (4) / triplanar (5) mapping: textures follow world (or object) position, not UVs.
    struct Projection {
        int mode = 0;
        double scale = 1;
        double scale_v = 0;  // 0: same as scale
        double offset_u = 0, offset_v = 0;
        bool object_space = false;
    };
    std::unordered_map<ObjRef, int, ObjRefHash> terrain_layers_;
    int terrain_layer_material(ObjRef ref);
    void add_terrain(int node, const Go& go, ObjRef terrain, bool collides, bool tree_colliders, bool active);
    std::vector<Projection> projections_;  // per material (missing = none)
    // Vertex-colour layer blend graphs: material -> (vertex colour channel, layer material);
    // channel -1 is the base layer.
    std::unordered_map<int, std::vector<std::pair<int, int>>> layers_;
    bool split_layers(std::shared_ptr<MeshData>& data, std::vector<int>& mats);
    std::shared_ptr<MeshData> bake_projection(const MeshData& src, const std::vector<int>& mats, const M4& w,
                                              bool& world_dependent);
    int default_material_ = -1, collision_material_ = -1;

    struct Instance {
        std::shared_ptr<MeshData> mesh;
        std::vector<int> materials;
        M4 world;
        V3 lo, hi;
        int node = -1;  // render node (renderers only)
        std::string name;
        int layer = 0;
    };
    std::vector<Instance> instances_;
    void autosplines();
    struct RenderRef {
        int node;
        ObjRef xf;
        V3 lo, hi;
    };
    std::vector<RenderRef> render_refs_;  // active, exported render meshes (not all-blended)
    struct PendingCollider {
        ObjRef xf;
        std::string name;
        V3 lo, hi;
        bool own_subtree_only = false;  // collider on a visible object: only its children
    };
    std::vector<PendingCollider> pending_colliders_;
    bool is_within(ObjRef xf, ObjRef ancestor);
    void resolve_pending_colliders();
    struct DecalJob {
        int node;
        M4 world;
        Value proj;
        int file;
        std::string name;
        bool active;
    };
    std::vector<DecalJob> decals_;
    std::map<int, int> layer_counts_;

    void load_hierarchy();
    const M4& world(const ObjRef& xf);
    int visit(const ObjRef& xf, bool parent_active, int depth, const M4* parent_world = nullptr,
              const OutNode* root = nullptr);
    bool suppress_colliders_ = false;
    void components(int node, const Go& go, const M4& w, bool active);
    void add_renderer(int node, const Go& go, const M4& w, ObjRef renderer, ObjRef filter, bool skinned,
                      bool active, ObjRef& mesh_ref, std::shared_ptr<MeshData>& used);
    void add_collider(int node, const Go& go, const M4& w, ObjRef c, int cls, ObjRef render_mesh,
                      const std::shared_ptr<MeshData>& render_data);
    void add_light(int node, const Go& go, ObjRef c);
    void add_spline(int node, const Go& go, const Value& mv, const M4& w);
    void project_decals();
    void flatten();

    std::shared_ptr<MeshData> mesh(ObjRef ref);
    int material(ObjRef ref);
    int default_material();
    int collision_material();
    int add_mesh(const std::string& key, OutMesh m);
    int image(ImageJob job);
    std::string shader_name(ObjRef ref);
    std::string script_name(ObjRef ref);
    std::string texture_name(ObjRef ref);
    std::unordered_map<ObjRef, std::string, ObjRefHash> texture_names_;
    int add_node(OutNode n) {
        sc_.nodes.push_back(std::move(n));
        return (int)sc_.nodes.size() - 1;
    }
};

std::shared_ptr<MeshData> Builder::mesh(ObjRef ref) {
    if (!ref.valid()) return nullptr;
    auto it = meshes_.find(ref);
    if (it != meshes_.end()) return it->second;
    std::shared_ptr<MeshData> m;
    try {
        m = decode_mesh(db_, ref);
        if (m) ensure_normals(*m);
        if (m && ref.builtin()) ++st_.builtin_meshes;
    } catch (const std::exception& e) {
        log_warn("mesh %lld: %s", (long long)ref.id, e.what());
    }
    if (!m) {
        ++st_.missing_meshes;
    } else if (m->vertex_count == 0) {
        ++st_.empty_meshes;
        log_verbose("mesh '%s' is empty in the map itself", m->name.c_str());
        m = nullptr;
    }
    meshes_[ref] = m;
    return m;
}

std::string Builder::shader_name(ObjRef ref) {
    if (!ref.valid()) return "";
    auto it = shader_names_.find(ref);
    if (it != shader_names_.end()) return it->second;
    std::string name;
    if (ref.builtin()) {
        name = "builtin";
    } else {
        try {
            Value s = db_.read(ref);
            name = s["m_ParsedForm"]["m_Name"].s();
            if (name.empty()) name = s["m_Name"].s();
        } catch (const std::exception&) {
        }
    }
    shader_names_[ref] = name;
    return name;
}

std::string Builder::texture_name(ObjRef ref) {
    if (!ref.valid() || ref.builtin()) return "";
    auto it = texture_names_.find(ref);
    if (it != texture_names_.end()) return it->second;
    std::string name;
    try {
        name = db_.read(ref)["m_Name"].s();
    } catch (const std::exception&) {
    }
    texture_names_[ref] = name;
    return name;
}

std::string Builder::script_name(ObjRef ref) {
    if (!ref.valid()) return "";
    auto it = script_names_.find(ref);
    if (it != script_names_.end()) return it->second;
    std::string name;
    try {
        name = db_.read(ref)["m_ClassName"].s();
    } catch (const std::exception&) {
    }
    script_names_[ref] = name;
    return name;
}

int Builder::image(ImageJob job) {
    std::string key = std::to_string(job.role);
    for (auto& s : job.src) key += "|" + std::to_string(s.file) + ":" + std::to_string(s.id);
    for (float p : job.params) key += "|" + std::to_string(p);
    auto it = image_keys_.find(key);
    if (it != image_keys_.end()) return it->second;
    std::string base;
    for (auto& s : job.src)
        if (base.empty()) base = texture_name(s);
    if (base.empty()) base = "texture";
    static const char* suffix[] = {"", "_normal", "_orm", "_orm", "_emissive", "", "_orm"};
    job.name = base + suffix[job.role];
    sc_.images.push_back(std::move(job));
    int index = (int)sc_.images.size() - 1;
    image_keys_[key] = index;
    return index;
}

int Builder::default_material() {
    if (default_material_ < 0) {
        OutMaterial m;
        m.name = "XL_Default";
        m.base[0] = m.base[1] = m.base[2] = 0.8f;
        m.roughness = 0.8f;
        sc_.materials.push_back(m);
        receives_decals_.push_back(true);
        default_material_ = (int)sc_.materials.size() - 1;
    }
    return default_material_;
}

int Builder::collision_material() {
    if (collision_material_ < 0) {
        OutMaterial m;
        m.name = "XL_Collision";
        m.base[0] = 1.0f;
        m.base[1] = 0.25f;
        m.base[2] = 0.75f;
        m.base[3] = 0.35f;
        m.alpha_mode = 2;
        m.double_sided = true;
        m.roughness = 1;
        m.extras.set("xl_collision_material", true);
        sc_.materials.push_back(m);
        receives_decals_.push_back(false);
        collision_material_ = (int)sc_.materials.size() - 1;
    }
    return collision_material_;
}

int Builder::add_mesh(const std::string& key, OutMesh m) {
    if (!key.empty()) {
        auto it = mesh_keys_.find(key);
        if (it != mesh_keys_.end()) return it->second;
    }
    sc_.meshes.push_back(std::move(m));
    int index = (int)sc_.meshes.size() - 1;
    if (!key.empty()) mesh_keys_[key] = index;
    return index;
}

int Builder::material(ObjRef ref) {
    if (!ref.valid() || ref.builtin()) return default_material();
    auto it = materials_.find(ref);
    if (it != materials_.end()) return it->second;

    Value m;
    try {
        m = db_.read(ref);
    } catch (const std::exception& e) {
        log_warn("material %lld: %s", (long long)ref.id, e.what());
    }
    if (m.is_null()) {
        materials_[ref] = default_material();
        return materials_[ref];
    }

    OutMaterial om;
    om.name = m["m_Name"].s();
    om.shader = shader_name(db_.resolve(ref.file, m["m_Shader"]));
    std::map<std::string, TexSlot> tex;
    std::map<std::string, double> fl;
    std::map<std::string, Color> col;
    const Value& props = m["m_SavedProperties"];
    for (auto& p : props["m_TexEnvs"].items) {
        TexSlot s;
        s.tex = db_.resolve(ref.file, p["second"]["m_Texture"]);
        s.scale = as_vec2(p["second"]["m_Scale"]);
        s.offset = as_vec2(p["second"]["m_Offset"]);
        tex[p["first"].s()] = s;
    }
    for (auto& p : props["m_Floats"].items) fl[p["first"].s()] = p["second"].num();
    for (auto& p : props["m_Ints"].items) fl[p["first"].s()] = p["second"].num();
    for (auto& p : props["m_Colors"].items) col[p["first"].s()] = as_color(p["second"]);
    std::string keywords = m["m_ShaderKeywords"].s();
    for (auto& k : m["m_ValidKeywords"].items) keywords += " " + k.s();

    const std::string& sh = om.shader;
    const bool graph_shader = sh.rfind("Shader Graphs/", 0) == 0;
    bool hdrp = sh.rfind("HDRP/", 0) == 0;
    bool decal = icontains(sh, "decal");
    bool layered = icontains(sh, "LayeredLit");
    std::string sfx = layered ? "0" : "";

    auto find_tex = [&](std::initializer_list<std::string> names) -> const TexSlot* {
        for (auto& n : names) {
            auto t = tex.find(n);
            if (t != tex.end() && t->second.tex.valid() && !t->second.tex.builtin()) return &t->second;
        }
        return nullptr;
    };
    // Custom Shader Graphs expose slots under generated names ("Texture2D_85643C7C"), so
    // fall back to what the slot and its texture are called.
    auto pick_by_name = [&](int (*scorer)(const std::string&, const std::string&)) -> const TexSlot* {
        const TexSlot* best = nullptr;
        int best_score = 0;
        for (auto& [name, slot] : tex) {
            if (!slot.tex.valid() || slot.tex.builtin()) continue;
            int s = scorer(name, texture_name(slot.tex));
            if (s > best_score) {
                best = &slot;
                best_score = s;
            }
        }
        return best;
    };
    auto f = [&](std::initializer_list<std::string> names, double def) {
        for (auto& n : names) {
            auto it2 = fl.find(n);
            if (it2 != fl.end()) return it2->second;
        }
        return def;
    };
    auto has_f = [&](const std::string& n) { return fl.count(n) > 0; };
    auto c = [&](std::initializer_list<std::string> names) -> const Color* {
        for (auto& n : names) {
            auto it2 = col.find(n);
            if (it2 != col.end()) return &it2->second;
        }
        return nullptr;
    };

    // Base colour.
    const TexSlot* base = find_tex({"_BaseColorMap" + sfx, "_BaseColorMap", "_BaseMap", "_MainTex", "_UnlitColorMap",
                                    "_Albedo", "_AlbedoMap", "_Diffuse", "_DiffuseMap", "_MainTexture", "_BaseTexture"});
    bool base_by_name = false;
    if (!base) {
        base = pick_by_name(base_texture_score);
        base_by_name = base != nullptr;
    }
    // A graph with generated slot names may carry stale _BaseColor/_Color values from an
    // earlier shader; its real tint lives in an unnamed property, so leave the texture untinted.
    const Color* bc = base_by_name ? nullptr
                                   : c({"_BaseColor" + sfx, "_BaseColor", "_Color", "_UnlitColor", "_MainColor", "_Tint",
                                        "_TintColor"});
    if (!base && graph_shader) {
        // An untextured graph's colour is in its own properties (_shallowater, _albedo_color...);
        // _BaseColor/_Color are usually stale defaults from the material's earlier shader.
        static const char* kPrefer[][3] = {{"base", "albedo", "main"}, {"shallow", "water", "surface"},
                                           {"color", "colour", "tint"}};
        static const std::set<std::string> kStale = {"_BaseColor", "_Color", "_EmissionColor", "_SpecColor",
                                                     "_EmissiveColor", "_UnlitColor"};
        const Color* pick = nullptr;
        for (auto& group : kPrefer) {
            for (auto& [name, value] : col) {
                if (kStale.count(name) || icontains(name, "emiss") || icontains(name, "spec")) continue;
                bool match = false;
                for (const char* w : group) match = match || icontains(name, w);
                if (match && (value.r + value.g + value.b) > 1e-3) {
                    pick = &value;
                    break;
                }
            }
            if (pick) break;
        }
        if (pick) bc = pick;
    }
    if (bc) {
        om.base[0] = srgb_to_linear(bc->r);
        om.base[1] = srgb_to_linear(bc->g);
        om.base[2] = srgb_to_linear(bc->b);
        om.base[3] = (float)std::clamp(bc->a, 0.0, 1.0);
    }
    int uv = 0;
    Projection proj;
    if (hdrp && !decal) {
        int uvbase = (int)f({"_UVBase" + sfx, "_UVBase"}, 0);
        bool tri = keywords.find("_MAPPING_TRIPLANAR") != std::string::npos;
        bool planar = keywords.find("_MAPPING_PLANAR") != std::string::npos;
        if (uvbase == 5 || (tri && uvbase < 4)) proj.mode = 5;
        else if (uvbase == 4 || planar) proj.mode = 4;
        else if (uvbase >= 0 && uvbase <= 3) uv = uvbase;
        if (proj.mode) {
            proj.scale = f({"_TexWorldScale" + sfx, "_TexWorldScale"}, 1.0);
            proj.object_space = f({"_ObjectSpaceUVMapping" + sfx, "_ObjectSpaceUVMapping"}, 0) > 0.5;
            om.extras.set("xl_uv_mapping", proj.mode == 4 ? "planar" : "triplanar");
            om.extras.set("xl_tex_world_scale", proj.scale);
        }
    }
    // Normal map.
    const TexSlot* nrm = find_tex({"_NormalMap" + sfx, "_NormalMap", "_BumpMap", "_NormalTex", "_Normal", "_NormalTexture"});
    if (!nrm) nrm = pick_by_name(normal_texture_score);
    if (nrm) {
        ImageJob j;
        j.role = ImageJob::Normal;
        j.src[0] = nrm->tex;
        om.normal_tex = {image(std::move(j)), uv};
        om.normal_scale = (float)f({"_NormalScale" + sfx, "_NormalScale", "_BumpScale"}, 1.0);
    }

    // Metal / roughness / occlusion.
    om.metallic = (float)f({"_Metallic" + sfx, "_Metallic"}, 0.0);
    double smooth = f({"_Smoothness" + sfx, "_Smoothness", "_Glossiness"}, 0.5);
    om.roughness = (float)std::clamp(1.0 - smooth, 0.0, 1.0);
    const TexSlot* mask = find_tex({"_MaskMap" + sfx, "_MaskMap"});
    if (mask) {
        ImageJob j;
        j.role = ImageJob::MaskHDRP;
        j.src[0] = mask->tex;
        j.params[0] = (float)f({"_SmoothnessRemapMin" + sfx, "_SmoothnessRemapMin"}, 0.0);
        j.params[1] = (float)f({"_SmoothnessRemapMax" + sfx, "_SmoothnessRemapMax"}, 1.0);
        j.params[2] = (float)f({"_AORemapMin" + sfx, "_AORemapMin"}, 0.0);
        j.params[3] = (float)f({"_AORemapMax" + sfx, "_AORemapMax"}, 1.0);
        if (has_f("_MetallicRemapMax" + sfx) || has_f("_MetallicRemapMax")) {
            j.params[4] = (float)f({"_MetallicRemapMin" + sfx, "_MetallicRemapMin"}, 0.0);
            j.params[5] = (float)f({"_MetallicRemapMax" + sfx, "_MetallicRemapMax"}, 1.0);
        } else {  // HDRP 7 and older: the mask's red channel is the metallic (the slider is hidden)
            j.params[4] = 0;
            j.params[5] = 1;
        }
        om.orm_tex = {image(std::move(j)), uv};
        om.orm_has_occlusion = true;
        om.metallic = 1;
        om.roughness = 1;
    } else {
        const TexSlot* metal = find_tex({"_MetallicGlossMap"});
        const TexSlot* occ = find_tex({"_OcclusionMap"});
        if (metal || occ) {
            ImageJob j;
            j.role = ImageJob::MetallicStd;
            if (metal) j.src[0] = metal->tex;
            if (occ) j.src[1] = occ->tex;
            bool albedo_alpha = f({"_SmoothnessTextureChannel"}, 0) > 0.5;
            if (albedo_alpha && base) j.src[2] = base->tex;
            j.params[0] = om.metallic;
            j.params[1] = (float)(metal ? f({"_GlossMapScale"}, 1.0) : smooth);
            j.params[2] = (float)f({"_OcclusionStrength"}, 1.0);
            j.params[3] = metal ? 1.0f : 0.0f;
            j.params[4] = albedo_alpha ? 1.0f : 0.0f;
            om.orm_tex = {image(std::move(j)), uv};
            om.orm_has_occlusion = occ != nullptr;
            om.metallic = 1;
            om.roughness = 1;
        } else {
            // Graphs with one greyscale map per channel (e.g. "Custom_Shader_Metallic").
            const TexSlot* metal2 = find_tex({"_MetallicMap", "_MetalnessMap", "_MetalMap", "_Metalness"});
            const TexSlot* rough = find_tex({"_RoughnessMap", "_RoughMap", "_Roughness"});
            const TexSlot* gloss = rough ? nullptr : find_tex({"_SmoothnessMap", "_GlossinessMap", "_GlossMap"});
            const TexSlot* ao = find_tex({"_AmbientOcclusionMap", "_AOMap", "_AO", "_OcclusionTexture"});
            if (metal2 || rough || gloss || ao) {
                ImageJob j;
                j.role = ImageJob::PackORM;
                if (ao) j.src[0] = ao->tex;
                if (rough || gloss) j.src[1] = (rough ? rough : gloss)->tex;
                if (metal2) j.src[2] = metal2->tex;
                j.params[0] = gloss ? 1.0f : 0.0f;
                j.params[1] = om.roughness;
                j.params[2] = metal2 ? 1.0f : om.metallic;
                om.orm_tex = {image(std::move(j)), uv};
                om.orm_has_occlusion = ao != nullptr;
                om.metallic = 1;
                om.roughness = 1;
            }
        }
    }

    // Emission.
    const TexSlot* emap = find_tex({"_EmissiveColorMap", "_EmissionMap"});
    const Color* ec = nullptr;
    bool emission_linear = false;
    bool graph = sh.rfind("Shader Graphs/", 0) == 0;
    static const Color white;
    if (hdrp) {
        ec = c({"_EmissiveColor"});
        emission_linear = true;
    } else if (graph) {
        // Graphs often keep a stale white _EmissionColor from an earlier shader; only an
        // emissive map means the graph actually emits.
        if (emap) ec = &white;
    } else if (keywords.find("_EMISSION") != std::string::npos) {
        ec = c({"_EmissionColor"});
    }
    if (ec) {
        float e[3] = {(float)ec->r, (float)ec->g, (float)ec->b};
        if (!emission_linear)
            for (auto& v : e) v = srgb_to_linear(v);
        float peak = std::max({e[0], e[1], e[2]});
        if (peak > 1e-4f) {
            float scale = peak > 1 ? 1 / peak : 1;
            for (int k = 0; k < 3; ++k) om.emissive[k] = e[k] * scale;
            om.emissive_strength = std::min(peak > 1 ? peak : 1.0f, 10.0f);
            if (emap) {
                // Colour baked into the texture so Blender links it straight to Emission Color.
                ImageJob j;
                j.role = ImageJob::Emissive;
                j.src[0] = emap->tex;
                if (om.emissive[0] != om.emissive[1] || om.emissive[1] != om.emissive[2]) {
                    for (int k = 0; k < 3; ++k) {
                        j.params[k] = om.emissive[k];
                        om.emissive[k] = 1;
                    }
                    j.params[3] = 1;
                    j.params[4] = 1;
                }
                om.emissive_tex = {image(std::move(j)), uv};
            }
        }
    }

    // Transparency and culling.
    bool kw_alphatest = keywords.find("_ALPHATEST_ON") != std::string::npos;
    bool kw_transparent = keywords.find("_SURFACE_TYPE_TRANSPARENT") != std::string::npos;
    bool kw_double = keywords.find("_DOUBLESIDED_ON") != std::string::npos;
    if (decal) {
        om.alpha_mode = 2;
        om.base[3] *= (float)f({"_DecalBlend"}, 1.0);
        om.extras.set("xl_decal", true);
    } else if (hdrp || graph) {
        // HDRP shader graphs share HDRP/Lit's surface options (and may carry a stale _Mode
        // from the Standard shader, which must not be read).
        if (kw_transparent || f({"_SurfaceType"}, 0) > 0.5) om.alpha_mode = 2;
        else if (kw_alphatest || f({"_AlphaCutoffEnable"}, 0) > 0.5) om.alpha_mode = 1;
        else if (graph) om.auto_alpha = true;  // alpha clip can live in the graph itself
        om.alpha_cutoff = (float)f({"_AlphaCutoff"}, 0.5);
    } else if (has_f("_Surface")) {  // URP
        if (f({"_Surface"}, 0) > 0.5) om.alpha_mode = 2;
        else if (f({"_AlphaClip"}, 0) > 0.5) om.alpha_mode = 1;
        om.alpha_cutoff = (float)f({"_Cutoff"}, 0.5);
    } else if (has_f("_Mode")) {  // Standard
        int mode = (int)f({"_Mode"}, 0);
        if (mode == 1) om.alpha_mode = 1;
        else if (mode >= 2) om.alpha_mode = 2;
        om.alpha_cutoff = (float)f({"_Cutoff"}, 0.5);
    } else {
        std::string render_type;
        for (auto& p : m["stringTagMap"].items)
            if (p["first"].s() == "RenderType") render_type = p["second"].s();
        int64_t queue = m["m_CustomRenderQueue"].i64(-1);
        if (render_type == "TransparentCutout" || icontains(sh, "cutout") || (queue >= 2450 && queue < 3000))
            om.alpha_mode = 1;
        else if (render_type == "Transparent" || icontains(sh, "transparent") || icontains(sh, "particles") || queue >= 3000)
            om.alpha_mode = 2;
        else
            om.auto_alpha = true;
        om.alpha_cutoff = (float)f({"_Cutoff", "_AlphaCutoff"}, 0.5);
    }
    bool foliage = false;
    for (const std::string* s : std::initializer_list<const std::string*>{&om.name, &sh})
        for (const char* w : {"leaf", "leaves", "foliage", "grass", "branch", "bush", "plant", "ivy", "fern", "flower",
                              "hedge", "shrub", "vine", "weed"})
            foliage = foliage || icontains(*s, w);
    om.double_sided = f({"_DoubleSidedEnable"}, 0) > 0.5 || kw_double || f({"_CullMode", "_Cull"}, 2) < 0.5 ||
                      om.alpha_mode == 2 || foliage;
    om.unlit = icontains(sh, "unlit") || icontains(sh, "particles");

    // The colour factor is baked into the texture, so Blender wires the image straight
    // into the BSDF instead of through a multiply node.
    if (base) {
        ImageJob j;
        j.role = ImageJob::Color;
        j.src[0] = base->tex;
        bool tinted = false;
        for (float v : om.base) tinted = tinted || std::fabs(v - 1.0f) > 1e-3f;
        if (tinted) {
            for (int k = 0; k < 4; ++k) {
                j.params[k] = om.base[k];
                om.base[k] = 1;
            }
            j.params[4] = 1;
        }
        om.base_tex = {image(std::move(j)), uv};
    }

    const TexSlot* xform = base ? base : nrm ? nrm : mask;
    if (xform && (xform->scale.x != 1 || xform->scale.y != 1 || xform->offset.x != 0 || xform->offset.y != 0)) {
        om.uv_transform = true;
        om.uv_scale[0] = (float)xform->scale.x;
        om.uv_scale[1] = (float)xform->scale.y;
        om.uv_offset[0] = (float)xform->offset.x;
        om.uv_offset[1] = (float)xform->offset.y;
    }
    if (!sh.empty()) om.extras.set("xl_shader", sh);

    // Graph tiling properties: world-projected when the graph says so in its name; otherwise
    // decided per mesh (bake_projection mode 6).
    auto slot_name = [&](const TexSlot* s) -> std::string {
        for (auto& [name, slot] : tex)
            if (&slot == s) return name;
        return {};
    };
    auto graph_projection = [&](const std::string& slot) {
        Projection p;
        for (const std::string& n : {slot + "_TILING", slot + "_Tiling", slot + "Tiling", std::string("_TILING"),
                                     std::string("_Tiling"), std::string("Tiling")}) {
            auto it2 = fl.find(n);
            if (it2 == fl.end() || it2->second <= 0) continue;
            p.scale = it2->second;
            p.mode = icontains(sh, "triplanar") ? 5 : (icontains(sh, "planar") || icontains(sh, "world")) ? 4 : 6;
            break;
        }
        return p;
    };
    if (graph && !decal && base && !proj.mode) {
        proj = graph_projection(slot_name(base));
        if (proj.mode) {
            om.extras.set("xl_uv_mapping", proj.mode == 5 ? "graph_triplanar" : proj.mode == 4 ? "graph_planar" : "graph_tiling");
            om.extras.set("xl_tex_world_scale", proj.scale);
        }
    }

    bool receives = !decal && f({"_SupportDecals"}, 1.0) > 0.5;
    sc_.materials.push_back(om);
    receives_decals_.push_back(receives);
    int index = (int)sc_.materials.size() - 1;
    auto set_projection = [&](int i, const Projection& p) {
        if (!p.mode) return;
        projections_.resize(sc_.materials.size());
        projections_[i] = p;
    };
    set_projection(index, proj);

    // Vertex-colour layer blends (_TEXTURE_01.._0N with _NORMAL_0N and _TEXTURE_0N_TILING): one
    // plain material per layer. Layer n >= 2 is weighted by vertex colour channel n - 2 (R, G, B, A);
    // add_renderer gives each triangle to its strongest layer.
    if (graph && base && slot_name(base) == "_TEXTURE_01") {
        std::vector<std::pair<int, int>> layers = {{-1, index}};
        for (int n = 2; n <= 5; ++n) {
            char slot[16], nslot[16];
            std::snprintf(slot, sizeof slot, "_TEXTURE_%02d", n);
            std::snprintf(nslot, sizeof nslot, "_NORMAL_%02d", n);
            const TexSlot* lt = find_tex({slot});
            if (!lt) continue;
            OutMaterial lm = om;
            lm.name = om.name + " L" + std::to_string(n);
            lm.orm_tex = {};
            lm.orm_has_occlusion = false;
            lm.metallic = 0;
            lm.roughness = (float)std::clamp(1.0 - f({"_SMOOTHNESS", "_Smoothness"}, 0.5), 0.0, 1.0);
            ImageJob cj;
            cj.role = ImageJob::Color;
            cj.src[0] = lt->tex;
            lm.base_tex = {image(std::move(cj)), 0};
            lm.normal_tex = {};
            if (const TexSlot* ln = find_tex({nslot})) {
                ImageJob nj;
                nj.role = ImageJob::Normal;
                nj.src[0] = ln->tex;
                lm.normal_tex = {image(std::move(nj)), 0};
            }
            Projection lp = graph_projection(slot);
            lm.extras.set("xl_layer_of", om.name);
            if (lp.mode) lm.extras.set("xl_tex_world_scale", lp.scale);
            sc_.materials.push_back(std::move(lm));
            receives_decals_.push_back(receives);
            int li = (int)sc_.materials.size() - 1;
            set_projection(li, lp);
            layers.push_back({n - 2, li});
        }
        if (layers.size() > 1) {
            layers_[index] = std::move(layers);
            sc_.materials[index].extras.set("xl_layer_of", sc_.materials[index].name);
        }
    }
    materials_[ref] = index;
    return index;
}

void Builder::load_hierarchy() {
    for (int cls : {(int)kTransform, (int)kRectTransform})
        for (auto& ref : db_.objects_of(cls)) {
            Value v = db_.read(ref);
            Xf x;
            x.go = db_.resolve(ref.file, v["m_GameObject"]);
            x.father = db_.resolve(ref.file, v["m_Father"]);
            for (auto& c : v["m_Children"].items) {
                ObjRef cr = db_.resolve(ref.file, c);
                if (cr.valid()) x.children.push_back(cr);
            }
            x.t = as_v3(v["m_LocalPosition"]);
            x.r = as_quat(v["m_LocalRotation"]);
            x.s = as_v3(v["m_LocalScale"], {1, 1, 1});
            xfs_[ref] = std::move(x);
        }
    for (auto& ref : db_.objects_of(kGameObject)) {
        Value v = db_.read(ref);
        Go g;
        g.name = v["m_Name"].s();
        g.layer = (int)v["m_Layer"].i64();
        g.tag = (int)v["m_Tag"].i64();
        g.active = v["m_IsActive"].is_null() ? true : v["m_IsActive"].truthy();
        for (auto& c : v["m_Component"].items) {
            const Value& p = c.has("component") ? c["component"] : c["second"];
            ObjRef cr = db_.resolve(ref.file, p);
            if (!cr.valid()) continue;
            int cls = db_.class_of(cr);
            if (cls == kTransform || cls == kRectTransform) g.transform = cr;
            g.comps.push_back(cr);
        }
        gos_[ref] = std::move(g);
    }
    st_.game_objects = gos_.size();

    // LOD levels past the first are left out unless asked for.
    if (!opt_.all_lods)
        for (auto& ref : db_.objects_of(kLODGroup)) {
            Value v = db_.read(ref);
            auto& lods = v["m_LODs"].items;
            for (size_t l = 1; l < lods.size(); ++l)
                for (auto& r : lods[l]["renderers"].items) {
                    ObjRef rr = db_.resolve(ref.file, r["renderer"]);
                    bool in_lod0 = false;
                    for (auto& r0 : lods[0]["renderers"].items)
                        in_lod0 = in_lod0 || db_.resolve(ref.file, r0["renderer"]) == rr;
                    if (rr.valid() && !in_lod0) lod_skip_.insert(rr);
                }
        }
}

const M4& Builder::world(const ObjRef& ref) {
    static const M4 identity;
    auto it = xfs_.find(ref);
    if (it == xfs_.end()) return identity;
    Xf& x = it->second;
    if (x.state == 2) return x.world;
    if (x.state == 1) return identity;  // cycle guard
    x.state = 1;
    M4 local = trs(x.t, x.r, x.s);
    x.world = x.father.valid() && xfs_.count(x.father) ? world(x.father) * local : local;
    x.state = 2;
    return x.world;
}

// With `parent_world` the object is placed under that world transform instead of its own
// (prefab instances such as terrain trees); `root` then replaces its local transform.
int Builder::visit(const ObjRef& ref, bool parent_active, int depth, const M4* parent_world, const OutNode* root) {
    auto xit = xfs_.find(ref);
    if (xit == xfs_.end() || depth > 1000) return -1;
    auto git = gos_.find(xit->second.go);
    if (git == gos_.end()) return -1;
    const Go& go = git->second;
    bool active = parent_active && go.active;
    if (!active && !opt_.include_inactive) {
        ++st_.skipped_inactive;
        return -1;
    }
    for (auto& c : go.comps)
        if (db_.class_of(c) == 223) return -1;  // Canvas: UI, not level geometry
    const Xf& x = xit->second;
    OutNode n;
    n.name = go.name;
    n.t = root ? root->t : x.t;
    n.r = root ? root->r : x.r;
    n.s = root ? root->s : x.s;
    if (go.layer) n.extras.set("xl_layer", go.layer);
    if (go.tag) n.extras.set("xl_tag", go.tag);
    if (!active) n.extras.set("xl_inactive", true);
    M4 w = parent_world ? *parent_world * trs(n.t, n.r, n.s) : world(ref);
    int index = add_node(std::move(n));
    ++layer_counts_[go.layer];
    components(index, go, w, active);
    std::vector<ObjRef> children = x.children;
    for (auto& c : children) {
        int ci = visit(c, active, depth + 1, parent_world ? &w : nullptr, nullptr);
        if (ci >= 0) sc_.nodes[index].children.push_back(ci);
    }
    return index;
}

void Builder::components(int node, const Go& go, const M4& w, bool active) {
    ObjRef filter, renderer, terrain;
    bool skinned = false, terrain_collides = false, tree_colliders = false;
    std::vector<std::pair<ObjRef, int>> colliders;
    for (auto& c : go.comps) {
        int cls = db_.class_of(c);
        switch (cls) {
        case kMeshFilter: filter = c; break;
        case kMeshRenderer: renderer = c; break;
        case kSkinnedMeshRenderer: renderer = c; skinned = true; break;
        case kMeshCollider: case kBoxCollider: case kSphereCollider: case kCapsuleCollider:
            colliders.push_back({c, cls});
            break;
        case kLight:
            if (opt_.lights) add_light(node, go, c);
            break;
        case kTerrain:
            terrain = c;
            break;
        case kTerrainCollider: {
            Value tc = db_.read(c);
            terrain_collides = tc["m_Enabled"].is_null() || tc["m_Enabled"].truthy();
            tree_colliders = terrain_collides && (tc["m_EnableTreeColliders"].is_null() || tc["m_EnableTreeColliders"].truthy());
            break;
        }
        case kMonoBehaviour: {
            Value mv;
            try {
                mv = db_.read(c);
            } catch (const std::exception&) {
                break;
            }
            std::string cls_name = script_name(db_.resolve(c.file, mv["m_Script"]));
            bool enabled = mv["m_Enabled"].is_null() || mv["m_Enabled"].truthy();
            if (!enabled || (!active && !opt_.include_inactive)) break;
            if (cls_name.find("DecalProjector") != std::string::npos && opt_.decals != DecalMode::None)
                decals_.push_back({node, w, std::move(mv), c.file, go.name, active});
            else if (cls_name == "SplineComputer" && opt_.splines)
                add_spline(node, go, mv, w);
            break;
        }
        default: break;
        }
    }
    if (terrain.valid()) add_terrain(node, go, terrain, terrain_collides, tree_colliders, active);
    ObjRef render_mesh;
    std::shared_ptr<MeshData> render_data;
    if (renderer.valid()) add_renderer(node, go, w, renderer, filter, skinned, active, render_mesh, render_data);
    for (auto& [c, cls] : colliders) add_collider(node, go, w, c, cls, render_mesh, render_data);
    if (sc_.nodes[node].mesh >= 0) {
        bool has_mode = false;
        for (auto& p : sc_.nodes[node].extras.o) has_mode = has_mode || p.first == "sk8_collision_mode";
        if (!has_mode) sc_.nodes[node].extras.set("sk8_collision_mode", "none");
    }
}

void Builder::add_renderer(int node, const Go& go, const M4& w, ObjRef renderer, ObjRef filter, bool skinned,
                           bool active, ObjRef& mesh_ref, std::shared_ptr<MeshData>& used) {
    if (lod_skip_.count(renderer)) {
        ++st_.skipped_lods;
        return;
    }
    Value rv = db_.read(renderer);
    if (!rv["m_Enabled"].is_null() && !rv["m_Enabled"].truthy()) return;
    if (skinned) {
        mesh_ref = db_.resolve(renderer.file, rv["m_Mesh"]);
    } else {
        if (!filter.valid()) return;
        mesh_ref = db_.resolve(filter.file, db_.read(filter)["m_Mesh"]);
    }
    auto m = mesh(mesh_ref);
    if (!m) return;
    std::vector<int> mats;
    for (auto& p : rv["m_Materials"].items) mats.push_back(material(db_.resolve(renderer.file, p)));
    ++st_.renderers;

    const Value& sbi = rv["m_StaticBatchInfo"];
    size_t batch_count = (size_t)sbi["subMeshCount"].i64();
    std::string key;
    std::shared_ptr<MeshData> data;
    if (batch_count > 0) {
        // Static batching baked this renderer into a world-space combined mesh; take its
        // submeshes back out and return them to the object's local space.
        size_t first = (size_t)sbi["firstSubMesh"].i64();
        M4 base;
        ObjRef root = db_.resolve(renderer.file, rv["m_StaticBatchRoot"]);
        if (root.valid() && xfs_.count(root)) base = world(root);
        data = transformed(*subset(*m, first, batch_count), inverse_affine(w) * base);
        data->name = go.name;
        ++st_.batched;
        if (verbose_logging()) {
            V3 lo, hi;
            mesh_bounds(*data, lo, hi);
            log_verbose("unbatched '%s': local bounds centre %.2f m from its pivot, size %.2f m (object at %.1f %.1f %.1f)",
                        go.name.c_str(), length((lo + hi) * 0.5), length(hi - lo), w.m[0][3], w.m[1][3], w.m[2][3]);
        }
    } else {
        data = m;
        key = std::to_string(mesh_ref.file) + ":" + std::to_string(mesh_ref.id);
    }
    used = data;  // collider matching compares against the mesh as authored
    std::vector<int> submats;
    for (size_t s = 0; s < data->subs.size(); ++s) {
        int mat = s < mats.size() ? mats[s] : kSkipSubmesh;
        submats.push_back(mat);
        key += "," + std::to_string(mat);
    }
    if (split_layers(data, submats)) key += "|layers";
    OutMesh om;
    om.name = batch_count ? go.name : m->name;
    om.data = data;
    om.materials = submats;
    for (size_t s = 0; s < data->subs.size(); ++s)
        if (submats[s] != kSkipSubmesh) st_.triangles += data->subs[s].idx.size() / 3;
    bool world_dependent = false;
    if (auto baked = bake_projection(*data, om.materials, w, world_dependent)) {
        data = baked;
        om.data = data;
        if (world_dependent) key.clear();
        else key += "|projected";
        ++st_.projected;
    }
    sc_.nodes[node].mesh = add_mesh(batch_count ? std::string() : key, std::move(om));
    if (active) {
        Instance inst;
        inst.mesh = data;
        inst.materials = sc_.meshes[sc_.nodes[node].mesh].materials;
        inst.world = w;
        V3 lo, hi;
        mesh_bounds(*data, lo, hi);
        world_bounds(w, lo, hi, inst.lo, inst.hi);
        bool all_blend = true;
        for (int mi : submats)
            all_blend = all_blend && (mi == kSkipSubmesh || (mi >= 0 && sc_.materials[mi].alpha_mode == 2));
        if (!all_blend) render_refs_.push_back({node, go.transform, inst.lo, inst.hi});
        inst.node = node;
        inst.name = go.name;
        inst.layer = go.layer;
        instances_.push_back(std::move(inst));
    }
}

// A TerrainLayer as a plain material. Its texture tiles over the terrain by tile size and
// offset, like Unity's terrain shader: uv = (local xz + offset) / tile size.
int Builder::terrain_layer_material(ObjRef ref) {
    auto it = terrain_layers_.find(ref);
    if (it != terrain_layers_.end()) return it->second;
    Value l;
    try {
        l = db_.read(ref);
    } catch (const std::exception&) {
    }
    if (l.is_null()) return terrain_layers_[ref] = default_material();
    OutMaterial m;
    m.name = l["m_Name"].s().empty() ? "TerrainLayer" : l["m_Name"].s();
    m.extras.set("xl_terrain_layer", true);
    ObjRef diffuse = db_.resolve(ref.file, l["m_DiffuseTexture"]);
    ObjRef normal = db_.resolve(ref.file, l["m_NormalMapTexture"]);
    ObjRef mask = db_.resolve(ref.file, l["m_MaskMapTexture"]);
    if (diffuse.valid() && !diffuse.builtin()) {
        ImageJob j;
        j.role = ImageJob::Color;
        j.src[0] = diffuse;
        Vec4v tint = l.has("m_DiffuseRemapMax") ? as_vec4(l["m_DiffuseRemapMax"]) : Vec4v{1, 1, 1, 1};
        if (std::fabs(tint.x - 1) > 1e-3 || std::fabs(tint.y - 1) > 1e-3 || std::fabs(tint.z - 1) > 1e-3) {
            j.params[0] = (float)tint.x;
            j.params[1] = (float)tint.y;
            j.params[2] = (float)tint.z;
            j.params[3] = 1;
            j.params[4] = 1;
        }
        m.base_tex = {image(std::move(j)), 0};
    }
    if (normal.valid() && !normal.builtin()) {
        ImageJob j;
        j.role = ImageJob::Normal;
        j.src[0] = normal;
        m.normal_tex = {image(std::move(j)), 0};
        m.normal_scale = (float)l["m_NormalScale"].num(1);
    }
    m.metallic = (float)l["m_Metallic"].num(0);
    m.roughness = (float)std::clamp(1.0 - l["m_Smoothness"].num(0), 0.0, 1.0);
    if (mask.valid() && !mask.builtin()) {  // HDRP terrain mask: R metallic, G AO, A smoothness
        Vec4v lo = l.has("m_MaskMapRemapMin") ? as_vec4(l["m_MaskMapRemapMin"]) : Vec4v{0, 0, 0, 0};
        Vec4v hi = l.has("m_MaskMapRemapMax") ? as_vec4(l["m_MaskMapRemapMax"]) : Vec4v{1, 1, 1, 1};
        ImageJob j;
        j.role = ImageJob::MaskHDRP;
        j.src[0] = mask;
        j.params[0] = (float)lo.w;
        j.params[1] = (float)hi.w;
        j.params[2] = (float)lo.y;
        j.params[3] = (float)hi.y;
        j.params[4] = (float)lo.x;
        j.params[5] = (float)hi.x;
        m.orm_tex = {image(std::move(j)), 0};
        m.metallic = 1;
        m.roughness = 1;
    }
    Vec2 tile = l.has("m_TileSize") ? as_vec2(l["m_TileSize"]) : Vec2{15, 15};
    Vec2 off = as_vec2(l["m_TileOffset"]);
    if (std::fabs(tile.x) < 1e-6) tile.x = 15;
    if (std::fabs(tile.y) < 1e-6) tile.y = 15;
    Projection p;
    p.mode = 4;
    p.object_space = true;
    p.scale = 1.0 / tile.x;
    p.scale_v = 1.0 / tile.y;
    p.offset_u = off.x / tile.x;
    p.offset_v = off.y / tile.y;
    sc_.materials.push_back(std::move(m));
    receives_decals_.push_back(true);
    int index = (int)sc_.materials.size() - 1;
    projections_.resize(sc_.materials.size());
    projections_[index] = p;
    terrain_layers_[ref] = index;
    return index;
}

// Unity Terrain: the heightmap as a grid mesh (terrain-local, from the GameObject's position),
// each triangle given to the terrain layer with the most splat-map weight there.
void Builder::add_terrain(int node, const Go& go, ObjRef terrain, bool collides, bool tree_colliders, bool active) {
    Value tv = db_.read(terrain);
    if (!tv["m_Enabled"].is_null() && !tv["m_Enabled"].truthy()) return;
    if (!tv["m_DrawHeightmap"].is_null() && !tv["m_DrawHeightmap"].truthy()) return;
    ObjRef data_ref = db_.resolve(terrain.file, tv["m_TerrainData"]);
    Value td;
    try {
        td = db_.read(data_ref);
    } catch (const std::exception& e) {
        log_warn("terrain '%s': %s", go.name.c_str(), e.what());
        return;
    }
    const Value& hm = td["m_Heightmap"];
    int res = (int)hm["m_Resolution"].i64(0);
    if (res <= 1) res = (int)hm["m_Width"].i64(0);
    V3 scale = as_v3(hm["m_Scale"], {1, 1, 1});
    const Value& heights = hm["m_Heights"];
    if (res < 2 || heights.count() < (size_t)res * res) {
        log_warn("terrain '%s' has no readable heightmap", go.name.c_str());
        return;
    }
    ++st_.terrains;
    // Samples to keep per side (a 4097 heightmap would otherwise be 33M triangles).
    std::vector<int> cols;
    int step = std::max(1, (int)std::ceil((res - 1) / (double)std::max(16, opt_.terrain_resolution)));
    for (int i = 0; i < res - 1; i += step) cols.push_back(i);
    cols.push_back(res - 1);
    int n = (int)cols.size();
    auto m = std::make_shared<MeshData>();
    m->name = go.name;
    for (int zi : cols)
        for (int xi : cols) {
            double h = heights.elem((size_t)zi * res + xi) / 32766.0;
            m->pos.insert(m->pos.end(), {(float)(xi * scale.x), (float)(h * scale.y), (float)(zi * scale.z)});
            ++m->vertex_count;
        }
    const Value& holes = hm["m_Holes"];
    bool has_holes = holes.count() == (size_t)(res - 1) * (res - 1);
    V3 size{(res - 1) * scale.x, scale.y, (res - 1) * scale.z};

    // Splat weights: layer i is channel i % 4 of alpha map i / 4.
    const Value& splat = td["m_SplatDatabase"];
    std::vector<int> layer_mats;
    for (auto& p : splat["m_TerrainLayers"].items) layer_mats.push_back(terrain_layer_material(db_.resolve(data_ref.file, p)));
    if (layer_mats.empty())  // pre-2018.3 terrains keep splat prototypes instead of layers
        layer_mats.push_back(default_material());
    std::vector<Image> alphas;
    for (auto& p : splat["m_AlphaTextures"].items) {
        Image img;
        std::string why;
        if (!decode_texture(db_, db_.resolve(data_ref.file, p), 0, img, why)) log_warn("terrain splat map: %s", why.c_str());
        alphas.push_back(std::move(img));
    }
    // Bilinear splat weight of a layer at terrain-normalised (u, v).
    auto weight = [&](size_t layer, double u, double v) -> double {
        const Image* a = layer / 4 < alphas.size() ? &alphas[layer / 4] : nullptr;
        if (!a || a->w == 0) return layer == 0 ? 1.0 : 0.0;
        double fx = std::clamp(u, 0.0, 1.0) * (a->w - 1), fy = std::clamp(v, 0.0, 1.0) * (a->h - 1);
        int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, a->w - 1), y1 = std::min(y0 + 1, a->h - 1);
        double tx = fx - x0, ty = fy - y0;
        auto at = [&](int x, int y) { return a->px[((size_t)y * a->w + x) * 4 + layer % 4] / 255.0; };
        return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
    };
    const size_t L = layer_mats.size();
    std::vector<float> vw(m->vertex_count * L);
    for (size_t v = 0; v < m->vertex_count; ++v)
        for (size_t l = 0; l < L; ++l)
            vw[v * L + l] = (float)weight(l, m->pos[v * 3] / size.x, m->pos[v * 3 + 2] / size.z);

    // Each area goes to its strongest layer; triangles are cut along the layer borders.
    std::vector<uint32_t> grid_tris;
    for (int r = 0; r + 1 < n; ++r)
        for (int c = 0; c + 1 < n; ++c) {
            if (has_holes && holes.elem((size_t)cols[r] * (res - 1) + cols[c]) == 0) continue;
            uint32_t a = r * n + c, b = a + 1, d = a + n, e = d + 1;
            grid_tris.insert(grid_tris.end(), {a, d, b, b, d, e});
        }
    std::vector<MeshData::Sub> parts;
    std::map<CutKey, uint32_t> cuts;
    split_by_weights(*m, grid_tris, vw, L, parts, cuts);
    std::vector<int> mats;
    for (size_t l = 0; l < parts.size(); ++l)
        if (!parts[l].idx.empty()) {
            m->subs.push_back(std::move(parts[l]));
            mats.push_back(layer_mats[l]);
        }
    if (m->subs.empty()) return;
    ensure_normals(*m);
    std::shared_ptr<MeshData> data = m;
    bool world_dependent = false;
    if (auto baked = bake_projection(*data, mats, M4{}, world_dependent)) data = baked;
    for (auto& s : data->subs) st_.triangles += s.idx.size() / 3;

    OutMesh om;
    om.name = go.name;
    om.data = data;
    om.materials = mats;
    OutNode tn;
    tn.name = go.name + "_terrain";
    tn.mesh = add_mesh("", std::move(om));
    tn.extras.set("xl_terrain", true);
    tn.extras.set("sk8_collision_mode", collides ? "triangle_mesh" : "none");
    int ti = add_node(std::move(tn));
    sc_.nodes[node].children.push_back(ti);
    if (active) {
        M4 w = world(go.transform);
        Instance inst;
        inst.mesh = data;
        inst.materials = mats;
        inst.world = w;
        V3 lo, hi;
        mesh_bounds(*data, lo, hi);
        world_bounds(w, lo, hi, inst.lo, inst.hi);
        render_refs_.push_back({ti, go.transform, inst.lo, inst.hi});
        instances_.push_back(std::move(inst));
    }
    log_verbose("terrain '%s': %dx%d samples (step %d), %.0f x %.0f m, %zu layers", go.name.c_str(), n, n, step, size.x,
                size.z, layer_mats.size());

    // Trees painted on the terrain: each instance is its prototype prefab, placed at a
    // normalised terrain position, turned about Y and scaled by width (X/Z) and height (Y).
    bool draw_trees = tv["m_DrawTreesAndFoliage"].is_null() || tv["m_DrawTreesAndFoliage"].truthy();
    const Value& dd = td["m_DetailDatabase"];
    const auto& trees = dd["m_TreeInstances"].items;
    if (!opt_.trees || !draw_trees || !active || trees.empty()) return;
    std::vector<ObjRef> prototypes;
    for (auto& p : dd["m_TreePrototypes"].items) {
        auto g = gos_.find(db_.resolve(data_ref.file, p["prefab"]));
        prototypes.push_back(g != gos_.end() ? g->second.transform : ObjRef{});
    }
    OutNode group;
    group.name = go.name + "_trees";
    int gi = add_node(std::move(group));
    sc_.nodes[node].children.push_back(gi);
    M4 tw = world(go.transform);
    bool saved = suppress_colliders_;
    suppress_colliders_ = !tree_colliders;
    size_t placed = 0;
    for (auto& t : trees) {
        int64_t index = t["index"].i64(-1);
        if (index < 0 || index >= (int64_t)prototypes.size() || !prototypes[(size_t)index].valid()) continue;
        V3 p = as_v3(t["position"]);
        double angle = t["rotation"].num(0), ws = t["widthScale"].num(1), hs = t["heightScale"].num(1);
        OutNode at;
        at.t = {p.x * size.x, p.y * size.y, p.z * size.z};
        at.r = {0, std::sin(angle * 0.5), 0, std::cos(angle * 0.5)};
        at.s = {ws, hs, ws};
        int ti = visit(prototypes[(size_t)index], true, 0, &tw, &at);
        if (ti < 0) continue;
        sc_.nodes[gi].children.push_back(ti);
        ++placed;
    }
    suppress_colliders_ = saved;
    st_.trees += placed;
    log_verbose("terrain '%s': %zu trees from %zu prototypes", go.name.c_str(), placed, prototypes.size());
}

// Grind lines for maps without splines of their own: the top creases and tube tops of the
// collidable objects Skater XL marks grindable (layer 12 Grindable, 16 Coping) or whose names
// say rail, coping, ledge and so on.
void Builder::autosplines() {
    if (opt_.autosplines == 0 || (opt_.autosplines == 1 && st_.splines > 0)) return;
    static const char* kNames[] = {"rail", "coping", "ledge", "grind", "curb", "kerb", "bench", "pipe", "hubba",
                                   "manny", "manual", "planter", "flatbar", "kinker", "funbox"};
    for (auto& inst : instances_) {
        if (inst.node < 0 || !inst.mesh) continue;
        // Named or layered grindables count even without collision of their own (copings often
        // rely on the ramp's collider; a grind curve brings its own). --autospline-all looks at
        // everything that collides.
        bool grindable = inst.layer == 12 || inst.layer == 16;
        for (const char* w : kNames) grindable = grindable || icontains(inst.name, w);
        if (!grindable && opt_.autospline_all) {
            for (auto& p : sc_.nodes[inst.node].extras.o)
                if (p.first == "sk8_collision_mode" && p.second.t == Json::Str && p.second.s != "none") grindable = true;
        }
        if (!grindable) continue;
        const MeshData& m = *inst.mesh;
        std::vector<V3> pos(m.vertex_count);
        for (size_t v = 0; v < m.vertex_count; ++v)
            pos[v] = xform_point(inst.world, {m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]});
        std::vector<uint32_t> tris;
        bool mirrored = det3(inst.world) < 0;
        for (size_t s = 0; s < m.subs.size(); ++s) {
            if (s < inst.materials.size() && inst.materials[s] == kSkipSubmesh) continue;
            auto& idx = m.subs[s].idx;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                tris.insert(tris.end(), {idx[t], mirrored ? idx[t + 2] : idx[t + 1], mirrored ? idx[t + 1] : idx[t + 2]});
            }
        }
        auto lines = find_grind_lines(pos, tris);
        if (lines.empty()) continue;
        ++st_.autospline_objects;
        for (auto& line : lines) {
            OutCurve c;
            c.name = inst.name + "_auto";
            c.points = std::move(line);
            sc_.auto_curves.push_back(std::move(c));
            ++st_.autosplines;
        }
    }
}

bool Builder::is_within(ObjRef xf, ObjRef ancestor) {
    for (int depth = 0; xf.valid() && depth < 256; ++depth) {
        if (xf == ancestor) return true;
        auto it = xfs_.find(xf);
        if (it == xfs_.end()) return false;
        xf = it->second.father;
    }
    return false;
}

// A collider on a GameObject with no visible mesh (a "COLLIDER" child or sibling) gives its
// collision to the visible meshes it overlaps: first within its parent's group, widening up
// the hierarchy only when nothing there overlaps it.
void Builder::resolve_pending_colliders() {
    const double pad = 0.02;
    for (auto& pc : pending_colliders_) {
        auto xit = xfs_.find(pc.xf);
        ObjRef scope = pc.own_subtree_only ? pc.xf
                       : xit != xfs_.end() && xit->second.father.valid() ? xit->second.father
                                                                          : pc.xf;
        std::vector<int> hits;
        for (int depth = 0; depth < 256 && hits.empty(); ++depth) {
            if (pc.own_subtree_only && depth > 0) break;
            for (auto& rr : render_refs_) {
                if (rr.hi.x < pc.lo.x - pad || rr.lo.x > pc.hi.x + pad || rr.hi.y < pc.lo.y - pad ||
                    rr.lo.y > pc.hi.y + pad || rr.hi.z < pc.lo.z - pad || rr.lo.z > pc.hi.z + pad)
                    continue;
                if (scope.valid() && !is_within(rr.xf, scope)) continue;
                hits.push_back(rr.node);
            }
            if (!scope.valid()) break;
            auto sit = xfs_.find(scope);
            scope = sit != xfs_.end() ? sit->second.father : ObjRef{};  // invalid = whole scene
        }
        if (hits.empty()) {
            if (!pc.own_subtree_only) {
                ++st_.colliders_unmatched;
                log_verbose("collider '%s' overlaps no visible mesh", pc.name.c_str());
            }
            continue;
        }
        if (!pc.own_subtree_only) ++st_.colliders_handed;
        for (int n : hits) {
            auto& ex = sc_.nodes[n].extras;
            bool tagged = false;
            for (auto& p : ex.o)
                if (p.first == "sk8_collision_mode") {
                    if (p.second.t == Json::Str && p.second.s == "none") p.second = Json("triangle_mesh");
                    tagged = true;
                }
            if (!tagged) ex.set("sk8_collision_mode", "triangle_mesh");
            log_verbose("collider '%s' -> '%s' collides", pc.name.c_str(), sc_.nodes[n].name.c_str());
        }
    }
}

// Splits submeshes whose material is a vertex-colour layer blend into one submesh per layer,
// each triangle going to the layer with the largest average weight. Returns true if split.
bool Builder::split_layers(std::shared_ptr<MeshData>& data, std::vector<int>& mats) {
    if (data->col.empty()) return false;
    bool any = false;
    for (int m : mats) any = any || layers_.count(m);
    if (!any) return false;
    bool alpha_varies = false;
    for (size_t v = 1; v < data->vertex_count && !alpha_varies; ++v)
        alpha_varies = std::fabs(data->col[v * 4 + 3] - data->col[3]) > 1e-3f;
    auto out = std::make_shared<MeshData>(*data);
    out->subs.clear();
    std::vector<int> out_mats;
    std::map<CutKey, uint32_t> cuts;
    const size_t original_vertices = data->vertex_count;
    for (size_t s = 0; s < data->subs.size(); ++s) {
        auto it = layers_.find(mats[s]);
        if (it == layers_.end()) {
            out->subs.push_back(data->subs[s]);
            out_mats.push_back(mats[s]);
            continue;
        }
        const auto& layers = it->second;
        // Per-vertex layer weights: base = what the painted channels leave, then R, G, B (A).
        std::vector<float> vw(original_vertices * layers.size(), 0.0f);
        for (size_t v = 0; v < original_vertices; ++v) {
            const float* c = &data->col[v * 4];
            float used = 0;
            for (auto& [channel, mat] : layers)
                if (channel >= 0 && (channel < 3 || alpha_varies)) used += c[channel];
            for (size_t l = 0; l < layers.size(); ++l) {
                int channel = layers[l].first;
                vw[v * layers.size() + l] = channel < 0 ? std::max(0.0f, 1.0f - used)
                                            : (channel < 3 || alpha_varies) ? c[channel]  // constant alpha is not paint
                                                                            : 0.0f;
            }
        }
        std::vector<MeshData::Sub> parts;
        split_by_weights(*out, data->subs[s].idx, vw, layers.size(), parts, cuts);
        for (size_t l = 0; l < layers.size(); ++l)
            if (!parts[l].idx.empty()) {
                out->subs.push_back(std::move(parts[l]));
                out_mats.push_back(layers[l].second);
            }
    }
    data = out;
    mats = out_mats;
    return true;
}

// True when a submesh's UV0 is just a top-down projection of its world X/Z (terrain-style
// meshes), so a graph tiling property can only mean world-space tiling.
bool uv_is_world_planar(const MeshData& m, const MeshData::Sub& sub, const M4& w) {
    if (m.uv[0].empty() || sub.idx.size() < 3) return false;
    std::vector<uint32_t> verts(sub.idx.begin(), sub.idx.end());
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    if (verts.size() < 4) return false;
    auto corr = [&](auto fa, auto fb) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = (double)verts.size();
        for (uint32_t v : verts) {
            double a = fa(v), b = fb(v);
            sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
        }
        double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
        return va > 1e-12 && vb > 1e-12 ? std::fabs((sab - sa * sb / n) / std::sqrt(va * vb)) : 0.0;
    };
    auto world = [&](uint32_t v) { return xform_point(w, {m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]}); };
    auto u = [&](uint32_t v) { return (double)m.uv[0][v * 2]; };
    auto vv = [&](uint32_t v) { return (double)m.uv[0][v * 2 + 1]; };
    auto x = [&](uint32_t v) { return world(v).x; };
    auto z = [&](uint32_t v) { return world(v).z; };
    const double k = 0.995;
    return (corr(u, x) > k && corr(vv, z) > k) || (corr(u, z) > k && corr(vv, x) > k);
}

// Bakes HDRP planar/triplanar texture projection into UV0 for the submeshes whose material
// uses it: planar maps world XZ; triplanar becomes a box projection picking XZ, XY or ZY per
// triangle by its dominant normal axis (HDRP blends the three by normal^3 per pixel). UVs are
// position * _TexWorldScale, as HDRP computes them. Returns null when nothing is projected.
std::shared_ptr<MeshData> Builder::bake_projection(const MeshData& src, const std::vector<int>& mats, const M4& w,
                                                   bool& world_dependent) {
    auto proj_of = [&](size_t s) -> const Projection* {
        int mi = s < mats.size() ? mats[s] : -1;
        if (mi < 0 || mi >= (int)projections_.size() || !projections_[mi].mode) return nullptr;
        return &projections_[mi];
    };
    bool any = false;
    for (size_t s = 0; s < src.subs.size(); ++s) any = any || proj_of(s);
    if (!any) return nullptr;

    auto out = std::make_shared<MeshData>(src);
    if (out->uv[0].empty()) out->uv[0].assign(out->vertex_count * 2, 0.0f);
    auto P = [&](uint32_t v) { return V3{src.pos[v * 3], src.pos[v * 3 + 1], src.pos[v * 3 + 2]}; };
    for (size_t s = 0; s < out->subs.size(); ++s) {
        const Projection* p = proj_of(s);
        if (!p) continue;
        int mode = p->mode;
        // A graph tiling below 1 only makes sense per metre of world space (on unwrapped UVs it
        // would stretch one texture past the whole UV square); otherwise a mesh whose UVs are a
        // plain world X/Z projection gives the graph away.
        if (mode == 6) mode = (p->scale < 1.0 || uv_is_world_planar(src, src.subs[s], w)) ? 5 : 7;
        if (!p->object_space && mode != 7) world_dependent = true;
        std::unordered_map<uint64_t, uint32_t> remap;
        auto& idx = out->subs[s].idx;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            V3 q[3];
            for (int k = 0; k < 3; ++k) q[k] = p->object_space ? P(idx[t + k]) : xform_point(w, P(idx[t + k]));
            int axis = 1;  // 0: ZY (faces +-X), 1: XZ (faces +-Y), 2: XY (faces +-Z), 3: mesh UV x tiling
            if (mode == 7) {
                axis = 3;
            } else if (mode == 5) {
                V3 n = cross(q[1] - q[0], q[2] - q[0]);
                double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
                axis = (ay >= ax && ay >= az) ? 1 : (ax >= az ? 0 : 2);
            }
            for (int k = 0; k < 3; ++k) {
                uint32_t v = idx[t + k];
                uint64_t key = (uint64_t)v * 4 + axis;
                auto it = remap.find(key);
                if (it != remap.end()) {
                    idx[t + k] = it->second;
                    continue;
                }
                uint32_t nv = (uint32_t)out->vertex_count++;
                out->pos.insert(out->pos.end(), &src.pos[v * 3], &src.pos[v * 3] + 3);
                if (!src.nrm.empty()) out->nrm.insert(out->nrm.end(), &src.nrm[v * 3], &src.nrm[v * 3] + 3);
                if (!src.col.empty()) out->col.insert(out->col.end(), &src.col[v * 4], &src.col[v * 4] + 4);
                for (int u = 1; u < 4; ++u)
                    if (!src.uv[u].empty()) out->uv[u].insert(out->uv[u].end(), &src.uv[u][v * 2], &src.uv[u][v * 2] + 2);
                const V3& c = q[k];
                double a = axis == 0 ? c.z : c.x, b = axis == 1 ? c.z : c.y;
                if (axis == 3) {
                    a = src.uv[0].empty() ? 0.0 : src.uv[0][v * 2];
                    b = src.uv[0].empty() ? 0.0 : src.uv[0][v * 2 + 1];
                }
                out->uv[0].push_back((float)(a * p->scale + p->offset_u));
                out->uv[0].push_back((float)(b * (p->scale_v != 0 ? p->scale_v : p->scale) + p->offset_v));
                remap[key] = nv;
                idx[t + k] = nv;
            }
        }
    }
    auto compact = subset(*out, 0, out->subs.size());  // drop vertices only the old UVs used
    compact->name = src.name;
    return compact;
}

void Builder::add_collider(int node, const Go& go, const M4& w, ObjRef c, int cls, ObjRef render_mesh,
                           const std::shared_ptr<MeshData>& render_data) {
    if (suppress_colliders_) return;
    Value cv = db_.read(c);
    if (!cv["m_Enabled"].is_null() && !cv["m_Enabled"].truthy()) return;
    bool trigger = cv["m_IsTrigger"].truthy();
    if (trigger && !opt_.triggers) return;
    ++st_.colliders;
    auto local_bounds = [&](V3& lo, V3& hi) -> bool {
        V3 c0 = as_v3(cv["m_Center"]), e;
        switch (cls) {
        case kBoxCollider: {
            V3 s = as_v3(cv["m_Size"], {1, 1, 1});
            e = {std::fabs(s.x) * 0.5, std::fabs(s.y) * 0.5, std::fabs(s.z) * 0.5};
            break;
        }
        case kSphereCollider: {
            double r = std::fabs(cv["m_Radius"].num(0.5));
            e = {r, r, r};
            break;
        }
        case kCapsuleCollider: {
            double r = std::fabs(cv["m_Radius"].num(0.5)), h = std::max(r, std::fabs(cv["m_Height"].num(2)) * 0.5);
            int dir = (int)cv["m_Direction"].i64(1);
            e = {dir == 0 ? h : r, dir == 1 ? h : r, dir == 2 ? h : r};
            break;
        }
        case kMeshCollider: {
            auto m = mesh(db_.resolve(c.file, cv["m_Mesh"]));
            if (!m) return false;
            mesh_bounds(*m, lo, hi);
            return true;
        }
        default: return false;
        }
        lo = c0 - e;
        hi = c0 + e;
        return true;
    };
    // Without --colliders the object's render mesh stands in for its collider. A collider on a
    // GameObject with nothing visible is handed to the visible meshes it overlaps later
    // (resolve_pending_colliders).
    auto stand_in = [&] {
        if (trigger) return;
        bool visible = sc_.nodes[node].mesh >= 0;
        if (visible) {
            bool tagged = false;
            for (auto& p : sc_.nodes[node].extras.o) tagged = tagged || p.first == "sk8_collision_mode";
            if (!tagged) sc_.nodes[node].extras.set("sk8_collision_mode", "triangle_mesh");
        }
        // The collider can also cover visible children (a rail collider around its legs, a ramp's
        // collision mesh around its copings); an invisible one covers its neighbours.
        V3 lo, hi;
        if (!go.transform.valid() || !local_bounds(lo, hi)) return;
        PendingCollider pc;
        pc.xf = go.transform;
        pc.name = go.name;
        pc.own_subtree_only = visible;
        world_bounds(w, lo, hi, pc.lo, pc.hi);
        pending_colliders_.push_back(std::move(pc));
    };
    if (cls != kMeshCollider && !opt_.colliders) {
        stand_in();
        return;
    }
    std::shared_ptr<MeshData> geo;
    std::string kind, key;
    const char* mode = "hull";
    switch (cls) {
    case kMeshCollider: {
        ObjRef mr = db_.resolve(c.file, cv["m_Mesh"]);
        bool convex = cv["m_Convex"].truthy();
        mode = convex ? "hull" : "triangle_mesh";
        auto m = mesh(mr);
        size_t tris = 0, rtris = 0;
        if (m)
            for (auto& s : m->subs) tris += s.idx.size();
        if (render_data)
            for (auto& s : render_data->subs) rtris += s.idx.size();
        bool same = mr == render_mesh ||
                    (m && render_data && render_data->vertex_count == m->vertex_count && rtris == tris && !trigger);
        if (same && sc_.nodes[node].mesh >= 0 && !trigger) {
            sc_.nodes[node].extras.set("sk8_collision_mode", mode);
            ++st_.colliders_on_render;
            return;
        }
        if (!m || !opt_.colliders) {
            stand_in();
            return;
        }
        geo = m;
        kind = "mesh";
        key = "col:" + std::to_string(mr.file) + ":" + std::to_string(mr.id);
        break;
    }
    case kBoxCollider: {
        V3 size = as_v3(cv["m_Size"], {1, 1, 1}), center = as_v3(cv["m_Center"]);
        M4 m = trs(center, {}, size);
        geo = transformed(*builtin_mesh(10202), m);
        geo->name = "BoxCollider";
        kind = "box";
        break;
    }
    case kSphereCollider: {
        double r = cv["m_Radius"].num(0.5);
        M4 m = trs(as_v3(cv["m_Center"]), {}, {r * 2, r * 2, r * 2});
        geo = transformed(*builtin_mesh(10207), m);
        geo->name = "SphereCollider";
        kind = "sphere";
        break;
    }
    case kCapsuleCollider: {
        auto cap = capsule_mesh(cv["m_Radius"].num(0.5), cv["m_Height"].num(2), (int)cv["m_Direction"].i64(1));
        geo = transformed(*cap, trs(as_v3(cv["m_Center"]), {}, {1, 1, 1}));
        geo->name = "CapsuleCollider";
        kind = "capsule";
        break;
    }
    default: return;
    }
    if (key.empty()) {  // identical primitive colliders (every tree's trunk capsule) share one mesh
        char buf[256];
        V3 c0 = as_v3(cv["m_Center"]), sz = as_v3(cv["m_Size"]);
        std::snprintf(buf, sizeof buf, "col:%s:%.4f,%.4f,%.4f:%.4f,%.4f,%.4f:%.4f:%.4f:%lld", kind.c_str(), c0.x, c0.y,
                      c0.z, sz.x, sz.y, sz.z, cv["m_Radius"].num(0), cv["m_Height"].num(0),
                      (long long)cv["m_Direction"].i64(0));
        key = buf;
    }
    OutMesh om;
    om.name = geo->name;
    om.data = geo;
    om.materials.assign(geo->subs.size(), collision_material());
    OutNode n;
    n.name = go.name + "_col";
    n.mesh = add_mesh(key, std::move(om));
    ++st_.colliders_exported;
    n.extras.set("xl_collider", kind);
    n.extras.set("sk8_collision_mode", mode);
    if (trigger) n.extras.set("xl_trigger", true);
    if (go.layer) n.extras.set("xl_layer", go.layer);
    if (go.tag) n.extras.set("xl_tag", go.tag);
    int ni = add_node(std::move(n));
    sc_.nodes[node].children.push_back(ni);
}

void Builder::add_light(int node, const Go& go, ObjRef c) {
    Value lv = db_.read(c);
    if (!lv["m_Enabled"].is_null() && !lv["m_Enabled"].truthy()) return;
    int type = (int)lv["m_Type"].i64(2);
    OutLight l;
    l.name = go.name;
    if (type == 0) l.type = 1;
    else if (type == 1) l.type = 2;
    else l.type = 0;  // point, and area lights approximated as points
    Color col = as_color(lv["m_Color"]);
    V3 k{1, 1, 1};
    if (lv["m_UseColorTemperature"].truthy()) k = blackbody(lv["m_ColorTemperature"].num(6500));
    l.color[0] = srgb_to_linear(col.r) * (float)k.x;
    l.color[1] = srgb_to_linear(col.g) * (float)k.y;
    l.color[2] = srgb_to_linear(col.b) * (float)k.z;
    l.intensity = (float)lv["m_Intensity"].num(1);
    l.range = (float)lv["m_Range"].num(10);
    double spot = lv["m_SpotAngle"].num(30);
    l.outer = (float)(std::clamp(spot, 1.0, 179.0) * 0.5 * kPi / 180);
    l.inner = (float)(lv.has("m_InnerSpotAngle") ? std::clamp(lv["m_InnerSpotAngle"].num(), 0.0, spot) * 0.5 * kPi / 180
                                                  : l.outer * 0.8);
    sc_.lights.push_back(l);
    OutNode n;
    n.name = go.name + "_light";
    n.r = {0, 1, 0, 0};  // glTF lights shine down -Z, Unity's down +Z
    n.light = (int)sc_.lights.size() - 1;
    int ni = add_node(std::move(n));
    sc_.nodes[node].children.push_back(ni);
    ++st_.lights;
}

// Dreamteck Splines SplineComputer: grind paths in Skater XL maps.
void Builder::add_spline(int node, const Go& go, const Value& mv, const M4& w) {
    const Value& sp = mv["spline"];
    const auto& pts = sp["points"].items;
    if (pts.size() < 2) return;
    bool local = mv["_space"].i64(1) == 1;
    bool closed = sp["closed"].truthy();
    int type = (int)sp["type"].i64(3);  // 0 CatmullRom, 1 BSpline, 2 Bezier, 3 Linear
    M4 to_local = local ? M4{} : inverse_affine(w);
    std::vector<V3> p, t1, t2;
    for (auto& q : pts) {
        p.push_back(xform_point(to_local, as_v3(q["position"])));
        t1.push_back(xform_point(to_local, as_v3(q["tangent"])));
        t2.push_back(xform_point(to_local, as_v3(q["tangent2"])));
    }
    size_t n = p.size();
    size_t segments = closed ? n : n - 1;
    auto at = [&](long i) -> V3 {
        if (closed) return p[((i % (long)n) + n) % n];
        if (i < 0) return p[0] * 2 - p[1];
        if (i >= (long)n) return p[n - 1] * 2 - p[n - 2];
        return p[(size_t)i];
    };
    std::vector<V3> out;
    const int steps = type == 3 ? 1 : 12;
    for (size_t s = 0; s < segments; ++s) {
        for (int k = 0; k < steps; ++k) {
            double t = (double)k / steps;
            V3 q;
            long i = (long)s;
            if (type == 2) {
                V3 a = p[s], b = t2[s], cc = t1[(s + 1) % n], d = p[(s + 1) % n];
                double u = 1 - t;
                q = a * (u * u * u) + b * (3 * u * u * t) + cc * (3 * u * t * t) + d * (t * t * t);
            } else if (type == 0) {
                V3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                double t2v = t * t, t3 = t2v * t;
                q = (p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2v + (-p0 + p1 * 3 - p2 * 3 + p3) * t3) * 0.5;
            } else if (type == 1) {
                V3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                double u = 1 - t;
                q = (p0 * (u * u * u) + p1 * (3 * t * t * t - 6 * t * t + 4) + p2 * (-3 * t * t * t + 3 * t * t + 3 * t + 1) +
                     p3 * (t * t * t)) * (1.0 / 6.0);
            } else {
                q = p[s] * (1 - t) + p[(s + 1) % n] * t;
            }
            out.push_back(q);
        }
    }
    out.push_back(closed ? out.front() : p[n - 1]);

    auto m = std::make_shared<MeshData>();
    m->name = go.name + "_spline";
    MeshData::Sub sub;
    for (auto& q : out) {
        m->pos.insert(m->pos.end(), {(float)q.x, (float)q.y, (float)q.z});
        ++m->vertex_count;
    }
    for (uint32_t k = 0; k + 1 < (uint32_t)out.size(); ++k) sub.idx.insert(sub.idx.end(), {k, k + 1});
    m->subs.push_back(std::move(sub));

    // The same path as a curve, in world space: linear stays a poly line; Bezier keeps its
    // handles; Catmull-Rom and uniform B-spline convert exactly to Bezier handles.
    OutCurve curve;
    curve.name = go.name + "_spline";
    curve.closed = closed;
    curve.bezier = type != 3;
    for (size_t i = 0; i < n; ++i) {
        long k = (long)i;
        if (type == 3) {
            curve.points.push_back(p[i]);
        } else if (type == 2) {
            curve.points.push_back(p[i]);
            curve.left.push_back(t1[i]);
            curve.right.push_back(t2[i]);
        } else if (type == 0) {
            V3 d = (at(k + 1) - at(k - 1)) * (1.0 / 6.0);
            curve.points.push_back(p[i]);
            curve.left.push_back(p[i] - d);
            curve.right.push_back(p[i] + d);
        } else {
            V3 a = at(k - 1), b = at(k), c = at(k + 1);
            curve.points.push_back((a + b * 4 + c) * (1.0 / 6.0));
            curve.left.push_back((a + b * 2) * (1.0 / 3.0));
            curve.right.push_back((b * 2 + c) * (1.0 / 3.0));
        }
    }
    for (auto* list : {&curve.points, &curve.left, &curve.right})
        for (auto& v : *list) v = xform_point(w, v);
    sc_.curves.push_back(std::move(curve));
    ++st_.splines;
    if (!opt_.spline_meshes) return;

    OutMesh om;
    om.name = m->name;
    om.data = m;
    om.lines = true;
    om.materials = {default_material()};
    OutNode nn;
    nn.name = go.name + "_spline";
    nn.mesh = add_mesh("", std::move(om));
    nn.extras.set("xl_spline", true);
    nn.extras.set("xl_spline_closed", closed);
    static const char* names[] = {"catmull_rom", "bspline", "bezier", "linear"};
    nn.extras.set("xl_spline_type", type >= 0 && type < 4 ? names[type] : "unknown");
    nn.extras.set("sk8_collision_mode", "none");
    int ni = add_node(std::move(nn));
    sc_.nodes[node].children.push_back(ni);
}

// HDRP/URP decal projectors: clip the scene's triangles against each projector box
// and give them the decal's UVs, so the decals become real geometry.
void Builder::project_decals() {
    for (auto& d : decals_) {
        const Value& pv = d.proj;
        V3 size = as_v3(pv["m_Size"], {1, 1, 1});
        V3 off = pv.has("m_Offset") ? as_v3(pv["m_Offset"]) : as_v3(pv["m_Pivot"], {0, 0, size.z * 0.5});
        Vec2 uvs = pv.has("m_UVScale") ? as_vec2(pv["m_UVScale"]) : Vec2{1, 1};
        Vec2 uvb = as_vec2(pv["m_UVBias"]);
        int mat = material(db_.resolve(d.file, pv["m_Material"]));
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) continue;
        ++st_.decals;
        V3 lo = off - size * 0.5, hi = off + size * 0.5;

        auto m = std::make_shared<MeshData>();
        m->name = d.name + "_decal";
        MeshData::Sub sub;
        size_t dbg_near = 0, dbg_inbox = 0, dbg_facing = 0, dbg_norecv = 0;
        if (opt_.decals == DecalMode::Quad) {
            double z = off.z;
            V3 c[4] = {{lo.x, lo.y, z}, {hi.x, lo.y, z}, {hi.x, hi.y, z}, {lo.x, hi.y, z}};
            double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int k = 0; k < 4; ++k) {
                m->pos.insert(m->pos.end(), {(float)c[k].x, (float)c[k].y, (float)c[k].z});
                m->nrm.insert(m->nrm.end(), {0.0f, 0.0f, -1.0f});
                m->uv[0].insert(m->uv[0].end(), {(float)(uv[k][0] * uvs.x + uvb.x), (float)(uv[k][1] * uvs.y + uvb.y)});
            }
            m->vertex_count = 4;
            sub.idx = {0, 2, 1, 0, 3, 2};
        } else {
            double start_fade = pv["m_StartAngleFade"].num(180), end_fade = pv["m_EndAngleFade"].num(180);
            double max_angle = std::min(end_fade, 80.0);
            if (start_fade < end_fade) max_angle = std::min(max_angle, (start_fade + end_fade) * 0.5);
            double min_facing = std::cos(max_angle * kPi / 180);
            M4 inv = inverse_affine(d.world);
            V3 wlo, whi;
            world_bounds(d.world, lo, hi, wlo, whi);
            double zscale = length(xform_dir(d.world, {0, 0, 1}));
            double lift = zscale > 1e-9 ? 0.002 / zscale : 0.002;
            for (auto& inst : instances_) {
                if (inst.hi.x < wlo.x || inst.lo.x > whi.x || inst.hi.y < wlo.y || inst.lo.y > whi.y ||
                    inst.hi.z < wlo.z || inst.lo.z > whi.z)
                    continue;
                M4 to_proj = inv * inst.world;
                const MeshData& src = *inst.mesh;
                std::vector<V3> lp(src.vertex_count);
                bool computed = false;
                ++dbg_near;
                for (size_t s = 0; s < src.subs.size(); ++s) {
                    int im = s < inst.materials.size() ? inst.materials[s] : kSkipSubmesh;
                    if (im < 0 || im >= (int)receives_decals_.size() || !receives_decals_[im]) {
                        ++dbg_norecv;
                        continue;
                    }
                    if (!computed) {
                        for (size_t v = 0; v < src.vertex_count; ++v)
                            lp[v] = xform_point(to_proj, {src.pos[v * 3], src.pos[v * 3 + 1], src.pos[v * 3 + 2]});
                        computed = true;
                    }
                    auto& idx = src.subs[s].idx;
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        V3 a = lp[idx[t]], b = lp[idx[t + 1]], c = lp[idx[t + 2]];
                        if ((a.x < lo.x && b.x < lo.x && c.x < lo.x) || (a.x > hi.x && b.x > hi.x && c.x > hi.x) ||
                            (a.y < lo.y && b.y < lo.y && c.y < lo.y) || (a.y > hi.y && b.y > hi.y && c.y > hi.y) ||
                            (a.z < lo.z && b.z < lo.z && c.z < lo.z) || (a.z > hi.z && b.z > hi.z && c.z > hi.z))
                            continue;
                        ++dbg_inbox;
                        V3 fn = cross(b - a, c - a);
                        if (det3(to_proj) < 0) fn = -fn;
                        double fl = length(fn);
                        if (fl < 1e-12) continue;
                        fn = fn * (1 / fl);
                        if (-fn.z < min_facing) {  // must face back up the projection (+Z)
                            ++dbg_facing;
                            continue;
                        }
                        std::vector<V3> poly = {a, b, c}, next;
                        for (int axis = 0; axis < 3 && !poly.empty(); ++axis)
                            for (int side = 0; side < 2 && !poly.empty(); ++side) {
                                double limit = side ? hi[axis] : lo[axis];
                                auto inside = [&](const V3& q) { return side ? q[axis] <= limit : q[axis] >= limit; };
                                next.clear();
                                for (size_t k = 0; k < poly.size(); ++k) {
                                    const V3& cur = poly[k];
                                    const V3& prv = poly[(k + poly.size() - 1) % poly.size()];
                                    bool ci = inside(cur), pi = inside(prv);
                                    if (ci != pi) {
                                        double tt = (limit - prv[axis]) / (cur[axis] - prv[axis]);
                                        next.push_back(prv + (cur - prv) * tt);
                                    }
                                    if (ci) next.push_back(cur);
                                }
                                poly.swap(next);
                            }
                        if (poly.size() < 3) continue;
                        uint32_t first = (uint32_t)m->vertex_count;
                        for (auto& q : poly) {
                            V3 lifted = q - V3{0, 0, lift};
                            double u = (q.x - off.x) / size.x + 0.5, v = (q.y - off.y) / size.y + 0.5;
                            m->pos.insert(m->pos.end(), {(float)lifted.x, (float)lifted.y, (float)lifted.z});
                            m->nrm.insert(m->nrm.end(), {(float)fn.x, (float)fn.y, (float)fn.z});
                            m->uv[0].insert(m->uv[0].end(), {(float)(u * uvs.x + uvb.x), (float)(v * uvs.y + uvb.y)});
                            ++m->vertex_count;
                        }
                        for (uint32_t k = 1; k + 1 < (uint32_t)poly.size(); ++k) {
                            uint32_t i0 = first, i1 = first + k, i2 = first + k + 1;
                            V3 e = cross(poly[k] - poly[0], poly[k + 1] - poly[0]);
                            if (e.z > 0) std::swap(i1, i2);  // front face toward the projector
                            sub.idx.insert(sub.idx.end(), {i0, i1, i2});
                        }
                    }
                }
            }
        }
        if (sub.idx.empty()) {
            ++st_.empty_decals;
            log_verbose("decal '%s' (%s) hit no geometry: %zu nearby objects, %zu triangles in the box, %zu facing away, "
                        "%zu on non-receiving materials", d.name.c_str(), sc_.materials[mat].name.c_str(), dbg_near,
                        dbg_inbox, dbg_facing, dbg_norecv);
            continue;
        }
        st_.decal_triangles += sub.idx.size() / 3;
        m->subs.push_back(std::move(sub));
        OutMesh om;
        om.name = m->name;
        om.data = m;
        om.materials = {mat};
        OutNode n;
        n.name = d.name + "_decal";
        n.mesh = add_mesh("", std::move(om));
        n.extras.set("xl_decal", true);
        if (!d.active) n.extras.set("xl_inactive", true);
        n.extras.set("sk8_collision_mode", "none");
        int ni = add_node(std::move(n));
        sc_.nodes[d.node].children.push_back(ni);
    }
}

// Replaces the GameObject tree with one root-level node per mesh or light, carrying its
// world transform. Grouping empties go away (and with them Blender's relationship lines).
void Builder::flatten() {
    std::vector<OutNode> old;
    old.swap(sc_.nodes);
    std::vector<int> roots;
    roots.swap(sc_.roots);
    std::function<void(int, const M4&)> walk = [&](int i, const M4& parent) {
        M4 w = parent * trs(old[i].t, old[i].r, old[i].s);
        if (old[i].mesh >= 0 || old[i].light >= 0) {
            OutNode f;
            f.name = old[i].name;
            f.mesh = old[i].mesh;
            f.light = old[i].light;
            f.extras = old[i].extras;
            if (!decompose(w, f.t, f.r, f.s)) {
                // Sheared by a non-uniform parent scale: bake the linear part into the mesh.
                f.t = {w.m[0][3], w.m[1][3], w.m[2][3]};
                f.r = {};
                f.s = {1, 1, 1};
                if (f.mesh >= 0 && sc_.meshes[f.mesh].data) {
                    M4 linear = w;
                    linear.m[0][3] = linear.m[1][3] = linear.m[2][3] = 0;
                    OutMesh copy = sc_.meshes[f.mesh];
                    copy.data = transformed(*copy.data, linear, !copy.lines);
                    f.mesh = add_mesh("", std::move(copy));
                    ++st_.baked_skew;
                }
            }
            sc_.roots.push_back(add_node(std::move(f)));
            ++st_.flattened;
        }
        for (int c : old[i].children) walk(c, w);
    };
    for (int r : roots) walk(r, M4{});
}

Scene Builder::run() {
    load_hierarchy();
    // Only scene files hold the level; prefabs in shared-asset files (terrain tree prototypes,
    // spawnable props) are placed where something references them, not at the origin.
    std::set<int> scene_files;
    for (int f = 0; f < (int)db_.files.size(); ++f)
        for (auto& o : db_.files[f]->objects)
            if (o.class_id == 104 || o.class_id == 157 || o.class_id == 196) {  // Render/Lightmap/NavMesh settings
                scene_files.insert(f);
                break;
            }
    std::vector<ObjRef> roots;
    for (auto& [ref, x] : xfs_)
        if ((!x.father.valid() || !xfs_.count(x.father)) && (scene_files.empty() || scene_files.count(ref.file)))
            roots.push_back(ref);
    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        int n = visit(r, true, 0);
        if (n >= 0) sc_.roots.push_back(n);
    }
    project_decals();
    resolve_pending_colliders();
    autosplines();
    if (opt_.flatten) flatten();
    if (verbose_logging())
        for (auto& [layer, count] : layer_counts_) log_verbose("layer %d: %d objects", layer, count);
    return std::move(sc_);
}

} // namespace

Scene build_scene(const Database& db, const Options& opt, Stats& stats) {
    Builder b(db, opt, stats);
    return b.run();
}

void resolve_texture_alpha(Scene& sc) {
    for (auto& m : sc.materials) {
        if (!m.auto_alpha || m.alpha_mode != 0) continue;
        int i = m.base_tex.image;
        if (i < 0 || i >= (int)sc.images.size() || !sc.images[i].ok || !sc.images[i].cutout) continue;
        m.alpha_mode = 1;
        m.double_sided = true;
        m.extras.set("xl_alpha_from_texture", true);
    }
}

} // namespace xl
