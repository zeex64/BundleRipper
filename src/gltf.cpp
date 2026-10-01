#include "gltf.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace xl {

namespace {

constexpr int kFloat = 5126, kU16 = 5123, kU32 = 5125;
constexpr int kArrayBuffer = 34962, kElementBuffer = 34963;

std::string uri_escape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string safe_file_name(std::string s) {
    for (auto& c : s)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            (unsigned char)c < 32)
            c = '_';
    if (s.empty()) s = "texture";
    return s;
}

class Writer {
public:
    Writer(const Scene& sc, const fs::path& out, const GltfOptions& opt)
        : sc_(sc), out_(out), external_(opt.external_textures), opt_(opt) {}
    GltfResult write();

private:
    const Scene& sc_;
    fs::path out_;
    bool external_;
    GltfOptions opt_;
    std::map<std::tuple<const MeshData*, int, int>, int> tiled_uvs_;  // (mesh, uv set, material) -> accessor
    int tiled_uv(const MeshData& m, int set, int material);
    std::vector<uint8_t> bin_;
    Json views_ = Json::array(), accessors_ = Json::array(), meshes_ = Json::array(), materials_ = Json::array();
    Json textures_ = Json::array(), images_ = Json::array(), samplers_ = Json::array(), nodes_ = Json::array();
    std::set<std::string> extensions_;
    std::map<std::pair<int, int>, int> texture_keys_, sampler_keys_;
    std::vector<int> image_index_;  // scene image -> glTF image (-1 failed)
    std::vector<int> mesh_index_;   // scene mesh -> glTF mesh (-1 empty)
    int default_material_ = -1;

    struct Attr {
        int pos = -1, nrm = -1, col = -1, uv[4] = {-1, -1, -1, -1};
        std::vector<int> idx;
    };
    std::unordered_map<const MeshData*, Attr> attrs_;

    int view(const void* data, size_t bytes, int target) {
        while (bin_.size() % 4) bin_.push_back(0);
        size_t offset = bin_.size();
        bin_.insert(bin_.end(), (const uint8_t*)data, (const uint8_t*)data + bytes);
        Json v = Json::object();
        v.set("buffer", 0);
        v.set("byteOffset", (uint64_t)offset);
        v.set("byteLength", (uint64_t)bytes);
        if (target) v.set("target", target);
        views_.push(std::move(v));
        return (int)views_.a.size() - 1;
    }
    int accessor(int v, int comp, size_t count, const char* type, Json mn = {}, Json mx = {}) {
        Json a = Json::object();
        a.set("bufferView", v);
        a.set("componentType", comp);
        a.set("count", (uint64_t)count);
        a.set("type", type);
        if (mn.t != Json::Null) a.set("min", std::move(mn));
        if (mx.t != Json::Null) a.set("max", std::move(mx));
        accessors_.push(std::move(a));
        return (int)accessors_.a.size() - 1;
    }

    const Attr& attributes(const MeshData& m, bool lines);
    int texture(const TexRef& t);
    Json texture_info(const TexRef& t, const OutMaterial& m);
    void write_images();
    void write_materials();
    void write_meshes();
    void write_nodes();
    int default_material();
};

