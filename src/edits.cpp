#include "edits.h"
#include "json_parse.h"
#include "scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace xl {

const char* const kBehaviorKeys[kBehaviorCount] = {
    "exclude_from_edge_generation", "include_in_surface_analysis", "exclude_from_grinding", "jump_pad", "boost_pad",
    "wipeout", "slide", "do_not_align", "stairs", "camera_occluder"};
const char* const kBehaviorLabels[kBehaviorCount] = {
    "No generated edges", "Include in surface analysis", "Disable grinding", "Jump pad", "Boost pad",
    "Force wipeout", "Slide", "Do not align skater", "Stairs", "Camera occluder"};

const char* const kGrindSurfaceIds[kGrindSurfaceCount] = {
    "material_37225248", "material_37226144", "material_37226528", "material_37227424", "material_37228128"};
const char* const kGrindSurfaceLabels[kGrindSurfaceCount] = {"Concrete", "Metal (thin)", "Metal", "Metal rail",
                                                             "Wood (thick, rough)"};
const char* const kLightTimeLabels[kLightTimes] = {"Morning", "Noon", "Afternoon", "Evening", "Night",
                                                   "Weather day", "Weather night"};
const char* const kAudioPresetIds[kAudioPresets] = {"tunnel", "drips", "custom", "region"};
const char* const kAudioPresetLabels[kAudioPresets] = {"Tunnel room tone", "Water drips", "Custom BehaviorAsset",
                                                       "Native region"};

const char* collision_id(Collision c) {
    switch (c) {
    case Collision::TriangleMesh: return "triangle_mesh";
    case Collision::ConvexParts: return "convex_parts";
    case Collision::Hull: return "hull";
    case Collision::None: return "none";
    case Collision::Water: return "water";
    }
    return "triangle_mesh";
}

bool ObjectEdit::empty() const {
    return !removed && !collision_only && !collision && !surface && !round_rail && !hide_from_map && !behavior && !audio &&
           !friction;
}
bool MaterialEdit::empty() const { return !invisible && !surface && !alpha && !alpha_cutoff && !domain; }  // invisible: unset
bool Edits::empty() const { return count() == 0; }
size_t Edits::count() const {
    size_t n = (spawn ? 1 : 0) + bus_stops.size() + splines.size() + ripped_splines.size() + user_lights.size() +
               dropped_lights.size() + audio_volumes.size();
    for (auto& [k, e] : objects) n += !e.empty();
    for (auto& [k, e] : materials) n += !e.empty();
    for (auto& [k, e] : lights) n += !e.empty();
    return n;
}

namespace {
fs::path beside(const fs::path& map, const wchar_t* suffix) {
    fs::path m = map;
    if (!m.has_filename()) m = m.parent_path();
    return m.parent_path() / (m.filename().wstring() + suffix);
}
} // namespace

fs::path edits_file(const fs::path& map) { return beside(map, L".spotbuilder.json"); }
fs::path legacy_edits_file(const fs::path& map) { return beside(map, L".bundleripper.json"); }
fs::path existing_edits_file(const fs::path& map) {
    std::error_code ec;
    fs::path now = edits_file(map), old = legacy_edits_file(map);
    return !fs::exists(now, ec) && fs::exists(old, ec) ? old : now;
}

Quat marker_rotation(double yaw) {
    // Studio faces a spawn along the empty's glTF local -Z; the writer mirrors X, so a Unity turn
    // of yaw + 180 degrees about Y lands it facing `yaw` in the game.
    double half = (yaw + 180.0) * 3.14159265358979 / 360.0;
    return {0, std::sin(half), 0, std::cos(half)};
}

// ---- file format ----------------------------------------------------------------------------

