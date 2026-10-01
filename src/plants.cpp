#include "plants.h"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace xl {

namespace {

std::vector<ObjRef> components(const Database& db, ObjRef go) {
    std::vector<ObjRef> out;
    Value v = db.read(go);
    for (auto& c : v["m_Component"].items) {
        ObjRef r = db.resolve(go.file, c.has("component") ? c["component"] : c["second"]);
        if (r.valid()) out.push_back(r);
    }
    return out;
}

class Counter {
public:
    explicit Counter(const Database& db) : db_(db) {}

    // Triangles a renderer draws (its mesh's submeshes).
    size_t renderer_triangles(ObjRef renderer) {
        if (!renderer.valid() || renderer.builtin()) return 0;
        int cls = db_.class_of(renderer);
        ObjRef mesh;
        Value rv = db_.read(renderer);
        if (cls == kSkinnedMeshRenderer) {
            mesh = db_.resolve(renderer.file, rv["m_Mesh"]);
        } else if (cls == kMeshRenderer) {
            ObjRef go = db_.resolve(renderer.file, rv["m_GameObject"]);
            if (!go.valid()) return 0;
            for (ObjRef c : components(db_, go))
                if (db_.class_of(c) == kMeshFilter) mesh = db_.resolve(c.file, db_.read(c)["m_Mesh"]);
        }
        return mesh_triangles(mesh);
    }

    size_t mesh_triangles(ObjRef mesh) {
        if (!mesh.valid() || mesh.builtin()) return 0;
        auto it = meshes_.find(mesh);
        if (it != meshes_.end()) return it->second;
        size_t n = 0;
        try {
            Value v = db_.read(mesh);  // hold it: the loop below reads inside it
            for (auto& sm : v["m_SubMeshes"].items) n += (size_t)sm["indexCount"].i64() / 3;
        } catch (const std::exception&) {
        }
        meshes_[mesh] = n;
        return n;
    }

    // Triangles per LOD level of a prefab: its first LODGroup's levels, else everything it renders.
    std::vector<size_t> prefab(ObjRef root) {
        std::vector<ObjRef> gos, renderers;
        ObjRef lod_group;
        std::vector<ObjRef> stack = {root};
        while (!stack.empty() && gos.size() < 4096) {
            ObjRef go = stack.back();
            stack.pop_back();
            gos.push_back(go);
            for (ObjRef c : components(db_, go)) {
                int cls = db_.class_of(c);
                if (cls == kLODGroup && !lod_group.valid()) lod_group = c;
                else if (cls == kMeshRenderer || cls == kSkinnedMeshRenderer) renderers.push_back(c);
                else if (cls == kTransform || cls == kRectTransform) {
                    Value xf = db_.read(c);
                    for (auto& child : xf["m_Children"].items) {
                        ObjRef t = db_.resolve(c.file, child);
                        if (!t.valid()) continue;
                        ObjRef g = db_.resolve(t.file, db_.read(t)["m_GameObject"]);
                        if (g.valid()) stack.push_back(g);
                    }
                }
            }
        }
        std::vector<size_t> levels;
        if (lod_group.valid()) {
            Value group = db_.read(lod_group);
            for (auto& lod : group["m_LODs"].items) {
                size_t n = 0;
                for (auto& r : lod["renderers"].items) n += renderer_triangles(db_.resolve(lod_group.file, r["renderer"]));
                levels.push_back(n);
            }
            // A level without meshes (culled, or a billboard) falls back a level, like the export.
            for (size_t i = 1; i < levels.size(); ++i)
                if (!levels[i]) levels[i] = levels[i - 1];
        }
        if (levels.empty()) {
            size_t n = 0;
            for (ObjRef r : renderers) n += renderer_triangles(r);
            levels.push_back(n);
        }
        return levels;
    }

private:
    const Database& db_;
    std::unordered_map<ObjRef, size_t, ObjRefHash> meshes_;
};

} // namespace

std::vector<PlantInfo> terrain_plants(const Database& db) {
    Counter counter(db);
    std::map<std::string, PlantInfo> by_name;
    for (ObjRef ref : db.objects_of(kTerrainData)) {
        try {
            Value td = db.read(ref);
            const Value& dd = td["m_DetailDatabase"];
            const auto& protos = dd["m_TreePrototypes"].items;
            std::vector<size_t> counts(protos.size(), 0);
            for (auto& t : dd["m_TreeInstances"].items) {
                int64_t i = t["index"].i64(-1);
                if (i >= 0 && i < (int64_t)counts.size()) ++counts[(size_t)i];
            }
            for (size_t k = 0; k < protos.size(); ++k) {
                ObjRef go = db.resolve(ref.file, protos[k]["prefab"]);
                if (!go.valid() || go.builtin()) continue;
                std::string name = db.read(go)["m_Name"].s();
                PlantInfo& p = by_name[name];
                p.name = name;
                p.placed += counts[k];
                if (p.lod_triangles.empty()) p.lod_triangles = counter.prefab(go);
            }
        } catch (const std::exception&) {
        }
    }
    std::vector<PlantInfo> out;
    for (auto& [n, p] : by_name) out.push_back(std::move(p));
    std::stable_sort(out.begin(), out.end(), [](const PlantInfo& a, const PlantInfo& b) { return a.placed > b.placed; });
    return out;
}

int plant_level(const PlantInfo& plant, int own, int tree_lod, int lod) {
    int level = own >= 0 ? own : tree_lod >= 0 ? tree_lod : lod;
    int last = std::max(0, (int)plant.lod_triangles.size() - 1);
    return std::clamp(level, 0, last);
}

} // namespace xl