const Writer::Attr& Writer::attributes(const MeshData& m, bool lines) {
    auto it = attrs_.find(&m);
    if (it != attrs_.end()) return it->second;
    Attr a;
    size_t n = m.vertex_count;
    std::vector<float> buf(n * 3);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < n; ++i) {
        float p[3] = {-m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]};
        for (int k = 0; k < 3; ++k) {
            buf[i * 3 + k] = p[k];
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    a.pos = accessor(view(buf.data(), buf.size() * 4, kArrayBuffer), kFloat, n, "VEC3", Json::list(lo[0], lo[1], lo[2]),
                     Json::list(hi[0], hi[1], hi[2]));
    if (!m.nrm.empty() && !lines) {
        for (size_t i = 0; i < n; ++i) {
            float x = -m.nrm[i * 3], y = m.nrm[i * 3 + 1], z = m.nrm[i * 3 + 2];
            float l = std::sqrt(x * x + y * y + z * z);
            if (l < 1e-12f) { x = 0; y = 1; z = 0; l = 1; }
            buf[i * 3] = x / l;
            buf[i * 3 + 1] = y / l;
            buf[i * 3 + 2] = z / l;
        }
        a.nrm = accessor(view(buf.data(), buf.size() * 4, kArrayBuffer), kFloat, n, "VEC3");
    }
    if (!m.col.empty() && opt_.vertex_colors)
        a.col = accessor(view(m.col.data(), m.col.size() * 4, kArrayBuffer), kFloat, n, "VEC4");
    for (int k = 0; k < 4; ++k) {
        if (m.uv[k].empty()) continue;
        std::vector<float> uv(n * 2);
        for (size_t i = 0; i < n; ++i) {
            uv[i * 2] = m.uv[k][i * 2];
            uv[i * 2 + 1] = 1.0f - m.uv[k][i * 2 + 1];
        }
        a.uv[k] = accessor(view(uv.data(), uv.size() * 4, kArrayBuffer), kFloat, n, "VEC2");
    }
    bool wide = n > 65535;
    for (auto& s : m.subs) {
        if (s.idx.empty()) {
            a.idx.push_back(-1);
            continue;
        }
        std::vector<uint32_t> idx = s.idx;
        if (!lines)
            for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
        if (wide) {
            a.idx.push_back(accessor(view(idx.data(), idx.size() * 4, kElementBuffer), kU32, idx.size(), "SCALAR"));
        } else {
            std::vector<uint16_t> small(idx.begin(), idx.end());
            a.idx.push_back(accessor(view(small.data(), small.size() * 2, kElementBuffer), kU16, small.size(), "SCALAR"));
        }
    }
    return attrs_.emplace(&m, std::move(a)).first->second;
}

// A material's texture tiling applied to a UV set, for its primitives only.
int Writer::tiled_uv(const MeshData& m, int set, int material) {
    auto key = std::make_tuple(&m, set, material);
    auto it = tiled_uvs_.find(key);
    if (it != tiled_uvs_.end()) return it->second;
    const OutMaterial& mat = sc_.materials[material];
    size_t n = m.vertex_count;
    std::vector<float> uv(n * 2);
    for (size_t i = 0; i < n; ++i) {
        // Unity: uv' = uv * scale + offset with v up; glTF flips v.
        uv[i * 2] = m.uv[set][i * 2] * mat.uv_scale[0] + mat.uv_offset[0];
        uv[i * 2 + 1] = 1.0f - (m.uv[set][i * 2 + 1] * mat.uv_scale[1] + mat.uv_offset[1]);
    }
    int acc = accessor(view(uv.data(), uv.size() * 4, kArrayBuffer), kFloat, n, "VEC2");
    tiled_uvs_[key] = acc;
    return acc;
}

void Writer::write_images() {
    fs::path dir;
    std::string dir_name;
    if (external_) {
        dir_name = out_.stem().string() + "_textures";
        dir = out_.parent_path() / dir_name;
        fs::create_directories(dir);
    }
    std::set<std::string> used;
    for (auto& job : sc_.images) {
        if (!job.ok || job.png.empty()) {
            image_index_.push_back(-1);
            continue;
        }
        std::string name = safe_file_name(job.name);
        std::string unique = name;
        for (int k = 2; used.count(lower(unique)); ++k) unique = name + "_" + std::to_string(k);
        used.insert(lower(unique));
        Json img = Json::object();
        img.set("name", unique);
        if (external_) {
            std::ofstream f(dir / (unique + ".png"), std::ios::binary);
            f.write((const char*)job.png.data(), (std::streamsize)job.png.size());
            img.set("uri", uri_escape(dir_name + "/" + unique + ".png"));
        } else {
            img.set("bufferView", view(job.png.data(), job.png.size(), 0));
            img.set("mimeType", "image/png");
        }
        images_.push(std::move(img));
        image_index_.push_back((int)images_.a.size() - 1);
    }
}