namespace {

const Json* field(const Json& o, const char* k) {
    if (o.t != Json::Obj) return nullptr;
    for (auto& [key, v] : o.o)
        if (key == k) return &v;
    return nullptr;
}
std::optional<double> number(const Json& o, const char* k) {
    const Json* v = field(o, k);
    if (!v) return std::nullopt;
    if (v->t == Json::Int) return (double)v->i;
    if (v->t == Json::Num) return v->n;
    return std::nullopt;
}
bool flag(const Json& o, const char* k) {
    const Json* v = field(o, k);
    return v && v->t == Json::Bool && v->b;
}
std::string text(const Json& o, const char* k) {
    const Json* v = field(o, k);
    return v && v->t == Json::Str ? v->s : std::string();
}

const char* const kCollisionIds[] = {"triangle_mesh", "convex_parts", "hull", "none", "water"};
const char* const kAlphaIds[] = {"auto", "opaque", "mask", "blend"};
const std::pair<Domain, const char*> kDomainIds[] = {
    {Domain::Surface, "surface"}, {Domain::Decal, "decal"}, {Domain::Foliage, "foliage"}, {Domain::Ocean, "ocean"}};

Json vec(const V3& v) { return Json::list(v.x, v.y, v.z); }
V3 vec_of(const Json& o, const char* k) {
    const Json* v = field(o, k);
    if (!v || v->t != Json::Arr || v->a.size() != 3) return {};
    auto num = [](const Json& j) { return j.t == Json::Int ? (double)j.i : j.t == Json::Num ? j.n : 0.0; };
    return {num(v->a[0]), num(v->a[1]), num(v->a[2])};
}

Json marker_json(const Marker& m, bool bus_stop) {
    Json j = Json::object();
    if (bus_stop) {
        j.set("name", m.name);
        j.set("shelter", m.shelter);
    }
    j.set("position", vec(m.position));
    j.set("yaw", m.yaw);
    return j;
}
Marker marker_of(const Json& j) {
    Marker m;
    m.position = vec_of(j, "position");
    m.yaw = number(j, "yaw").value_or(0);
    m.name = text(j, "name");
    if (const Json* s = field(j, "shelter"); s && s->t == Json::Bool) m.shelter = s->b;
    return m;
}

Json grind_json(const GrindSettings& g) {
    Json j = Json::object();
    j.set("enabled", g.enabled);
    j.set("radius", g.radius);
    j.set("surface", kGrindSurfaceIds[std::clamp(g.surface, 0, kGrindSurfaceCount - 1)]);
    return j;
}
GrindSettings grind_of(const Json& j) {
    GrindSettings g;
    if (const Json* e = field(j, "enabled"); e && e->t == Json::Bool) g.enabled = e->b;
    g.radius = number(j, "radius").value_or(g.radius);
    std::string surface = text(j, "surface");
    for (int i = 0; i < kGrindSurfaceCount; ++i)
        if (surface == kGrindSurfaceIds[i]) g.surface = i;
    return g;
}
const char* const kNpcKindIds[] = {"pedestrian", "vehicle", "bus"};

Json part_json(const ContactPart& p) {
    Json j = Json::object();
    j.set("dynamic", p.dynamic);
    j.set("static", p.statik);
    j.set("restitution", p.restitution);
    return j;
}
ContactPart part_of(const Json& j, ContactPart d) {
    d.dynamic = number(j, "dynamic").value_or(d.dynamic);
    d.statik = number(j, "static").value_or(d.statik);
    d.restitution = number(j, "restitution").value_or(d.restitution);
    return d;
}

} // namespace

