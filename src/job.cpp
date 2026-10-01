#include "job.h"
#include "curves_obj.h"
#include "images.h"
#include "log.h"
#include "studio_build.h"
#include "texture.h"
#include "unity.h"

#include <Windows.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;

namespace xl {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

namespace {

fs::path normalized_input(const fs::path& in) {
    fs::path input = fs::absolute(in).lexically_normal();
    if (!input.has_filename()) input = input.parent_path();
    return input;
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
    std::vector<PlantInfo> plants = terrain_plants(db);
    if (!plants.empty()) {
        log_info("terrain plants (names for --plant-lod):");
        for (auto& p : plants) {
            std::string lods;
            for (size_t i = 0; i < p.lod_triangles.size(); ++i)
                lods += (i ? ", " : "") + std::string("LOD") + std::to_string(i) + " " + std::to_string(p.lod_triangles[i]);
            log_info("  %6zu  %-32s %s", p.placed, p.name.c_str(), lods.c_str());
        }
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

} // namespace

fs::path output_path(const Job& job) {
    if (!job.output.empty()) return job.output;
    if (job.input.empty()) return {};
    fs::path input = normalized_input(job.input);
    std::string ext = lower(u8(input.extension()));
    std::error_code ec;
    bool known = !fs::is_directory(input, ec) &&
                 (ext == ".bundle" || ext == ".unity3d" || ext == ".assets" || ext == ".ab");
    return input.parent_path() / ((known ? input.stem() : input.filename()).wstring() + L".glb");
}

std::string mod_name(const Job& job) {
    std::string text = job.mod.name.empty() ? u8(output_path(job).stem()) : job.mod.name;
    std::string out;
    for (char c : text) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!keep && !out.empty() && out.back() == '_') continue;
        out += keep ? c : '_';
    }
    auto trim = [](char c) { return c == '_' || c == '.'; };
    while (!out.empty() && trim(out.front())) out.erase(out.begin());
    if (out.size() > 64) out.resize(64);
    while (!out.empty() && trim(out.back())) out.pop_back();
    return out;
}

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Loads the map's files; throws when there is nothing usable.
void load_database(const Job& job, Database& db, Clock::time_point t0) {
    if (job.input.empty()) throw std::runtime_error("no map given");
    if (!fs::exists(job.input)) throw std::runtime_error("not found: " + u8(job.input));
    fs::path input = normalized_input(job.input);
    verbose_logging() = job.verbose;
    log_info("loading %s", u8(input).c_str());
    db.load(input);
    if (db.files.empty()) throw std::runtime_error("no Unity asset data found in " + u8(input));
    for (auto& f : db.files)
        if (!f->type_trees)
            throw std::runtime_error("'" + f->name + "' has no type trees (a stripped game build, not a mod bundle)");
    log_info("  %zu serialized file(s), Unity %s (%.1fs)", db.files.size(), db.files[0]->unity_version.c_str(), since(t0));
}

// Builds the scene and optimizes its meshes, logging what it found.
Scene build_and_optimize(const Job& job, const Database& db, Stats& st, Clock::time_point t0) {
    const Options& opt = job.opt;
    auto secs = [&] { return since(t0); };
    log_info("building scene");
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

    OptimizeOptions optimize = job.optimize;
    optimize.keep_colors = job.gltf.vertex_colors;
    if (optimize.enabled) {
        if (optimize.max_error > 0) log_info("optimizing meshes (simplify within %g mm)", optimize.max_error * 1000);
        else log_info("optimizing meshes (lossless)");
        OptimizeStats os;
        optimize_meshes(scene, optimize, os);
        auto pct = [](size_t a, size_t b) { return b ? 100.0 * (double)a / (double)b : 100.0; };
        log_info("  %zu meshes: %zu -> %zu vertices (%.0f%%), %zu -> %zu triangles (%.0f%%); %zu simplified",
                 os.meshes, os.vertices_before, os.vertices_after, pct(os.vertices_after, os.vertices_before),
                 os.triangles_before, os.triangles_after, pct(os.triangles_after, os.triangles_before), os.simplified);
        log_info("  triangles drawn over every placed copy: %zu -> %zu (%.0f%%)", os.drawn_before, os.drawn_after,
                 pct(os.drawn_after, os.drawn_before));
        if (os.kept_whole)
            log_verbose("  %zu submeshes kept whole: no simplification of them stayed within the limit", os.kept_whole);
        log_info("  (%.1fs)", secs());
    }
    return scene;
}

} // namespace