int Writer::texture(const TexRef& t) {
    if (t.image < 0 || t.image >= (int)image_index_.size() || image_index_[t.image] < 0) return -1;
    const ImageJob& job = sc_.images[t.image];
    auto wrap = [](int w) { return w == 1 ? 33071 : (w == 2 || w == 3) ? 33648 : 10497; };
    int wu = wrap(job.wrap_u), wv = wrap(job.wrap_v);
    int filter = job.filter == 0 ? 0 : 1;
    int skey = wu * 100000 + wv * 2 + filter;
    auto sit = sampler_keys_.find({skey, 0});
    int sampler;
    if (sit == sampler_keys_.end()) {
        Json s = Json::object();
        s.set("wrapS", wu);
        s.set("wrapT", wv);
        s.set("magFilter", filter ? 9729 : 9728);
        s.set("minFilter", filter ? 9987 : 9984);
        samplers_.push(std::move(s));
        sampler = (int)samplers_.a.size() - 1;
        sampler_keys_[{skey, 0}] = sampler;
    } else {
        sampler = sit->second;
    }
    auto key = std::make_pair(image_index_[t.image], sampler);
    auto it = texture_keys_.find(key);
    if (it != texture_keys_.end()) return it->second;
    Json tex = Json::object();
    tex.set("sampler", sampler);
    tex.set("source", key.first);
    textures_.push(std::move(tex));
    int index = (int)textures_.a.size() - 1;
    texture_keys_[key] = index;
    return index;
}

Json Writer::texture_info(const TexRef& t, const OutMaterial& m) {
    int tex = texture(t);
    if (tex < 0) return {};
    (void)m;  // tiling is baked into the primitives' UVs (tiled_uv)
    Json info = Json::object();
    info.set("index", tex);
    if (t.uv) info.set("texCoord", t.uv);
    return info;
}

void Writer::write_materials() {
    for (auto& m : sc_.materials) {
        Json j = Json::object();
        j.set("name", m.name);
        Json pbr = Json::object();
        pbr.set("baseColorFactor", Json::list(m.base[0], m.base[1], m.base[2], m.base[3]));
        Json bt = texture_info(m.base_tex, m);
        if (bt.t != Json::Null) pbr.set("baseColorTexture", std::move(bt));
        pbr.set("metallicFactor", m.metallic);
        pbr.set("roughnessFactor", m.roughness);
        Json orm = texture_info(m.orm_tex, m);
        if (orm.t != Json::Null) {
            pbr.set("metallicRoughnessTexture", orm);
            if (m.orm_has_occlusion && opt_.occlusion) j.set("occlusionTexture", orm);
        }
        j.set("pbrMetallicRoughness", std::move(pbr));
        Json nt = texture_info(m.normal_tex, m);
        if (nt.t != Json::Null) {
            if (m.normal_scale != 1) nt.set("scale", m.normal_scale);
            j.set("normalTexture", std::move(nt));
        }
        if (m.emissive[0] > 0 || m.emissive[1] > 0 || m.emissive[2] > 0) {
            j.set("emissiveFactor", Json::list(m.emissive[0], m.emissive[1], m.emissive[2]));
            Json et = texture_info(m.emissive_tex, m);
            if (et.t != Json::Null) j.set("emissiveTexture", std::move(et));
            if (m.emissive_strength > 1) {
                j.child("extensions").child("KHR_materials_emissive_strength").set("emissiveStrength", m.emissive_strength);
                extensions_.insert("KHR_materials_emissive_strength");
            }
        }
        Json extras = m.extras;
        if (m.alpha_mode == 1 && opt_.standard_alpha) {
            j.set("alphaMode", "MASK");
            j.set("alphaCutoff", m.alpha_cutoff);
        } else if (m.alpha_mode == 1) {
            j.set("alphaMode", "BLEND");
            extras.set("xl_alpha_mode", "mask");
            extras.set("xl_alpha_cutoff", m.alpha_cutoff);
            Json& studio = extras.child("sk8_material");
            studio.set("alpha", 2);  // Studio's alpha enum: auto, opaque, mask, blend
            studio.set("alpha_cutoff", m.alpha_cutoff);
        } else if (m.alpha_mode == 2) {
            j.set("alphaMode", "BLEND");
        }
        if (m.double_sided) j.set("doubleSided", true);
        if (m.unlit) {
            j.child("extensions").child("KHR_materials_unlit");
            extensions_.insert("KHR_materials_unlit");
        }
        if (!extras.empty()) j.set("extras", extras);
        materials_.push(std::move(j));
    }
}