namespace {

bool edits_from_json(const Json& root, Edits& out, bool keep_empty) {
    Edits e;
    if (const Json* objects = field(root, "objects"); objects && objects->t == Json::Obj)
        for (auto& [key, j] : objects->o) {
            ObjectEdit o;
            o.removed = flag(j, "removed");
            o.collision_only = flag(j, "collision_only");
            std::string c = text(j, "collision");
            for (int i = 0; i < 5; ++i)
                if (c == kCollisionIds[i]) o.collision = (Collision)i;
            if (auto s = number(j, "surface")) o.surface = (int)*s;
            o.round_rail = flag(j, "round_rail");
            o.hide_from_map = flag(j, "hide_from_map");
            if (const Json* b = field(j, "behavior"); b && b->t == Json::Obj) {
                std::array<bool, kBehaviorCount> flags{};
                for (int i = 0; i < kBehaviorCount; ++i) flags[i] = flag(*b, kBehaviorKeys[i]);
                o.behavior = flags;
            }
            if (const Json* a = field(j, "audio"); a && a->t == Json::Obj) {
                AudioOverride au;
                au.softness = number(*a, "softness").value_or(au.softness);
                au.smoothness = number(*a, "smoothness").value_or(au.smoothness);
                au.min_impact_force = number(*a, "min_impact_force").value_or(au.min_impact_force);
                au.impact_cooldown = number(*a, "impact_cooldown_release").value_or(au.impact_cooldown);
                au.ignore_player_collisions = flag(*a, "ignore_player_collisions");
                o.audio = au;
            }
            if (const Json* fr = field(j, "friction"); fr && fr->t == Json::Obj) {
                FrictionOverride fo;
                if (const Json* p = field(*fr, "deck")) fo.deck = part_of(*p, fo.deck);
                if (const Json* p = field(*fr, "truck")) fo.truck = part_of(*p, fo.truck);
                if (const Json* p = field(*fr, "wheel")) fo.wheel = part_of(*p, fo.wheel);
                o.friction = fo;
            }
            if (keep_empty || !o.empty()) e.objects[key] = o;
        }
    if (const Json* materials = field(root, "materials"); materials && materials->t == Json::Obj)
        for (auto& [key, j] : materials->o) {
            MaterialEdit m;
            if (const Json* v = field(j, "invisible"); v && v->t == Json::Bool) m.invisible = v->b;
            if (auto s = number(j, "surface")) m.surface = (int)*s;
            std::string a = text(j, "alpha");
            for (int i = 0; i < 4; ++i)
                if (a == kAlphaIds[i]) m.alpha = i;
            if (auto c = number(j, "alpha_cutoff")) m.alpha_cutoff = *c;
            std::string d = text(j, "domain");
            for (auto& [dv, name] : kDomainIds)
                if (d == name) m.domain = dv;
            if (keep_empty || !m.empty()) e.materials[key] = m;
        }
    if (const Json* s = field(root, "spawn"); s && s->t == Json::Obj) e.spawn = marker_of(*s);
    if (const Json* list = field(root, "splines"); list && list->t == Json::Arr)
        for (auto& j : list->a) {
            UserSpline sp;
            sp.name = text(j, "name");
            sp.npc = text(j, "type") == "npc_route";
            sp.closed = flag(j, "closed");
            if (const Json* pts = field(j, "points"); pts && pts->t == Json::Arr)
                for (auto& pt : pts->a) {
                    Json holder = Json::object();
                    holder.set("p", pt);
                    sp.points.push_back(vec_of(holder, "p"));
                }
            if (const Json* g = field(j, "grind"); g && g->t == Json::Obj) sp.grind = grind_of(*g);
            if (const Json* r = field(j, "route"); r && r->t == Json::Obj) {
                std::string kind = text(*r, "kind");
                for (int i = 0; i < 3; ++i)
                    if (kind == kNpcKindIds[i]) sp.route.kind = (NpcKind)i;
                sp.route.width = number(*r, "width").value_or(sp.route.width);
                sp.route.spacing = number(*r, "spacing").value_or(sp.route.spacing);
                sp.route.weight = number(*r, "weight").value_or(sp.route.weight);
                sp.route.speed = (int)number(*r, "speed").value_or(sp.route.speed);
                if (const Json* b = field(*r, "bidirectional"); b && b->t == Json::Bool) sp.route.bidirectional = b->b;
                sp.route.stairs = flag(*r, "stairs");
            }
            e.splines.push_back(std::move(sp));
        }
    if (const Json* rs = field(root, "ripped_splines"); rs && rs->t == Json::Obj)
        for (auto& [key, j] : rs->o) {
            RippedSpline r;
            r.grind = grind_of(j);
            if (field(j, "offset")) r.offset = vec_of(j, "offset");
            if (keep_empty || !r.is_default()) e.ripped_splines[key] = r;
        }
    if (const Json* ls = field(root, "lights"); ls && ls->t == Json::Obj)
        for (auto& [key, j] : ls->o) {
            LightEdit l;
            l.removed = flag(j, "removed");
            if (auto v = number(j, "range")) l.range = *v;
            if (auto v = number(j, "times")) l.times = (int)*v;
            std::string mode = text(j, "area_mode");
            if (mode == "area") l.area_mode = 0;
            else if (mode == "spotlight") l.area_mode = 1;
            if (keep_empty || !l.empty()) e.lights[key] = l;
        }
    if (const Json* list = field(root, "user_lights"); list && list->t == Json::Arr)
        for (auto& j : list->a) {
            UserLight l;
            l.name = text(j, "name");
            l.position = vec_of(j, "position");
            l.yaw = number(j, "yaw").value_or(l.yaw);
            l.pitch = number(j, "pitch").value_or(l.pitch);
            l.spot = text(j, "type") == "spot";
            l.area = text(j, "type") == "area";
            if (const Json* sz = field(j, "size"); sz && sz->t == Json::Arr && sz->a.size() == 2) {
                l.width = sz->a[0].t == Json::Int ? (double)sz->a[0].i : sz->a[0].n;
                l.height = sz->a[1].t == Json::Int ? (double)sz->a[1].i : sz->a[1].n;
            }
            V3 c = vec_of(j, "color");
            if (field(j, "color")) l.color[0] = (float)c.x, l.color[1] = (float)c.y, l.color[2] = (float)c.z;
            l.intensity = number(j, "intensity").value_or(l.intensity);
            l.range = number(j, "range").value_or(l.range);
            l.cone = number(j, "cone").value_or(l.cone);
            l.times = (int)number(j, "times").value_or(l.times);
            l.source = text(j, "source");
            e.user_lights.push_back(l);
        }
    if (const Json* list = field(root, "audio_volumes"); list && list->t == Json::Arr)
        for (auto& j : list->a) {
            AudioVolume a;
            a.name = text(j, "name");
            a.center = vec_of(j, "center");
            a.yaw = number(j, "yaw").value_or(0);
            if (field(j, "half_size")) a.half = vec_of(j, "half_size");
            std::string preset = text(j, "preset");
            for (int i = 0; i < kAudioPresets; ++i)
                if (preset == kAudioPresetIds[i]) a.preset = i;
            a.region_tag = text(j, "region_tag");
            a.behavior = text(j, "behavior");
            if (field(j, "density_group")) a.density_group = text(j, "density_group");
            a.activation_distance = number(j, "activation_distance").value_or(a.activation_distance);
            a.indooriness = number(j, "indooriness").value_or(a.indooriness);
            a.density = number(j, "density").value_or(a.density);
            a.priority = (int)number(j, "priority").value_or(a.priority);
            a.additive = flag(j, "additive");
            a.child_regions = flag(j, "allow_in_child_regions");
            e.audio_volumes.push_back(a);
        }
    if (const Json* b = field(root, "bus_stops"); b && b->t == Json::Arr)
        for (auto& j : b->a) e.bus_stops.push_back(marker_of(j));
    if (const Json* d = field(root, "dropped_lights"); d && d->t == Json::Arr)
        for (auto& j : d->a)
            if (j.t == Json::Str) e.dropped_lights.insert(j.s);
    out = std::move(e);
    return true;
}

Json edits_to_json(const Edits& e, bool keep_empty) {
    Json root = Json::object();
    root.set("about", "Spotbuilder map edits (positions are Unity world space, yaw in degrees about +Y)");
    root.set("format", 1);
    Json objects = Json::object();
    for (auto& [key, o] : e.objects) {
        if (o.empty() && !keep_empty) continue;
        Json j = Json::object();
        if (o.removed) j.set("removed", true);
        if (o.collision_only) j.set("collision_only", true);
        if (o.collision) j.set("collision", collision_id(*o.collision));
        if (o.surface) j.set("surface", *o.surface);
        if (o.round_rail) j.set("round_rail", true);
        if (o.hide_from_map) j.set("hide_from_map", true);
        if (o.behavior) {
            Json b = Json::object();
            for (int i = 0; i < kBehaviorCount; ++i) b.set(kBehaviorKeys[i], (*o.behavior)[i]);
            j.set("behavior", b);
        }
        if (o.audio) {
            Json a = Json::object();
            a.set("softness", o.audio->softness);
            a.set("smoothness", o.audio->smoothness);
            a.set("min_impact_force", o.audio->min_impact_force);
            a.set("impact_cooldown_release", o.audio->impact_cooldown);
            a.set("ignore_player_collisions", o.audio->ignore_player_collisions);
            j.set("audio", a);
        }
        if (o.friction) {
            Json fr = Json::object();
            fr.set("deck", part_json(o.friction->deck));
            fr.set("truck", part_json(o.friction->truck));
            fr.set("wheel", part_json(o.friction->wheel));
            j.set("friction", fr);
        }
        objects.set(key, j);
    }
    root.set("objects", objects);
    Json materials = Json::object();
    for (auto& [key, m] : e.materials) {
        if (m.empty() && !keep_empty) continue;
        Json j = Json::object();
        if (m.invisible) j.set("invisible", *m.invisible);
        if (m.surface) j.set("surface", *m.surface);
        if (m.alpha) j.set("alpha", kAlphaIds[std::clamp(*m.alpha, 0, 3)]);
        if (m.alpha_cutoff) j.set("alpha_cutoff", *m.alpha_cutoff);
        if (m.domain)
            for (auto& [dv, name] : kDomainIds)
                if (dv == *m.domain) j.set("domain", name);
        materials.set(key, j);
    }
    root.set("materials", materials);
    if (e.spawn) root.set("spawn", marker_json(*e.spawn, false));
    Json splines = Json::array();
    for (auto& sp : e.splines) {
        Json j = Json::object();
        j.set("name", sp.name);
        j.set("type", sp.npc ? "npc_route" : "grind");
        j.set("closed", sp.closed);
        Json pts = Json::array();
        for (auto& pt : sp.points) pts.push(vec(pt));
        j.set("points", pts);
        if (sp.npc) {
            Json r = Json::object();
            r.set("kind", kNpcKindIds[(int)sp.route.kind]);
            r.set("width", sp.route.width);
            r.set("spacing", sp.route.spacing);
            r.set("weight", sp.route.weight);
            r.set("speed", sp.route.speed);
            r.set("bidirectional", sp.route.bidirectional);
            r.set("stairs", sp.route.stairs);
            j.set("route", r);
        } else {
            j.set("grind", grind_json(sp.grind));
        }
        splines.push(j);
    }
    root.set("splines", splines);
    Json ripped = Json::object();
    for (auto& [key, r] : e.ripped_splines) {
        if (r.is_default() && !keep_empty) continue;
        Json j = grind_json(r.grind);
        if (r.offset.x != 0 || r.offset.y != 0 || r.offset.z != 0) j.set("offset", vec(r.offset));
        ripped.set(key, j);
    }
    root.set("ripped_splines", ripped);
    Json lights = Json::object();
    for (auto& [key, l] : e.lights) {
        if (l.empty() && !keep_empty) continue;
        Json j = Json::object();
        if (l.removed) j.set("removed", true);
        if (l.range) j.set("range", *l.range);
        if (l.times) j.set("times", *l.times);
        if (l.area_mode) j.set("area_mode", *l.area_mode ? "spotlight" : "area");
        lights.set(key, j);
    }
    root.set("lights", lights);
    Json user_lights = Json::array();
    for (auto& l : e.user_lights) {
        Json j = Json::object();
        j.set("name", l.name);
        j.set("type", l.area ? "area" : l.spot ? "spot" : "point");
        if (l.area) j.set("size", Json::list(l.width, l.height));
        j.set("position", vec(l.position));
        j.set("yaw", l.yaw);
        j.set("pitch", l.pitch);
        j.set("color", Json::list((double)l.color[0], (double)l.color[1], (double)l.color[2]));
        j.set("intensity", l.intensity);
        j.set("range", l.range);
        j.set("cone", l.cone);
        j.set("times", l.times);
        if (!l.source.empty()) j.set("source", l.source);
        user_lights.push(j);
    }
    root.set("user_lights", user_lights);
    if (!e.dropped_lights.empty()) {
        Json dropped = Json::array();
        for (auto& key : e.dropped_lights) dropped.push(key);
        root.set("dropped_lights", dropped);
    }
    Json volumes = Json::array();
    for (auto& a : e.audio_volumes) {
        Json j = Json::object();
        j.set("name", a.name);
        j.set("center", vec(a.center));
        j.set("yaw", a.yaw);
        j.set("half_size", vec(a.half));
        j.set("preset", kAudioPresetIds[std::clamp(a.preset, 0, kAudioPresets - 1)]);
        j.set("region_tag", a.region_tag);
        j.set("behavior", a.behavior);
        j.set("density_group", a.density_group);
        j.set("activation_distance", a.activation_distance);
        j.set("indooriness", a.indooriness);
        j.set("density", a.density);
        j.set("priority", a.priority);
        j.set("additive", a.additive);
        j.set("allow_in_child_regions", a.child_regions);
        volumes.push(j);
    }
    root.set("audio_volumes", volumes);
    Json stops = Json::array();
    for (auto& m : e.bus_stops) stops.push(marker_json(m, true));
    root.set("bus_stops", stops);
    return root;
}

} // namespace

