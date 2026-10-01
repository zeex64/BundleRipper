// Building a Skate mod with ReSkate Studio: the ripped map goes through Blender
// (src/blender/make_blend.py) into a .blend, which Studio's command line compiles and, if asked,
// installs into Skate.
#pragma once
#include "edits.h"
#include "scene.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xl {

struct ModBuild {
    std::string name;                  // mod folder name; empty: the map's name
    bool deploy = true;                // install into Skate's Mods folder
    std::filesystem::path package;     // Studio's build folder; empty: %LOCALAPPDATA%\Spotbuilder\Builds\<name>\package
    std::filesystem::path blender, studio_cli, game;  // empty: found automatically
    int pause_map = 0;                 // 0 3D, 1 2D
    int time_of_day = 0;               // 0 Studio's default, 1 morning ... 5 night
    bool gi = true;                    // bake global illumination
    int streaming = 0;                 // 0 automatic, 1 on, 2 off
    bool lods = true;                  // Studio's mesh LODs
    bool keep_blend = false;           // also save the .blend next to the map
};

struct StudioTools {
    std::filesystem::path blender, cli, game;
    std::string missing;  // what could not be found; empty when everything is there
};
// Blender, reskate_cli.exe and Skate's folder: the given paths, else ReSkate Studio's settings
// and the usual install places.
StudioTools find_studio_tools(const ModBuild& m);

// A curve going out: the map's own and auto splines (with their grind settings) and the user's
// grind curves and NPC routes.
struct ExportCurve {
    OutCurve curve;  // Unity world space
    bool npc = false;
    bool automatic = false;
    GrindSettings grind;
    NpcSettings route;
};
std::vector<ExportCurve> export_curves(const Scene& scene, const Edits* edits);
// The curves in Blender's space, for make_blend.py.
// The curves in Blender's space, for make_blend.py.
void write_curves_json(const std::vector<ExportCurve>& curves, const std::filesystem::path& file);

// Runs a program, handing every line it prints (stdout and stderr) to `line`; its exit code.
int run_process(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                const std::function<void(const std::string&)>& line);

bool skate_running();

// The .blend: imports the .glb and makes the curves, in Blender. Throws on failure.
void make_blend(const StudioTools& tools, const std::filesystem::path& glb, const std::filesystem::path& curves,
                const std::filesystem::path& blend);
// Compiles the .blend into a mod in `package` (and installs it when m.deploy). Throws on failure.
void compile_mod(const StudioTools& tools, const ModBuild& m, const std::string& name, const std::filesystem::path& blend,
                 const std::filesystem::path& package);

// Where builds are staged: %LOCALAPPDATA%\Spotbuilder\Builds\<name>
std::filesystem::path build_folder(const std::string& name);

} // namespace xl
