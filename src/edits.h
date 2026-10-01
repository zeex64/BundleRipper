// Map edits made in the window: per-object and per-material ReSkate Studio settings, objects left
// out, a player spawn and bus stops. They live beside the map (<map>.spotbuilder.json), so a
// later rip, from the window or the command line, applies them again; at export they become the
// glTF extras ReSkate Studio reads (sk8_object / sk8_material / sk8_* keys, "spawn" and
// "TravelPoint" empties).
#pragma once
#include "json.h"
#include "math.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xl {

struct Scene;
struct OutNode;
struct OutLight;

// Studio's collision modes, in its enum order (the number is what it stores).
enum class Collision { TriangleMesh = 0, ConvexParts = 1, Hull = 2, None = 3, Water = 4 };
const char* collision_id(Collision c);  // "triangle_mesh", ...

// The ten gameplay flags of Studio's "Override Gameplay Behavior", in its order.
constexpr int kBehaviorCount = 10;
extern const char* const kBehaviorKeys[kBehaviorCount];    // sk8 field names
extern const char* const kBehaviorLabels[kBehaviorCount];  // as Studio shows them

struct AudioOverride {
    double softness = 0.2, smoothness = 0.25, min_impact_force = 0.1, impact_cooldown = 0.25;
    bool ignore_player_collisions = false;
};
struct ContactPart {
    double dynamic = 0.7, statik = 0.8, restitution = 0;
};
struct FrictionOverride {
    ContactPart deck{0.5, 0.5, 0}, truck{0.7, 0.8, 0}, wheel{0.7, 0.8, 0};
};

// What the user changed on an object (unset: as ripped).
struct ObjectEdit {
    bool removed = false;         // left out of the export (with everything under it)
    bool collision_only = false;  // not drawn, still collides (its materials are exported invisible)
    std::optional<Collision> collision;
    std::optional<int> surface;   // packed native collision material (Studio's surface / audio donor)
    bool round_rail = false;
    bool hide_from_map = false;   // left out of the pause-menu map picture
    std::optional<std::array<bool, kBehaviorCount>> behavior;
    std::optional<AudioOverride> audio;
    std::optional<FrictionOverride> friction;
    bool empty() const;
};

// Studio's material domains that are safe to write (6, glass, crashes its converter).
enum class Domain { Surface = 0, Decal = 1, Foliage = 2, Ocean = 4 };

struct MaterialEdit {
    std::optional<bool> invisible;  // collision only: nothing is drawn with it (unset: as ripped; the
                                    // collider-only XL_Collision material starts invisible)
    std::optional<int> surface;   // packed native collision material
    std::optional<int> alpha;     // 0 auto, 1 opaque, 2 mask (cut-out), 3 blend
    std::optional<double> alpha_cutoff;
    std::optional<Domain> domain;
    bool empty() const;
};

// A placed point: Unity world position and facing (degrees about +Y; 0 faces +Z).
struct Marker {
    V3 position;
    double yaw = 0;
    std::string name;     // bus stops: the name shown in game
    bool shelter = true;  // bus stops: draw the shelter
};

// Studio's "Grind Curve" settings of a curve: an invisible sharp collision rail under it.
constexpr int kGrindSurfaceCount = 5;
extern const char* const kGrindSurfaceIds[kGrindSurfaceCount];     // Studio enum identifiers
extern const char* const kGrindSurfaceLabels[kGrindSurfaceCount];
struct GrindSettings {
    bool enabled = true;
    double radius = 0.03;  // metres, half the rail thickness (0.005 - 0.25)
    int surface = 3;       // index into kGrindSurfaceIds (3: metal rail)
};

// One of the map's own (or auto) splines as edited: its grind settings and a move.
struct RippedSpline {
    GrindSettings grind;
    V3 offset;  // added to every point (Unity metres)
    bool is_default() const {
        return grind.enabled && grind.radius == 0.03 && grind.surface == 3 && offset.x == 0 && offset.y == 0 && offset.z == 0;
    }
};

// Studio's NPC route settings of a curve.
enum class NpcKind { Pedestrian = 0, Vehicle = 1, Bus = 2 };
struct NpcSettings {
    NpcKind kind = NpcKind::Pedestrian;
    double width = 2, spacing = 5, weight = 1;
    int speed = 5;  // 1 - 9
    bool bidirectional = true, stairs = false;
};

// A spline drawn in the window: a grind curve or an NPC route (poly line, Unity world space).
struct UserSpline {
    std::string name;
    bool npc = false;
    bool closed = false;
    std::vector<V3> points;
    GrindSettings grind;
    NpcSettings route;
};

// Light times of day (Studio's bitmask).
constexpr int kLightTimes = 7;
extern const char* const kLightTimeLabels[kLightTimes];  // bit i = 1 << i
constexpr int kAllLightTimes = 127;