bool load_edits(const fs::path& file, Edits& out, std::string& error) {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        error = "cannot read the file";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_edits(ss.str(), out, error);
}

void save_edits(const fs::path& file, const Edits& e) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary);
    f << dump_pretty(edits_to_json(e, false));
}

std::string edits_text(const Edits& e, bool keep_empty) { return dump_pretty(edits_to_json(e, keep_empty)); }

bool parse_edits(const std::string& text, Edits& out, std::string& error, bool keep_empty) {
    Json root;
    if (!parse_json(text, root, error)) return false;
    return edits_from_json(root, out, keep_empty);
}

// ---- export ---------------------------------------------------------------------------------

namespace {

// Studio's surface profile (sk8_surface_profile_json): compact JSON with sorted keys.
std::string surface_profile(const ObjectEdit& o) {
    Json p = Json::object();  // keys added in sorted order
    if (o.audio) {
        Json a = Json::object();
        a.set("ignore_player_collisions", o.audio->ignore_player_collisions);
        a.set("impact_cooldown_release", o.audio->impact_cooldown);
        a.set("min_impact_force", o.audio->min_impact_force);
        a.set("smoothness", o.audio->smoothness);
        a.set("softness", o.audio->softness);
        p.set("audio", a);
    }
    if (o.behavior) {
        std::vector<std::pair<std::string, bool>> flags;
        for (int i = 0; i < kBehaviorCount; ++i) flags.push_back({kBehaviorKeys[i], (*o.behavior)[i]});
        std::sort(flags.begin(), flags.end());
        Json b = Json::object();
        for (auto& [k, v] : flags) b.set(k, v);
        p.set("behavior", b);
    }
    p.set("donor_material_packed", o.surface.value_or(32));
    p.set("format", 1);
    if (o.friction) {
        auto part = [](const ContactPart& c) {
            Json j = Json::object();
            j.set("dynamic", c.dynamic);
            j.set("restitution", c.restitution);
            j.set("static", c.statik);
            return j;
        };
        Json f = Json::object();
        f.set("deck", part(o.friction->deck));
        f.set("truck", part(o.friction->truck));
        f.set("wheel", part(o.friction->wheel));
        p.set("friction", f);
    }
    std::string s;
    p.dump(s);
    return s;
}

// A copy of the nested settings dict `name` (empty when missing). Edited separately and stored
// back once: setting other keys on the owner may move it.
Json nested(const Json& ex, const char* name) {
    for (auto& [k, v] : ex.o)
        if (k == name && v.t == Json::Obj) return v;
    return Json::object();
}

void object_extras(Json& ex, const ObjectEdit& o) {
    Json so = nested(ex, "sk8_object");
    if (o.collision) {
        so.set("collision_mode", (int)*o.collision);
        ex.set("sk8_collision_mode", collision_id(*o.collision));
    }
    if (o.surface) {
        so.set("collision_material", *o.surface);
        ex.set("sk8_collision_material_packed", *o.surface);
        ex.set("sk8_object_surface_authored", true);
    }
    if (o.round_rail) so.set("round_rail", true);
    if (o.hide_from_map) {
        so.set("hide_from_pause_map", true);
        ex.set("sk8_hide_from_pause_map", true);
    }
    if (o.behavior) {
        so.set("custom_behavior", true);
        so.set("_sk8_behavior_override_initialized", true);
        for (int i = 0; i < kBehaviorCount; ++i) so.set(kBehaviorKeys[i], (*o.behavior)[i]);
    }
    if (o.audio) {
        so.set("custom_audio", true);
        so.set("audio_softness", o.audio->softness);
        so.set("audio_smoothness", o.audio->smoothness);
        so.set("audio_min_impact_force", o.audio->min_impact_force);
        so.set("audio_impact_cooldown_release", o.audio->impact_cooldown);
        so.set("audio_ignore_player_collisions", o.audio->ignore_player_collisions);
    }
    if (o.friction) {
        so.set("custom_friction", true);
        for (auto [name, part] : {std::pair<const char*, const ContactPart*>{"deck", &o.friction->deck},
                                  {"truck", &o.friction->truck}, {"wheel", &o.friction->wheel}}) {
            so.set(std::string(name) + "_dynamic_friction", part->dynamic);
            so.set(std::string(name) + "_static_friction", part->statik);
            so.set(std::string(name) + "_restitution", part->restitution);
        }
    }
    if (o.behavior || o.audio || o.friction) {
        ex.set("sk8_surface_profile_json", surface_profile(o));
        ex.set("sk8_object_surface_authored", true);
    }
    if (!so.o.empty()) ex.set("sk8_object", std::move(so));
}

void material_extras(OutMaterial& m, const MaterialEdit& e) {
    Json sm = nested(m.extras, "sk8_material");
    if (e.invisible) sm.set("invisible", *e.invisible);
    if (e.surface) {
        sm.set("collision_material", *e.surface);
        m.extras.set("sk8_collision_material_packed", *e.surface);
    }
    if (e.alpha) {
        sm.set("alpha", *e.alpha);
        if (*e.alpha == 1) m.alpha_mode = 0, m.auto_alpha = false;
        else if (*e.alpha == 2) m.alpha_mode = 1, m.auto_alpha = false;
        else if (*e.alpha == 3) m.alpha_mode = 2, m.auto_alpha = false;
    }
    if (e.alpha_cutoff) {
        m.alpha_cutoff = (float)*e.alpha_cutoff;
        sm.set("alpha_cutoff", *e.alpha_cutoff);
    }
    if (e.domain) sm.set("domain", (int)*e.domain);
    if (!sm.o.empty()) m.extras.set("sk8_material", std::move(sm));
}

} // namespace

