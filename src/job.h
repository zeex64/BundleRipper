// One run of the tool, shared by the command line and the GUI.
#pragma once
#include "gltf.h"
#include "optimize.h"
#include "edits.h"
#include "plants.h"
#include "sky.h"
#include "scene.h"
#include "studio_build.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace xl {

// What to read, where to write, and every export option.
struct Job {
    std::filesystem::path input;
    std::filesystem::path output;         // empty: <map>.glb next to the map
    std::filesystem::path texture_dump;   // empty: no texture dump
    bool list = false;                    // print what the map contains instead of exporting
    bool verbose = false;
    // Map edits: the window passes its own; otherwise the <map>.spotbuilder.json beside the map
    // (or edits_path) is applied unless no_edits.
    std::shared_ptr<const Edits> edits;
    std::filesystem::path edits_path;
    bool no_edits = false;
    Options opt;
    GltfOptions gltf;
    OptimizeOptions optimize;
    // Build a Skate mod with ReSkate Studio instead of leaving a .glb next to the map.
    bool build_mod = false;
    ModBuild mod;
};

struct JobResult {
    std::filesystem::path output;  // the .glb written (empty for --list); a mod build: its package folder
    std::filesystem::path deployed;  // a mod build installed into Skate: the mod's folder
    bool mod = false;                // a mod build
    uint64_t bytes = 0;
    double seconds = 0;
};

// A map loaded for the scene view: the converted scene, with the textures it draws decoded to
// pixels (ImageJob::pixels) instead of PNGs.
struct Preview {
    Scene scene;
    std::vector<PlantInfo> plants;  // what the map paints on its terrains
    SkyImage sky;                   // the map's own skybox, when it has a picture one
    std::string name;  // the map's file or folder name
    double seconds = 0;
};

// The .glb a job writes: its output, or <map>.glb next to the map (or folder).
std::filesystem::path output_path(const Job& job);
// The mod folder name a mod build uses: the one asked for, else the map's name, made safe the
// way ReSkate Studio makes it (letters, digits, - . and _).
std::string mod_name(const Job& job);

// Loads the map, converts it and writes the .glb (and spline .obj files). Progress goes to the
// log; failures throw std::exception.
JobResult run_job(const Job& job);

// Loads and converts the map like run_job, but keeps the scene instead of writing it. Only the
// textures the scene view draws (base colour and emissive) are decoded, at most
// `texture_size` pixels on a side.
Preview load_preview(const Job& job, int texture_size);

// The trees, bushes and grass the map paints on its terrains, with their LOD levels.
std::vector<PlantInfo> scan_plants(const Job& job);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
inline std::string u8(const std::filesystem::path& p) { return narrow(p.wstring()); }

} // namespace xl