int Writer::default_material() {
    if (default_material_ < 0) {
        Json j = Json::object();
        j.set("name", "XL_NoMaterial");
        materials_.push(std::move(j));
        default_material_ = (int)materials_.a.size() - 1;
    }
    return default_material_;
}

void Writer::write_meshes() {
    for (auto& om : sc_.meshes) {
        if (!om.data || om.data->vertex_count == 0) {
            mesh_index_.push_back(-1);
            continue;
        }
        const Attr& a = attributes(*om.data, om.lines);
        Json prims = Json::array();
        for (size_t s = 0; s < om.data->subs.size() && s < a.idx.size(); ++s) {
            int mat = s < om.materials.size() ? om.materials[s] : -1;
            if (mat == -2 || a.idx[s] < 0) continue;
            Json p = Json::object();
            Json attrs = Json::object();
            attrs.set("POSITION", a.pos);
            if (a.nrm >= 0) attrs.set("NORMAL", a.nrm);
            int tiled_set = -1;
            if (mat >= 0 && mat < (int)sc_.materials.size() && sc_.materials[mat].uv_transform) {
                const OutMaterial& om2 = sc_.materials[mat];
                tiled_set = om2.base_tex.image >= 0     ? om2.base_tex.uv
                            : om2.normal_tex.image >= 0 ? om2.normal_tex.uv
                                                        : om2.orm_tex.uv;
                if (tiled_set < 0 || tiled_set > 3 || om.data->uv[tiled_set].empty()) tiled_set = -1;
            }
            for (int k = 0; k < 4; ++k)
                if (a.uv[k] >= 0)
                    attrs.set("TEXCOORD_" + std::to_string(k), k == tiled_set ? tiled_uv(*om.data, k, mat) : a.uv[k]);
            if (a.col >= 0) attrs.set("COLOR_0", a.col);
            p.set("attributes", std::move(attrs));
            p.set("indices", a.idx[s]);
            p.set("material", mat >= 0 ? mat : default_material());
            if (om.lines) p.set("mode", 1);
            prims.push(std::move(p));
        }
        if (prims.a.empty()) {
            mesh_index_.push_back(-1);
            continue;
        }
        Json j = Json::object();
        j.set("name", om.name);
        j.set("primitives", std::move(prims));
        meshes_.push(std::move(j));
        mesh_index_.push_back((int)meshes_.a.size() - 1);
    }
}

void Writer::write_nodes() {
    for (auto& n : sc_.nodes) {
        Json j = Json::object();
        j.set("name", n.name);
        if (n.t.x != 0 || n.t.y != 0 || n.t.z != 0) j.set("translation", Json::list(-n.t.x, n.t.y, n.t.z));
        Quat q = normalize(n.r);
        if (q.x != 0 || q.y != 0 || q.z != 0) j.set("rotation", Json::list(q.x, -q.y, -q.z, q.w));
        if (n.s.x != 1 || n.s.y != 1 || n.s.z != 1) j.set("scale", Json::list(n.s.x, n.s.y, n.s.z));
        if (n.mesh >= 0 && n.mesh < (int)mesh_index_.size() && mesh_index_[n.mesh] >= 0) j.set("mesh", mesh_index_[n.mesh]);
        if (!n.children.empty()) {
            Json c = Json::array();
            for (int k : n.children) c.push(k);
            j.set("children", std::move(c));
        }
        if (n.light >= 0) {
            j.set("extensions", Json::object()).set("KHR_lights_punctual", Json::object()).set("light", n.light);
            extensions_.insert("KHR_lights_punctual");
        }
        if (!n.extras.empty()) j.set("extras", n.extras);
        nodes_.push(std::move(j));
    }
}