UserLight light_from_map(const OutNode& n, const OutLight& L, const M4& world, const LightEdit* edit) {
    UserLight u;
    u.name = n.name;
    if (u.name.size() > 6 && u.name.compare(u.name.size() - 6, 6, "_light") == 0) u.name.resize(u.name.size() - 6);
    u.source = n.key;
    u.position = {world.m[0][3], world.m[1][3], world.m[2][3]};
    V3 d = normalize(xform_dir(world, V3{0, 0, -1}));  // the node turns glTF's -Z onto Unity's +Z
    u.yaw = std::atan2(d.x, d.z) * 180.0 / 3.14159265358979;
    u.pitch = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180.0 / 3.14159265358979;
    u.area = L.area;
    u.spot = L.type == 1 && !L.area;
    u.width = L.area_size[0], u.height = L.area_size[1];
    std::copy(L.color, L.color + 3, u.color);
    u.intensity = L.intensity;
    u.range = L.range > 0 ? L.range : 40.0;  // Studio's attenuation radius when the map gives none
    u.cone = L.outer * 2 * 180.0 / 3.14159265358979;
    u.times = kAllLightTimes;
    if (edit) {
        if (edit->range) u.range = *edit->range;
        if (edit->times) u.times = *edit->times;
    }
    return u;
}