JobResult run_job(const Job& job) {
    auto t0 = Clock::now();
    auto secs = [&] { return since(t0); };
    const Options& opt = job.opt;
    JobResult result;
    // A mod build needs Blender, Studio and the game; find them before the slow part.
    bool mod = job.build_mod && !job.list;
    StudioTools tools;
    std::string name;
    if (mod) {
        name = mod_name(job);
        if (name.empty()) throw std::runtime_error("the mod needs a name (letters or digits)");
        tools = find_studio_tools(job.mod);
        if (!tools.missing.empty()) throw std::runtime_error("cannot build the mod, " + tools.missing);
        if (job.mod.deploy && skate_running())
            throw std::runtime_error("Skate is running: close it before installing the mod (or turn off Install into Skate)");
        log_info("building the Skate mod \"%s\" with ReSkate Studio", name.c_str());
        log_verbose("Blender: %s", u8(tools.blender).c_str());
        log_verbose("Studio: %s", u8(tools.cli).c_str());
        log_verbose("Skate: %s", u8(tools.game).c_str());
    }
    Database db;
    load_database(job, db, t0);
    if (job.list) {
        list_contents(db);
        result.seconds = secs();
        return result;
    }
    if (!job.texture_dump.empty()) dump_textures(db, job.texture_dump, opt.max_texture_size);
    // Map edits: the window's, an --edits file, or the ones saved beside the map
    std::shared_ptr<const Edits> edits = job.edits;
    if (!edits && !job.no_edits) {
        fs::path file = job.edits_path.empty() ? existing_edits_file(normalized_input(job.input)) : job.edits_path;
        std::error_code ec;
        if (fs::exists(file, ec)) {
            auto loaded = std::make_shared<Edits>();
            std::string why;
            if (!load_edits(file, *loaded, why)) {
                if (!job.edits_path.empty()) throw std::runtime_error("cannot read " + u8(file) + ": " + why);
                log_warn("ignoring %s: %s", u8(file).c_str(), why.c_str());
            } else {
                edits = loaded;
                log_info("applying %zu map edits from %s", edits->count(), u8(file).c_str());
            }
        } else if (!job.edits_path.empty()) {
            throw std::runtime_error("not found: " + u8(file));
        }
    }
    Job with_edits = job;
    with_edits.opt.edits = edits && !edits->empty() ? edits.get() : nullptr;
    Stats st;
    Scene scene = build_and_optimize(with_edits, db, st, t0);
    fs::path output = output_path(job);
    fs::path stage;
    if (mod) {
        stage = build_folder(name);
        fs::create_directories(stage);
        output = stage / (widen(name) + L".glb");
    }

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
    GltfResult r = write_glb(scene, output, job.gltf);
    // The map's own sky, as a panorama next to a .glb (Blender: an Environment Texture). Never in
    // a mod: Skate draws its own sky for each time of day.
    if (!mod) {
        SkyImage sky;
        std::string why;
        if (extract_sky(db, 4096, sky, why)) {
            Image bottom_up = sky.img;  // encode_png flips Unity's bottom-up rows
            size_t row = (size_t)sky.img.w * 4;
            for (int y = 0; y < sky.img.h; ++y)
                std::copy_n(&sky.img.px[(size_t)y * row], row, &bottom_up.px[(size_t)(sky.img.h - 1 - y) * row]);
            std::vector<uint8_t> png = encode_png(bottom_up, 3);
            fs::path sky_png = output.parent_path() / (output.stem().wstring() + L"_sky.png");
            std::ofstream f(sky_png, std::ios::binary);
            f.write((const char*)png.data(), (std::streamsize)png.size());
            log_info("wrote the map's sky (%s) to %s", sky.source.c_str(), u8(sky_png).c_str());
        } else {
            log_verbose("no sky picture: %s", why.c_str());
        }
    }
    // The splines left switched on, and the grind curves drawn in the window
    std::vector<ExportCurve> curves = export_curves(scene, with_edits.opt.edits);
    if (mod) {
        log_info("  %zu nodes, %zu meshes, %zu materials, %zu images, %.1f MB (%.1fs)", r.nodes, r.meshes, r.materials, r.images,
                 r.bytes / 1048576.0, secs());
        fs::path curves_file = stage / L"curves.json";
        std::vector<ExportCurve> kept;
        for (auto& c : curves)
            if (c.npc || c.grind.enabled) kept.push_back(c);
        write_curves_json(kept, curves_file);
        fs::path blend = stage / (widen(name) + L".blend");
        log_info("making %s in Blender (%zu curves)", u8(blend.filename()).c_str(), kept.size());
        make_blend(tools, output, curves_file, blend);
        log_info("  (%.1fs)", secs());
        if (job.mod.keep_blend) {
            fs::path copy = output_path(job).replace_extension(L".blend");
            fs::copy_file(blend, copy, fs::copy_options::overwrite_existing);
            log_info("kept %s", u8(copy).c_str());
        }
        fs::path package = job.mod.package.empty() ? stage / L"package" : job.mod.package;
        log_info("compiling the mod with ReSkate Studio into %s", u8(package).c_str());
        compile_mod(tools, job.mod, name, blend, package);
        result.mod = true;
        result.output = package;
        if (job.mod.deploy) result.deployed = tools.game / L"Mods" / widen(name);
        result.bytes = r.bytes;
        result.seconds = secs();
        if (job.mod.deploy)
            log_info("done: installed into %s in %.1fs (start Skate to play it)", u8(result.deployed).c_str(), result.seconds);
        else
            log_info("done: the mod is in %s (%.1fs)", u8(package).c_str(), result.seconds);
        return result;
    }
    std::vector<OutCurve> grind, automatic;
    for (auto& c : curves)
        if (!c.npc && c.grind.enabled) (c.automatic ? automatic : grind).push_back(c.curve);
    if (!grind.empty()) {
        fs::path curves_path = output.parent_path() / (output.stem().wstring() + L"_splines.obj");
        size_t n = write_curves_obj(grind, curves_path);
        log_info("wrote %zu grind splines as curves to %s", n, u8(curves_path).c_str());
    }
    if (!automatic.empty()) {
        fs::path auto_path = output.parent_path() / (output.stem().wstring() + L"_autosplines.obj");
        size_t n = write_curves_obj(automatic, auto_path);
        log_info("wrote %zu auto grind splines from %zu objects to %s", n, st.autospline_objects, u8(auto_path).c_str());
    }
    result.output = output;
    result.bytes = r.bytes;
    result.seconds = secs();
    log_info("done: %zu nodes, %zu meshes, %zu materials, %zu images, %.1f MB in %.1fs", r.nodes, r.meshes, r.materials,
             r.images, r.bytes / 1048576.0, result.seconds);
    return result;
}