// Studio settings of a ripped light (unset: Studio's defaults).
struct LightEdit {
    bool removed = false;
    std::optional<double> range;  // metres
    std::optional<int> times;     // bitmask of kLightTimeLabels
    std::optional<int> area_mode; // 0 area, 1 spotlight
    bool empty() const { return !removed && !range && !times && !area_mode; }
};

// A light added in the window, or one of the map's own made editable (exported as a glTF
// punctual light; an area light as a wide spot the mod build turns back into one).
struct UserLight {
    std::string name;
    std::string source;           // the map light it was made from (its node key); empty: added here
    V3 position;
    double yaw = 0, pitch = -90;  // where a spot points (degrees; pitch -90 straight down)
    bool spot = false;
    bool area = false;            // a rectangle shining to one side (Studio area light)
    double width = 2, height = 1; // area: metres
    float color[3] = {1, 0.95f, 0.85f};
    double intensity = 200;       // candela (area: straight out from its face)
    double range = 15;            // metres
    double cone = 60;             // spot: full angle in degrees
    int times = kAllLightTimes;
};

// Studio's audio volume: an ambience box (footprint turned about the vertical only).
constexpr int kAudioPresets = 4;
extern const char* const kAudioPresetIds[kAudioPresets];     // tunnel, drips, custom, region
extern const char* const kAudioPresetLabels[kAudioPresets];
struct AudioVolume {
    std::string name;
    V3 center;
    double yaw = 0;
    V3 half{5, 2.5, 5};  // half size along its own X, Y (up) and Z
    int preset = 0;
    std::string region_tag;  // preset "region": an audio region tag path (studio_audio.h)
    std::string behavior;    // preset "custom": a BehaviorAsset path
    std::string density_group = "audio/amb/_system/densitygroups/dgo_dg_groundbeds";
    double activation_distance = 15, indooriness = 1, density = 1;
    int priority = 6;
    bool additive = false, child_regions = false;
};

struct Edits {
    std::map<std::string, ObjectEdit> objects;      // by node key (OutNode::key)
    std::map<std::string, MaterialEdit> materials;  // by material key (OutMaterial::key)
    std::optional<Marker> spawn;
    std::vector<Marker> bus_stops;
    std::vector<UserSpline> splines;
    std::map<std::string, RippedSpline> ripped_splines;  // the map's own and auto splines, by OutCurve::key
    std::map<std::string, LightEdit> lights;              // ripped lights, by node key
    std::vector<UserLight> user_lights;
    std::set<std::string> dropped_lights;  // map lights made editable and then deleted (node keys)
    std::vector<AudioVolume> audio_volumes;
    bool empty() const;
    size_t count() const;  // things changed
};

// <map>.spotbuilder.json next to the map file or folder (where edits are saved).
std::filesystem::path edits_file(const std::filesystem::path& map);
// The file to read a map's edits from: edits_file, or the <map>.bundleripper.json the tool wrote
// under its old name (BundleRipper) when only that one exists.
std::filesystem::path existing_edits_file(const std::filesystem::path& map);
std::filesystem::path legacy_edits_file(const std::filesystem::path& map);
bool load_edits(const std::filesystem::path& file, Edits& out, std::string& error);
void save_edits(const std::filesystem::path& file, const Edits& edits);
// Edits as JSON text (what the file holds), for the clipboard. `keep_empty` keeps settings
// entries that change nothing (a copied "as ripped" object resets the ones it is pasted on).
std::string edits_text(const Edits& edits, bool keep_empty = false);
bool parse_edits(const std::string& text, Edits& out, std::string& error, bool keep_empty = false);

// Bakes the edits into a built scene (before flattening): removed subtrees go, Studio keys go
// into node/material extras, collision-only objects get invisible material copies. Markers are
// added by add_markers, after flattening (they have no mesh).
void apply_edits(Scene& scene, const Edits& edits);
void add_markers(Scene& scene, const Edits& edits);

// One of the map's lights as an editable light: its place, aim, colour, intensity, range, cone or
// size, and the range and times of day `edit` gave it.
UserLight light_from_map(const OutNode& node, const OutLight& light, const M4& world, const LightEdit* edit);
// Whether two lights are the same (an imported light nobody changed).
bool same_light(const UserLight& a, const UserLight& b);

// Unity world position -> Blender (Z up), through glTF's mirrored X.
inline V3 unity_to_blender(const V3& u) { return {-u.x, -u.z, u.y}; }

// Unity rotation that makes a glTF empty face `yaw` (degrees, Unity) the way Studio reads spawns
// and bus stops (their local -Z in glTF).
Quat marker_rotation(double yaw);

} // namespace xl