bool same_light(const UserLight& a, const UserLight& b) {
    auto close = [](double x, double y, double eps) { return std::fabs(x - y) <= eps * std::max(1.0, std::fabs(y)); };
    return a.name == b.name && a.source == b.source && a.spot == b.spot && a.area == b.area && a.times == b.times &&
           close(a.position.x, b.position.x, 1e-6) && close(a.position.y, b.position.y, 1e-6) && close(a.position.z, b.position.z, 1e-6) &&
           close(a.yaw, b.yaw, 1e-5) && close(a.pitch, b.pitch, 1e-5) && close(a.width, b.width, 1e-6) && close(a.height, b.height, 1e-6) &&
           close(a.color[0], b.color[0], 1e-6) && close(a.color[1], b.color[1], 1e-6) && close(a.color[2], b.color[2], 1e-6) &&
           close(a.intensity, b.intensity, 1e-6) && close(a.range, b.range, 1e-6) && close(a.cone, b.cone, 1e-5);
}

void apply_edits(Scene& sc, const Edits& e) {
    // Materials by key
    for (auto& m : sc.materials) {
        auto it = e.materials.find(m.key);
        if (it != e.materials.end()) material_extras(m, it->second);
    }
    // Objects: Studio keys, collision-only copies, removals
    std::unordered_map<int, int> invisible_copy;  // material -> its collision-only copy
    auto invisible = [&](int mi) {
        auto it = invisible_copy.find(mi);
        if (it != invisible_copy.end()) return it->second;
        OutMaterial copy;
        if (mi >= 0 && mi < (int)sc.materials.size()) copy = sc.materials[mi];
        else copy.name = "XL_Material";
        copy.name += " (collision only)";
        copy.key += "/collision_only";
        copy.extras.child("sk8_material").set("invisible", true);
        sc.materials.push_back(std::move(copy));
        return invisible_copy[mi] = (int)sc.materials.size() - 1;
    };
    std::set<int> removed;
    std::set<std::string> replaced = e.dropped_lights;  // map lights made editable go out as those instead
    for (auto& l : e.user_lights)
        if (!l.source.empty()) replaced.insert(l.source);
    for (size_t i = 0; i < sc.nodes.size(); ++i) {  // Studio light settings
        OutNode& n = sc.nodes[i];
        if (n.light < 0) continue;
        if (replaced.count(n.key) && n.light < (int)sc.lights.size() && sc.lights[n.light].type != 2) {
            removed.insert((int)i);
            continue;
        }
        auto it = e.lights.find(n.key);
        if (it == e.lights.end()) continue;
        const LightEdit& l = it->second;
        if (l.removed) {
            removed.insert((int)i);
            continue;
        }
        if (l.range) {
            n.extras.set("sk8_light_range", *l.range);
            if (n.light < (int)sc.lights.size()) sc.lights[n.light].range = (float)*l.range;
        }
        if (l.times) n.extras.set("sk8_light_tod", *l.times);
        if (l.area_mode) n.extras.set("sk8_light_area_mode", *l.area_mode ? "SPOTLIGHT" : "AREA");
    }
    for (size_t i = 0; i < sc.nodes.size(); ++i) {
        OutNode& n = sc.nodes[i];
        auto it = e.objects.find(n.key);
        if (it == e.objects.end()) continue;
        const ObjectEdit& o = it->second;
        if (o.removed) {
            removed.insert((int)i);
            continue;
        }
        object_extras(n.extras, o);
        if (o.collision_only && n.mesh >= 0 && n.mesh < (int)sc.meshes.size()) {
            OutMesh copy = sc.meshes[n.mesh];
            if (copy.lines) continue;
            size_t subs = copy.data ? copy.data->subs.size() : copy.materials.size();
            copy.materials.resize(std::max(copy.materials.size(), subs), -1);
            for (auto& mi : copy.materials)
                if (mi != -2) mi = invisible(mi);
            copy.name += " (collision only)";
            sc.meshes.push_back(std::move(copy));
            n.mesh = (int)sc.meshes.size() - 1;
        }
    }
    if (!removed.empty()) {  // detach removed subtrees
        for (auto& n : sc.nodes)
            n.children.erase(std::remove_if(n.children.begin(), n.children.end(), [&](int c) { return removed.count(c) > 0; }),
                             n.children.end());
        sc.roots.erase(std::remove_if(sc.roots.begin(), sc.roots.end(), [&](int r) { return removed.count(r) > 0; }),
                       sc.roots.end());
    }
}