std::vector<PlantInfo> scan_plants(const Job& job) {
    auto t0 = Clock::now();
    Database db;
    load_database(job, db, t0);
    log_info("scanning terrain plants");
    std::vector<PlantInfo> plants = terrain_plants(db);
    size_t placed = 0;
    for (auto& p : plants) placed += p.placed;
    log_info("done: %zu plant types, %zu placed (%.1fs)", plants.size(), placed, since(t0));
    return plants;
}

Preview load_preview(const Job& job_in, int texture_size) {
    auto t0 = Clock::now();
    // The view keeps Unity's tree (for its object list) and every LOD level, tagged, so LOD
    // choices switch live instead of needing a reload.
    Job job = job_in;
    job.opt.flatten = false;
    job.opt.tag_lods = true;
    job.opt.all_lods = false;
    Preview p;
    p.name = u8(normalized_input(job.input).filename());
    Database db;
    load_database(job, db, t0);
    Stats st;
    p.plants = terrain_plants(db);
    {
        std::string why;
        if (extract_sky(db, 2048, p.sky, why)) log_info("  sky: %s", p.sky.source.c_str());
        else log_verbose("no sky picture: %s", why.c_str());
    }
    p.scene = build_and_optimize(job, db, st, t0);
    Scene& scene = p.scene;
    if (job.opt.textures) {
        // Only what the viewport samples: base colour (with cut-out detection) and emissive.
        std::vector<bool> wanted(scene.images.size(), false);
        for (auto& m : scene.materials)
            for (const TexRef* t : {&m.base_tex, &m.emissive_tex})
                if (t->image >= 0 && t->image < (int)wanted.size()) wanted[t->image] = true;
        std::vector<ImageJob> jobs;
        std::vector<size_t> index;
        for (size_t i = 0; i < scene.images.size(); ++i)
            if (wanted[i]) {
                jobs.push_back(scene.images[i]);
                jobs.back().keep_pixels = true;
                index.push_back(i);
            }
        int size = job.opt.max_texture_size > 0 ? std::min(job.opt.max_texture_size, texture_size) : texture_size;
        log_info("converting %zu textures", jobs.size());
        run_image_jobs(db, jobs, size);
        for (size_t k = 0; k < jobs.size(); ++k) scene.images[index[k]] = std::move(jobs[k]);
        resolve_texture_alpha(scene);
    } else {
        for (auto& m : scene.materials) m.base_tex = m.normal_tex = m.orm_tex = m.emissive_tex = {};
    }
    p.seconds = since(t0);
    log_info("done: loaded %zu objects in %.1fs", scene.nodes.size(), p.seconds);
    return p;
}

} // namespace xl
