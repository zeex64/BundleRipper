#include "options.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cwctype>
#include <stdexcept>

namespace fs = std::filesystem;

namespace xl {

namespace {

using Get = std::function<double(const Job&)>;
using Set = std::function<void(Job&, double)>;
using Enabled = std::function<bool(const Job&)>;

OptionDef toggle(const char* group, const char* label, const char* help, const char* on, const char* off,
                 std::function<bool&(Job&)> field, Enabled enabled = nullptr) {
    OptionDef d;
    d.group = group, d.label = label, d.help = help, d.kind = OptionDef::Toggle, d.flag = on, d.off_flag = off;
    d.get = [field](const Job& j) { return field(const_cast<Job&>(j)) ? 1.0 : 0.0; };
    d.set = [field](Job& j, double v) { field(j) = v != 0; };
    d.enabled = std::move(enabled);
    return d;
}

OptionDef choice(const char* group, const char* label, const char* help, const char* flag, const char* value_name,
                 std::vector<OptionDef::Item> items, Get get, Set set, Enabled enabled = nullptr) {
    OptionDef d;
    d.group = group, d.label = label, d.help = help, d.kind = OptionDef::Choice, d.flag = flag;
    d.value_name = value_name, d.items = std::move(items), d.get = std::move(get), d.set = std::move(set);
    d.enabled = std::move(enabled);
    return d;
}

OptionDef path_option(const char* group, const char* label, const char* help, OptionDef::Kind kind, const char* flag,
                      const char* value_name, const char* placeholder, std::function<fs::path&(Job&)> field,
                      Enabled enabled = nullptr) {
    OptionDef d;
    d.group = group, d.label = label, d.help = help, d.kind = kind, d.flag = flag, d.value_name = value_name;
    d.placeholder = placeholder, d.path = std::move(field), d.enabled = std::move(enabled);
    return d;
}

OptionDef off_help(OptionDef d, const char* text) {
    d.off_help = text;
    return d;
}

const std::vector<OptionDef::Item> kLodItems = {
    {0, "LOD0 (full detail)"}, {1, "LOD1"}, {2, "LOD2"}, {3, "LOD3"}, {99, "Lowest the map has", "lowest"}};

std::vector<OptionDef> build_table() {
    std::vector<OptionDef> t;
    const Enabled mod = [](const Job& j) { return j.build_mod; };

    // Skate mod
    t.push_back(toggle("Skate mod", "Build a Skate mod",
                       "Build a Skate mod with ReSkate Studio instead of writing a .glb: the map goes through Blender "
                       "into a .blend (with the map edits, grind curves and NPC routes) and Studio's compile-map turns "
                       "that into the mod. Needs Blender and ReSkate Studio; they are found from Studio's settings.",
                       "--build-mod", nullptr, [](Job& j) -> bool& { return j.build_mod; }));
    {
        OptionDef d;
        d.group = "Skate mod", d.label = "Mod name", d.kind = OptionDef::Text;
        d.help = "The mod's folder name in Skate's Mods folder (letters, digits, - and .).";
        d.flag = "--mod-name", d.value_name = "<name>", d.placeholder = "the map's name";
        d.text = [](Job& j) -> std::string& { return j.mod.name; };
        d.enabled = mod;
        t.push_back(std::move(d));
    }
    t.push_back(off_help(toggle("Skate mod", "Install into Skate",
                                "Put the built mod in Skate's Mods folder so it is there the next time Skate starts "
                                "(Skate must be closed).",
                                nullptr, "--no-deploy", [](Job& j) -> bool& { return j.mod.deploy; }, mod),
                         "build the mod without installing it into Skate"));
    t.push_back(choice("Skate mod", "Pause map", "The map picture in the pause menu: a 3D view or a flat 2D one.",
                       "--pause-map", "<kind>", {{0, "3D", "3d"}, {1, "2D", "2d"}},
                       [](const Job& j) { return (double)j.mod.pause_map; },
                       [](Job& j, double v) { j.mod.pause_map = v != 0 ? 1 : 0; }, mod));
    t.push_back(choice("Skate mod", "Time of day", "The time of day the map starts at.", "--time-of-day", "<time>",
                       {{0, "Studio's default", "default"}, {1, "Morning", "morning"}, {2, "Noon", "noon"},
                        {3, "Afternoon", "afternoon"}, {4, "Evening", "evening"}, {5, "Night", "night"}},
                       [](const Job& j) { return (double)j.mod.time_of_day; },
                       [](Job& j, double v) { j.mod.time_of_day = std::clamp((int)v, 0, 5); }, mod));
    t.push_back(off_help(toggle("Skate mod", "Global illumination", "Bake bounced light for the map (slower builds).",
                                nullptr, "--no-gi", [](Job& j) -> bool& { return j.mod.gi; }, mod),
                         "skip baking bounced light (faster builds, flatter lighting)"));
    t.push_back(choice("Skate mod", "World streaming",
                       "Load a big map in cells around the player. Automatic: Studio decides from the map's size.",
                       "--streaming", "<mode>", {{0, "Automatic", "auto"}, {1, "On", "on"}, {2, "Off", "off"}},
                       [](const Job& j) { return (double)j.mod.streaming; },
                       [](Job& j, double v) { j.mod.streaming = std::clamp((int)v, 0, 2); }, mod));
    t.push_back(off_help(toggle("Skate mod", "Mesh LODs", "Let Studio make lower-detail meshes for distant objects.",
                                nullptr, "--no-mod-lods", [](Job& j) -> bool& { return j.mod.lods; }, mod),
                         "don't let Studio make lower-detail meshes"));
    t.push_back(toggle("Skate mod", "Keep the .blend",
                       "Also save <map>.blend next to the map, to open in Blender and build from ReSkate Studio later.",
                       "--keep-blend", nullptr, [](Job& j) -> bool& { return j.mod.keep_blend; }, mod));
    t.push_back(path_option("Skate mod tools", "Package folder",
                            "Where Studio builds the mod. Default: %LOCALAPPDATA%\\Spotbuilder\\Builds\\<name>\\package.",
                            OptionDef::Folder, "--package", "<folder>", "Automatic",
                            [](Job& j) -> fs::path& { return j.mod.package; }, mod));
    {
        OptionDef d = path_option("Skate mod tools", "Blender", "blender.exe. Default: the one ReSkate Studio uses.",
                                  OptionDef::File, "--blender", "<blender.exe>", "Found automatically",
                                  [](Job& j) -> fs::path& { return j.mod.blender; }, mod);
        d.filter = L"blender.exe";
        t.push_back(std::move(d));
    }
    {
        OptionDef d = path_option("Skate mod tools", "ReSkate Studio",
                                  "Studio's command line, reskate_cli.exe (next to ReSkate Studio.exe). Default: "
                                  "found from a running Studio or the usual install places.",
                                  OptionDef::File, "--studio-cli", "<reskate_cli.exe>", "Found automatically",
                                  [](Job& j) -> fs::path& { return j.mod.studio_cli; }, mod);
        d.filter = L"reskate_cli.exe";
        t.push_back(std::move(d));
    }
    t.push_back(path_option("Skate mod tools", "Skate folder", "Skate's game folder. Default: the one ReSkate Studio uses.",
                            OptionDef::Folder, "--game", "<folder>", "Found automatically",
                            [](Job& j) -> fs::path& { return j.mod.game; }, mod));

    // Textures
    t.push_back(off_help(toggle("Textures", "Textures",
                       "Convert the map's textures. Off: geometry and material colours only (quick test rips).",
                       nullptr, "--no-textures", [](Job& j) -> bool& { return j.opt.textures; }),
                       "geometry and material colours only, no textures (quick test rips)"));
    t.push_back(toggle("Textures", "Separate PNG files",
                       "Write the textures as PNGs in a <name>_textures folder next to the .glb instead of inside it.",
                       "--external-textures", nullptr, [](Job& j) -> bool& { return j.gltf.external_textures; },
                       [](const Job& j) { return j.opt.textures; }));
    t.push_back(choice("Textures", "Max texture size", "Use smaller mip levels for textures bigger than this.",
                       "--max-texture-size", "<px>",
                       {{0, "Full size"}, {8192, "8192 px"}, {4096, "4096 px"}, {2048, "2048 px"}, {1024, "1024 px"},
                        {512, "512 px"}},
                       [](const Job& j) { return (double)j.opt.max_texture_size; },
                       [](Job& j, double v) { j.opt.max_texture_size = std::max(0, (int)v); },
                       [](const Job& j) { return j.opt.textures; }));

    // Objects
    t.push_back(off_help(toggle("Objects", "Collision objects",
                       "Collider-only geometry as <name>_col objects. Off: the visible meshes take over their "
                       "collision (sk8_collision_mode).",
                       "--colliders", "--no-colliders", [](Job& j) -> bool& { return j.opt.colliders; }),
                       "leave out the collision-only <name>_col objects; the visible meshes then take over their "
                       "collision (sk8_collision_mode)"));
    t.push_back(toggle("Objects", "Trigger colliders", "Include trigger colliders too.", "--triggers", nullptr,
                       [](Job& j) -> bool& { return j.opt.triggers; }));
    t.push_back(toggle("Objects", "Hidden objects",
                       "Also export disabled GameObjects (tagged xl_inactive). Some maps hide optional parts, like "
                       "graffiti toggled from an in-game menu, this way.",
                       "--include-inactive", nullptr, [](Job& j) -> bool& { return j.opt.include_inactive; }));
    t.push_back(off_help(toggle("Objects", "Lights", "Export lights (KHR_lights_punctual).", nullptr, "--no-lights",
                                [](Job& j) -> bool& { return j.opt.lights; }),
                         "leave out lights"));
    t.push_back(choice("Objects", "Decals",
                       "Projected: decals cut onto the geometry under them. Flat quads: one quad per projector.",
                       "--decals", "<mode>",
                       {{(int)DecalMode::Project, "Projected onto surfaces", "project"},
                        {(int)DecalMode::Quad, "Flat quads", "quad"},
                        {(int)DecalMode::None, "Off", "none"}},
                       [](const Job& j) { return (double)(int)j.opt.decals; },
                       [](Job& j, double v) { j.opt.decals = (DecalMode)(int)v; }));

    // Terrain
    t.push_back(off_help(toggle("Terrain", "Trees and grass", "Place the trees and grass painted on Unity terrains.",
                                nullptr, "--no-trees", [](Job& j) -> bool& { return j.opt.trees; }),
                         "leave out the trees and grass painted on Unity terrains"));
    {
        OptionDef d;
        d.group = "Terrain", d.label = "Terrain resolution", d.kind = OptionDef::Integer;
        d.help = "Most grid squares per Unity terrain side.";
        d.flag = "--terrain-resolution", d.value_name = "<n>", d.min = 16, d.max = 4096, d.unit = "squares";
        d.get = [](const Job& j) { return (double)j.opt.terrain_resolution; };
        d.set = [](Job& j, double v) { j.opt.terrain_resolution = std::max(1, (int)v); };
        t.push_back(std::move(d));
    }

    // Level of detail
    t.push_back(choice("Level of detail", "LOD level",
                       "Which level of every LODGroup to export. A level past a group's last picks its last.",
                       "--lod", "<n>", kLodItems, [](const Job& j) { return (double)j.opt.lod; },
                       [](Job& j, double v) { j.opt.lod = std::max(0, (int)v); },
                       [](const Job& j) { return !j.opt.all_lods; }));
    {
        auto items = kLodItems;
        items.insert(items.begin(), {-1, "Same as LOD level", "same"});
        t.push_back(choice("Level of detail", "Trees and grass LOD",
                           "LOD level for the trees and grass painted on terrains. Their lower levels cut foliage a "
                           "lot (The Lost Loop: 42M to 8M triangles at the lowest).",
                           "--tree-lod", "<n>", std::move(items), [](const Job& j) { return (double)j.opt.tree_lod; },
                           [](Job& j, double v) { j.opt.tree_lod = std::max(-1, (int)v); },
                           [](const Job& j) { return !j.opt.all_lods && j.opt.trees; }));
    }
    t.push_back(toggle("Level of detail", "Every LOD level", "Export all LOD levels instead of one.", "--all-lods",
                       nullptr, [](Job& j) -> bool& { return j.opt.all_lods; }));

    // Mesh optimisation
    t.push_back(off_help(toggle("Mesh optimisation", "Optimise meshes",
                       "Merge duplicate vertices, drop data nothing uses and reorder for the GPU (lossless), then "
                       "simplify within the limit below.",
                       nullptr, "--no-optimize", [](Job& j) -> bool& { return j.optimize.enabled; }),
                       "write meshes exactly as decoded (no clean-up or simplification)"));
    {
        OptionDef d;
        d.group = "Mesh optimisation", d.label = "Simplify limit", d.kind = OptionDef::Number;
        d.help = "Drop vertices only where the surface moves less than this, with normals and UVs checked "
                 "against the original too. 0: lossless clean-up only.";
        d.flag = "--simplify", d.off_flag = "--no-simplify", d.value_name = "<mm>", d.min = 0, d.max = 20, d.unit = "mm";
        d.get = [](const Job& j) { return j.optimize.max_error * 1000.0; };
        d.set = [](Job& j, double v) { j.optimize.max_error = std::max(0.0, v) / 1000.0; };
        d.enabled = [](const Job& j) { return j.optimize.enabled; };
        t.push_back(std::move(d));
    }

    // Grind splines
    t.push_back(off_help(toggle("Grind splines", "Map splines",
                       "The map's own grind splines, as curves in <name>_splines.obj (import it into the same "
                       "Blender scene).",
                       nullptr, "--no-splines", [](Job& j) -> bool& { return j.opt.splines; }),
                       "leave out the map's own grind splines (otherwise curves in <name>_splines.obj)"));
    t.push_back(toggle("Grind splines", "Also as line meshes", "Put the splines in the .glb as line meshes too.",
                       "--spline-meshes", nullptr, [](Job& j) -> bool& { return j.opt.spline_meshes; },
                       [](const Job& j) { return j.opt.splines; }));
    {
        OptionDef d;  // Options::autosplines is 0 (never) or 2 (always)
        d.group = "Grind splines", d.label = "Auto grind splines", d.kind = OptionDef::Toggle;
        d.help = "Find grind lines on rails, copings and ledges, written to <name>_autosplines.obj.";
        d.flag = "--autosplines", d.off_flag = "--no-autosplines";
        d.off_help = "don't write <name>_autosplines.obj (grind lines found on rails, copings and ledges)";
        d.get = [](const Job& j) { return j.opt.autosplines ? 1.0 : 0.0; };
        d.set = [](Job& j, double v) {
            j.opt.autosplines = v != 0 ? 2 : 0;
            if (v == 0) j.opt.autospline_all = false;
        };
        t.push_back(std::move(d));
    }
    {
        OptionDef d = toggle("Grind splines", "Search every object",
                             "Look for grind lines on every collidable object, not just grindable-looking ones.",
                             "--autospline-all", nullptr, [](Job& j) -> bool& { return j.opt.autospline_all; },
                             [](const Job& j) { return j.opt.autosplines != 0; });
        d.get = [](const Job& j) { return j.opt.autosplines && j.opt.autospline_all ? 1.0 : 0.0; };
        d.set = [](Job& j, double v) {
            j.opt.autospline_all = v != 0;
            if (v != 0) j.opt.autosplines = 2;
        };
        t.push_back(std::move(d));
    }

    // Blender
    {
        OptionDef d = toggle("Blender", "Keep hierarchy",
                             "Keep Unity's GameObject tree instead of flat world-space objects.", "--keep-hierarchy",
                             nullptr, [](Job& j) -> bool& { return j.opt.flatten; });
        d.get = [](const Job& j) { return j.opt.flatten ? 0.0 : 1.0; };
        d.set = [](Job& j, double v) { j.opt.flatten = v == 0; };
        t.push_back(std::move(d));
    }
    t.push_back(toggle("Blender", "Vertex colours",
                       "Keep mesh vertex colours (Blender multiplies them into the base colour).", "--vertex-colors",
                       nullptr, [](Job& j) -> bool& { return j.gltf.vertex_colors; }));
    t.push_back(toggle("Blender", "Standard alpha",
                       "Write cut-outs as glTF MASK (Blender adds Alpha Clip nodes) instead of BLEND tagged "
                       "sk8_material.alpha = mask.",
                       "--standard-alpha", nullptr, [](Job& j) -> bool& { return j.gltf.standard_alpha; }));
    t.push_back(toggle("Blender", "Ambient occlusion", "Link ambient occlusion (Blender adds a glTF side node group).",
                       "--occlusion", nullptr, [](Job& j) -> bool& { return j.gltf.occlusion; }));

    // Extras
    {
        OptionDef d;
        d.group = "Extras", d.label = "Dump textures to", d.kind = OptionDef::Folder;
        d.help = "Also save every texture in the map as a PNG in this folder.";
        d.flag = "--dump-textures", d.value_name = "<folder>", d.placeholder = "Off";
        d.path = [](Job& j) -> fs::path& { return j.texture_dump; };
        t.push_back(std::move(d));
    }
    t.push_back(toggle("Extras", "Detailed log", "More detail in the log.", "--verbose", nullptr,
                       [](Job& j) -> bool& { return j.verbose; }));
    for (auto& d : t) {
        std::string group = d.group, flag = d.flag ? d.flag : "";
        if (flag == "--build-mod") d.format = -1;
        else if (group == "Skate mod" || group == "Skate mod tools") d.format = 2;
        else if (group == "Blender" || flag == "--external-textures" || flag == "--spline-meshes") d.format = 1;
    }
    return t;
}

bool same_flag(const std::wstring& a, const char* flag) {
    if (!flag) return false;
    size_t n = std::char_traits<char>::length(flag);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (a[i] != (wchar_t)flag[i]) return false;
    return true;
}

bool same_name(const std::wstring& a, const char* name) {
    if (!name) return false;
    size_t n = std::char_traits<char>::length(name);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::towlower(a[i]) != (wchar_t)std::tolower((unsigned char)name[i])) return false;
    return true;
}

std::wstring wide_ascii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string format_number(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

const std::vector<OptionDef>& option_table() {
    static const std::vector<OptionDef> table = build_table();
    return table;
}

ParsedArgs parse_args(const std::vector<std::wstring>& args) {
    ParsedArgs r;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto next = [&]() -> const std::wstring& {
            if (i + 1 >= args.size()) throw std::runtime_error("missing value after " + narrow(a));
            return args[++i];
        };
        if (a == L"-h" || a == L"--help" || a == L"/?") {
            r.help = true;
            continue;
        }
        if (a == L"--gui") {
            r.gui = true;
            continue;
        }
        if (a == L"-o" || a == L"--output") {
            r.job.output = next();
            continue;
        }
        if (a == L"--list") {
            r.job.list = true;
            continue;
        }
        if (a == L"-v") {
            r.job.verbose = true;
            continue;
        }
        if (a == L"--edits") {
            r.job.edits_path = next();
            continue;
        }
        if (a == L"--no-edits") {
            r.job.no_edits = true;
            continue;
        }
        if (a == L"--plant-lod") {  // <name>=<level|lowest|default>, once per plant type
            std::wstring v = next();
            size_t eq = v.rfind(L'=');
            if (eq == std::wstring::npos || eq == 0) throw std::runtime_error("--plant-lod takes <name>=<level>");
            std::string name = narrow(v.substr(0, eq));
            std::wstring level = v.substr(eq + 1);
            if (same_name(level, "default") || same_name(level, "same")) r.job.opt.plant_lod.erase(name);
            else if (same_name(level, "lowest")) r.job.opt.plant_lod[name] = 99;
            else {
                try {
                    r.job.opt.plant_lod[name] = std::max(0, std::stoi(level));
                } catch (const std::exception&) {
                    throw std::runtime_error("bad level in --plant-lod " + narrow(v));
                }
            }
            continue;
        }
        const OptionDef* hit = nullptr;
        bool off = false;
        for (auto& d : option_table()) {
            if (same_flag(a, d.flag)) hit = &d;
            else if (same_flag(a, d.off_flag)) hit = &d, off = true;
            if (hit) break;
        }
        if (hit) {
            const OptionDef& d = *hit;
            try {
                switch (d.kind) {
                case OptionDef::Toggle: d.set(r.job, off ? 0 : 1); break;
                case OptionDef::Number: d.set(r.job, off ? 0 : std::stod(next())); break;
                case OptionDef::Integer: d.set(r.job, std::stoi(next())); break;
                case OptionDef::Folder:
                case OptionDef::File: d.path(r.job) = next(); break;
                case OptionDef::Text: d.text(r.job) = narrow(next()); break;
                case OptionDef::Choice: {
                    const std::wstring& v = next();
                    const OptionDef::Item* item = nullptr;
                    bool numeric = false;
                    for (auto& it : d.items) {
                        if (same_name(v, it.cli)) item = &it;
                        numeric = numeric || !it.cli;
                    }
                    if (item) d.set(r.job, item->value);
                    else if (numeric) d.set(r.job, std::stoi(v));
                    else {
                        std::string names;
                        for (auto& it : d.items) names += std::string(names.empty() ? "" : ", ") + it.cli;
                        throw std::runtime_error(std::string(d.flag) + " takes " + names);
                    }
                    break;
                }
                }
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("bad value for " + narrow(a));
            } catch (const std::out_of_range&) {
                throw std::runtime_error("bad value for " + narrow(a));
            }
            continue;
        }
        if (!a.empty() && a[0] == L'-') throw std::runtime_error("unknown option " + narrow(a) + " (see --help)");
        if (!r.job.input.empty()) throw std::runtime_error("more than one input given: " + narrow(a));
        r.job.input = a;
    }
    return r;
}

std::vector<std::wstring> job_args(const Job& job) {
    std::vector<std::wstring> out;
    if (!job.input.empty()) out.push_back(job.input.wstring());
    if (!job.output.empty()) {
        out.push_back(L"-o");
        out.push_back(job.output.wstring());
    }
    if (!job.edits_path.empty()) {
        out.push_back(L"--edits");
        out.push_back(job.edits_path.wstring());
    }
    if (job.no_edits) out.push_back(L"--no-edits");
    for (auto& [name, level] : job.opt.plant_lod) {
        out.push_back(L"--plant-lod");
        out.push_back(widen(name) + L"=" + (level >= 99 ? std::wstring(L"lowest") : std::to_wstring(level)));
    }
    const Job defaults;
    for (auto& d : option_table()) {
        if (d.is_path()) {
            fs::path& p = d.path(const_cast<Job&>(job));
            if (!p.empty()) {
                out.push_back(wide_ascii(d.flag));
                out.push_back(p.wstring());
            }
            continue;
        }
        if (d.kind == OptionDef::Text) {
            std::string& s = d.text(const_cast<Job&>(job));
            if (!s.empty()) {
                out.push_back(wide_ascii(d.flag));
                out.push_back(widen(s));
            }
            continue;
        }
        double v = d.get(job);
        if (v == d.get(defaults)) continue;
        switch (d.kind) {
        case OptionDef::Toggle:
            if (const char* f = v != 0 ? d.flag : d.off_flag) out.push_back(wide_ascii(f));
            break;
        case OptionDef::Number:
            if (v == 0 && d.off_flag) {
                out.push_back(wide_ascii(d.off_flag));
            } else {
                out.push_back(wide_ascii(d.flag));
                out.push_back(wide_ascii(format_number(v)));
            }
            break;
        case OptionDef::Integer:
            out.push_back(wide_ascii(d.flag));
            out.push_back(std::to_wstring((long long)v));
            break;
        case OptionDef::Choice: {
            out.push_back(wide_ascii(d.flag));
            std::wstring value = std::to_wstring((long long)v);
            for (auto& it : d.items)
                if (it.value == (int)v && it.cli) value = wide_ascii(it.cli);
            out.push_back(value);
            break;
        }
        default: break;
        }
    }
    return out;
}

std::wstring join_args(const std::vector<std::wstring>& args) {
    std::wstring s;
    for (auto& a : args) {
        if (!s.empty()) s += L' ';
        if (a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos) s += L'"' + a + L'"';
        else s += a;
    }
    return s;
}

std::string usage_text() {
    std::string s =
        "Spotbuilder - opens levels from Unity asset bundles (Skater XL maps and more), edits them, and\n"
        "exports a .glb or builds a Skate mod with ReSkate Studio\n"
        "\n"
        "usage: Spotbuilder <map file or folder> [options]\n"
        "       Spotbuilder                        opens the window (so does double-clicking the exe)\n"
        "       Spotbuilder --gui [map] [options]  opens the window with these settings\n"
        "\n";
    auto line = [&](const std::string& left, const std::string& help) {
        std::string l = "  " + left;
        if (l.size() < 28) l.resize(28, ' ');
        else l += "\n" + std::string(28, ' ');
        // Wrap the help text at 100 columns.
        size_t col = 28;
        std::string word;
        auto flush = [&] {
            if (word.empty()) return;
            if (col > 28 && col + 1 + word.size() > 100) {
                l += "\n" + std::string(28, ' ');
                col = 28;
            } else if (col > 28) {
                l += ' ', ++col;
            }
            l += word;
            col += word.size();
            word.clear();
        };
        for (char c : help) {
            if (c == ' ') flush();
            else word += c;
        }
        flush();
        s += l + "\n";
    };
    line("-o, --output <file.glb>", "output path (default: <map>.glb next to the map)");
    line("--list", "print what the map contains and exit");
    line("-v, --verbose", "more detail");
    line("-h, --help", "this text");
    line("--edits <file.json>", "apply these map edits (default: <map>.spotbuilder.json beside the map, which the "
                                "window saves, when it exists)");
    line("--no-edits", "ignore saved map edits");
    line("--plant-lod <name>=<n>", "LOD level for one tree/bush/grass type painted on terrains: a number, lowest, "
                                   "or default (the trees-and-grass LOD). Repeat for each type; the window's Terrain "
                                   "plants card lists them.");
    const Job defaults;
    const char* group = "";
    for (auto& d : option_table()) {
        if (std::string(group) != d.group) {
            group = d.group;
            s += "\n" + std::string(group) + "\n";
        }
        if (d.flag && std::string(d.flag) == "--verbose") continue;
        std::string left, help = d.help;
        switch (d.kind) {
        case OptionDef::Toggle: {
            bool on = d.get(defaults) != 0;
            const char* f = on ? d.off_flag : d.flag;
            if (!f) continue;
            left = f;
            if (on && d.off_help) {
                help = d.off_help;
                help[0] = (char)std::toupper((unsigned char)help[0]);
                help += ".";
            }
            break;
        }
        default: left = std::string(d.flag) + " " + d.value_name; break;
        }
        if (d.kind == OptionDef::Choice) {
            std::string values;
            bool numeric = false;
            for (auto& it : d.items) {
                if (it.cli) values += std::string(values.empty() ? "" : ", ") + it.cli;
                else numeric = true;
            }
            if (!values.empty()) help += numeric ? " Also: " + values + "." : " One of: " + values + ".";
        }
        double def = d.takes_string() ? 0 : d.get(defaults);
        if (d.kind == OptionDef::Number || d.kind == OptionDef::Integer)
            help += " Default " + format_number(def) + (*d.unit ? std::string(" ") + d.unit : "") + ".";
        else if (d.kind == OptionDef::Choice)
            for (auto& it : d.items)
                if (it.value == (int)def) help += std::string(" Default: ") + (it.cli ? it.cli : it.label) + ".";
        line(left, help);
        if (d.kind == OptionDef::Number && d.off_flag) line(d.off_flag, std::string("same as ") + d.flag + " 0");
    }
    return s;
}

} // namespace xl