namespace {

Quat quat_mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Unity rotation turning glTF's light axis (local -Z after the writer's mirror) to `yaw`/`pitch`.
Quat light_rotation(double yaw_deg, double pitch_deg) {
    double yaw = yaw_deg * 3.14159265358979 / 180.0, pitch = pitch_deg * 3.14159265358979 / 180.0;
    V3 d{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    V3 t = d * -1.0;  // local +Z goes opposite the light
    double psi = std::atan2(t.x, t.z), phi = -std::asin(std::clamp(t.y, -1.0, 1.0));
    Quat qy{0, std::sin(psi * 0.5), 0, std::cos(psi * 0.5)}, qx{std::sin(phi * 0.5), 0, 0, std::cos(phi * 0.5)};
    return quat_mul(qy, qx);
}

} // namespace

void add_markers(Scene& sc, const Edits& e) {
    auto add = [&](const Marker& m, std::string name) {
        OutNode n;
        n.name = std::move(name);
        n.key = "xl:marker:" + n.name;
        n.t = m.position;
        n.r = marker_rotation(m.yaw);
        sc.nodes.push_back(n);
        sc.roots.push_back((int)sc.nodes.size() - 1);
        return (int)sc.nodes.size() - 1;
    };
    if (e.spawn) {
        int i = add(*e.spawn, "spawn");
        sc.nodes[i].extras.set("xl_marker", "player_spawn");
    }
    for (size_t k = 0; k < e.user_lights.size(); ++k) {
        const UserLight& u = e.user_lights[k];
        OutLight l;
        l.name = u.name.empty() ? "Light " + std::to_string(k + 1) : u.name;
        l.type = u.spot || u.area ? 1 : 0;
        std::copy(u.color, u.color + 3, l.color);
        l.intensity = (float)u.intensity;
        l.range = (float)u.range;
        l.outer = (float)(std::clamp(u.cone, 1.0, 179.0) * 0.5 * 3.14159265358979 / 180.0);
        l.inner = l.outer * 0.8f;
        sc.lights.push_back(l);
        OutNode n;
        n.name = l.name;
        n.key = "xl:light:" + std::to_string(k);
        n.t = u.position;
        n.r = light_rotation(u.yaw, u.pitch);
        n.light = (int)sc.lights.size() - 1;
        n.extras.set("xl_marker", "light");
        n.extras.set("sk8_light_range", u.range);
        n.extras.set("sk8_light_tod", u.times);
        if (u.area) {  // glTF has no area lights: make_blend.py turns this spot into one
            n.extras.set("xl_area_size", Json::list(u.width, u.height));
            n.extras.set("xl_area_intensity", u.intensity);
            sc.lights.back().outer = (float)(85.0 * 3.14159265358979 / 180.0);
            sc.lights.back().inner = 0;
        }
        sc.nodes.push_back(n);
        sc.roots.push_back((int)sc.nodes.size() - 1);
    }
    for (size_t k = 0; k < e.audio_volumes.size(); ++k) {
        const AudioVolume& a = e.audio_volumes[k];
        OutNode n;
        n.name = a.name.empty() ? "Audio Volume " + std::to_string(k + 1) : a.name;
        n.key = "xl:audio:" + std::to_string(k);
        n.t = a.center;
        double half = a.yaw * 3.14159265358979 / 360.0;
        n.r = {0, std::sin(half), 0, std::cos(half)};
        n.s = {std::max(0.01, a.half.x), std::max(0.01, a.half.y), std::max(0.01, a.half.z)};
        Json& ex = n.extras;
        ex.set("xl_marker", "audio_volume");
        ex.set("sk8_audio_enabled", true);
        ex.set("sk8_audio_preset", kAudioPresetIds[std::clamp(a.preset, 0, kAudioPresets - 1)]);
        if (!a.region_tag.empty()) ex.set("sk8_audio_region_tag", a.region_tag);
        ex.set("sk8_audio_behavior", a.behavior);
        ex.set("sk8_audio_density_group", a.density_group);
        ex.set("sk8_audio_activation_distance", a.activation_distance);
        ex.set("sk8_audio_indooriness", a.indooriness);
        ex.set("sk8_audio_density", a.density);
        ex.set("sk8_audio_priority", a.priority);
        ex.set("sk8_audio_additive", a.additive);
        ex.set("sk8_audio_allow_in_child_regions", a.child_regions);
        sc.nodes.push_back(n);
        sc.roots.push_back((int)sc.nodes.size() - 1);
    }
    for (size_t k = 0; k < e.bus_stops.size(); ++k) {
        const Marker& m = e.bus_stops[k];
        char suffix[16] = "";
        if (k) std::snprintf(suffix, sizeof suffix, ".%03zu", k);
        int i = add(m, std::string("TravelPoint") + suffix);
        Json& ex = sc.nodes[i].extras;
        ex.set("xl_marker", "bus_stop");
        ex.set("bus_stop_name", m.name.empty() ? "Bus Stop " + std::to_string(k + 1) : m.name);
        ex.set("bus_stop_shelter", m.shelter);
    }
}

} // namespace xl