GltfResult Writer::write() {
    write_images();
    write_materials();
    write_meshes();
    write_nodes();

    Json doc = Json::object();
    Json asset = Json::object();
    asset.set("version", "2.0");
    asset.set("generator", "Spotbuilder");
    doc.set("asset", std::move(asset));
    doc.set("scene", 0);
    Json scene = Json::object();
    scene.set("name", out_.stem().string());
    Json roots = Json::array();
    for (int r : sc_.roots) roots.push(r);
    scene.set("nodes", std::move(roots));
    doc.set("scenes", Json::array()).push(std::move(scene));
    doc.set("nodes", nodes_);
    if (!meshes_.a.empty()) doc.set("meshes", meshes_);
    if (!materials_.a.empty()) doc.set("materials", materials_);
    if (!textures_.a.empty()) doc.set("textures", textures_);
    if (!images_.a.empty()) doc.set("images", images_);
    if (!samplers_.a.empty()) doc.set("samplers", samplers_);
    if (!accessors_.a.empty()) doc.set("accessors", accessors_);
    if (!views_.a.empty()) doc.set("bufferViews", views_);
    while (bin_.size() % 4) bin_.push_back(0);
    if (!bin_.empty()) doc.set("buffers", Json::array()).push(Json::object()).set("byteLength", (uint64_t)bin_.size());
    if (!sc_.lights.empty()) {
        Json lights = Json::array();
        for (auto& l : sc_.lights) {
            Json j = Json::object();
            j.set("name", l.name);
            static const char* types[] = {"point", "spot", "directional"};
            j.set("type", types[l.type]);
            j.set("color", Json::list(l.color[0], l.color[1], l.color[2]));
            j.set("intensity", l.intensity);
            if (l.type != 2 && l.range > 0) j.set("range", l.range);
            if (l.type == 1) {
                Json s = Json::object();
                s.set("innerConeAngle", l.inner);
                s.set("outerConeAngle", l.outer);
                j.set("spot", std::move(s));
            }
            lights.push(std::move(j));
        }
        doc.set("extensions", Json::object()).set("KHR_lights_punctual", Json::object()).set("lights", std::move(lights));
        extensions_.insert("KHR_lights_punctual");
    }
    if (!extensions_.empty()) {
        Json used = Json::array();
        for (auto& e : extensions_) used.push(e);
        doc.set("extensionsUsed", std::move(used));
    }

    std::string json;
    json.reserve(1 << 20);
    doc.dump(json);
    while (json.size() % 4) json += ' ';
    uint64_t total = 12 + 8 + json.size() + (bin_.empty() ? 0 : 8 + bin_.size());
    if (total > 0xffffffffull)
        throw std::runtime_error("the GLB would be " + std::to_string(total >> 20) +
                                 " MB, over glTF's 4 GB limit; use --external-textures or --max-texture-size");
    std::ofstream f(out_, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + out_.string());
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    u32(0x46546C67);
    u32(2);
    u32((uint32_t)total);
    u32((uint32_t)json.size());
    u32(0x4E4F534A);
    f.write(json.data(), (std::streamsize)json.size());
    if (!bin_.empty()) {
        u32((uint32_t)bin_.size());
        u32(0x004E4942);
        f.write((const char*)bin_.data(), (std::streamsize)bin_.size());
    }
    if (!f) throw std::runtime_error("failed writing " + out_.string());
    GltfResult r;
    r.bytes = total;
    r.nodes = nodes_.a.size();
    r.meshes = meshes_.a.size();
    r.materials = materials_.a.size();
    r.images = images_.a.size();
    return r;
}

} // namespace

GltfResult write_glb(const Scene& scene, const fs::path& out, const GltfOptions& options) {
    Writer w(scene, out, options);
    return w.write();
}

} // namespace xl
