// BundleRipper: rips levels out of Unity asset bundles (Skater XL mod maps and other Unity
// games) to a binary glTF.
//
//   BundleRipper <map bundle or folder> [options]
//
// Dropping a map file on the exe writes "<map>.glb" next to it.
#include "curves_obj.h"
#include "gltf.h"
#include "images.h"
#include "log.h"
#include "scene.h"
#include "texture.h"
#include "unity.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace xl;

namespace {

const char* kUsage =
    "BundleRipper - rips levels out of Unity asset bundles (Skater XL maps and more) to .glb\n"
    "\n"
    "usage: BundleRipper <map file or folder> [options]\n"
    "\n"
    "  -o, --output <file.glb>   output path (default: <map>.glb next to the map)\n"
    "  --external-textures       write PNGs to <name>_textures\\ instead of inside the .glb\n"
    "  --max-texture-size <px>   use smaller mip levels for textures bigger than this\n"
    "  --no-textures             geometry and material colours only\n"
    "  --include-inactive        also export disabled GameObjects (tagged xl_inactive)\n"
    "  --all-lods                export every LOD level, not just LOD0\n"
    "  --no-colliders            leave out the collision-only <name>_col objects; the visible meshes\n"
    "                            then take over their collision (sk8_collision_mode)\n"
    "  --triggers                include trigger colliders\n"
    "  --keep-hierarchy          keep Unity's GameObject tree instead of flat world-space objects\n"
    "  --vertex-colors           keep mesh vertex colours (Blender multiplies them into base colour)\n"
    "  --standard-alpha          write cut-outs as glTF MASK (Blender adds Alpha Clip nodes) instead\n"
    "                            of BLEND tagged sk8_material.alpha = mask\n"
    "  --occlusion               link ambient occlusion too (Blender adds a glTF side node group)\n"
    "  --decals <mode>           project (default): clip decals onto the geometry,\n"
    "                            quad: one flat quad per projector, none: skip\n"
    "  --no-splines              leave out grind splines (otherwise written as curves to\n"
    "                            <name>_splines.obj; import it into the same Blender scene)\n"
    "  --spline-meshes           also put the splines in the .glb as line meshes\n"
    "  --no-autosplines          don't write <name>_autosplines.obj (grind lines found on rails,\n"
    "                            copings and ledges)\n"
    "  --autospline-all          look for grind lines on every collidable object\n"
    "  --terrain-resolution <n>  most grid squares per Unity terrain side (default 1024)\n"
    "  --no-trees                leave out trees painted on Unity terrains\n"
    "  --no-lights               leave out lights\n"
    "  --list                    print what the map contains and exit\n"
    "  --dump-textures <folder>  also save every texture as a PNG\n"
    "  -v, --verbose             more detail\n";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string u8(const fs::path& p) { return narrow(p.wstring()); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

bool owns_console() {
    DWORD ids[4];
    return GetConsoleProcessList(ids, 4) <= 1;
}

void list_contents(const Database& db) {
    for (auto& f : db.files) {
        std::map<int, size_t> counts;
        for (auto& o : f->objects) ++counts[o.class_id];
        log_info("%s  (Unity %s, %zu objects)", f->name.c_str(), f->unity_version.c_str(), f->objects.size());
        static const std::map<int, const char*> names = {
            {kGameObject, "GameObject"}, {kTransform, "Transform"}, {kMaterial, "Material"},
            {kMeshRenderer, "MeshRenderer"}, {kTexture2D, "Texture2D"}, {kMeshFilter, "MeshFilter"}, {kMesh, "Mesh"},
            {kShader, "Shader"}, {kMeshCollider, "MeshCollider"}, {kBoxCollider, "BoxCollider"}, {kLight, "Light"},
            {kMonoBehaviour, "MonoBehaviour"}, {kMonoScript, "MonoScript"}, {kSphereCollider, "SphereCollider"},
            {kCapsuleCollider, "CapsuleCollider"}, {kSkinnedMeshRenderer, "SkinnedMeshRenderer"},
            {kTerrainData, "TerrainData"}, {kTerrain, "Terrain"}, {kLODGroup, "LODGroup"}, {kRectTransform, "RectTransform"}};
        for (auto& [cls, n] : counts) {
            auto it = names.find(cls);
            log_info("  %6zu  %s", n, it != names.end() ? it->second : ("class " + std::to_string(cls)).c_str());
        }
    }
    std::map<std::string, size_t> scripts;
    for (auto& ref : db.objects_of(kMonoBehaviour)) {
        try {
            Value v = db.read(ref);
            ObjRef s = db.resolve(ref.file, v["m_Script"]);
            scripts[s.valid() ? db.read(s)["m_ClassName"].s() : "(missing script)"]++;
        } catch (const std::exception&) {
        }
    }
    if (!scripts.empty()) {
        log_info("scripts:");
        for (auto& [n, c] : scripts) log_info("  %6zu  %s", c, n.c_str());
    }
    log_info("textures:");
    for (auto& ref : db.objects_of(kTexture2D)) {
        try {
            TexInfo t = texture_info(db.read(ref));
            log_info("  %5d x %-5d %-14s %s", t.width, t.height, texture_format_name(t.format), t.name.c_str());
        } catch (const std::exception&) {
        }
    }
}

void dump_textures(const Database& db, const fs::path& dir, int max_size) {
    std::vector<ImageJob> jobs;
    for (auto& ref : db.objects_of(kTexture2D)) {
        ImageJob j;
        j.role = ImageJob::Raw;
        j.src[0] = ref;
        try {
            j.name = db.read(ref)["m_Name"].s();
        } catch (const std::exception&) {
        }
        jobs.push_back(std::move(j));
    }
    log_info("dumping %zu textures to %s", jobs.size(), u8(dir).c_str());
    run_image_jobs(db, jobs, max_size);
    fs::create_directories(dir);
    std::map<std::string, int> used;
    for (auto& j : jobs) {
        if (!j.ok) continue;
        std::string name = j.name.empty() ? "texture" : j.name;
        for (auto& c : name)
            if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
        int n = used[lower(name)]++;
        if (n) name += "_" + std::to_string(n + 1);
        std::ofstream f(dir / widen(name + ".png"), std::ios::binary);
        f.write((const char*)j.png.data(), (std::streamsize)j.png.size());
    }
}

int run(int argc, wchar_t** argv) {
    fs::path input, output, texture_dump;
    bool list = false;
    Options opt;
    GltfOptions gltf;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + narrow(a));
            return argv[++i];
        };
        if (a == L"-h" || a == L"--help" || a == L"/?") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == L"-o" || a == L"--output") output = next();
        else if (a == L"--external-textures") gltf.external_textures = true;
        else if (a == L"--vertex-colors") gltf.vertex_colors = true;
        else if (a == L"--standard-alpha") gltf.standard_alpha = true;
        else if (a == L"--occlusion") gltf.occlusion = true;
        else if (a == L"--max-texture-size") opt.max_texture_size = std::stoi(next());
        else if (a == L"--no-textures") opt.textures = false;
        else if (a == L"--include-inactive") opt.include_inactive = true;
        else if (a == L"--all-lods") opt.all_lods = true;
        else if (a == L"--colliders") opt.colliders = true;
        else if (a == L"--no-colliders") opt.colliders = false;
        else if (a == L"--keep-hierarchy") opt.flatten = false;
        else if (a == L"--triggers") opt.triggers = true;
        else if (a == L"--no-splines") opt.splines = false;
        else if (a == L"--spline-meshes") opt.spline_meshes = true;
        else if (a == L"--autosplines") opt.autosplines = 2;
        else if (a == L"--no-autosplines") opt.autosplines = 0;
        else if (a == L"--autospline-all") {
            opt.autosplines = 2;
            opt.autospline_all = true;
        }
        else if (a == L"--terrain-resolution") opt.terrain_resolution = std::stoi(next());
        else if (a == L"--no-trees") opt.trees = false;
        else if (a == L"--no-lights") opt.lights = false;
        else if (a == L"--list") list = true;
        else if (a == L"--dump-textures") texture_dump = next();
        else if (a == L"-v" || a == L"--verbose") verbose_logging() = true;
        else if (a == L"--decals") {
            std::wstring m = next();
            if (m == L"project") opt.decals = DecalMode::Project;
            else if (m == L"quad" || m == L"quads") opt.decals = DecalMode::Quad;
            else if (m == L"none") opt.decals = DecalMode::None;
            else throw std::runtime_error("--decals takes project, quad or none");
        } else if (!a.empty() && a[0] == L'-') {
            throw std::runtime_error("unknown option " + narrow(a) + " (see --help)");
        } else if (input.empty()) {
            input = a;
        } else {
            throw std::runtime_error("more than one input given: " + narrow(a));
        }
    }
    if (input.empty()) {
        std::fputs(kUsage, stdout);
        return 1;
    }
    if (!fs::exists(input)) throw std::runtime_error("not found: " + u8(input));
    input = fs::absolute(input).lexically_normal();
    if (!input.has_filename()) input = input.parent_path();
    if (output.empty()) {
        std::string ext = lower(u8(input.extension()));
        bool known = !fs::is_directory(input) && (ext == ".bundle" || ext == ".unity3d" || ext == ".assets" || ext == ".ab");
        output = input.parent_path() / ((known ? input.stem() : input.filename()).wstring() + L".glb");
    }

    auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    log_info("loading %s", u8(input).c_str());
    Database db;
    db.load(input);
    if (db.files.empty()) throw std::runtime_error("no Unity asset data found in " + u8(input));
    for (auto& f : db.files)
        if (!f->type_trees)
            throw std::runtime_error("'" + f->name + "' has no type trees (a stripped game build, not a mod bundle)");
    log_info("  %zu serialized file(s), Unity %s (%.1fs)", db.files.size(), db.files[0]->unity_version.c_str(), secs());
    if (list) {
        list_contents(db);
        return 0;
    }
    if (!texture_dump.empty()) dump_textures(db, texture_dump, opt.max_texture_size);

    log_info("building scene");
    Stats st;
    Scene scene = build_scene(db, opt, st);
    log_info("  %zu game objects, %zu renderers (%zu unbatched from static batches), %zu triangles", st.game_objects,
             st.renderers, st.batched, st.triangles);
    log_info("  skipped %zu inactive objects and %zu lower-LOD renderers", st.skipped_inactive, st.skipped_lods);
    log_info("  %zu colliders: %zu share their render mesh, %zu exported as _col objects%s", st.colliders,
             st.colliders_on_render, st.colliders_exported, opt.colliders ? "" : " (turned off with --no-colliders)");
    if (st.colliders_handed || st.colliders_unmatched)
        log_info("  %zu colliders on invisible objects handed their collision to the visible meshes they overlap; "
                 "%zu overlap nothing visible and are dropped", st.colliders_handed, st.colliders_unmatched);
    log_info("  %zu grind splines, %zu lights", st.splines, st.lights);
    if (opt.flatten)
        log_info("  flattened to %zu world-space objects%s", st.flattened,
                 st.baked_skew ? (" (" + std::to_string(st.baked_skew) + " sheared ones baked)").c_str() : "");
    if (opt.decals != DecalMode::None)
        log_info("  %zu decal projectors -> %zu decal triangles", st.decals, st.decal_triangles);
    if (st.builtin_meshes) log_info("  %zu Unity built-in primitive meshes rebuilt", st.builtin_meshes);
    if (st.projected) log_info("  %zu objects got UVs baked from HDRP planar/triplanar mapping", st.projected);
    if (st.missing_meshes) log_warn("%zu meshes could not be read", st.missing_meshes);
    if (st.empty_meshes) log_info("  %zu meshes are empty in the map itself (skipped)", st.empty_meshes);
    if (st.terrains) log_info("  %zu Unity terrain(s) exported as meshes, %zu terrain trees placed", st.terrains, st.trees);
    log_info("  (%.1fs)", secs());

    if (opt.textures) {
        log_info("converting %zu textures", scene.images.size());
        run_image_jobs(db, scene.images, opt.max_texture_size);
        resolve_texture_alpha(scene);
        log_info("  (%.1fs)", secs());
    } else {
        scene.images.clear();
        for (auto& m : scene.materials) m.base_tex = m.normal_tex = m.orm_tex = m.emissive_tex = {};
    }

    log_info("writing %s", u8(output).c_str());
    if (output.has_parent_path()) fs::create_directories(output.parent_path());
    GltfResult r = write_glb(scene, output, gltf);
    if (!scene.curves.empty()) {
        fs::path curves_path = output.parent_path() / (output.stem().wstring() + L"_splines.obj");
        size_t n = write_curves_obj(scene.curves, curves_path);
        log_info("wrote %zu grind splines as curves to %s", n, u8(curves_path).c_str());
    }
    if (!scene.auto_curves.empty()) {
        fs::path auto_path = output.parent_path() / (output.stem().wstring() + L"_autosplines.obj");
        size_t n = write_curves_obj(scene.auto_curves, auto_path);
        log_info("wrote %zu auto grind splines from %zu objects to %s", n, st.autospline_objects, u8(auto_path).c_str());
    }
    log_info("done: %zu nodes, %zu meshes, %zu materials, %zu images, %.1f MB in %.1fs", r.nodes, r.meshes, r.materials,
             r.images, r.bytes / 1048576.0, secs());
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool pause = owns_console();
    int code;
    try {
        code = run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stdout, "error: %s\n", e.what());
        code = 1;
    }
    if (pause) {
        std::fputs("\npress Enter to close\n", stdout);
        std::getchar();
    }
    return code;
}
