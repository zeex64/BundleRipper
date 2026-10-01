// The trees, bushes and grass a map paints on its Unity terrains (terrain tree prototypes).
#pragma once
#include "unity.h"

#include <string>
#include <vector>

namespace xl {

struct PlantInfo {
    std::string name;                   // the prototype prefab's name (what --plant-lod matches)
    size_t placed = 0;                  // painted copies over every terrain
    std::vector<size_t> lod_triangles;  // triangles at each LOD level; one entry when it has no LODs
};

// Every prototype painted on the map's terrains, most placed first.
std::vector<PlantInfo> terrain_plants(const Database& db);

// The LOD level a plant uses: its own choice, else the trees-and-grass LOD, else the LOD level;
// clamped to the levels it has (so 99 means its lowest).
int plant_level(const PlantInfo& plant, int own, int tree_lod, int lod);

} // namespace xl
