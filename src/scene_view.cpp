#include "scene_view.h"
#include "math.h"
#include "studio_audio.h"
#include "studio_surfaces.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xl {

using namespace ui;

namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

const char* kViewShader = R"(
cbuffer Frame : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos; uint selected;
    float3 sun_dir; uint flags;   // 1 textures, 2 editor shading, 4 game lighting, 8 sun shadows, 16 fog, 32 map sky, 64 night sky
    float4 flat_color;
    float3 sun_color; float exposure;      // the sun (or moon): colour x illuminance (lux)
    float3 sky_color; uint light_count;    // the sky's illuminance on an open floor
    float3 ground_color; float fog_density;
    float3 fog_color; float shadow_texel;  // fog: luminance; shadow map texel in metres
    row_major float4x4 shadow_proj;
    float4 ray_x; float4 ray_y; float4 ray_z;  // view rays for the sky: right, up (per unit of NDC), forward
    uint tiles_x; uint tiles_y; uint slices; float slice_scale;  // light clusters: 32 px tiles x depth slices
    float cluster_near; float3 cluster_pad;
};
cbuffer Material : register(b1) {
    float4 base;
    float3 emissive; float cutoff;
    float4 uv_st;                 // uv * xy + zw (Unity convention: v up)
    uint mat_flags; uint3 pad;    // 1 base texture, 2 cut-out, 4 blend, 8 unlit, 16 emissive texture
};
Texture2D base_tex : register(t0);
Texture2D emissive_tex : register(t1);
Buffer<float4> lights : register(t2);   // 4 per light: pos+range, colour x cd + type, dir + cos outer, cos inner + size
Texture2D<float> shadow_map : register(t3);
Texture2D sky_tex : register(t4);       // the map's own skybox (equirectangular)
SamplerState samp : register(s0);
SamplerComparisonState shadow_cmp : register(s1);
Buffer<uint2> cluster_grid : register(t5);   // per cluster: first index, count
Buffer<uint> cluster_lights : register(t6);  // light numbers, cluster by cluster
static const float PI = 3.14159265;

struct VIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    float4 r0 : WORLD0; float4 r1 : WORLD1; float4 r2 : WORLD2; uint node : NODE;
};
struct VOut {
    float4 pos : SV_Position; float3 wpos : WPOS; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    nointerpolation uint node : NODE;
};
VOut vs_main(VIn i) {
    VOut o;
    float4 p = float4(i.pos, 1);
    float3 w = float3(dot(i.r0, p), dot(i.r1, p), dot(i.r2, p));
    o.wpos = w;
    o.nrm = float3(dot(i.r0.xyz, i.nrm), dot(i.r1.xyz, i.nrm), dot(i.r2.xyz, i.nrm));
    o.pos = mul(view_proj, float4(w, 1));
    o.uv = i.uv * uv_st.xy + uv_st.zw;
    o.node = i.node;
    return o;
}
float4 surface(VOut i) {
    float4 c = base;
    if ((mat_flags & 1) && (flags & 1)) c *= base_tex.Sample(samp, i.uv);
    if (mat_flags & 2) clip(c.a - cutoff);
    return c;
}
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float sun_shadow(float3 wpos, float3 n) {
    if (!(flags & 8)) return 1;
    float4 sp = mul(shadow_proj, float4(wpos + n * shadow_texel * 1.5, 1));
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1 || sp.z > 1) return 1;
    float t = 1.0 / 4096, s = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            s += shadow_map.SampleCmpLevelZero(shadow_cmp, uv + float2(x, y) * t, sp.z - 0.00001);
    return s / 9;
}
// Illuminance from one light (lux): Frostbite's windowed inverse square, as ReSkate Studio bakes it.
float3 light_at(uint k, float3 wpos, float3 n) {
    float4 a = lights[k * 4], b = lights[k * 4 + 1], c = lights[k * 4 + 2], d = lights[k * 4 + 3];
    float3 l = a.xyz - wpos;
    float d2 = dot(l, l), r2 = a.w * a.w;
    if (d2 >= r2) return 0;
    l *= rsqrt(max(d2, 1e-8));
    float q = d2 / r2, win = saturate(1 - q * q);
    float att = win * win / max(d2, 0.01);
    float lobe = 1;
    if (b.w > 1.5) lobe = saturate(dot(-l, c.xyz));                  // area: a lit rectangle facing one way
    else if (b.w > 0.5) lobe = smoothstep(c.w, d.x, dot(-l, c.xyz)); // spot
    return b.rgb * (att * saturate(dot(n, l)) * lobe);
}
float4 ps_main(VOut i) : SV_Target {
    float4 c = surface(i);
    float3 lit = c.rgb;
    float3 n = normalize(i.nrm);
    if (dot(n, cam_pos - i.wpos) < 0) n = -n;   // light both sides
    float3 e = emissive;
    if ((mat_flags & 16) && (flags & 1)) e *= emissive_tex.Sample(samp, i.uv).rgb;
    if (flags & 4) {
        if (!(mat_flags & 8)) {
            float3 E = sun_color * saturate(dot(n, -sun_dir)) * sun_shadow(i.wpos, n);
            E += lerp(ground_color, sky_color, n.y * 0.5 + 0.5);
            if (light_count > 0 && tiles_x > 0) {
                uint2 tile = min(uint2(i.pos.xy) / 32, uint2(tiles_x - 1, tiles_y - 1));
                float vz = dot(i.wpos - cam_pos, ray_z.xyz);
                uint slice = (uint)clamp(log(max(vz, cluster_near) / cluster_near) * slice_scale, 0, slices - 1);
                uint2 range = cluster_grid[(slice * tiles_y + tile.y) * tiles_x + tile.x];
                for (uint k = 0; k < range.y; ++k) E += light_at(cluster_lights[range.x + k], i.wpos, n);
            }
            lit = c.rgb / PI * E * exposure;
            if (flags & 16) lit = lerp(lit, fog_color * exposure, 1 - exp(-length(i.wpos - cam_pos) * fog_density));
            lit = aces(lit);
        }
        lit += e;
    } else {
        if ((flags & 2) && !(mat_flags & 8)) {
            float sun = saturate(dot(n, -sun_dir));
            float3 sky = lerp(float3(0.32, 0.29, 0.26), float3(0.56, 0.63, 0.76), n.y * 0.5 + 0.5);
            lit = c.rgb * (sky * 0.8 + sun * float3(1.0, 0.95, 0.88) * 0.85);
        }
        lit += e;
    }
    if (i.node & 0x80000000u) lit = lerp(lit, float3(0.22, 0.42, 1.0), 0.35);
    return float4(lit, (mat_flags & 4) ? c.a : 1);
}
void ps_shadow(VOut i) { surface(i); }
struct IdOut { uint id : SV_Target0; float4 pos : SV_Target1; };
IdOut ps_id(VOut i) {
    float4 c = surface(i);
    IdOut o;
    o.id = (i.node & 0x7fffffffu) + 1 + (uint)(c.a * 0);
    o.pos = float4(i.wpos, 1);
    return o;
}
float4 ps_flat(VOut i) : SV_Target { return flat_color; }

struct LIn { float3 pos : POSITION; float4 color : COLOR; };
struct LOut { float4 pos : SV_Position; float4 color : COLOR; };
LOut vs_line(LIn i) { LOut o; o.pos = mul(view_proj, float4(i.pos, 1)); o.color = i.color; return o; }
float4 ps_line(LOut i) : SV_Target { return float4(i.color.rgb, i.color.a * flat_color.a); }

// The sky behind everything: the map's skybox, or a clear sky lit like the time of day.
struct SOut { float4 pos : SV_Position; float2 ndc : NDC; };
SOut vs_sky(uint id : SV_VertexID) {
    SOut o;
    float2 p = float2((id << 1) & 2, id & 2) * 2 - 1;
    o.pos = float4(p, 0, 1);
    o.ndc = p;
    return o;
}
float hash(float3 p) { return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453); }
float4 ps_sky(SOut i) : SV_Target {
    float3 d = normalize(ray_z.xyz + i.ndc.x * ray_x.xyz + i.ndc.y * ray_y.xyz);
    if (flags & 32) {
        float u = atan2(d.x, d.z) / (2 * PI) + 0.5, v = acos(clamp(d.y, -1, 1)) / PI;
        return float4(sky_tex.SampleLevel(samp, float2(u, v), 0).rgb, 1);
    }
    float3 to_sun = -sun_dir;
    float h = d.y;
    float3 zenith = sky_color / PI * 0.8 * float3(0.55, 0.72, 1.0), horizon = fog_color;
    float3 col = lerp(horizon, zenith, pow(saturate(h), 0.45));
    if (h < 0) col = horizon * lerp(1.0, 0.45, saturate(-h * 5));
    float cs = dot(d, to_sun);
    col += sun_color / PI * (0.0006 * pow(saturate(cs), 48) + 0.02 * pow(saturate(cs), 900));
    if (cs > 0.99993 && h > -0.01) col += sun_color * 0.05;
    if ((flags & 64) && h > 0) {  // night: stars
        float3 cell = floor(d * 380);
        float star = step(0.9965, hash(cell)) * hash(cell + 7.1);
        col += star * sky_color / PI * 6;
    }
    return float4(aces(col * exposure), 1);
}
)";

struct FrameCB {
    float view_proj[16];
    float cam[3];
    uint32_t selected;
    float sun[3];
    uint32_t flags;
    float flat[4];
    float sun_color[3];
    float exposure;
    float sky_color[3];
    uint32_t light_count;
    float ground_color[3];
    float fog_density;
    float fog_color[3];
    float shadow_texel;
    float shadow_proj[16];
    float ray_x[4], ray_y[4], ray_z[4];
    uint32_t tiles_x, tiles_y, slices;
    float slice_scale;
    float cluster_near;
    float cluster_pad[3];
};
struct MaterialCB {
    float base[4];
    float emissive[3];
    float cutoff;
    float uv_st[4];
    uint32_t flags;
    uint32_t pad[3];
};
struct Instance {
    float r[12];
    uint32_t node;
};
struct LineVertex {
    float p[3];
    uint32_t rgba;
};

// Node categories (from the extras the scene builder writes).
enum : unsigned {
    kIsCollider = 1, kIsTrigger = 2, kIsDecal = 4, kIsInactive = 8, kIsTerrain = 16, kIsLight = 32, kIsSpline = 64,
    kIsTrees = 128
};

// The object list's sections.
enum Category : uint8_t { kLevel, kTerrainCat, kPlants, kCollision, kDecals, kLights, kSplines, kCategories };
const char* kCategoryNames[kCategories] = {"Level", "Terrain", "Terrain plants", "Collision", "Decals", "Map lights", "Splines"};

struct Part {
    uint32_t first = 0, count = 0;
    int material = -1;
};
struct Buffers {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    V3 lo, hi;
    size_t vertices = 0;
};
struct Mesh {
    const Buffers* buffers = nullptr;
    std::vector<Part> parts;
    size_t triangles = 0;
    std::string name;
};
struct Material {
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* base = nullptr;
    ID3D11ShaderResourceView* emissive = nullptr;
    int alpha_mode = 0;
};
struct Node {
    int parent = -1;
    M4 world;
    V3 lo, hi;
    bool bounded = false;
    unsigned flags = 0;
    std::string lower;
};
struct Draw {
    int mesh;
    int part;
};

constexpr int kMarkerVertices = 8192;
constexpr double kPi = 3.14159265358979;

// Skate's times of day, from its environment assets (lighting/ve/tod/bam/ve_bam_high_<hhmm>):
// the sun (the moon at night), its illuminance and the auto-exposure range. Weather day and
// weather night have no environment of their own.
struct TimeOfDay {
    const char* name;
    double azimuth, elevation;  // degrees: toward the sun in the game is (sin az cos el, sin el, cos az cos el)
    float sun[3];
    double lux;
    double ev_min, ev_max;
    double fog_visibility;      // metres
};
const TimeOfDay kTimes[5] = {
    {"Morning", 356, 15, {1.0f, 0.817f, 0.623f}, 55000, 6.5, 10.0, 1700},
    {"Noon", 155, 43, {1.0f, 0.829f, 0.669f}, 65000, 7.0, 11.3, 5000},
    {"Afternoon", 241, 28, {1.0f, 0.911f, 0.810f}, 73000, 7.0, 11.4, 6000},
    {"Evening", 320, 13, {0.987f, 0.549f, 0.501f}, 55000, 7.0, 10.0, 1500},
    {"Night", 241, 55, {0.416f, 0.605f, 1.0f}, 100, 5.7, 7.4, 4100},
};
constexpr int kMaxLights = 1024;
constexpr int kShadowSize = 4096;

std::string lower_copy(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

uint32_t pack(int r, int g, int b, int a = 255) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

std::string with_commas(size_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

} // namespace

struct SceneView::Impl {
    ID3D11Device* device;
    ID3D11DeviceContext* ctx;
    Fonts fonts;

    // Pipeline
    ID3D11VertexShader* vs_main = nullptr;
    ID3D11VertexShader* vs_line = nullptr;
    ID3D11PixelShader* ps_main = nullptr;
    ID3D11PixelShader* ps_id = nullptr;
    ID3D11PixelShader* ps_flat = nullptr;
    ID3D11PixelShader* ps_line = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11InputLayout* line_layout = nullptr;
    ID3D11RasterizerState* rs_solid = nullptr;
    ID3D11RasterizerState* rs_wire = nullptr;
    ID3D11RasterizerState* rs_decal = nullptr;
    ID3D11RasterizerState* rs_overlay = nullptr;
    ID3D11DepthStencilState* ds_write = nullptr;
    ID3D11DepthStencilState* ds_read = nullptr;
    ID3D11DepthStencilState* ds_off = nullptr;
    ID3D11BlendState* bs_alpha = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* frame_cb = nullptr;
    ID3D11Buffer* default_cb = nullptr;
    ID3D11VertexShader* vs_sky = nullptr;
    ID3D11PixelShader* ps_sky = nullptr;
    ID3D11PixelShader* ps_shadow = nullptr;
    ID3D11Buffer* light_buf = nullptr;
    ID3D11ShaderResourceView* light_srv = nullptr;
    ID3D11Texture2D* shadow_tex = nullptr;
    ID3D11DepthStencilView* shadow_dsv = nullptr;
    ID3D11ShaderResourceView* shadow_srv = nullptr;
    ID3D11SamplerState* shadow_sampler = nullptr;
    ID3D11DepthStencilState* ds_shadow = nullptr;
    ID3D11RasterizerState* rs_shadow = nullptr;
    ID3D11ShaderResourceView* sky_srv = nullptr;  // the map's own skybox
    ID3D11Buffer* grid_buf = nullptr;
    ID3D11ShaderResourceView* grid_srv = nullptr;
    size_t grid_capacity = 0;
    ID3D11Buffer* index_buf = nullptr;
    ID3D11ShaderResourceView* index_srv = nullptr;
    size_t index_capacity = 0;
    std::string error;

    // Targets
    int tw = 0, th = 0;
    ID3D11Texture2D* color = nullptr;
    ID3D11RenderTargetView* color_rtv = nullptr;
    ID3D11ShaderResourceView* color_srv = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* ids = nullptr;
    ID3D11RenderTargetView* ids_rtv = nullptr;
    ID3D11Texture2D* id_staging = nullptr;
    ID3D11Texture2D* posmap = nullptr;  // world position under each pixel (picking)
    ID3D11RenderTargetView* pos_rtv = nullptr;
    ID3D11Texture2D* pos_staging = nullptr;

    // Scene
    bool has_scene = false;
    Preview preview;
    std::vector<Buffers*> buffers;
    std::vector<Mesh> meshes;  // per OutMesh
    std::vector<Material> materials;
    Material default_material;
    std::vector<ID3D11ShaderResourceView*> textures;
    std::vector<Node> nodes;
    std::vector<Draw> opaque, blended, decals;
    ID3D11Buffer* instances = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // per mesh: first instance, count
    std::vector<int> slot;                             // per node: its instance, -1 hidden
    ID3D11Buffer* lines = nullptr;
    uint32_t spline_lines = 0, auto_lines = 0;  // vertex counts (map splines first)
    V3 scene_lo, scene_hi;
    size_t drawn_triangles = 0, draw_calls = 0;

    // View settings
    bool show_textures = true, show_lighting = true, wireframe = false, show_hidden = false;
    bool show_splines = true, show_autosplines = true;
    // Lighting preview: 0 flat, 1 editor shading, 2.. the game's times of day (kTimes)
    int light_mode = 3;
    bool show_lights = true, show_shadows = true, show_fog = true, use_map_sky = true;
    float ev_comp = 0;
    FrameCB lighting = {};          // this frame's lighting (set_frame adds the camera)
    std::vector<int> light_nodes;   // the map's lights
    size_t lights_shown = 0;
    size_t cluster_entries = 0;
    // The map's lights are made editable on load (Edits::user_lights with a source); the
    // originals they replace (and ones deleted) are not drawn, lit or exported.
    std::unordered_map<std::string, int> light_by_key;  // map light node by key
    std::unordered_set<std::string> replaced_keys;      // this frame's replaced map lights
    bool lights_open = true;
    bool light_replaced(const OutNode& n) const { return replaced_keys.count(n.key) > 0; }
    void update_replaced() {
        replaced_keys.clear();
        if (!edits) return;
        for (auto& k : edits->dropped_lights) replaced_keys.insert(k);
        for (auto& l : edits->user_lights)
            if (!l.source.empty()) replaced_keys.insert(l.source);
    }
    // The map's light a node key names, as an editable light (its values from the map).
    bool map_light_values(const std::string& key, const Edits& e, UserLight& out) const {
        auto f = light_by_key.find(key);
        if (f == light_by_key.end()) return false;
        const OutNode& n = preview.scene.nodes[f->second];
        auto le = e.lights.find(key);
        out = light_from_map(n, preview.scene.lights[n.light], nodes[f->second].world, le != e.lights.end() ? &le->second : nullptr);
        return true;
    }
    bool unchanged_import(const Edits& e, const UserLight& l) const {
        UserLight m;
        return !l.source.empty() && map_light_values(l.source, e, m) && same_light(l, m);
    }
    bool filter_dirty = true;
    bool show_cat[kCategories] = {true, true, true, true, true, true, true};
    bool cat_open[kCategories] = {true, false, true, false, false, false, false};

    // Object list data (per node unless noted)
    std::vector<uint8_t> category;
    std::vector<int> node_plant;        // index into plants, or -1
    std::vector<int> lod_group;         // LOD group, or -1
    std::vector<uint32_t> lod_levels;   // the levels it draws at (bits)
    std::vector<int> lod_count;         // levels in its group
    std::unordered_map<int, uint32_t> group_present;  // per LOD group: levels that have a mesh
    std::vector<PlantInfo> plants;
    std::vector<std::vector<int>> plant_instances;    // per plant: its painted copies
    std::vector<std::vector<int>> members;            // per category: Level's tree roots, the others' objects
    size_t cat_count[kCategories] = {};
    std::unordered_set<int> hidden_plants, open_plants;

    // Map edits (owned by the window): shown live, edited in the inspector
    Edits* edits = nullptr;
    int edits_version = 0, built_version = -1;
    bool edits_changed = false;
    std::vector<uint8_t> edit_state;   // per node: 1 left out, 2 collision only
    std::vector<bool> material_hidden; // per material: collision only (invisible)
    ID3D11Buffer* ghost_instances = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> ghost_ranges;
    std::vector<int> ghost_slot;
    ID3D11Buffer* marker_vb = nullptr;
    size_t marker_capacity = kMarkerVertices;
    // Things placed or drawn in the window, as opposed to the map's own objects
    enum ItemKind { kNoItem, kSpawnItem, kBusItem, kLightItem, kAudioItem, kSplineItem, kCurveItem };
    struct Item {
        int kind = kNoItem;
        int index = 0;  // bus stop, light, audio volume or spline; kCurveItem: the map's curves, then the auto ones
        bool operator==(const Item& o) const { return kind == o.kind && (kind == kNoItem || index == o.index); }
    };
    Item item;       // the selected one
    int point = -1;  // the selected point of a selected spline
    enum Placing { kNotPlacing, kPlaceSpawn, kPlaceBus, kPlaceMove, kPlaceLight, kPlaceSpot, kPlaceAudio, kAddPoints, kPlaceArea };
    int placing = kNotPlacing;
    int insert_after = -1;  // kAddPoints: new points go after this one (-1: at the end)
    double draw_lift = 0;   // drawn points sit this far above the surface clicked
    // Gizmo drag: -1 none, 0 over the surfaces under the mouse, 1..3 along X, Y, Z
    int drag_axis = -1;
    bool press_on_gizmo = false;  // the current left press began on a handle (so it selects nothing)
    V3 drag_from;
    ImVec2 drag_mouse, drag_dir;  // where the drag began; the axis on screen, in pixels per metre
    char surface_filter[64] = {};
    char region_filter[64] = {};
    int editing_material = -1;
    bool markers_open = true, curves_open = true;
    bool set_open[2] = {false, false};  // the map's splines: map, auto lists opened
    float left_w = 0, right_w = 0;      // side panel widths (0: the default)
    int inspector_tab = 0;              // objects: 0 Studio settings, 1 details
    int curve_lines_version = -1;       // the edits version the curve lines were coloured for
    Item test_item;                     // SPOTBUILDER_SNAPSHOT_ITEM
    int test_select_all = -1;           // SPOTBUILDER_SNAPSHOT_SELECTALL
    int test_light = -1;                // SPOTBUILDER_SNAPSHOT_MAPLIGHT=<n>: select and frame the map's nth light
    std::string message;                // for the window to show once (copied, pasted...)
    // SPOTBUILDER_PROFILE=<file>: GPU time of each pass over 120 frames, written to the file
    std::wstring profile_file;
    ID3D11Query* prof_disjoint = nullptr;
    ID3D11Query* prof_stamps[8] = {};
    int prof_count = 0, prof_frames = 0;
    double prof_sum[8] = {}, prof_cpu = 0;
    void stamp() {
        if (prof_disjoint && prof_count < 8) ctx->End(prof_stamps[prof_count++]);
    }
    bool test_context = false;          // SPOTBUILDER_SNAPSHOT_CONTEXT
    float test_context_x = 0, test_context_y = 0;
    // The selection (see "selection" below)
    static constexpr int kNodeSel = 100;
    std::set<std::pair<int, int>> picked;
    // What the gizmo sits on: the things picked by hand. "Select all of a kind" and the like add to
    // the selection but leave this alone, so the gizmo stays where you were working.
    std::set<std::pair<int, int>> pivot_set;
    int sel_version = 0, built_sel_version = -1;
    std::pair<int, int> anchor{-1, -1};  // the list row a Shift+click extends from
    bool show_rails = false;             // grind rail preview on every grind curve
    bool curve_lines_dirty = true;
    bool right_flew = false;             // the right button flew the camera (so it opens no menu)
    // Gizmo drag of several things
    std::vector<V3*> drag_targets;
    std::vector<V3> drag_starts;
    V3 drag_hit_from;
    bool drag_has_hit = false, drag_snap = false, drag_curves = false;
    Item drag_pivot;  // the one thing a centre drag lands on surfaces (the rest follow it)
    // The context menu
    bool open_context = false;
    std::pair<int, int> ctx_target{-1, -1};  // what was right-clicked ({-1, -1}: nothing)
    int ctx_plant = -1, ctx_section = -1, ctx_set = -1;
    bool ctx_has_point = false;
    V3 ctx_point;
    ImVec2 context_pos{-1, -1};

    void bump() {  // an edit changed what the view draws: rebuild, redraw and tell the window to save
        ++edits_version;
        filter_dirty = true;
        edits_changed = true;
    }
    void touch() { edits_changed = true; }  // an edit the view draws live (markers, curves): just save

    // The LOD settings the instance buffer was built with (they switch live)
    int live_lod = 0, live_tree_lod = -1;
    bool live_all = false;
    std::map<std::string, int> live_plants;

    // Camera (Unity axes: left-handed, Y up)
    V3 cam{0, 10, -30};
    double yaw = 0, pitch = -0.2, speed = 10, orbit = 30;
    const double fov = 60.0 * 3.14159265358979 / 180.0;

    // Panels
    int selected = -1;
    bool scroll_to_selected = false;
    std::unordered_set<int> expanded;
    char search[256] = {};
    bool test_pick = false;
    bool test_marker = false;
    float test_pick_x = 0, test_pick_y = 0;

    Impl(ID3D11Device* d, ID3D11DeviceContext* c, const Fonts& f) : device(d), ctx(c), fonts(f) { create_pipeline(); }
    ~Impl() {
        clear();
        release_targets();
        for (auto** p : {(IUnknown**)&vs_main, (IUnknown**)&vs_line, (IUnknown**)&ps_main, (IUnknown**)&ps_id,
                         (IUnknown**)&ps_flat, (IUnknown**)&ps_line, (IUnknown**)&layout, (IUnknown**)&line_layout,
                         (IUnknown**)&rs_solid, (IUnknown**)&rs_wire, (IUnknown**)&rs_decal, (IUnknown**)&rs_overlay,
                         (IUnknown**)&ds_write, (IUnknown**)&ds_read, (IUnknown**)&ds_off, (IUnknown**)&bs_alpha,
                         (IUnknown**)&sampler, (IUnknown**)&frame_cb, (IUnknown**)&default_cb, (IUnknown**)&marker_vb,
                         (IUnknown**)&vs_sky, (IUnknown**)&ps_sky, (IUnknown**)&ps_shadow, (IUnknown**)&light_srv,
                         (IUnknown**)&light_buf, (IUnknown**)&shadow_srv, (IUnknown**)&shadow_dsv, (IUnknown**)&shadow_tex,
                         (IUnknown**)&shadow_sampler, (IUnknown**)&ds_shadow, (IUnknown**)&rs_shadow, (IUnknown**)&sky_srv,
                         (IUnknown**)&grid_srv, (IUnknown**)&grid_buf, (IUnknown**)&index_srv, (IUnknown**)&index_buf})
            release(*p);
    }

    // ---- pipeline ------------------------------------------------------------------------

    ID3DBlob* compile(const char* entry, const char* target) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(kViewShader, std::strlen(kViewShader), "scene_view", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            error = std::string("shader ") + entry + ": " + (errors ? (const char*)errors->GetBufferPointer() : "failed");
            release(code);
        }
        release(errors);
        return code;
    }

    void create_pipeline() {
        ID3DBlob* vsm = compile("vs_main", "vs_4_0");
        ID3DBlob* vsl = compile("vs_line", "vs_4_0");
        ID3DBlob* psm = compile("ps_main", "ps_4_0");
        ID3DBlob* psi = compile("ps_id", "ps_4_0");
        ID3DBlob* psf = compile("ps_flat", "ps_4_0");
        ID3DBlob* psl = compile("ps_line", "ps_4_0");
        if (vsm && vsl && psm && psi && psf && psl) {
            device->CreateVertexShader(vsm->GetBufferPointer(), vsm->GetBufferSize(), nullptr, &vs_main);
            device->CreateVertexShader(vsl->GetBufferPointer(), vsl->GetBufferSize(), nullptr, &vs_line);
            device->CreatePixelShader(psm->GetBufferPointer(), psm->GetBufferSize(), nullptr, &ps_main);
            device->CreatePixelShader(psi->GetBufferPointer(), psi->GetBufferSize(), nullptr, &ps_id);
            device->CreatePixelShader(psf->GetBufferPointer(), psf->GetBufferSize(), nullptr, &ps_flat);
            device->CreatePixelShader(psl->GetBufferPointer(), psl->GetBufferSize(), nullptr, &ps_line);
            const D3D11_INPUT_ELEMENT_DESC main[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                {"WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                {"WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                {"NODE", 0, DXGI_FORMAT_R32_UINT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            };
            device->CreateInputLayout(main, 7, vsm->GetBufferPointer(), vsm->GetBufferSize(), &layout);
            const D3D11_INPUT_ELEMENT_DESC line[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            device->CreateInputLayout(line, 2, vsl->GetBufferPointer(), vsl->GetBufferSize(), &line_layout);
        }
        for (ID3DBlob* b : {vsm, vsl, psm, psi, psf, psl})
            if (b) b->Release();

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;  // mirrored copies and one-sided cards both show
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &rs_solid);
        rd.FillMode = D3D11_FILL_WIREFRAME;
        device->CreateRasterizerState(&rd, &rs_wire);
        rd.DepthBias = 64;  // reversed depth: positive pulls toward the camera
        rd.SlopeScaledDepthBias = 1.5f;
        device->CreateRasterizerState(&rd, &rs_overlay);
        rd.FillMode = D3D11_FILL_SOLID;
        device->CreateRasterizerState(&rd, &rs_decal);

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;  // reversed Z
        device->CreateDepthStencilState(&dd, &ds_write);
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        device->CreateDepthStencilState(&dd, &ds_read);
        dd.DepthEnable = FALSE;
        device->CreateDepthStencilState(&dd, &ds_off);

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &bs_alpha);

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &sampler);

        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(FrameCB);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&cb, nullptr, &frame_cb);

        D3D11_BUFFER_DESC mb = {};
        mb.ByteWidth = sizeof(LineVertex) * kMarkerVertices;
        mb.Usage = D3D11_USAGE_DYNAMIC;
        mb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        mb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&mb, nullptr, &marker_vb);

        MaterialCB grey = {{0.7f, 0.7f, 0.72f, 1}, {0, 0, 0}, 0.5f, {1, 1, 0, 0}, 0, {}};
        default_cb = constant_buffer(grey);
        default_material.cb = default_cb;

        // Lighting preview: the sky, the sun's shadow map and the light buffer
        ID3DBlob* vss = compile("vs_sky", "vs_4_0");
        ID3DBlob* pss = compile("ps_sky", "ps_4_0");
        ID3DBlob* psh = compile("ps_shadow", "ps_4_0");
        if (vss) device->CreateVertexShader(vss->GetBufferPointer(), vss->GetBufferSize(), nullptr, &vs_sky);
        if (pss) device->CreatePixelShader(pss->GetBufferPointer(), pss->GetBufferSize(), nullptr, &ps_sky);
        if (psh) device->CreatePixelShader(psh->GetBufferPointer(), psh->GetBufferSize(), nullptr, &ps_shadow);
        for (ID3DBlob* b : {vss, pss, psh})
            if (b) b->Release();
        D3D11_BUFFER_DESC lb = {};
        lb.ByteWidth = 16 * 4 * kMaxLights;
        lb.Usage = D3D11_USAGE_DYNAMIC;
        lb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        lb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(device->CreateBuffer(&lb, nullptr, &light_buf))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC lv = {};
            lv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            lv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            lv.Buffer.FirstElement = 0;
            lv.Buffer.NumElements = 4 * kMaxLights;
            device->CreateShaderResourceView(light_buf, &lv, &light_srv);
        }
        D3D11_TEXTURE2D_DESC st = {};
        st.Width = st.Height = kShadowSize;
        st.MipLevels = st.ArraySize = 1;
        st.Format = DXGI_FORMAT_R32_TYPELESS;
        st.SampleDesc.Count = 1;
        st.Usage = D3D11_USAGE_DEFAULT;
        st.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&st, nullptr, &shadow_tex))) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
            dv.Format = DXGI_FORMAT_D32_FLOAT;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(shadow_tex, &dv, &shadow_dsv);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_FLOAT;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(shadow_tex, &sv, &shadow_srv);
        }
        D3D11_SAMPLER_DESC cs = {};
        cs.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cs.AddressU = cs.AddressV = cs.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        cs.BorderColor[0] = cs.BorderColor[1] = cs.BorderColor[2] = cs.BorderColor[3] = 1;
        cs.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        cs.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&cs, &shadow_sampler);
        D3D11_DEPTH_STENCIL_DESC sd2 = {};
        sd2.DepthEnable = TRUE;
        sd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        sd2.DepthFunc = D3D11_COMPARISON_LESS;
        device->CreateDepthStencilState(&sd2, &ds_shadow);
        D3D11_RASTERIZER_DESC sr = {};
        sr.FillMode = D3D11_FILL_SOLID;
        sr.CullMode = D3D11_CULL_NONE;
        sr.DepthClipEnable = FALSE;  // casters beyond the box still cast
        sr.DepthBias = 400;
        sr.SlopeScaledDepthBias = 2.0f;
        device->CreateRasterizerState(&sr, &rs_shadow);
    }

    ID3D11Buffer* constant_buffer(const MaterialCB& data) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(MaterialCB);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {&data, 0, 0};
        ID3D11Buffer* b = nullptr;
        device->CreateBuffer(&d, &init, &b);
        return b;
    }

    ID3D11Buffer* static_buffer(const void* data, size_t bytes, UINT bind) {
        if (!bytes) return nullptr;
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (UINT)bytes;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
        ID3D11Buffer* b = nullptr;
        device->CreateBuffer(&d, &init, &b);
        return b;
    }

    // ---- targets -------------------------------------------------------------------------

    void release_targets() {
        release(color_srv);
        release(color_rtv);
        release(color);
        release(dsv);
        release(depth);
        release(ids_rtv);
        release(ids);
        release(id_staging);
        release(pos_rtv);
        release(posmap);
        release(pos_staging);
        tw = th = 0;
    }

    void ensure_targets(int w, int h) {
        if (w == tw && h == th && color) return;
        release_targets();
        tw = w, th = h;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // Stored sRGB: rendered through an sRGB view, shown by ImGui through a plain one.
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&td, nullptr, &color);
        D3D11_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(color, &rv, &color_rtv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(color, &sv, &color_srv);
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        device->CreateTexture2D(&td, nullptr, &depth);
        device->CreateDepthStencilView(depth, nullptr, &dsv);
        td.Format = DXGI_FORMAT_R32_UINT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&td, nullptr, &ids);
        device->CreateRenderTargetView(ids, nullptr, &ids_rtv);
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateTexture2D(&td, nullptr, &posmap);
        device->CreateRenderTargetView(posmap, nullptr, &pos_rtv);
        td.Width = td.Height = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&td, nullptr, &pos_staging);
        td.Format = DXGI_FORMAT_R32_UINT;
        device->CreateTexture2D(&td, nullptr, &id_staging);
    }

    // ---- scene upload --------------------------------------------------------------------

    void clear() {
        for (Buffers* b : buffers) {
            release(b->vb);
            release(b->ib);
            delete b;
        }
        buffers.clear();
        for (auto& m : materials) release(m.cb);
        materials.clear();
        for (auto*& t : textures) release(t);
        textures.clear();
        release(instances);
        release(ghost_instances);
        release(lines);
        meshes.clear();
        nodes.clear();
        opaque.clear(), blended.clear(), decals.clear();
        ranges.clear();
        slot.clear();
        preview = Preview();
        has_scene = false;
        selected = -1;
        item = {};
        point = -1;
        picked.clear();
        pivot_set.clear();
        ++sel_version;
        light_nodes.clear();
        release(sky_srv);
        placing = kNotPlacing;
        drag_axis = -1;
        drag_targets.clear();
        editing_material = -1;
        expanded.clear();
    }

    // An sRGB texture with a box-filtered mip chain; rows stay bottom-up so Unity UVs sample it
    // directly.
    ID3D11ShaderResourceView* texture(const ImageJob& job) {
        if (!job.ok || job.pixels.empty() || job.px_w <= 0 || job.px_h <= 0) return nullptr;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<std::pair<int, int>> dims;
        levels.push_back(job.pixels);
        dims.push_back({job.px_w, job.px_h});
        while (dims.back().first > 1 || dims.back().second > 1) {
            auto [w, h] = dims.back();
            int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            const std::vector<uint8_t>& src = levels.back();
            std::vector<uint8_t> dst((size_t)nw * nh * 4);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
                        int y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
                        int s = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                                src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
                        dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                    }
            levels.push_back(std::move(dst));
            dims.push_back({nw, nh});
        }
        std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
        for (size_t i = 0; i < levels.size(); ++i) init[i] = {levels[i].data(), (UINT)dims[i].first * 4, 0};
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)job.px_w;
        td.Height = (UINT)job.px_h;
        td.MipLevels = (UINT)levels.size();
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        device->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
        return srv;
    }

    void set(Preview&& p) {
        clear();
        preview = std::move(p);
        Scene& sc = preview.scene;

        // Textures, then materials
        textures.assign(sc.images.size(), nullptr);
        for (size_t i = 0; i < sc.images.size(); ++i) {
            textures[i] = texture(sc.images[i]);
            sc.images[i].pixels = {};
        }
        auto srv = [&](const TexRef& t) -> ID3D11ShaderResourceView* {
            return t.image >= 0 && t.image < (int)textures.size() ? textures[t.image] : nullptr;
        };
        for (auto& m : sc.materials) {
            MaterialCB cb = {};
            std::copy(m.base, m.base + 4, cb.base);
            for (int k = 0; k < 3; ++k) cb.emissive[k] = m.emissive[k] * std::min(m.emissive_strength, 4.0f);
            cb.cutoff = m.alpha_cutoff;
            cb.uv_st[0] = m.uv_transform ? m.uv_scale[0] : 1;
            cb.uv_st[1] = m.uv_transform ? m.uv_scale[1] : 1;
            cb.uv_st[2] = m.uv_transform ? m.uv_offset[0] : 0;
            cb.uv_st[3] = m.uv_transform ? m.uv_offset[1] : 0;
            Material g;
            g.base = srv(m.base_tex);
            g.emissive = srv(m.emissive_tex);
            g.alpha_mode = m.alpha_mode;
            cb.flags = (g.base ? 1u : 0u) | (m.alpha_mode == 1 ? 2u : 0u) | (m.alpha_mode == 2 ? 4u : 0u) |
                       (m.unlit ? 8u : 0u) | (g.emissive ? 16u : 0u);
            g.cb = constant_buffer(cb);
            materials.push_back(g);
        }

        // Meshes: one vertex/index buffer per shared mesh, parts per submesh
        std::unordered_map<const MeshData*, Buffers*> by_data;
        meshes.resize(sc.meshes.size());
        for (size_t i = 0; i < sc.meshes.size(); ++i) {
            const OutMesh& om = sc.meshes[i];
            Mesh& gm = meshes[i];
            gm.name = om.name;
            if (!om.data || om.lines || !om.data->vertex_count) continue;
            const MeshData& md = *om.data;
            Buffers*& b = by_data[&md];
            if (!b) {
                b = new Buffers;
                buffers.push_back(b);
                size_t n = md.vertex_count;
                int set = 0;
                for (int k = 0; k < 4; ++k)
                    if (!md.uv[k].empty()) {
                        set = k;
                        break;
                    }
                std::vector<float> v(n * 8, 0.0f);
                b->lo = {1e30, 1e30, 1e30}, b->hi = {-1e30, -1e30, -1e30};
                for (size_t k = 0; k < n; ++k) {
                    float* o = &v[k * 8];
                    std::copy(&md.pos[k * 3], &md.pos[k * 3] + 3, o);
                    if (md.nrm.size() == n * 3) std::copy(&md.nrm[k * 3], &md.nrm[k * 3] + 3, o + 3);
                    else o[4] = 1;
                    if (md.uv[set].size() == n * 2) std::copy(&md.uv[set][k * 2], &md.uv[set][k * 2] + 2, o + 6);
                    V3 p{o[0], o[1], o[2]};
                    b->lo = vmin(b->lo, p), b->hi = vmax(b->hi, p);
                }
                std::vector<uint32_t> idx;
                for (auto& s : md.subs) idx.insert(idx.end(), s.idx.begin(), s.idx.end());
                b->vb = static_buffer(v.data(), v.size() * 4, D3D11_BIND_VERTEX_BUFFER);
                b->ib = static_buffer(idx.data(), idx.size() * 4, D3D11_BIND_INDEX_BUFFER);
                b->vertices = n;
            }
            gm.buffers = b;
            uint32_t at = 0;
            for (size_t s = 0; s < md.subs.size(); ++s) {
                Part part;
                part.first = at;
                part.count = (uint32_t)md.subs[s].idx.size();
                part.material = s < om.materials.size() ? om.materials[s] : -1;
                at += part.count;
                if (part.count && part.material != -2) {
                    gm.parts.push_back(part);
                    gm.triangles += part.count / 3;
                }
            }
        }

        // Nodes: world matrices, categories, bounds
        nodes.resize(sc.nodes.size());
        for (size_t i = 0; i < sc.nodes.size(); ++i) {
            const OutNode& n = sc.nodes[i];
            Node& g = nodes[i];
            g.lower = lower_copy(n.name);
            for (auto& [k, v] : n.extras.o) {
                if (k == "xl_collider") g.flags |= kIsCollider;
                else if (k == "xl_trigger") g.flags |= kIsTrigger;
                else if (k == "xl_decal") g.flags |= kIsDecal;
                else if (k == "xl_inactive") g.flags |= kIsInactive;
                else if (k == "xl_terrain") g.flags |= kIsTerrain;
                else if (k == "xl_spline") g.flags |= kIsSpline;
                else if (k == "xl_terrain_trees") g.flags |= kIsTrees;
            }
            if (n.light >= 0) g.flags |= kIsLight;
            for (int c : n.children)
                if (c >= 0 && c < (int)nodes.size()) nodes[c].parent = (int)i;
        }
        scene_lo = {1e30, 1e30, 1e30}, scene_hi = {-1e30, -1e30, -1e30};
        std::vector<std::pair<int, M4>> stack;
        for (int r : sc.roots) stack.push_back({r, M4{}});
        while (!stack.empty()) {
            auto [i, parent] = stack.back();
            stack.pop_back();
            if (i < 0 || i >= (int)nodes.size()) continue;
            const OutNode& n = sc.nodes[i];
            Node& g = nodes[i];
            g.world = parent * trs(n.t, n.r, n.s);
            if (n.mesh >= 0 && n.mesh < (int)meshes.size() && meshes[n.mesh].buffers) {
                const Buffers& b = *meshes[n.mesh].buffers;
                g.lo = {1e30, 1e30, 1e30}, g.hi = {-1e30, -1e30, -1e30};
                for (int c = 0; c < 8; ++c) {
                    V3 p = xform_point(g.world, {c & 1 ? b.hi.x : b.lo.x, c & 2 ? b.hi.y : b.lo.y, c & 4 ? b.hi.z : b.lo.z});
                    g.lo = vmin(g.lo, p), g.hi = vmax(g.hi, p);
                }
                g.bounded = true;
                if (!(g.flags & (kIsCollider | kIsInactive))) scene_lo = vmin(scene_lo, g.lo), scene_hi = vmax(scene_hi, g.hi);
            }
            for (int c : n.children) stack.push_back({c, g.world});
        }
        if (scene_lo.x > scene_hi.x) scene_lo = {-10, -10, -10}, scene_hi = {10, 10, 10};

        // Draw lists by how the material blends
        for (size_t i = 0; i < meshes.size(); ++i)
            for (size_t p = 0; p < meshes[i].parts.size(); ++p) {
                int mi = meshes[i].parts[p].material;
                int mode = mi >= 0 && mi < (int)materials.size() ? materials[mi].alpha_mode : 0;
                (mode == 2 ? blended : opaque).push_back({(int)i, (int)p});
            }
        std::vector<bool> decal_mesh(meshes.size(), false);
        for (size_t i = 0; i < sc.nodes.size(); ++i)
            if ((nodes[i].flags & kIsDecal) && sc.nodes[i].mesh >= 0) decal_mesh[sc.nodes[i].mesh] = true;
        auto move_decals = [&](std::vector<Draw>& list) {
            for (auto it = list.begin(); it != list.end();)
                if (decal_mesh[it->mesh]) decals.push_back(*it), it = list.erase(it);
                else ++it;
        };
        move_decals(opaque);
        move_decals(blended);

        build_curve_lines();

        // Categories, LOD tags and painted plants
        size_t count = sc.nodes.size();
        category.assign(count, kLevel);
        node_plant.assign(count, -1);
        lod_group.assign(count, -1);
        lod_levels.assign(count, 0);
        lod_count.assign(count, 0);
        group_present.clear();
        plants = preview.plants;
        plant_instances.assign(plants.size(), {});
        members.assign(kCategories, {});
        std::fill(std::begin(cat_count), std::end(cat_count), 0);
        std::unordered_map<std::string, int> plant_index;
        for (size_t i = 0; i < plants.size(); ++i) plant_index[plants[i].name] = (int)i;
        for (size_t i = 0; i < count; ++i) {
            for (auto& [k, v] : sc.nodes[i].extras.o) {
                if (k == "xl_lod_group" && v.t == Json::Int) lod_group[i] = (int)v.i;
                else if (k == "xl_lod_levels" && v.t == Json::Int) lod_levels[i] = (uint32_t)v.i;
                else if (k == "xl_lod_count" && v.t == Json::Int) lod_count[i] = (int)v.i;
                else if (k == "xl_plant" && v.t == Json::Str) {
                    auto it = plant_index.find(v.s);
                    if (it != plant_index.end()) node_plant[i] = it->second;
                }
            }
            if (lod_group[i] >= 0 && sc.nodes[i].mesh >= 0) group_present[lod_group[i]] |= lod_levels[i];
        }
        struct Visit {
            int node;
            uint8_t inherited;
            int plant;
        };
        std::vector<Visit> walk;
        for (int r : sc.roots) {
            walk.push_back({r, kLevel, -1});
            members[kLevel].push_back(r);
        }
        while (!walk.empty()) {
            Visit v = walk.back();
            walk.pop_back();
            if (v.node < 0 || v.node >= (int)count) continue;
            const Node& g = nodes[v.node];
            uint8_t c = v.inherited;
            if (g.flags & kIsCollider) c = kCollision;
            else if (g.flags & kIsDecal) c = kDecals;
            else if (g.flags & kIsTerrain) c = kTerrainCat;
            else if (g.flags & kIsSpline) c = kSplines;
            else if (g.flags & kIsLight) c = kLights;
            category[v.node] = c;
            if (node_plant[v.node] < 0) node_plant[v.node] = v.plant;
            int plant = node_plant[v.node];
            if (c != kLevel && c != kPlants) members[c].push_back(v.node);
            if (sc.nodes[v.node].mesh >= 0 || sc.nodes[v.node].light >= 0) ++cat_count[c];
            for (int ch : sc.nodes[v.node].children) {
                if (ch < 0 || ch >= (int)count) continue;
                if (g.flags & kIsTrees) {  // a terrain's painted copies
                    int p = node_plant[ch];
                    if (p >= 0) plant_instances[p].push_back(ch);
                    walk.push_back({ch, kPlants, p});
                } else {
                    walk.push_back({ch, c == kPlants ? kPlants : kLevel, plant});
                }
            }
        }
        for (auto& list : members) std::sort(list.begin(), list.end());
        light_nodes.clear();
        for (size_t i = 0; i < count; ++i)
            if (sc.nodes[i].light >= 0 && sc.nodes[i].light < (int)sc.lights.size()) light_nodes.push_back((int)i);
        light_by_key.clear();
        for (int i : light_nodes)
            if (!sc.nodes[i].key.empty()) light_by_key[sc.nodes[i].key] = i;

        // Keep only what the panels show: vertex data lives on the GPU now.
        for (auto& om : sc.meshes) om.data.reset();
        has_scene = true;
        filter_dirty = true;
        wchar_t keys_file[1024] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_KEYS", keys_file, 1024)) {  // test hook: object and material keys
            FILE* f = _wfopen(keys_file, L"wb");
            if (f) {
                for (auto& n : sc.nodes) std::fprintf(f, "node\t%s\t%s\n", n.key.c_str(), n.name.c_str());
                for (auto& m : sc.materials) std::fprintf(f, "material\t%s\t%s\n", m.key.c_str(), m.name.c_str());
                std::fclose(f);
            }
        }
        test_marker = GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_MARKER", nullptr, 0) > 0;
        wchar_t item_env[64] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_ITEM", item_env, 64))  // test hook: <kind>,<index> to select
            swscanf_s(item_env, L"%d,%d", &test_item.kind, &test_item.index);
        wchar_t prof_env[1024] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_PROFILE", prof_env, 1024)) profile_file = prof_env;
        wchar_t light_env[16] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_LIGHT", light_env, 16)) light_mode = std::clamp(_wtoi(light_env), 0, 6);
        wchar_t ml_env[32] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_MAPLIGHT", ml_env, 32)) test_light = _wtoi(ml_env);
        wchar_t all_env[32] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_SELECTALL", all_env, 32))  // <category>, 99: every map spline
            test_select_all = _wtoi(all_env);
        wchar_t ctx_env[64] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_CONTEXT", ctx_env, 64) &&
            swscanf_s(ctx_env, L"%f,%f", &test_context_x, &test_context_y) == 2)
            test_context = true;
        wchar_t env[64] = {};
        if (GetEnvironmentVariableW(L"SPOTBUILDER_SNAPSHOT_PICK", env, 64) &&
            swscanf_s(env, L"%f,%f", &test_pick_x, &test_pick_y) == 2)
            test_pick = true;
        release(sky_srv);
        if (preview.sky.ok()) sky_srv = sky_texture(preview.sky.img);
        preview.sky.img.px = {};
        frame_bounds(scene_lo, scene_hi, true);
    }

    // The map's skybox panorama (rows top-down, sRGB).
    ID3D11ShaderResourceView* sky_texture(const Image& img) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)img.w;
        td.Height = (UINT)img.h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {img.px.data(), (UINT)img.w * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&td, &init, &tex))) device->CreateShaderResourceView(tex, nullptr, &srv);
        release(tex);
        return srv;
    }

    // Whether an object is drawn: its section and plant type are shown, it is not a hidden
    // object (unless those are shown), and it belongs to the LOD level currently chosen.
    bool visible(int i) const {
        if (!show_cat[category[i]]) return false;
        if ((nodes[i].flags & kIsInactive) && !show_hidden) return false;
        int p = node_plant[i];
        if (p >= 0 && hidden_plants.count(p)) return false;
        if (lod_group[i] >= 0 && !live_all) {
            int want = live_lod;
            if (p >= 0) {
                auto own = live_plants.find(plants[p].name);
                want = own != live_plants.end() ? own->second : live_tree_lod >= 0 ? live_tree_lod : live_lod;
            }
            int chosen = std::clamp(want, 0, std::max(0, lod_count[i] - 1));
            auto present = group_present.find(lod_group[i]);
            uint32_t have = present != group_present.end() ? present->second : 0;
            while (chosen > 0 && !(have & (1u << std::min(chosen, 31)))) --chosen;  // a level without meshes falls back
            if (!(lod_levels[i] & (1u << std::min(chosen, 31)))) return false;
        }
        return true;
    }

    // Picks up LOD choices made anywhere (settings drawer, plant rows): they only filter.
    void apply_live_lods(const Job& job) {
        const Options& o = job.opt;
        if (o.lod == live_lod && o.tree_lod == live_tree_lod && o.all_lods == live_all && o.plant_lod == live_plants) return;
        live_lod = o.lod, live_tree_lod = o.tree_lod, live_all = o.all_lods, live_plants = o.plant_lod;
        filter_dirty = true;
    }

    // Whether a material is collision only as ripped (the collider-only XL_Collision material).
    static bool material_invisible_default(const OutMaterial& m) {
        for (auto& [k, v] : m.extras.o)
            if (k == "sk8_material" && v.t == Json::Obj)
                for (auto& [k2, v2] : v.o)
                    if (k2 == "invisible" && v2.t == Json::Bool) return v2.b;
        return false;
    }

    // Instance buffers of the drawn objects grouped by mesh: normal ones, and "ghosts" (objects set
    // to collision only), drawn as outlines. Objects left out of the export are not drawn.
    void rebuild_instances() {
        filter_dirty = false;
        built_version = edits_version;
        built_sel_version = sel_version;
        release(instances);
        release(ghost_instances);
        const Scene& sc = preview.scene;
        // what the edits do to each object (a left-out object takes its children with it)
        edit_state.assign(nodes.size(), 0);
        material_hidden.assign(sc.materials.size(), false);
        for (size_t m = 0; m < sc.materials.size(); ++m) material_hidden[m] = material_invisible_default(sc.materials[m]);
        if (edits) {
            for (size_t m = 0; m < sc.materials.size(); ++m) {
                auto it = edits->materials.find(sc.materials[m].key);
                if (it != edits->materials.end() && it->second.invisible) material_hidden[m] = *it->second.invisible;
            }
            if (!edits->objects.empty()) {
                std::vector<std::pair<int, bool>> stack;
                for (int r : sc.roots) stack.push_back({r, false});
                while (!stack.empty()) {
                    auto [i, gone] = stack.back();
                    stack.pop_back();
                    if (i < 0 || i >= (int)nodes.size()) continue;
                    auto it = edits->objects.find(sc.nodes[i].key);
                    if (it != edits->objects.end()) {
                        gone = gone || it->second.removed;
                        if (it->second.collision_only) edit_state[i] = 2;
                    }
                    if (gone) edit_state[i] = 1;
                    for (int c : sc.nodes[i].children) stack.push_back({c, gone});
                }
            }
        }
        auto build = [&](uint8_t want, std::vector<std::pair<uint32_t, uint32_t>>& rng, std::vector<int>& slots) {
            std::vector<std::vector<int>> by_mesh(meshes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                int m = sc.nodes[i].mesh;
                if (m >= 0 && m < (int)meshes.size() && meshes[m].buffers && edit_state[i] == want && visible((int)i))
                    by_mesh[m].push_back((int)i);
            }
            std::vector<Instance> data;
            rng.assign(meshes.size(), {0, 0});
            slots.assign(nodes.size(), -1);
            for (size_t m = 0; m < meshes.size(); ++m) {
                rng[m] = {(uint32_t)data.size(), (uint32_t)by_mesh[m].size()};
                for (int i : by_mesh[m]) {
                    Instance in = {};
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 4; ++c) in.r[r * 4 + c] = (float)nodes[i].world.m[r][c];
                    in.node = (uint32_t)i | (node_picked(i) ? 0x80000000u : 0u);
                    slots[i] = (int)data.size();
                    data.push_back(in);
                }
            }
            return static_buffer(data.data(), data.size() * sizeof(Instance), D3D11_BIND_VERTEX_BUFFER);
        };
        instances = build(0, ranges, slot);
        ghost_instances = build(2, ghost_ranges, ghost_slot);
    }

    // ---- camera --------------------------------------------------------------------------

    V3 forward() const { return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)}; }
    V3 right() const { return normalize(cross(V3{0, 1, 0}, forward())); }
    V3 up() const { return cross(forward(), right()); }

    void frame_bounds(const V3& lo, const V3& hi, bool whole_scene) {
        V3 c = (lo + hi) * 0.5;
        double r = std::max(0.5, length(hi - lo) * 0.5);
        if (whole_scene) yaw = 0.6, pitch = -0.45;
        orbit = r / std::sin(fov * 0.5) * 1.05;
        cam = c - forward() * orbit;
        if (whole_scene) speed = std::clamp(r * 0.1, 4.0, 80.0);
    }

    void frame_selection() {
        V3 lo, hi;
        if (picked.size() > 1 && selection_bounds(lo, hi)) {
            frame_bounds(lo, hi, false);
            return;
        }
        if (item_bounds(item, lo, hi)) {
            frame_bounds(lo, hi, false);
            return;
        }
        if (selected >= 0 && nodes[selected].bounded) {
            frame_bounds(nodes[selected].lo, nodes[selected].hi, false);
        } else if (selected >= 0) {  // nothing to measure (a light): around where it is
            V3 p = node_position(selected);
            frame_bounds(p - V3{2, 2, 2}, p + V3{2, 2, 2}, false);
        } else {
            frame_bounds(scene_lo, scene_hi, true);
        }
    }

    V3 node_position(int i) const { return {nodes[i].world.m[0][3], nodes[i].world.m[1][3], nodes[i].world.m[2][3]}; }

    void view_proj(float out[16]) const {
        V3 f = forward(), r = right(), u = up();
        double aspect = th > 0 ? (double)tw / th : 1;
        double fy = 1.0 / std::tan(fov * 0.5), fx = fy / aspect, near_z = 0.05;
        double view[4][4] = {{r.x, r.y, r.z, -dot(r, cam)}, {u.x, u.y, u.z, -dot(u, cam)}, {f.x, f.y, f.z, -dot(f, cam)}, {0, 0, 0, 1}};
        // Reversed, infinite far plane: depth = near / z.
        double m[4][4] = {{fx * view[0][0], fx * view[0][1], fx * view[0][2], fx * view[0][3]},
                          {fy * view[1][0], fy * view[1][1], fy * view[1][2], fy * view[1][3]},
                          {0, 0, 0, near_z},
                          {view[2][0], view[2][1], view[2][2], view[2][3]}};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = (float)m[i][j];
    }

    // ---- rendering -----------------------------------------------------------------------

    void set_frame(float a, ImVec4 colour = col::accent) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(frame_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        FrameCB f = lighting;
        view_proj(f.view_proj);
        f.cam[0] = (float)cam.x, f.cam[1] = (float)cam.y, f.cam[2] = (float)cam.z;
        f.selected = selected >= 0 ? (uint32_t)selected : 0xffffffffu;
        f.flat[0] = colour.x, f.flat[1] = colour.y, f.flat[2] = colour.z, f.flat[3] = a;
        double aspect = th > 0 ? (double)tw / th : 1, ty = std::tan(fov * 0.5), tx = ty * aspect;
        V3 rx = right() * tx, ry = up() * ty, rz = forward();
        f.ray_x[0] = (float)rx.x, f.ray_x[1] = (float)rx.y, f.ray_x[2] = (float)rx.z;
        f.ray_y[0] = (float)ry.x, f.ray_y[1] = (float)ry.y, f.ray_y[2] = (float)ry.z;
        f.ray_z[0] = (float)rz.x, f.ray_z[1] = (float)rz.y, f.ray_z[2] = (float)rz.z;
        std::memcpy(mapped.pData, &f, sizeof f);
        ctx->Unmap(frame_cb, 0);
    }

    void bind_mesh_pipeline(ID3D11PixelShader* ps) {
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_main, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &frame_cb);
        ctx->PSSetConstantBuffers(0, 1, &frame_cb);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetSamplers(1, 1, &shadow_sampler);
        ctx->PSSetShaderResources(2, 1, &light_srv);
        ctx->PSSetShaderResources(3, 1, &shadow_srv);
        ctx->PSSetShaderResources(5, 1, &grid_srv);
        ctx->PSSetShaderResources(6, 1, &index_srv);
    }

    // Draws the listed parts for every instance in `inst`. parts: 0 all, 1 those whose material is
    // drawn, 2 those whose material was set to collision only.
    void draw_list(const std::vector<Draw>& list, bool count, ID3D11Buffer* inst,
                   const std::vector<std::pair<uint32_t, uint32_t>>& rng, int parts = 1) {
        if (!inst) return;
        const Buffers* bound = nullptr;
        for (const Draw& d : list) {
            auto [first, n] = rng[d.mesh];
            if (!n) continue;
            const Mesh& m = meshes[d.mesh];
            const Part& p = m.parts[d.part];
            if (parts) {
                bool hidden = p.material >= 0 && p.material < (int)material_hidden.size() && material_hidden[p.material];
                if (hidden != (parts == 2)) continue;
            }
            if (m.buffers != bound) {
                bound = m.buffers;
                ID3D11Buffer* vbs[2] = {bound->vb, inst};
                UINT strides[2] = {32, sizeof(Instance)}, offsets[2] = {0, 0};
                ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
                ctx->IASetIndexBuffer(bound->ib, DXGI_FORMAT_R32_UINT, 0);
            }
            const Material& mat = p.material >= 0 && p.material < (int)materials.size() ? materials[p.material] : default_material;
            ctx->VSSetConstantBuffers(1, 1, &mat.cb);
            ctx->PSSetConstantBuffers(1, 1, &mat.cb);
            ID3D11ShaderResourceView* srvs[2] = {mat.base, mat.emissive};
            ctx->PSSetShaderResources(0, 2, srvs);
            ctx->DrawIndexedInstanced(p.count, n, p.first, 0, first);
            if (count) {
                drawn_triangles += (size_t)p.count / 3 * n;
                ++draw_calls;
            }
        }
    }

    // Spawn and bus stops: a ring on the ground, a post, and an arrow along the facing.
    void marker_lines(std::vector<LineVertex>& out, const Marker& m, uint32_t rgba, bool bus) const {
        auto add = [&](V3 a, V3 b) {
            out.push_back({{(float)a.x, (float)a.y, (float)a.z}, rgba});
            out.push_back({{(float)b.x, (float)b.y, (float)b.z}, rgba});
        };
        V3 p = m.position;
        double yr = m.yaw * kPi / 180.0;
        V3 d{std::sin(yr), 0, std::cos(yr)}, r{std::cos(yr), 0, -std::sin(yr)};
        double radius = bus ? 0.9 : 0.6;
        for (int k = 0; k < 32; ++k) {
            double a0 = k * 2 * kPi / 32, a1 = (k + 1) * 2 * kPi / 32;
            add(p + V3{std::cos(a0) * radius, 0.02, std::sin(a0) * radius}, p + V3{std::cos(a1) * radius, 0.02, std::sin(a1) * radius});
        }
        V3 top = p + V3{0, bus ? 2.6 : 1.8, 0};
        add(p, top);
        V3 tip = p + d * (radius + 0.9) + V3{0, 0.02, 0};
        add(p + V3{0, 0.02, 0}, tip);
        add(tip, tip - d * 0.35 + r * 0.25);
        add(tip, tip - d * 0.35 - r * 0.25);
        if (bus) {  // a sign at the top
            add(top, top + r * 0.6);
            add(top + r * 0.6, top + r * 0.6 - V3{0, 0.5, 0});
            add(top + r * 0.6 - V3{0, 0.5, 0}, top - V3{0, 0.5, 0});
        }
    }

    // ---- curves ----------------------------------------------------------------------------

    // The points a curve draws through: Bezier segments sampled `steps` times, a closed curve back
    // to its start.
    static std::vector<V3> curve_samples(const OutCurve& c, int steps = 12) {
        std::vector<V3> out;
        size_t n = c.points.size();
        if (n < 2) return out;
        bool bezier = c.bezier && c.left.size() == n && c.right.size() == n;
        size_t segments = c.closed ? n : n - 1;
        out.push_back(c.points[0]);
        for (size_t s = 0; s < segments; ++s) {
            const V3& p0 = c.points[s % n];
            const V3& p3 = c.points[(s + 1) % n];
            int k_steps = bezier ? steps : 1;
            for (int k = 1; k <= k_steps; ++k) {
                double t = (double)k / k_steps, u = 1 - t;
                out.push_back(bezier ? p0 * (u * u * u) + c.right[s % n] * (3 * u * u * t) + c.left[(s + 1) % n] * (3 * u * t * t) +
                                           p3 * (t * t * t)
                                     : p3);
            }
        }
        return out;
    }
    static std::vector<V3> spline_samples(const UserSpline& s) {
        std::vector<V3> out = s.points;
        if (s.closed && out.size() > 2) out.push_back(out.front());
        return out;
    }
    static double polyline_length(const std::vector<V3>& pts) {
        double l = 0;
        for (size_t i = 1; i < pts.size(); ++i) l += length(pts[i] - pts[i - 1]);
        return l;
    }
    const OutCurve* ripped_curve(int index) const {
        const Scene& sc = preview.scene;
        if (index < 0) return nullptr;
        if (index < (int)sc.curves.size()) return &sc.curves[index];
        index -= (int)sc.curves.size();
        return index < (int)sc.auto_curves.size() ? &sc.auto_curves[index] : nullptr;
    }
    bool curve_is_auto(int index) const { return index >= (int)preview.scene.curves.size(); }
    bool curve_shown(int index) const { return show_cat[kSplines] && (curve_is_auto(index) ? show_autosplines : show_splines); }
    const RippedSpline* ripped_entry(const OutCurve& c) const {
        if (!edits || c.key.empty()) return nullptr;
        auto it = edits->ripped_splines.find(c.key);
        return it != edits->ripped_splines.end() ? &it->second : nullptr;
    }
    GrindSettings ripped_grind(const OutCurve& c) const {
        const RippedSpline* r = ripped_entry(c);
        return r ? r->grind : GrindSettings{};
    }
    V3 ripped_offset(const OutCurve& c) const {
        const RippedSpline* r = ripped_entry(c);
        return r ? r->offset : V3{};
    }
    // A map spline's points where it is now (moved by its offset).
    std::vector<V3> ripped_samples(int index, int steps = 12) const {
        const OutCurve* c = ripped_curve(index);
        if (!c) return {};
        std::vector<V3> pts = curve_samples(*c, steps);
        V3 d = ripped_offset(*c);
        if (d.x != 0 || d.y != 0 || d.z != 0)
            for (V3& p : pts) p = p + d;
        return pts;
    }
    static bool default_grind(const GrindSettings& g) { return g.enabled && g.radius == 0.03 && g.surface == 3; }

    // The collision ReSkate Studio builds under a grind curve (grind_prism in its add-on): an
    // 8-sided prism of the rail radius hanging from the line with one corner on it; runs break
    // at corners sharper than 75 degrees. Unity space (Y up).
    static void rail_lines(std::vector<LineVertex>& v, const std::vector<V3>& pts, double radius, uint32_t colour) {
        std::vector<V3> p;
        for (const V3& q : pts)
            if (p.empty() || length(q - p.back()) >= 0.02) p.push_back(q);
        if (p.size() < 2) return;
        const int sides = 8;
        auto run = [&](size_t a, size_t b) {  // points a..b
            size_t n = b - a + 1;
            if (n < 2) return;
            std::vector<V3> dirs(n - 1), rings(n * sides);
            for (size_t s = 0; s + 1 < n; ++s) dirs[s] = normalize(p[a + s + 1] - p[a + s]);
            V3 previous_up;
            bool have_up = false;
            for (size_t k = 0; k < n; ++k) {
                V3 before = dirs[k == 0 ? 0 : k - 1], after = dirs[std::min(k, n - 2)];
                V3 t = before + after;
                t = length(t) > 1e-6 ? normalize(t) : after;
                V3 up = V3{0, 1, 0} - t * t.y;
                if (length(up) < 1e-3) {  // a vertical run keeps the last frame's up
                    V3 fallback = have_up ? previous_up : V3{0, 0, 1};
                    up = fallback - t * dot(fallback, t);
                }
                up = normalize(up);
                previous_up = up, have_up = true;
                V3 side = cross(t, up);
                V3 bend = after - before;
                bend = bend - t * dot(bend, t);
                double stretch = 0;
                if (length(bend) > 1e-6) {  // a mitred joint keeps the radius on both segments
                    bend = normalize(bend);
                    stretch = 1.0 / std::max(0.5, std::min(1.0, dot(before, t))) - 1.0;
                }
                V3 first;
                for (int s = 0; s < sides; ++s) {
                    double ang = 2 * kPi * s / sides;
                    V3 o = (up * std::cos(ang) + side * std::sin(ang)) * radius;
                    if (stretch != 0) o = o + bend * (dot(o, bend) * stretch);
                    if (s == 0) first = o;
                    rings[k * sides + s] = o;
                }
                V3 centre = p[a + k] - first;  // the top corner lies on the line
                for (int s = 0; s < sides; ++s) rings[k * sides + s] = centre + rings[k * sides + s];
            }
            for (size_t k = 0; k < n; ++k)
                for (int s = 0; s < sides; ++s) {
                    line(v, rings[k * sides + s], rings[k * sides + (s + 1) % sides], colour);
                    if (k + 1 < n) line(v, rings[k * sides + s], rings[(k + 1) * sides + s], colour);
                }
        };
        const double split = std::cos(75.0 * kPi / 180);
        size_t start = 0;
        for (size_t i = 1; i + 1 < p.size(); ++i)
            if (dot(normalize(p[i] - p[i - 1]), normalize(p[i + 1] - p[i])) < split) {
                run(start, i);
                start = i;
            }
        run(start, p.size() - 1);
    }
    static constexpr uint32_t kRailColour = 0x8c50afffu;  // pack(255, 175, 80, 140)

    // The map's splines as lines: its own, then the automatic ones; those switched off in grey.
    // With the rail preview on, each grind curve's collision too.
    void build_curve_lines() {
        curve_lines_version = edits_version;
        curve_lines_dirty = false;
        release(lines);
        const Scene& sc = preview.scene;
        std::vector<LineVertex> lv;
        auto add_curves = [&](int first, size_t count, uint32_t rgba) {
            for (size_t k = 0; k < count; ++k) {
                const OutCurve& c = *ripped_curve(first + (int)k);
                GrindSettings g = ripped_grind(c);
                uint32_t colour = g.enabled ? rgba : pack(100, 103, 115);
                std::vector<V3> pts = ripped_samples(first + (int)k);
                for (size_t i = 1; i < pts.size(); ++i) line(lv, pts[i - 1], pts[i], colour);
                if (show_rails && g.enabled) rail_lines(lv, c.bezier ? ripped_samples(first + (int)k, 4) : pts, g.radius, kRailColour);
            }
        };
        add_curves(0, sc.curves.size(), pack(255, 90, 200));
        spline_lines = (uint32_t)lv.size();
        add_curves((int)sc.curves.size(), sc.auto_curves.size(), pack(70, 220, 255));
        auto_lines = (uint32_t)lv.size() - spline_lines;
        if (!lv.empty()) lines = static_buffer(lv.data(), lv.size() * sizeof(LineVertex), D3D11_BIND_VERTEX_BUFFER);
    }

    // ---- placed things as lines ------------------------------------------------------------

    static uint32_t spline_colour(const UserSpline& s) {
        if (!s.npc) return s.grind.enabled ? pack(255, 150, 60) : pack(130, 112, 98);
        switch (s.route.kind) {
        case NpcKind::Vehicle: return pack(90, 160, 255);
        case NpcKind::Bus: return pack(250, 200, 70);
        default: return pack(110, 220, 120);
        }
    }
    static ImVec4 unpack(uint32_t c) {
        return ImVec4((c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f, 1);
    }
    static V3 light_dir(double yaw_deg, double pitch_deg) {
        double y = yaw_deg * kPi / 180, p = pitch_deg * kPi / 180;
        return {std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
    }
    static void line(std::vector<LineVertex>& v, const V3& a, const V3& b, uint32_t c) {
        v.push_back({{(float)a.x, (float)a.y, (float)a.z}, c});
        v.push_back({{(float)b.x, (float)b.y, (float)b.z}, c});
    }
    // A light: three rings, and for a spot its cone along `dir`.
    static void light_lines(std::vector<LineVertex>& v, const V3& p, const V3* dir, double cone_deg, uint32_t c) {
        const double r = 0.3;
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 16; ++k) {
                auto ring = [&](int i) {
                    double a = i * 2 * kPi / 16, x = std::cos(a) * r, y = std::sin(a) * r;
                    return axis == 0 ? p + V3{0, x, y} : axis == 1 ? p + V3{x, 0, y} : p + V3{x, y, 0};
                };
                line(v, ring(k), ring(k + 1), c);
            }
        if (!dir) return;
        V3 d = normalize(*dir);
        double len = 2.0, rad = std::tan(std::clamp(cone_deg, 1.0, 170.0) * 0.5 * kPi / 180) * len;
        rad = std::min(rad, 6.0);
        V3 side = normalize(std::fabs(d.y) > 0.9 ? cross(d, V3{1, 0, 0}) : cross(d, V3{0, 1, 0}));
        V3 up2 = cross(d, side);
        V3 base = p + d * len;
        auto rim = [&](int i) {
            double a = i * 2 * kPi / 24;
            return base + side * (std::cos(a) * rad) + up2 * (std::sin(a) * rad);
        };
        for (int k = 0; k < 24; ++k) line(v, rim(k), rim(k + 1), c);
        for (int k = 0; k < 24; k += 6) line(v, p, rim(k), c);
    }
    // An audio volume: its box, and an arrow on its floor the way it faces.
    static void box_lines(std::vector<LineVertex>& v, const AudioVolume& a, uint32_t c) {
        double yr = a.yaw * kPi / 180;
        V3 fwd{std::sin(yr), 0, std::cos(yr)}, rt{std::cos(yr), 0, -std::sin(yr)}, up{0, 1, 0};
        auto corner = [&](int i) {
            return a.center + rt * ((i & 1 ? 1 : -1) * a.half.x) + up * ((i & 2 ? 1 : -1) * a.half.y) +
                   fwd * ((i & 4 ? 1 : -1) * a.half.z);
        };
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4})
                if (!(i & bit)) line(v, corner(i), corner(i | bit), c);
        V3 floor = a.center - up * a.half.y + up * 0.02;
        double k = std::min(a.half.z, 2.0);
        line(v, floor, floor + fwd * k, c);
        line(v, floor + fwd * k, floor + fwd * (k * 0.7) + rt * (k * 0.2), c);
        line(v, floor + fwd * k, floor + fwd * (k * 0.7) - rt * (k * 0.2), c);
    }
    bool light_removed(const OutNode& n) const {
        if (!edits || n.key.empty()) return false;
        auto it = edits->lights.find(n.key);
        return it != edits->lights.end() && it->second.removed;
    }

    // An area light: its rectangle and an arrow the way it shines.
    static void area_lines(std::vector<LineVertex>& v, const UserLight& l, uint32_t c) {
        V3 d = light_dir(l.yaw, l.pitch);
        V3 side = normalize(std::fabs(d.y) > 0.95 ? cross(d, V3{1, 0, 0}) : cross(V3{0, 1, 0}, d));
        V3 up2 = cross(d, side);
        V3 a = side * (l.width * 0.5), b = up2 * (l.height * 0.5), p = l.position;
        V3 corners[4] = {p - a - b, p + a - b, p + a + b, p - a + b};
        for (int i = 0; i < 4; ++i) line(v, corners[i], corners[(i + 1) % 4], c);
        line(v, corners[0], corners[2], c);
        line(v, corners[1], corners[3], c);
        double k = std::min(1.0, std::max(l.width, l.height) * 0.5);
        line(v, p, p + d * k, c);
        line(v, p + d * k, p + d * (k * 0.75) + side * (k * 0.12), c);
        line(v, p + d * k, p + d * (k * 0.75) - side * (k * 0.12), c);
    }

    void draw_markers() {
        if (!edits || !marker_vb) return;
        std::vector<LineVertex> v;
        const uint32_t white = pack(255, 255, 255);
        if (edits->spawn) marker_lines(v, *edits->spawn, item_picked(Item{kSpawnItem}) ? white : pack(80, 230, 120), false);
        for (size_t k = 0; k < edits->bus_stops.size(); ++k)
            marker_lines(v, edits->bus_stops[k], item_picked(Item{kBusItem, (int)k}) ? white : pack(250, 200, 70), true);
        for (size_t k = 0; k < edits->user_lights.size(); ++k) {
            const UserLight& l = edits->user_lights[k];
            V3 d = light_dir(l.yaw, l.pitch);
            uint32_t c = item_picked(Item{kLightItem, (int)k}) ? white : pack(255, 220, 110);
            if (l.area) area_lines(v, l, c);
            else light_lines(v, l.position, l.spot ? &d : nullptr, l.cone, c);
        }
        if (show_cat[kLights]) {  // the map's own lights
            const Scene& sc = preview.scene;
            for (size_t i = 0; i < sc.nodes.size(); ++i) {
                int li = sc.nodes[i].light;
                if (li < 0 || li >= (int)sc.lights.size() || (i < edit_state.size() && edit_state[i] == 1) || light_replaced(sc.nodes[i]))
                    continue;
                const OutLight& L = sc.lights[li];
                V3 d = normalize(xform_dir(nodes[i].world, V3{0, 0, -1}));
                uint32_t c = node_picked((int)i) ? white : light_removed(sc.nodes[i]) ? pack(105, 105, 115) : pack(230, 190, 90);
                light_lines(v, node_position((int)i), L.type != 0 ? &d : nullptr, L.outer * 2 * 180 / kPi, c);
            }
        }
        for (size_t k = 0; k < edits->audio_volumes.size(); ++k)
            box_lines(v, edits->audio_volumes[k], item_picked(Item{kAudioItem, (int)k}) ? white : pack(80, 205, 220));
        for (size_t k = 0; k < edits->splines.size(); ++k) {
            const UserSpline& s = edits->splines[k];
            bool sel = item_picked(Item{kSplineItem, (int)k});
            uint32_t c = sel ? white : spline_colour(s);
            std::vector<V3> pts = spline_samples(s);
            for (size_t i = 1; i < pts.size(); ++i) line(v, pts[i - 1], pts[i], c);
            if (!s.npc && s.grind.enabled && (sel || show_rails)) rail_lines(v, pts, s.grind.radius, kRailColour);
            if (s.npc && !s.route.bidirectional)  // one way: arrows along it
                for (size_t i = 1; i < pts.size(); ++i) {
                    V3 d = pts[i] - pts[i - 1];
                    double l = length(d);
                    if (l < 0.5) continue;
                    d = d * (1.0 / l);
                    V3 m = (pts[i] + pts[i - 1]) * 0.5, side = normalize(cross(V3{0, 1, 0}, d));
                    double k2 = std::min(0.6, l * 0.25);
                    line(v, m, m - d * k2 + side * (k2 * 0.6), c);
                    line(v, m, m - d * k2 - side * (k2 * 0.6), c);
                }
        }
        // The selected map splines in white, with their rails while there are not too many
        size_t rail_budget = 20000;
        for (auto& [k, i] : picked) {
            if (k != kCurveItem) continue;
            const OutCurve* c = ripped_curve(i);
            if (!c) continue;
            std::vector<V3> pts = ripped_samples(i);
            for (size_t p = 1; p < pts.size(); ++p) line(v, pts[p - 1], pts[p], white);
            GrindSettings g = ripped_grind(*c);
            if (g.enabled && !show_rails && pts.size() <= rail_budget) {
                rail_lines(v, pts, g.radius, kRailColour);
                rail_budget -= pts.size();
            }
        }
        if (v.empty()) return;
        if (v.size() > marker_capacity) {  // grow the buffer
            release(marker_vb);
            marker_capacity = v.size() + v.size() / 2;
            D3D11_BUFFER_DESC mb = {};
            mb.ByteWidth = (UINT)(sizeof(LineVertex) * marker_capacity);
            mb.Usage = D3D11_USAGE_DYNAMIC;
            mb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            mb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&mb, nullptr, &marker_vb))) {
                marker_vb = nullptr;
                marker_capacity = 0;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(marker_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, v.data(), v.size() * sizeof(LineVertex));
        ctx->Unmap(marker_vb, 0);
        const float zero[4] = {};
        ctx->IASetInputLayout(line_layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        UINT stride = sizeof(LineVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &marker_vb, &stride, &offset);
        ctx->VSSetShader(vs_line, nullptr, 0);
        ctx->PSSetShader(ps_line, nullptr, 0);
        ctx->RSSetState(rs_solid);
        ctx->OMSetBlendState(bs_alpha, zero, 0xffffffff);
        set_frame(0.35f);
        ctx->OMSetDepthStencilState(ds_off, 0);
        ctx->Draw((UINT)v.size(), 0);
        set_frame(1);
        ctx->OMSetDepthStencilState(ds_read, 0);
        ctx->Draw((UINT)v.size(), 0);
    }

    // ---- lighting preview ------------------------------------------------------------------

    // The map's lights and the ones placed here that shine at this time of day (bit of
    // kLightTimeLabels), into the light buffer: 4 float4 each.
    size_t gather_lights(int tod_bit) {
        std::vector<float> data;
        auto push = [&](const V3& p, double range, const float* colour, double cd, int type, const V3& dir, double cos_out,
                        double cos_in, double w, double h) {
            if ((int)(data.size() / 16) >= kMaxLights) return;
            const float v[16] = {(float)p.x, (float)p.y, (float)p.z, (float)range,
                                 (float)(colour[0] * cd), (float)(colour[1] * cd), (float)(colour[2] * cd), (float)type,
                                 (float)dir.x, (float)dir.y, (float)dir.z, (float)cos_out,
                                 (float)cos_in, (float)w, (float)h, 0};
            data.insert(data.end(), v, v + 16);
        };
        if (edits)
            for (const UserLight& l : edits->user_lights) {
                if (!(l.times & tod_bit)) continue;
                double outer = std::clamp(l.cone, 1.0, 179.0) * 0.5 * kPi / 180;
                push(l.position, l.range, l.color, l.intensity, l.area ? 2 : l.spot ? 1 : 0, light_dir(l.yaw, l.pitch), std::cos(outer),
                     std::cos(outer * 0.8), l.width, l.height);
            }
        const Scene& sc = preview.scene;
        for (int i : light_nodes) {
            const OutNode& n = sc.nodes[i];
            const OutLight& L = sc.lights[n.light];
            if (L.type == 2) continue;  // a sun: in game the time of day lights instead
            if (i < (int)edit_state.size() && edit_state[i] == 1) continue;
            if (light_replaced(n)) continue;  // made editable: lit as that
            int times = kAllLightTimes;
            double range = L.range > 0 ? L.range : 40;  // Studio's default attenuation radius
            if (edits) {
                auto it = edits->lights.find(n.key);
                if (it != edits->lights.end()) {
                    if (it->second.removed) continue;
                    if (it->second.times) times = *it->second.times;
                    if (it->second.range) range = *it->second.range;
                }
            }
            if (!(times & tod_bit)) continue;
            push(node_position(i), range, L.color, L.intensity, L.area ? 2 : L.type == 1 ? 1 : 0,
                 normalize(xform_dir(nodes[i].world, V3{0, 0, -1})), std::cos(L.outer), std::cos(L.inner), L.area_size[0], L.area_size[1]);
        }
        if (!data.empty() && light_buf) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(light_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, data.data(), data.size() * sizeof(float));
                ctx->Unmap(light_buf, 0);
            }
        }
        build_clusters(data);
        return data.size() / 16;
    }

    // A dynamic typed buffer the shaders read, grown when it is too small.
    void upload_typed(ID3D11Buffer*& buf, ID3D11ShaderResourceView*& srv, size_t& capacity, DXGI_FORMAT format, size_t stride,
                      const void* data, size_t count) {
        if (count > capacity || !buf) {
            release(srv);
            release(buf);
            capacity = std::max<size_t>(count + count / 2, 1024);
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = (UINT)(capacity * stride);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &buf))) {
                capacity = 0;
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = (UINT)capacity;
            device->CreateShaderResourceView(buf, &sd, &srv);
        }
        if (!count) return;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, data, count * stride);
            ctx->Unmap(buf, 0);
        }
    }

    // Sorts the lights into clusters: 32-pixel screen tiles by exponential depth slices. A light
    // goes into the clusters its range sphere can touch, so a pixel only adds up the few lights
    // near it, and lights off screen or behind the camera cost nothing.
    void build_clusters(const std::vector<float>& data) {
        const int tile = 32, slices = 24;
        const double znear = 0.5, zfar = 4000;
        int tx = std::max(1, (tw + tile - 1) / tile), ty = std::max(1, (th + tile - 1) / tile);
        size_t cells = (size_t)tx * ty * slices;
        double scale = slices / std::log(zfar / znear);
        V3 rv = right(), uv = up(), fv = forward();
        double aspect = th > 0 ? (double)tw / th : 1, tan_y = std::tan(fov * 0.5), tan_x = tan_y * aspect;
        double grow_x = std::sqrt(1 + tan_x * tan_x), grow_y = std::sqrt(1 + tan_y * tan_y);
        size_t n = data.size() / 16;
        struct Span {
            int x0, x1, y0, y1, s0, s1;
        };
        std::vector<Span> spans;
        spans.reserve(n);
        std::vector<uint32_t> counts(cells, 0);
        auto slice_of = [&](double z) { return std::clamp((int)std::floor(std::log(std::max(z, znear) / znear) * scale), 0, slices - 1); };
        for (size_t k = 0; k < n; ++k) {
            const float* l = &data[k * 16];
            V3 d = V3{l[0], l[1], l[2]} - cam;
            double r = l[3], x = dot(d, rv), y = dot(d, uv), z = dot(d, fv);
            Span sp{0, -1, 0, -1, 0, -1};
            bool shown = z + r > znear && x - r * grow_x <= z * tan_x && -x - r * grow_x <= z * tan_x && y - r * grow_y <= z * tan_y &&
                         -y - r * grow_y <= z * tan_y;
            if (shown) {
                double zmin = std::max(z - r, znear), zmax = z + r;
                double nx0 = -1, nx1 = 1, ny0 = -1, ny1 = 1;
                if (z - r > znear) {  // the sphere's box, seen from its near and far depths
                    nx0 = std::min((x - r) / (zmin * tan_x), (x - r) / (zmax * tan_x));
                    nx1 = std::max((x + r) / (zmin * tan_x), (x + r) / (zmax * tan_x));
                    ny0 = std::min((y - r) / (zmin * tan_y), (y - r) / (zmax * tan_y));
                    ny1 = std::max((y + r) / (zmin * tan_y), (y + r) / (zmax * tan_y));
                }
                nx0 = std::max(nx0, -1.0), nx1 = std::min(nx1, 1.0), ny0 = std::max(ny0, -1.0), ny1 = std::min(ny1, 1.0);
                if (nx0 <= nx1 && ny0 <= ny1) {
                    sp.x0 = std::clamp((int)((nx0 * 0.5 + 0.5) * tw) / tile, 0, tx - 1);
                    sp.x1 = std::clamp((int)((nx1 * 0.5 + 0.5) * tw) / tile, 0, tx - 1);
                    sp.y0 = std::clamp((int)((0.5 - ny1 * 0.5) * th) / tile, 0, ty - 1);
                    sp.y1 = std::clamp((int)((0.5 - ny0 * 0.5) * th) / tile, 0, ty - 1);
                    sp.s0 = slice_of(zmin), sp.s1 = slice_of(zmax);
                }
            }
            spans.push_back(sp);
            for (int s = sp.s0; s <= sp.s1; ++s)
                for (int yy = sp.y0; yy <= sp.y1; ++yy)
                    for (int xx = sp.x0; xx <= sp.x1; ++xx) ++counts[((size_t)s * ty + yy) * tx + xx];
        }
        std::vector<uint32_t> grid(cells * 2);
        uint32_t total = 0;
        for (size_t c = 0; c < cells; ++c) grid[c * 2] = total, grid[c * 2 + 1] = 0, total += counts[c];
        std::vector<uint32_t> index(std::max<uint32_t>(total, 1));
        for (size_t k = 0; k < n; ++k) {
            const Span& sp = spans[k];
            for (int s = sp.s0; s <= sp.s1; ++s)
                for (int yy = sp.y0; yy <= sp.y1; ++yy)
                    for (int xx = sp.x0; xx <= sp.x1; ++xx) {
                        size_t c = ((size_t)s * ty + yy) * tx + xx;
                        index[grid[c * 2] + grid[c * 2 + 1]++] = (uint32_t)k;
                    }
        }
        upload_typed(grid_buf, grid_srv, grid_capacity, DXGI_FORMAT_R32G32_UINT, 8, grid.data(), cells);
        upload_typed(index_buf, index_srv, index_capacity, DXGI_FORMAT_R32_UINT, 4, index.data(), index.size());
        lighting.tiles_x = (uint32_t)tx, lighting.tiles_y = (uint32_t)ty, lighting.slices = (uint32_t)slices;
        lighting.slice_scale = (float)scale, lighting.cluster_near = (float)znear;
        cluster_entries = total;
    }

    // The sun's shadow map covers a square around where the camera looks, snapped to its texels
    // so it does not shimmer as the camera moves.
    void shadow_matrix(const V3& travel, float out[16], float& texel) const {
        double r = std::clamp(speed * 15.0, 40.0, 400.0);
        V3 c = cam + forward() * (r * 0.5);
        V3 d = normalize(travel);
        V3 rt = normalize(std::fabs(d.y) > 0.99 ? cross(d, V3{1, 0, 0}) : cross(V3{0, 1, 0}, d));
        V3 up = cross(d, rt);
        double t = 2 * r / kShadowSize;
        double cx = std::floor(dot(rt, c) / t) * t, cy = std::floor(dot(up, c) / t) * t, cz = dot(d, c);
        const double depth = 2000;  // metres either side along the sun
        const double m[16] = {rt.x / r, rt.y / r, rt.z / r, -cx / r,
                              up.x / r, up.y / r, up.z / r, -cy / r,
                              d.x / (2 * depth), d.y / (2 * depth), d.z / (2 * depth), (depth - cz) / (2 * depth),
                              0, 0, 0, 1};
        for (int i = 0; i < 16; ++i) out[i] = (float)m[i];
        texel = (float)t;
    }

    // This frame's lighting constants: editor shading, or the game's time of day with its sun,
    // sky, exposure, fog and the lights that shine then.
    void update_lighting() {
        lighting = FrameCB{};
        uint32_t flags = show_textures ? 1u : 0u;
        V3 travel = normalize(V3{-0.35, -1.0, 0.45});
        lights_shown = 0;
        if (light_mode == 1) flags |= 2;
        if (light_mode >= 2) {
            const TimeOfDay& t = kTimes[std::clamp(light_mode - 2, 0, 4)];
            double az = t.azimuth * kPi / 180, el = t.elevation * kPi / 180;
            // Toward the sun in the game is (sin az cos el, sin el, cos az cos el); its X is Unity's -X.
            V3 toward{-std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
            travel = toward * -1.0;
            double sin_el = std::max(std::sin(el), 0.0);
            double sky_lux = 0.185 * t.lux;  // ReSkate Studio's clear-sky estimate
            bool night = light_mode == 6;
            float low = night ? 1.0f : (float)std::clamp(1.0 - t.elevation / 25.0, 0.0, 1.0) * 0.4f;  // a low sun warms the sky
            const float blue[3] = {0.62f, 0.78f, 1.0f};
            for (int k = 0; k < 3; ++k) {
                float tint = blue[k] + (t.sun[k] - blue[k]) * low;
                lighting.sun_color[k] = (float)(t.sun[k] * t.lux);
                lighting.sky_color[k] = (float)(tint * sky_lux);
                lighting.ground_color[k] = (float)(0.2 * (t.sun[k] * t.lux * sin_el + lighting.sky_color[k]));
                lighting.fog_color[k] = (float)(lighting.sky_color[k] / kPi * 1.6 + t.sun[k] * t.lux / kPi * 0.004 * low);
            }
            // Auto exposure from the scene's average brightness (middle grey a little brighter), kept
            // above the time of day's lowest EV like the game. Its highest EVs (10-11.4) would leave a
            // physically lit day several stops too bright, so they only cap a little.
            double average = 0.18 * (t.lux * std::max(sin_el, 0.1) + sky_lux) / kPi;
            double ev = std::clamp(std::log2(std::max(average, 1e-4) * 8.0) - 1.0, t.ev_min, t.ev_max + 4.0) + ev_comp;
            lighting.exposure = (float)(1.0 / (1.2 * std::pow(2.0, ev)));
            lighting.fog_density = show_fog ? (float)(3.0 / t.fog_visibility) : 0.0f;
            flags |= 4;
            if (show_shadows) flags |= 8;
            if (show_fog) flags |= 16;
            if (use_map_sky && sky_srv) flags |= 32;
            if (night) flags |= 64;
            if (show_lights) lights_shown = gather_lights(1 << (light_mode - 2));
            if (show_shadows) shadow_matrix(travel, lighting.shadow_proj, lighting.shadow_texel);
        }
        lighting.light_count = (uint32_t)lights_shown;
        lighting.sun[0] = (float)travel.x, lighting.sun[1] = (float)travel.y, lighting.sun[2] = (float)travel.z;
        lighting.flags = flags;
    }

    void render_shadows() {
        if (!shadow_dsv) return;
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(3, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, shadow_dsv);
        ctx->ClearDepthStencilView(shadow_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp = {0, 0, (float)kShadowSize, (float)kShadowSize, 0, 1};
        ctx->RSSetViewports(1, &vp);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(frame_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        FrameCB f = lighting;
        std::memcpy(f.view_proj, lighting.shadow_proj, sizeof f.view_proj);
        std::memcpy(mapped.pData, &f, sizeof f);
        ctx->Unmap(frame_cb, 0);
        const float zero[4] = {};
        bind_mesh_pipeline(ps_shadow);
        ctx->PSSetShaderResources(3, 1, &none);
        ctx->RSSetState(rs_shadow);
        ctx->OMSetDepthStencilState(ds_shadow, 0);
        ctx->OMSetBlendState(nullptr, zero, 0xffffffff);
        draw_list(opaque, false, instances, ranges);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void draw_sky() {
        if (!vs_sky || !ps_sky) return;
        const float zero[4] = {};
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_sky, nullptr, 0);
        ctx->PSSetShader(ps_sky, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &frame_cb);
        ctx->PSSetConstantBuffers(0, 1, &frame_cb);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetShaderResources(4, 1, &sky_srv);
        ctx->RSSetState(rs_solid);
        ctx->OMSetDepthStencilState(ds_off, 0);
        ctx->OMSetBlendState(nullptr, zero, 0xffffffff);
        ctx->Draw(3, 0);
    }

    // The toolbar's lighting menu.
    void lighting_menu() {
        static const char* const names[] = {"Flat", "Editor", "Morning", "Noon", "Afternoon", "Evening", "Night"};
        std::string label = std::string("Lighting: ") + names[std::clamp(light_mode, 0, 6)];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(20));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(5)));
        ImGui::PushStyleColor(ImGuiCol_Button, light_mode >= 2 ? rgb(96, 142, 255, 0.22f) : col::field);
        ImGui::PushStyleColor(ImGuiCol_Text, light_mode >= 2 ? col::accent_hover : col::muted);
        if (ImGui::Button(label.c_str())) ImGui::OpenPopup("lighting");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::SetItemTooltip("Preview the map lit like the game at a time of day, with its lights");
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(280), 0), ImVec2(S(360), FLT_MAX));
        if (!ImGui::BeginPopup("lighting")) return;
        ImGui::PushFont(fonts.semibold);
        ImGui::TextUnformatted("Lighting");
        ImGui::PopFont();
        for (int m = 0; m < 7; ++m) {
            if (m == 2) {
                ImGui::Separator();
                ImGui::TextColored(col::muted, "Skate's times of day");
            }
            if (ImGui::RadioButton(names[m], light_mode == m)) light_mode = m;
            if (m == 0) ImGui::SetItemTooltip("Colours only");
            else if (m == 1) ImGui::SetItemTooltip("Soft shading for editing");
            else {
                const TimeOfDay& t = kTimes[m - 2];
                ImGui::SetItemTooltip("Sun %.0f lux, %.0f\xC2\xB0 up; exposure EV %.1f to %.1f (the game's environment assets)", t.lux,
                                      t.elevation, t.ev_min, t.ev_max);
            }
        }
        if (light_mode >= 2) {
            ImGui::Separator();
            switch_row("Light sources", show_lights);
            ImGui::SetItemTooltip("Your lights and the map's, each only at the times of day it is set to shine");
            switch_row("Sun shadows", show_shadows);
            switch_row("Fog", show_fog);
            if (sky_srv) {
                switch_row("The map's skybox", use_map_sky);
                ImGui::SetItemTooltip("The sky the map had in Skater XL (ReSkate Studio cannot put it in the game yet)");
            }
            ImGui::TextUnformatted("Exposure");
            ImGui::SameLine(S(90));
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##ev", &ev_comp, -4, 4, "%+.1f EV");
            ImGui::TextColored(col::muted, "%s lights shining now", with_commas(lights_shown).c_str());
            ImGui::SetItemTooltip("Each pixel only adds the lights whose range reaches it (%s light-cell pairs this frame)",
                                  with_commas(cluster_entries).c_str());
        }
        ImGui::EndPopup();
    }

    void profile_begin() {
        if (profile_file.empty()) return;
        if (!prof_disjoint) {
            D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&qd, &prof_disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto*& q : prof_stamps) device->CreateQuery(&qd, &q);
        }
        ctx->Begin(prof_disjoint);
        prof_count = 0;
        stamp();
    }
    void profile_end(double cpu_ms) {
        if (!prof_disjoint) return;
        stamp();
        ctx->End(prof_disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        while (ctx->GetData(prof_disjoint, &dj, sizeof dj, 0) != S_OK) {}
        if (dj.Disjoint) return;
        UINT64 t[8];
        for (int k = 0; k < prof_count; ++k)
            while (ctx->GetData(prof_stamps[k], &t[k], sizeof t[k], 0) != S_OK) {}
        for (int k = 1; k < prof_count; ++k) prof_sum[k] += (double)(t[k] - t[k - 1]) * 1000.0 / dj.Frequency;
        prof_cpu += cpu_ms;
        if (++prof_frames == 120) {
            FILE* f = _wfopen(profile_file.c_str(), L"wb");
            if (f) {
                const char* names[] = {"", "shadow map", "sky + opaque", "decals + blended", "ghosts + selection", "lines + markers"};
                double total = 0;
                for (int k = 1; k < prof_count; ++k) {
                    std::fprintf(f, "%-20s %8.3f ms\n", names[k], prof_sum[k] / prof_frames);
                    total += prof_sum[k] / prof_frames;
                }
                std::fprintf(f, "%-20s %8.3f ms\n%-20s %8.3f ms\nview %dx%d, lights %zu, triangles %zu, draws %zu, mode %d\n", "gpu total", total,
                             "cpu render()", prof_cpu / prof_frames, tw, th, lights_shown, drawn_triangles, draw_calls, light_mode);
                std::fclose(f);
            }
            profile_file.clear();
        }
    }

    void render() {
        auto cpu_start = std::chrono::steady_clock::now();
        profile_begin();
        if (filter_dirty || built_version != edits_version || built_sel_version != sel_version) rebuild_instances();
        if (curve_lines_dirty || curve_lines_version != edits_version) build_curve_lines();
        drawn_triangles = draw_calls = 0;
        update_lighting();
        bool game = light_mode >= 2;
        if (game && show_shadows) render_shadows();
        stamp();
        set_frame(1);
        const float sky[4] = {0.012f, 0.015f, 0.022f, 1};
        ctx->ClearRenderTargetView(color_rtv, sky);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        ctx->OMSetRenderTargets(1, &color_rtv, dsv);
        D3D11_VIEWPORT vp = {0, 0, (float)tw, (float)th, 0, 1};
        ctx->RSSetViewports(1, &vp);
        const float zero[4] = {};
        if (game) draw_sky();

        bind_mesh_pipeline(ps_main);
        ctx->RSSetState(wireframe ? rs_wire : rs_solid);
        ctx->OMSetDepthStencilState(ds_write, 0);
        ctx->OMSetBlendState(nullptr, zero, 0xffffffff);
        draw_list(opaque, true, instances, ranges);
        stamp();
        ctx->OMSetBlendState(bs_alpha, zero, 0xffffffff);
        ctx->OMSetDepthStencilState(ds_read, 0);
        ctx->RSSetState(wireframe ? rs_wire : rs_decal);
        draw_list(decals, true, instances, ranges);
        ctx->RSSetState(wireframe ? rs_wire : rs_solid);
        draw_list(blended, true, instances, ranges);
        stamp();

        // Collision only (an object or a material set so): a faint fill and an outline
        const ImVec4 ghost = rgb(255, 160, 70);
        bind_mesh_pipeline(ps_flat);
        ctx->OMSetBlendState(bs_alpha, zero, 0xffffffff);
        for (int pass = 0; pass < 2; ++pass) {
            set_frame(pass ? 0.55f : 0.10f, ghost);
            ctx->RSSetState(pass ? rs_overlay : rs_decal);
            for (const auto* list : {&opaque, &decals, &blended}) {
                draw_list(*list, false, ghost_instances, ghost_ranges, 0);
                draw_list(*list, false, instances, ranges, 2);
            }
        }

        // The selected object's wireframe on top
        bool normal = selected >= 0 && slot[selected] >= 0, ghosted = selected >= 0 && ghost_slot[selected] >= 0;
        if (normal || ghosted) {
            int m = preview.scene.nodes[selected].mesh;
            set_frame(0.9f);
            bind_mesh_pipeline(ps_flat);
            ctx->RSSetState(rs_overlay);
            ctx->OMSetBlendState(bs_alpha, zero, 0xffffffff);
            std::vector<std::pair<uint32_t, uint32_t>> one_range(meshes.size(), {0, 0});
            one_range[m] = {(uint32_t)(normal ? slot[selected] : ghost_slot[selected]), 1};
            std::vector<Draw> one;
            for (int p = 0; p < (int)meshes[m].parts.size(); ++p) one.push_back({m, p});
            draw_list(one, false, normal ? instances : ghost_instances, one_range, 0);
        }

        stamp();
        // Grind splines: faint where hidden behind geometry, solid where in view
        bool maps = show_cat[kSplines] && show_splines, autos = show_cat[kSplines] && show_autosplines;
        uint32_t first = maps ? 0 : spline_lines;
        uint32_t count = (maps ? spline_lines : 0) + (autos ? auto_lines : 0);
        if (!autos) count = maps ? spline_lines : 0;
        if (lines && count) {
            ctx->IASetInputLayout(line_layout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            UINT stride = sizeof(LineVertex), offset = 0;
            ctx->IASetVertexBuffers(0, 1, &lines, &stride, &offset);
            ctx->VSSetShader(vs_line, nullptr, 0);
            ctx->PSSetShader(ps_line, nullptr, 0);
            ctx->RSSetState(rs_solid);
            ctx->OMSetBlendState(bs_alpha, zero, 0xffffffff);
            set_frame(0.25f);
            ctx->OMSetDepthStencilState(ds_off, 0);
            ctx->Draw(count, first);
            set_frame(1);
            ctx->OMSetDepthStencilState(ds_read, 0);
            ctx->Draw(count, first);
        }
        draw_markers();
        ID3D11RenderTargetView* none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);
        profile_end(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu_start).count());
    }

    struct Hit {
        int node = -1;     // the object, or -1
        bool surface = false;
        V3 point;          // where the ray met it (Unity world space)
    };

    // What is under a pixel of the view (objects set to collision only included).
    Hit pick(int x, int y) {
        Hit hit;
        if (!has_scene || x < 0 || y < 0 || x >= tw || y >= th) return hit;
        if (filter_dirty || built_version != edits_version || built_sel_version != sel_version) rebuild_instances();
        set_frame(1);
        const UINT clear[4] = {0, 0, 0, 0};
        const float clear_pos[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(ids_rtv, (const float*)clear);
        ctx->ClearRenderTargetView(pos_rtv, clear_pos);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        ID3D11RenderTargetView* targets[2] = {ids_rtv, pos_rtv};
        ctx->OMSetRenderTargets(2, targets, dsv);
        D3D11_VIEWPORT vp = {0, 0, (float)tw, (float)th, 0, 1};
        ctx->RSSetViewports(1, &vp);
        const float zero[4] = {};
        bind_mesh_pipeline(ps_id);
        ctx->OMSetBlendState(nullptr, zero, 0xffffffff);
        ctx->OMSetDepthStencilState(ds_write, 0);
        for (int pass = 0; pass < 2; ++pass) {
            ID3D11Buffer* inst = pass ? ghost_instances : instances;
            const auto& rng = pass ? ghost_ranges : ranges;
            ctx->RSSetState(rs_solid);
            draw_list(opaque, false, inst, rng, 0);
            draw_list(blended, false, inst, rng, 0);
            ctx->RSSetState(rs_decal);
            draw_list(decals, false, inst, rng, 0);
        }
        ID3D11RenderTargetView* none[2] = {};
        ctx->OMSetRenderTargets(2, none, nullptr);
        D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
        ctx->CopySubresourceRegion(id_staging, 0, 0, 0, 0, ids, 0, &box);
        ctx->CopySubresourceRegion(pos_staging, 0, 0, 0, 0, posmap, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(id_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            uint32_t v = *(const uint32_t*)mapped.pData;
            ctx->Unmap(id_staging, 0);
            hit.node = v ? (int)v - 1 : -1;
        }
        if (hit.node >= 0 && SUCCEEDED(ctx->Map(pos_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            const float* f = (const float*)mapped.pData;
            hit.point = {f[0], f[1], f[2]};
            hit.surface = f[3] > 0.5f;
            ctx->Unmap(pos_staging, 0);
        }
        return hit;
    }

    // Screen position of a world point in the view, or false when behind the camera.
    bool project(const V3& p, ImVec2 origin, ImVec2& out) const {
        float m[16];
        view_proj(m);
        double cx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
        double cy = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
        double cw = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
        if (cw <= 1e-6) return false;
        out = ImVec2(origin.x + (float)((cx / cw * 0.5 + 0.5) * tw), origin.y + (float)((1 - (cy / cw * 0.5 + 0.5)) * th));
        return true;
    }

    // Distance in pixels from `m` to the screen segment a-b.
    static float segment_distance(ImVec2 m, ImVec2 a, ImVec2 b) {
        float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        float t = l2 > 0 ? std::clamp(((m.x - a.x) * dx + (m.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
        return std::hypot(m.x - (a.x + dx * t), m.y - (a.y + dy * t));
    }
    // Nearest screen distance from `mouse` to a line through `pts` (huge when off screen).
    float polyline_distance(const std::vector<V3>& pts, ImVec2 origin, ImVec2 mouse) const {
        float best = 1e9f;
        ImVec2 prev;
        bool have = false;
        for (const V3& p : pts) {
            ImVec2 s;
            bool ok = project(p, origin, s);
            if (ok && have) best = std::min(best, segment_distance(mouse, prev, s));
            prev = s, have = ok;
        }
        return best;
    }

    // What placed or drawn thing is under a screen point; one of the map's lights comes back in
    // `node` instead.
    Item item_at(ImVec2 mouse, ImVec2 origin, int& node) const {
        node = -1;
        if (!edits) return {};
        auto near_point = [&](const V3& p, float radius) {
            ImVec2 s;
            return project(p, origin, s) && std::hypot(s.x - mouse.x, s.y - mouse.y) < radius;
        };
        auto near_marker = [&](const Marker& m, double height) {
            for (double h : {0.0, height * 0.5, height})
                if (near_point(m.position + V3{0, h, 0}, S(14))) return true;
            return false;
        };
        if (edits->spawn && near_marker(*edits->spawn, 1.8)) return {kSpawnItem};
        for (size_t k = 0; k < edits->bus_stops.size(); ++k)
            if (near_marker(edits->bus_stops[k], 2.6)) return {kBusItem, (int)k};
        for (size_t k = 0; k < edits->user_lights.size(); ++k)
            if (near_point(edits->user_lights[k].position, S(14))) return {kLightItem, (int)k};
        for (size_t k = 0; k < edits->audio_volumes.size(); ++k) {
            const AudioVolume& a = edits->audio_volumes[k];
            if (near_point(a.center, S(14))) return {kAudioItem, (int)k};
            double yr = a.yaw * kPi / 180;
            V3 fwd{std::sin(yr), 0, std::cos(yr)}, rt{std::cos(yr), 0, -std::sin(yr)};
            for (int i = 0; i < 8; ++i)
                if (near_point(a.center + rt * ((i & 1 ? 1 : -1) * a.half.x) + V3{0, (i & 2 ? 1 : -1) * a.half.y, 0} +
                                   fwd * ((i & 4 ? 1 : -1) * a.half.z),
                               S(10)))
                    return {kAudioItem, (int)k};
        }
        for (size_t k = 0; k < edits->splines.size(); ++k)
            if (polyline_distance(spline_samples(edits->splines[k]), origin, mouse) < S(7)) return {kSplineItem, (int)k};
        const Scene& sc = preview.scene;
        if (show_cat[kLights])
            for (size_t i = 0; i < sc.nodes.size(); ++i)
                if (sc.nodes[i].light >= 0 && !light_replaced(sc.nodes[i]) && near_point(node_position((int)i), S(14))) {
                    node = (int)i;
                    return {};
                }
        int best = -1;
        float best_d = S(6);
        int total = (int)(sc.curves.size() + sc.auto_curves.size());
        for (int c = 0; c < total; ++c) {
            if (!curve_shown(c)) continue;
            float d = polyline_distance(ripped_samples(c, 6), origin, mouse);
            if (d < best_d) best_d = d, best = c;
        }
        if (best >= 0) return {kCurveItem, best};
        return {};
    }

    Marker* marker_of(const Item& it) {
        if (!edits) return nullptr;
        if (it.kind == kSpawnItem) return edits->spawn ? &*edits->spawn : nullptr;
        if (it.kind == kBusItem && it.index >= 0 && it.index < (int)edits->bus_stops.size()) return &edits->bus_stops[it.index];
        return nullptr;
    }
    bool item_valid(const Item& it) const {
        if (!edits) return false;
        auto in = [&](size_t n) { return it.index >= 0 && it.index < (int)n; };
        switch (it.kind) {
        case kSpawnItem: return edits->spawn.has_value();
        case kBusItem: return in(edits->bus_stops.size());
        case kLightItem: return in(edits->user_lights.size());
        case kAudioItem: return in(edits->audio_volumes.size());
        case kSplineItem: return in(edits->splines.size());
        case kCurveItem: return ripped_curve(it.index) != nullptr;
        }
        return false;
    }
    // What the gizmo moves: a marker, light or volume's position, or the selected spline point.
    V3* item_position(const Item& it) {
        if (!item_valid(it)) return nullptr;
        switch (it.kind) {
        case kSpawnItem: return &edits->spawn->position;
        case kBusItem: return &edits->bus_stops[it.index].position;
        case kLightItem: return &edits->user_lights[it.index].position;
        case kAudioItem: return &edits->audio_volumes[it.index].center;
        case kSplineItem: {
            auto& pts = edits->splines[it.index].points;
            return point >= 0 && point < (int)pts.size() ? &pts[point] : nullptr;
        }
        }
        return nullptr;
    }
    bool item_bounds(const Item& it, V3& lo, V3& hi) {
        if (!item_valid(it)) return false;
        std::vector<V3> pts;
        switch (it.kind) {
        case kSpawnItem:
        case kBusItem: {
            V3 p = marker_of(it)->position;
            lo = p - V3{2, 0, 2}, hi = p + V3{2, 2.5, 2};
            return true;
        }
        case kLightItem: {
            V3 p = edits->user_lights[it.index].position;
            lo = p - V3{3, 3, 3}, hi = p + V3{3, 3, 3};
            return true;
        }
        case kAudioItem: {
            const AudioVolume& a = edits->audio_volumes[it.index];
            double r = std::max(a.half.x, a.half.z);
            lo = a.center - V3{r, a.half.y, r}, hi = a.center + V3{r, a.half.y, r};
            return true;
        }
        case kSplineItem: {
            const UserSpline& s = edits->splines[it.index];
            if (point >= 0 && point < (int)s.points.size()) {
                lo = s.points[point] - V3{2, 2, 2}, hi = s.points[point] + V3{2, 2, 2};
                return true;
            }
            pts = s.points;
            break;
        }
        case kCurveItem: pts = ripped_samples(it.index, 4); break;
        }
        if (pts.empty()) return false;
        lo = {1e30, 1e30, 1e30}, hi = {-1e30, -1e30, -1e30};
        for (const V3& p : pts) lo = vmin(lo, p), hi = vmax(hi, p);
        lo = lo - V3{1, 1, 1}, hi = hi + V3{1, 1, 1};
        return true;
    }
    std::string item_name(const Item& it) const {
        if (!item_valid(it)) return "";
        switch (it.kind) {
        case kSpawnItem: return "Player spawn";
        case kBusItem: {
            const Marker& m = edits->bus_stops[it.index];
            return m.name.empty() ? "Bus stop " + std::to_string(it.index + 1) : m.name;
        }
        case kLightItem: {
            const UserLight& l = edits->user_lights[it.index];
            return l.name.empty() ? "Light " + std::to_string(it.index + 1) : l.name;
        }
        case kAudioItem: {
            const AudioVolume& a = edits->audio_volumes[it.index];
            return a.name.empty() ? "Audio volume " + std::to_string(it.index + 1) : a.name;
        }
        case kSplineItem: {
            const UserSpline& s = edits->splines[it.index];
            return s.name.empty() ? (s.npc ? "NPC route " : "Grind curve ") + std::to_string(it.index + 1) : s.name;
        }
        case kCurveItem: {
            const OutCurve* c = ripped_curve(it.index);
            return c->name.empty() ? "Spline " + std::to_string(it.index + 1) : c->name;
        }
        }
        return "";
    }
    std::string item_kind_text(const Item& it) const {
        if (!item_valid(it)) return "";
        switch (it.kind) {
        case kSpawnItem: return "Where the skater starts";
        case kBusItem: return "Bus stop (fast travel)";
        case kLightItem: return edits->user_lights[it.index].area ? "Area light" : edits->user_lights[it.index].spot ? "Spot light" : "Point light";
        case kAudioItem: return "Audio volume  \xC2\xB7  " + std::string(kAudioPresetLabels[std::clamp(edits->audio_volumes[it.index].preset, 0, kAudioPresets - 1)]);
        case kSplineItem: {
            const UserSpline& s = edits->splines[it.index];
            static const char* const kinds[] = {"pedestrians", "vehicles", "buses"};
            return s.npc ? std::string("NPC route  \xC2\xB7  ") + kinds[(int)s.route.kind] : "Grind curve";
        }
        case kCurveItem: return curve_is_auto(it.index) ? "Auto spline (found on the map's geometry)" : "Spline of the map";
        }
        return "";
    }
    ImVec4 item_colour(const Item& it) const {
        switch (it.kind) {
        case kSpawnItem: return rgb(80, 230, 120);
        case kBusItem: return rgb(250, 200, 70);
        case kLightItem: return rgb(255, 220, 110);
        case kAudioItem: return rgb(80, 205, 220);
        case kSplineItem: return item_valid(it) ? unpack(spline_colour(edits->splines[it.index])) : col::muted;
        case kCurveItem: return curve_is_auto(it.index) ? rgb(70, 220, 255) : rgb(255, 90, 200);
        }
        return col::muted;
    }

    // ---- UI ------------------------------------------------------------------------------

    // ---- selection -------------------------------------------------------------------------
    // `picked` holds everything selected; `selected` (a map object) or `item` is the active one,
    // the one the inspector shows. Map objects are (kNodeSel, node), the rest (Item kind, index).

    using Pick = std::pair<int, int>;
    bool node_picked(int i) const { return picked.count({kNodeSel, i}) > 0; }
    bool item_picked(const Item& it) const { return it.kind != kNoItem && picked.count({it.kind, it.index}) > 0; }

    // Drawing points ends; a curve left with fewer than two points is dropped.
    void finish_drawing() {
        if (placing != kAddPoints) return;
        placing = kNotPlacing;
        insert_after = -1;
        if (item.kind == kSplineItem && item_valid(item) && edits->splines[item.index].points.size() < 2) {
            edits->splines.erase(edits->splines.begin() + item.index);
            item = {};
            point = -1;
            picked.clear();
            ++sel_version;
            touch();
        }
    }

    void set_active(Pick p) {
        editing_material = -1;
        if (p.first == kNodeSel) selected = p.second, item = {};
        else if (p.first > kNoItem) item = {p.first, p.second}, selected = -1;
        else selected = -1, item = {};
    }

    void select_item(Item it) {
        if (placing == kAddPoints && !(it == item)) {
            int drawing = item.kind == kSplineItem ? item.index : -1;
            size_t before = edits ? edits->splines.size() : 0;
            finish_drawing();
            if (edits && edits->splines.size() < before && it.kind == kSplineItem && it.index > drawing) --it.index;
        }
        if (placing == kPlaceMove && !(it == item)) placing = kNotPlacing;
        if (!(it == item)) point = -1;
        item = it;
        if (it.kind != kNoItem) selected = -1;
        editing_material = -1;
        picked.clear();
        if (it.kind != kNoItem) picked.insert({it.kind, it.index});
        pivot_set = picked;
        ++sel_version;
    }

    void select(int node, bool from_view) {
        if (item.kind != kNoItem) select_item({});
        if (node != selected) editing_material = -1;
        selected = node;
        picked.clear();
        if (node >= 0) picked.insert({kNodeSel, node});
        pivot_set = picked;
        ++sel_version;
        if (from_view && node >= 0) {
            for (int p = nodes[node].parent; p >= 0; p = nodes[p].parent) expanded.insert(p);
            scroll_to_selected = true;
        }
    }
    void select_one(Pick p, bool from_view) {
        if (p.first == kNodeSel) select(p.second, from_view);
        else select_item(p.first > kNoItem ? Item{p.first, p.second} : Item{});
    }

    // Ctrl+click: in or out of the selection.
    void toggle_pick(Pick p) {
        if (p.first < 0) return;
        if (placing == kAddPoints) finish_drawing();
        if (placing == kPlaceMove) placing = kNotPlacing;
        if (picked.count(p)) {
            picked.erase(p);
            pivot_set.erase(p);
            bool was_active = (p.first == kNodeSel && p.second == selected) || (p.first != kNodeSel && item == Item{p.first, p.second});
            if (was_active) set_active(picked.empty() ? Pick{-1, -1} : *picked.rbegin());
        } else {
            picked.insert(p);
            pivot_set.insert(p);
            set_active(p);
        }
        point = -1;
        ++sel_version;
    }

    // Replaces the selection; `active` (else the first) is the one the inspector shows.
    void pick_many(const std::vector<Pick>& list, Pick active = {-1, -1}, bool keep_pivot = false) {
        if (placing == kAddPoints) finish_drawing();
        if (placing == kPlaceMove) placing = kNotPlacing;
        Pick was_active = selected >= 0 ? Pick{kNodeSel, selected} : item.kind != kNoItem ? Pick{item.kind, item.index} : Pick{-1, -1};
        picked.clear();
        for (auto& p : list)
            if (p.first >= 0) picked.insert(p);
        if (keep_pivot) {  // the gizmo stays on what was picked by hand, while it is still selected
            for (auto it = pivot_set.begin(); it != pivot_set.end();)
                it = picked.count(*it) ? std::next(it) : pivot_set.erase(it);
            if (pivot_set.empty() && picked.count(active)) pivot_set.insert(active);
            if (pivot_set.empty() && picked.count(was_active)) pivot_set.insert(was_active);
            if (picked.count(was_active) && !picked.count(active)) active = was_active;
        } else {
            pivot_set = picked;
        }
        if (active.first < 0 || !picked.count(active)) active = picked.empty() ? Pick{-1, -1} : *picked.begin();
        set_active(active);
        point = -1;
        ++sel_version;
    }
    // Adds to (or narrows) the selection without moving the gizmo: select all of a kind, and so on.
    void expand_selection(const std::vector<Pick>& list, Pick active = {-1, -1}) { pick_many(list, active, true); }

    // Everything of a kind, for "select all".
    std::vector<Pick> category_things(int c) const {
        std::vector<Pick> out;
        const Scene& sc = preview.scene;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (category[i] == c && (sc.nodes[i].mesh >= 0 || sc.nodes[i].light >= 0) && !(nodes[i].flags & kIsTrees))
                out.push_back({kNodeSel, (int)i});
        return out;
    }
    std::vector<Pick> plant_things(int p) const {
        std::vector<Pick> out;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (node_plant[i] == p && preview.scene.nodes[i].mesh >= 0) out.push_back({kNodeSel, (int)i});
        return out;
    }
    std::vector<Pick> curve_things(int set) const {  // 0 the map's splines, 1 auto ones, 2 both
        std::vector<Pick> out;
        int map_n = (int)preview.scene.curves.size(), auto_n = (int)preview.scene.auto_curves.size();
        if (set != 1)
            for (int k = 0; k < map_n; ++k) out.push_back({kCurveItem, k});
        if (set != 0)
            for (int k = 0; k < auto_n; ++k) out.push_back({kCurveItem, map_n + k});
        return out;
    }
    std::vector<Pick> item_things(int kind) const {
        std::vector<Pick> out;
        if (!edits) return out;
        size_t n = kind == kBusItem ? edits->bus_stops.size() : kind == kLightItem ? edits->user_lights.size()
                 : kind == kAudioItem ? edits->audio_volumes.size() : kind == kSplineItem ? edits->splines.size() : 0;
        if (kind == kSpawnItem && edits->spawn) out.push_back({kSpawnItem, 0});
        for (size_t k = 0; k < n; ++k) out.push_back({kind, (int)k});
        return out;
    }
    std::vector<Pick> spline_things(bool npc) const {
        std::vector<Pick> out;
        if (edits)
            for (size_t k = 0; k < edits->splines.size(); ++k)
                if (edits->splines[k].npc == npc) out.push_back({kSplineItem, (int)k});
        return out;
    }
    // The light a map object stands for: itself, or the light Unity put on it (a child node here).
    int light_of(int node) const {
        const Scene& sc = preview.scene;
        if (node < 0 || node >= (int)sc.nodes.size()) return -1;
        if (sc.nodes[node].light >= 0) return node;
        for (int c : sc.nodes[node].children)
            if (c >= 0 && c < (int)sc.nodes.size() && sc.nodes[c].light >= 0) return c;
        return -1;
    }
    std::vector<int> picked_nodes(bool lights) const {  // the selected map objects: their lights, or the rest
        std::vector<int> out;
        std::set<int> seen;
        for (auto& [k, i] : picked) {
            if (k != kNodeSel || i < 0 || i >= (int)nodes.size()) continue;
            bool is_light = preview.scene.nodes[i].light >= 0;
            if (!lights && !is_light) out.push_back(i);
            if (lights) {
                int l = light_of(i);
                if (l >= 0 && seen.insert(l).second) out.push_back(l);
            }
        }
        return out;
    }
    // The map's lights of a kind (-1 all, 0 point, 1 spot, 2 sun, 3 area) and the added ones
    // (-1 all, 0 point, 1 spot, 2 area).
    static int light_kind(const OutLight& l) { return l.area ? 3 : l.type; }
    std::vector<Pick> map_lights_of(int kind) const {
        std::vector<Pick> out;
        for (int i : light_nodes)
            if (!light_replaced(preview.scene.nodes[i]) && (kind < 0 || light_kind(preview.scene.lights[preview.scene.nodes[i].light]) == kind))
                out.push_back({kNodeSel, i});
        return out;
    }
    std::vector<Pick> user_lights_of(int kind) const {
        std::vector<Pick> out;
        if (edits)
            for (size_t k = 0; k < edits->user_lights.size(); ++k) {
                const UserLight& l = edits->user_lights[k];
                if (kind < 0 || (l.area ? 2 : l.spot ? 1 : 0) == kind) out.push_back({kLightItem, (int)k});
            }
        return out;
    }

    // Opens the list section an item is in and scrolls to it.
    void reveal_item(const Item& it) {
        if (it.kind == kSplineItem) curves_open = true;
        else if (it.kind == kCurveItem) cat_open[kSplines] = true, set_open[curve_is_auto(it.index) ? 1 : 0] = true;
        else markers_open = true;
        scroll_to_selected = true;
    }

    // Deletes every selected thing that was placed or drawn here (the map's own stay).
    void delete_selected() {
        if (!edits) return;
        std::map<int, std::vector<int>> by_kind;
        for (auto& [k, i] : picked)
            if (k != kNodeSel && k != kCurveItem && item_valid({k, i})) by_kind[k].push_back(i);
        if (by_kind.empty()) return;
        auto drop = [](auto& list, std::vector<int> idx) {
            std::sort(idx.rbegin(), idx.rend());
            for (int i : idx)
                if (i < (int)list.size()) list.erase(list.begin() + i);
        };
        for (auto& [k, idx] : by_kind) {
            if (k == kSpawnItem) edits->spawn.reset();
            else if (k == kBusItem) drop(edits->bus_stops, idx);
            else if (k == kLightItem) {
                for (int i : idx)
                    if (i < (int)edits->user_lights.size() && !edits->user_lights[i].source.empty())
                        edits->dropped_lights.insert(edits->user_lights[i].source);
                drop(edits->user_lights, idx);
            }
            else if (k == kAudioItem) drop(edits->audio_volumes, idx);
            else if (k == kSplineItem) drop(edits->splines, idx);
        }
        placing = kNotPlacing;
        pick_many({});
        touch();
    }
    // Copies of the selected placed or drawn things, a metre over.
    void duplicate_selected() {
        if (!edits) return;
        std::vector<Pick> list(picked.begin(), picked.end()), made;
        const V3 shift{1, 0, 1};
        for (auto& [k, i] : list) {
            if (!item_valid({k, i})) continue;
            if (k == kBusItem) {
                Marker m = edits->bus_stops[i];
                m.position = m.position + shift, m.name += " copy";
                edits->bus_stops.push_back(m);
                made.push_back({k, (int)edits->bus_stops.size() - 1});
            } else if (k == kLightItem) {
                UserLight l = edits->user_lights[i];
                l.position = l.position + shift, l.name += " copy", l.source.clear();
                edits->user_lights.push_back(l);
                made.push_back({k, (int)edits->user_lights.size() - 1});
            } else if (k == kAudioItem) {
                AudioVolume a = edits->audio_volumes[i];
                a.center = a.center + shift, a.name += " copy";
                edits->audio_volumes.push_back(a);
                made.push_back({k, (int)edits->audio_volumes.size() - 1});
            } else if (k == kSplineItem) {
                UserSpline s = edits->splines[i];
                for (V3& p : s.points) p = p + shift;
                s.name += " copy";
                edits->splines.push_back(s);
                made.push_back({k, (int)edits->splines.size() - 1});
            }
        }
        if (made.empty()) return;
        pick_many(made);
        touch();
    }
    void reverse_selected() {
        for (auto& [k, i] : picked)
            if (k == kSplineItem && item_valid({k, i})) std::reverse(edits->splines[i].points.begin(), edits->splines[i].points.end());
        point = -1;
        touch();
    }

    void delete_point() {
        if (item.kind != kSplineItem || !item_valid(item)) return;
        auto& pts = edits->splines[item.index].points;
        if (point < 0 || point >= (int)pts.size()) return;
        pts.erase(pts.begin() + point);
        point = std::min(point, (int)pts.size() - 1);
        touch();
    }
    // Backspace while drawing: takes back the point just added.
    void remove_drawn_point() {
        if (item.kind != kSplineItem || !item_valid(item)) return;
        auto& pts = edits->splines[item.index].points;
        if (pts.empty()) return;
        int at = insert_after >= 0 ? std::min(insert_after, (int)pts.size() - 1) : (int)pts.size() - 1;
        pts.erase(pts.begin() + at);
        if (insert_after >= 0) insert_after = std::max(0, at - 1);
        point = std::min(at, (int)pts.size() - 1);
        touch();
    }

    // A new grind curve or NPC route, drawn by clicking in the view.
    void start_curve(bool npc) {
        if (!edits) return;
        int same = 0;
        for (auto& s : edits->splines) same += s.npc == npc;
        UserSpline s;
        s.npc = npc;
        s.name = (npc ? "NPC route " : "Grind curve ") + std::to_string(same + 1);
        edits->splines.push_back(s);
        select_item({kSplineItem, (int)edits->splines.size() - 1});
        reveal_item(item);
        placing = kAddPoints;
        insert_after = -1;
        touch();
    }

    // ---- the map's splines: settings and moves -------------------------------------------

    void cleanup_ripped() {
        if (!edits) return;
        for (auto it = edits->ripped_splines.begin(); it != edits->ripped_splines.end();)
            it = it->second.is_default() ? edits->ripped_splines.erase(it) : std::next(it);
    }
    void set_ripped_grind(const OutCurve& c, const GrindSettings& g) {
        if (!edits || c.key.empty()) return;
        edits->ripped_splines[c.key].grind = g;
        cleanup_ripped();
        bump();  // its colour in the view
    }
    // Runs `fn` on the grind settings of every selected grind curve and map spline.
    void edit_picked_grinds(const std::function<void(GrindSettings&)>& fn) {
        if (!edits) return;
        std::set<std::string> done;
        for (auto& [k, i] : picked) {
            if (!item_valid({k, i})) continue;
            if (k == kSplineItem && !edits->splines[i].npc) {
                fn(edits->splines[i].grind);
            } else if (k == kCurveItem) {
                const OutCurve* c = ripped_curve(i);
                if (!c->key.empty() && done.insert(c->key).second) fn(edits->ripped_splines[c->key].grind);
            }
        }
        cleanup_ripped();
        bump();
    }
    bool picked_curves_moved() const {
        for (auto& [k, i] : picked)
            if (k == kCurveItem) {
                V3 d = ripped_offset(*ripped_curve(i));
                if (d.x != 0 || d.y != 0 || d.z != 0) return true;
            }
        return false;
    }
    void unmove_picked_curves() {
        for (auto& [k, i] : picked)
            if (k == kCurveItem) {
                const OutCurve* c = ripped_curve(i);
                auto it = edits->ripped_splines.find(c->key);
                if (it != edits->ripped_splines.end()) it->second.offset = {};
            }
        cleanup_ripped();
        bump();
    }
    // The selected map splines as grind curves to edit point by point; the originals switch off.
    void copy_curves() {
        if (!edits) return;
        std::vector<int> list;
        for (auto& [k, i] : picked)
            if (k == kCurveItem && ripped_curve(i)) list.push_back(i);
        std::vector<Pick> made;
        for (int index : list) {
            const OutCurve* c = ripped_curve(index);
            UserSpline s;
            s.name = (c->name.empty() ? std::string("Spline") : c->name) + " (edited)";
            s.closed = c->closed;
            s.points = ripped_samples(index, 6);
            if (c->closed && s.points.size() > 2) s.points.pop_back();
            s.grind = ripped_grind(*c);
            s.grind.enabled = true;
            if (!c->key.empty()) edits->ripped_splines[c->key].grind.enabled = false;
            edits->splines.push_back(s);
            made.push_back({kSplineItem, (int)edits->splines.size() - 1});
        }
        cleanup_ripped();
        bump();
        pick_many(made);
        if (!made.empty()) reveal_item({kSplineItem, made.back().second});
    }

    // ---- moving --------------------------------------------------------------------------

    // Where a thing sits when several move together (a spline: the middle of its points).
    bool thing_centre(int kind, int index, V3& out) {
        Item it{kind, index};
        if (!item_valid(it)) return false;
        if (kind == kSplineItem || kind == kCurveItem) {
            const std::vector<V3>& pts = kind == kSplineItem ? edits->splines[index].points : ripped_curve(index)->points;
            if (pts.empty()) return false;
            V3 sum;
            for (const V3& p : pts) sum = sum + p;
            out = sum * (1.0 / pts.size());
            if (kind == kCurveItem) out = out + ripped_offset(*ripped_curve(index));
            return true;
        }
        Item one = it;
        if (V3* p = item_position(one)) {
            out = *p;
            return true;
        }
        return false;
    }
    // Where the gizmo sits: the selected spline point, or the middle of everything that moves.
    bool selection_pivot(V3& out) {
        if (picked.size() == 1 && item.kind == kSplineItem && point >= 0) {
            V3* p = item_position(item);
            if (p) out = *p;
            return p != nullptr;
        }
        for (const auto* from : {&pivot_set, &picked}) {
            V3 sum;
            int n = 0;
            for (auto& [k, i] : *from) {
                V3 c;
                if (k != kNodeSel && picked.count({k, i}) && thing_centre(k, i, c)) sum = sum + c, ++n;
            }
            if (n) {
                out = sum * (1.0 / n);
                return true;
            }
        }
        return false;
    }
    // The one thing the gizmo sits on when that is a single marker, light, box or point (it then
    // lands on surfaces when dragged by its centre), else none.
    Item pivot_thing() const {
        if (picked.size() == 1 && item.kind == kSplineItem && point >= 0) return item;
        Item one;
        int n = 0;
        for (auto& [k, i] : pivot_set)
            if (k != kNodeSel && k != kCurveItem && picked.count({k, i})) one = {k, i}, ++n;
        if (n != 1 || one.kind == kSplineItem) return {};
        return one;
    }
    // Every position a move changes (the map's splines move by their offset).
    std::vector<V3*> move_targets() {
        std::vector<V3*> out;
        if (!edits) return out;
        if (picked.size() == 1 && item.kind == kSplineItem && point >= 0) {
            if (V3* p = item_position(item)) out.push_back(p);
            return out;
        }
        std::set<std::string> keys;
        for (auto& [k, i] : picked) {
            Item it{k, i};
            if (k == kNodeSel || !item_valid(it)) continue;
            if (k == kSplineItem) {
                for (V3& p : edits->splines[i].points) out.push_back(&p);
            } else if (k == kCurveItem) {
                const OutCurve* c = ripped_curve(i);
                if (!c->key.empty() && keys.insert(c->key).second) out.push_back(&edits->ripped_splines[c->key].offset);
            } else if (V3* p = item_position(it)) {
                out.push_back(p);
            }
        }
        return out;
    }
    bool selection_bounds(V3& lo, V3& hi) {
        lo = {1e30, 1e30, 1e30}, hi = {-1e30, -1e30, -1e30};
        bool any = false;
        for (auto& [k, i] : picked) {
            V3 a, b;
            if (k == kNodeSel) {
                if (i < 0 || i >= (int)nodes.size()) continue;
                if (nodes[i].bounded) a = nodes[i].lo, b = nodes[i].hi;
                else a = node_position(i) - V3{2, 2, 2}, b = node_position(i) + V3{2, 2, 2};
            } else if (!item_bounds({k, i}, a, b)) {
                continue;
            }
            lo = vmin(lo, a), hi = vmax(hi, b), any = true;
        }
        return any;
    }

    // How high above a clicked surface the selection goes.
    double surface_offset() const { return surface_offset(item); }
    double surface_offset(const Item& it) const {
        if (!item_valid(it)) return 0;
        if (it.kind == kAudioItem) return edits->audio_volumes[it.index].half.y;
        if (it.kind == kLightItem) return edits->user_lights[it.index].spot ? 4.0 : 2.5;
        if (it.kind == kSplineItem) return draw_lift;
        return 0;
    }

    // A click on a surface while placing.
    void place_at(const V3& p) {
        double facing = yaw * 180.0 / kPi;  // the way the camera looks
        switch (placing) {
        case kPlaceSpawn:
            edits->spawn = Marker{p, facing};
            select_item({kSpawnItem});
            placing = kNotPlacing;
            break;
        case kPlaceBus: {
            Marker m{p, facing};
            m.name = "Bus Stop " + std::to_string(edits->bus_stops.size() + 1);
            edits->bus_stops.push_back(m);
            select_item({kBusItem, (int)edits->bus_stops.size() - 1});
            placing = kNotPlacing;
            break;
        }
        case kPlaceLight:
        case kPlaceSpot:
        case kPlaceArea: {
            UserLight l;
            l.spot = placing == kPlaceSpot;
            l.area = placing == kPlaceArea;
            l.position = p + V3{0, l.spot ? 4.0 : l.area ? 3.0 : 2.5, 0};
            l.yaw = facing;
            l.name = (l.spot ? "Spot light " : l.area ? "Area light " : "Light ") + std::to_string(edits->user_lights.size() + 1);
            edits->user_lights.push_back(l);
            select_item({kLightItem, (int)edits->user_lights.size() - 1});
            placing = kNotPlacing;
            break;
        }
        case kPlaceAudio: {
            AudioVolume a;
            a.center = p + V3{0, a.half.y, 0};
            a.yaw = facing;
            a.name = "Audio volume " + std::to_string(edits->audio_volumes.size() + 1);
            edits->audio_volumes.push_back(a);
            select_item({kAudioItem, (int)edits->audio_volumes.size() - 1});
            placing = kNotPlacing;
            break;
        }
        case kAddPoints: {
            if (item.kind != kSplineItem || !item_valid(item)) {
                placing = kNotPlacing;
                break;
            }
            auto& pts = edits->splines[item.index].points;
            V3 q = p + V3{0, draw_lift, 0};
            if (insert_after >= 0 && insert_after < (int)pts.size()) {
                pts.insert(pts.begin() + insert_after + 1, q);
                point = ++insert_after;
            } else {
                pts.push_back(q);
                point = (int)pts.size() - 1;
            }
            break;
        }
        case kPlaceMove:
            if (V3* at = item_position(item)) *at = p + V3{0, surface_offset(), 0};
            placing = kNotPlacing;
            break;
        }
        reveal_item(item);
        touch();
    }

    // ---- the move gizmo ----------------------------------------------------------------------

    static V3 axis_of(int a) { return a == 1 ? V3{1, 0, 0} : a == 2 ? V3{0, 1, 0} : V3{0, 0, 1}; }
    double gizmo_length(const V3& p) const { return std::max(0.05, length(p - cam) * 0.12); }
    // The handle under the mouse: 0 the centre, 1..3 the X, Y, Z arrows; -1 none.
    int gizmo_handle(const V3& p, ImVec2 origin, ImVec2 mouse) const {
        ImVec2 c;
        if (!project(p, origin, c)) return -1;
        if (std::hypot(mouse.x - c.x, mouse.y - c.y) < S(9)) return 0;
        double len = gizmo_length(p);
        int best = -1;
        float best_d = S(7);
        for (int a = 1; a <= 3; ++a) {
            ImVec2 e;
            if (!project(p + axis_of(a) * len, origin, e)) continue;
            float d = segment_distance(mouse, c, e);
            if (d < best_d) best_d = d, best = a;
        }
        return best;
    }
    void draw_gizmo(ImDrawList* dl, const V3& p, ImVec2 origin, int hot) const {
        ImVec2 c;
        if (!project(p, origin, c)) return;
        double len = gizmo_length(p);
        for (int a = 1; a <= 3; ++a) {
            ImVec2 e;
            if (!project(p + axis_of(a) * len, origin, e)) continue;
            ImVec4 colour = a == 1 ? rgb(240, 85, 85) : a == 2 ? rgb(120, 215, 95) : rgb(85, 145, 255);
            if (hot == a) colour = mix(colour, ImVec4(1, 1, 1, 1), 0.45f);
            ImU32 u = ImGui::GetColorU32(colour);
            dl->AddLine(c, e, u, hot == a ? S(3.5f) : S(2.5f));
            float dx = e.x - c.x, dy = e.y - c.y, l = std::hypot(dx, dy);
            if (l < 1) continue;
            dx /= l, dy /= l;
            float hs = S(9);
            dl->AddTriangleFilled(ImVec2(e.x + dx * hs * 0.6f, e.y + dy * hs * 0.6f), ImVec2(e.x - dy * hs * 0.45f, e.y + dx * hs * 0.45f),
                                  ImVec2(e.x + dy * hs * 0.45f, e.y - dx * hs * 0.45f), u);
        }
        dl->AddCircleFilled(c, S(6.5f), ImGui::GetColorU32(hot == 0 ? col::accent_hover : rgb(255, 255, 255, 0.92f)), 20);
        dl->AddCircle(c, S(6.5f), ImGui::GetColorU32(rgb(0, 0, 0, 0.6f)), 20, S(1.2f));
    }
    // The point of the selected spline drawn under the mouse, or -1.
    int spline_point_at(ImVec2 mouse, ImVec2 origin) const {
        if (item.kind != kSplineItem || !item_valid(item)) return -1;
        const auto& pts = edits->splines[item.index].points;
        int best = -1;
        float best_d = S(8);
        for (size_t k = 0; k < pts.size(); ++k) {
            ImVec2 s;
            if (!project(pts[k], origin, s)) continue;
            float d = std::hypot(s.x - mouse.x, s.y - mouse.y);
            if (d < best_d) best_d = d, best = (int)k;
        }
        return best;
    }

    ImVec4 category_colour(int i) const {
        unsigned f = nodes[i].flags;
        if (f & kIsCollider) return rgb(240, 150, 70);
        if (f & kIsDecal) return rgb(190, 120, 255);
        if (f & kIsLight) return rgb(250, 210, 90);
        if (f & kIsTerrain) return rgb(110, 200, 120);
        if (f & kIsSpline) return rgb(255, 90, 200);
        if (preview.scene.nodes[i].mesh >= 0) return col::accent;
        return col::muted;
    }

    // An eye toggle at `pos`; dim and crossed out when off.
    bool eye(const char* id, bool& on, ImVec2 pos, float h) {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(pos);
        bool pressed = ImGui::InvisibleButton(id, ImVec2(S(22), h));
        bool hovered = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(cursor);
        if (pressed) on = !on;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(pos.x + S(11), pos.y + h * 0.5f);
        ImU32 colour = ImGui::GetColorU32(on ? (hovered ? col::text : col::muted) : rgb(90, 96, 110));
        float w = S(7), e = S(4.5f);
        dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y - e * 1.6f), ImVec2(c.x + w, c.y), colour, S(1.3f));
        dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y + e * 1.6f), ImVec2(c.x + w, c.y), colour, S(1.3f));
        dl->AddCircleFilled(c, S(2.2f), colour, 12);
        if (!on) dl->AddLine(ImVec2(c.x - w * 0.8f, c.y + e), ImVec2(c.x + w * 0.8f, c.y - e), colour, S(1.4f));
        if (hovered) ImGui::SetTooltip(on ? "Hide in the view" : "Show in the view");
        return pressed;
    }

    void arrow(ImDrawList* dl, ImVec2 at, float h, bool open) {
        ImVec2 c(at.x + S(5), at.y + h * 0.5f);
        float a = S(3.5f);
        ImU32 ac = ImGui::GetColorU32(col::muted);
        if (open) dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.6f), ImVec2(c.x + a, c.y - a * 0.6f), ImVec2(c.x, c.y + a * 0.8f), ac);
        else dl->AddTriangleFilled(ImVec2(c.x - a * 0.6f, c.y - a), ImVec2(c.x - a * 0.6f, c.y + a), ImVec2(c.x + a * 0.8f, c.y), ac);
    }

    struct Row {
        enum Type { Section, Object, Plant, Curves, CurveRow, MarkerSection, CurveSection, ItemRow, Hint, LightSection } type;
        int id;        // section, node, plant, curve set (0 map, 1 auto), map curve, item index, hint (0 spawn, 1 curve)
        int depth;
        int kind = 0;  // ItemRow: the item kind
    };

    // The "+" menus of the Markers and Curves sections (also the inspector's Add buttons).
    void add_marker_menu() {
        if (ImGui::Selectable(edits && edits->spawn ? "Move the player spawn\xE2\x80\xA6" : "Player spawn\xE2\x80\xA6")) {
            if (edits && edits->spawn) select_item({kSpawnItem}), placing = kPlaceMove;
            else placing = kPlaceSpawn;
        }
        if (ImGui::Selectable("Bus stop\xE2\x80\xA6")) placing = kPlaceBus;
        if (ImGui::Selectable("Audio volume\xE2\x80\xA6")) placing = kPlaceAudio;
    }
    void add_light_menu() {
        if (ImGui::Selectable("Point light\xE2\x80\xA6")) placing = kPlaceLight;
        if (ImGui::Selectable("Spot light\xE2\x80\xA6")) placing = kPlaceSpot;
        if (ImGui::Selectable("Area light\xE2\x80\xA6")) placing = kPlaceArea;
    }
    void add_curve_menu() {
        if (ImGui::Selectable("Grind curve\xE2\x80\xA6")) start_curve(false);
        if (ImGui::Selectable("NPC route\xE2\x80\xA6")) start_curve(true);
    }

    void hierarchy(ImVec2 size, Job& job) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
        ImGui::BeginChild("objects", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        const Scene& sc = preview.scene;
        ImGui::PushFont(fonts.semibold);
        ImGui::TextUnformatted("Objects");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(col::muted, "%s", with_commas(sc.nodes.size()).c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search\xE2\x80\xA6", search, sizeof search);

        // The rows: matching things while searching, else the sections with their open parts.
        std::vector<Row> rows;
        std::string q = lower_copy(search);
        auto add_tree = [&](int root, int depth) {
            std::vector<std::pair<int, int>> stack = {{root, depth}};
            while (!stack.empty()) {
                auto [i, d] = stack.back();
                stack.pop_back();
                if (i < 0 || i >= (int)nodes.size() || (nodes[i].flags & kIsTrees)) continue;
                rows.push_back({Row::Object, i, d});
                if (expanded.count(i))
                    for (auto it = sc.nodes[i].children.rbegin(); it != sc.nodes[i].children.rend(); ++it)
                        stack.push_back({*it, d + 1});
            }
        };
        int map_curves = (int)sc.curves.size(), auto_curves = (int)sc.auto_curves.size();
        if (!q.empty()) {
            auto matches = [&](const std::string& name) { return lower_copy(name).find(q) != std::string::npos; };
            if (edits) {
                auto add_items = [&](int kind, size_t n) {
                    for (size_t k = 0; k < n; ++k)
                        if (matches(item_name({kind, (int)k}))) rows.push_back({Row::ItemRow, (int)k, 0, kind});
                };
                if (edits->spawn && matches("player spawn")) rows.push_back({Row::ItemRow, 0, 0, kSpawnItem});
                add_items(kBusItem, edits->bus_stops.size());
                add_items(kLightItem, edits->user_lights.size());
                add_items(kAudioItem, edits->audio_volumes.size());
                add_items(kSplineItem, edits->splines.size());
            }
            for (int c = 0; c < map_curves + auto_curves; ++c)
                if (matches(item_name({kCurveItem, c}))) rows.push_back({Row::CurveRow, c, 0});
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].lower.find(q) != std::string::npos) rows.push_back({Row::Object, (int)i, 0});
        } else {
            rows.push_back({Row::MarkerSection, 0, 0});
            if (markers_open && edits) {
                if (edits->spawn) rows.push_back({Row::ItemRow, 0, 1, kSpawnItem});
                else rows.push_back({Row::Hint, 0, 1});
                for (size_t k = 0; k < edits->bus_stops.size(); ++k) rows.push_back({Row::ItemRow, (int)k, 1, kBusItem});
                for (size_t k = 0; k < edits->audio_volumes.size(); ++k) rows.push_back({Row::ItemRow, (int)k, 1, kAudioItem});
            }
            rows.push_back({Row::LightSection, 0, 0});
            if (lights_open && edits)
                for (size_t k = 0; k < edits->user_lights.size(); ++k) rows.push_back({Row::ItemRow, (int)k, 1, kLightItem});
            rows.push_back({Row::CurveSection, 0, 0});
            if (curves_open && edits) {
                if (edits->splines.empty()) rows.push_back({Row::Hint, 1, 1});
                for (size_t k = 0; k < edits->splines.size(); ++k) rows.push_back({Row::ItemRow, (int)k, 1, kSplineItem});
            }
            for (int c = 0; c < kCategories; ++c) {
                size_t left_lights = 0;  // the map's lights not made editable (suns, ones left out)
                if (c == kLights)
                    for (int i : members[c]) left_lights += !light_replaced(sc.nodes[i]);
                bool empty = c == kPlants ? plants.empty()
                             : c == kSplines ? members[c].empty() && map_curves == 0 && auto_curves == 0
                             : c == kLights ? left_lights == 0
                                             : members[c].empty();
                if (empty) continue;
                rows.push_back({Row::Section, c, 0});
                if (!cat_open[c]) continue;
                if (c == kLevel) {
                    for (int r : members[c]) add_tree(r, 1);
                } else if (c == kPlants) {
                    for (size_t p = 0; p < plants.size(); ++p) {
                        if (plant_instances[p].empty()) continue;
                        rows.push_back({Row::Plant, (int)p, 1});
                        if (open_plants.count((int)p))
                            for (int inst : plant_instances[p]) add_tree(inst, 2);
                    }
                } else {
                    if (c == kSplines) {
                        if (map_curves) {
                            rows.push_back({Row::Curves, 0, 1});
                            if (set_open[0])
                                for (int k = 0; k < map_curves; ++k) rows.push_back({Row::CurveRow, k, 2});
                        }
                        if (auto_curves) {
                            rows.push_back({Row::Curves, 1, 1});
                            if (set_open[1])
                                for (int k = 0; k < auto_curves; ++k) rows.push_back({Row::CurveRow, map_curves + k, 2});
                        }
                    }
                    for (int i : members[c])
                        if (c != kLights || !light_replaced(sc.nodes[i])) add_tree(i, 1);
                }
            }
        }

        ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_None);
        float row_h = ImGui::GetFrameHeight();
        float step = row_h + ImGui::GetStyle().ItemSpacing.y;
        if (scroll_to_selected) {
            for (size_t r = 0; r < rows.size(); ++r) {
                const Row& row = rows[r];
                bool hit = item.kind == kNoItem ? row.type == Row::Object && row.id == selected
                                                : (row.type == Row::ItemRow && item == Item{row.kind, row.id}) ||
                                                      (row.type == Row::CurveRow && item == Item{kCurveItem, row.id});
                if (hit) {
                    float y = r * step;
                    if (y < ImGui::GetScrollY() || y > ImGui::GetScrollY() + ImGui::GetWindowHeight() - step)
                        ImGui::SetScrollY(std::max(0.0f, y - ImGui::GetWindowHeight() * 0.4f));
                    break;
                }
            }
            scroll_to_selected = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size(), step);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Row& row = rows[r];
                ImGui::PushID(r);
                ImGui::PushID(row.id);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                float indent = row.depth * S(14);
                float text_y = p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
                auto dot_name = [&](ImVec4 dot, const std::string& name, ImVec4 colour, const std::string& extra, bool strike = false) {
                    float x = p.x + indent + S(14);
                    dl->AddCircleFilled(ImVec2(x + S(3), p.y + row_h * 0.5f), S(3), ImGui::GetColorU32(dot), 12);
                    dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + w - S(4), p.y + row_h), true);
                    dl->AddText(ImVec2(x + S(12), text_y), ImGui::GetColorU32(colour), name.c_str());
                    float nw = ImGui::CalcTextSize(name.c_str()).x;
                    if (strike) dl->AddLine(ImVec2(x + S(12), p.y + row_h * 0.5f), ImVec2(x + S(12) + nw, p.y + row_h * 0.5f), ImGui::GetColorU32(col::muted), S(1));
                    if (!extra.empty()) dl->AddText(ImVec2(x + S(20) + nw, text_y), ImGui::GetColorU32(col::muted), extra.c_str());
                    dl->PopClipRect();
                };
                switch (row.type) {
                case Row::Section: {
                    int c = row.id;
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                    if (ImGui::Selectable("##section", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, row_h))) cat_open[c] = !cat_open[c];
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ctx_reset(), ctx_section = c, open_context = true;
                    ImGui::PopStyleColor();
                    arrow(dl, ImVec2(p.x, p.y), row_h, cat_open[c]);
                    ImGui::PushFont(fonts.semibold);
                    dl->AddText(ImVec2(p.x + S(16), text_y), ImGui::GetColorU32(col::text), kCategoryNames[c]);
                    float tw = ImGui::CalcTextSize(kCategoryNames[c]).x;
                    ImGui::PopFont();
                    size_t n = c == kPlants ? 0 : cat_count[c];
                    if (c == kLights) {
                        n = 0;
                        for (int i : members[c]) n += !light_replaced(sc.nodes[i]);
                    }
                    if (c == kPlants)
                        for (auto& list : plant_instances) n += list.size();
                    if (c == kSplines) n += sc.curves.size() + sc.auto_curves.size();
                    dl->AddText(ImVec2(p.x + S(24) + tw, text_y), ImGui::GetColorU32(col::muted), with_commas(n).c_str());
                    if (eye("##eye", show_cat[c], ImVec2(p.x + w - S(22), p.y), row_h)) {
                        filter_dirty = true;
                        if (selected >= 0 && !visible(selected) && !(nodes[selected].flags & kIsLight)) selected = -1;
                    }
                    break;
                }
                case Row::Plant: {
                    int pi = row.id;
                    const PlantInfo& plant = plants[pi];
                    bool open = open_plants.count(pi) > 0;
                    if (ImGui::Selectable("##plant", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, row_h))) {
                        if (open) open_plants.erase(pi);
                        else open_plants.insert(pi);
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ctx_reset(), ctx_plant = pi, open_context = true;
                    arrow(dl, ImVec2(p.x + indent, p.y), row_h, open);
                    float combo_w = S(70);
                    dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + w - combo_w - S(30), p.y + row_h), true);
                    dl->AddText(ImVec2(p.x + indent + S(16), text_y), ImGui::GetColorU32(col::text), plant.name.c_str());
                    float nw = ImGui::CalcTextSize(plant.name.c_str()).x;
                    dl->AddText(ImVec2(p.x + indent + S(24) + nw, text_y), ImGui::GetColorU32(col::muted),
                                with_commas(plant_instances[pi].size()).c_str());
                    dl->PopClipRect();
                    // Its LOD level, live
                    auto& own = job.opt.plant_lod;
                    auto it = own.find(plant.name);
                    int choice = it != own.end() ? it->second : -1;
                    int level = plant_level(plant, choice, job.opt.tree_lod, job.opt.lod);
                    int last = (int)plant.lod_triangles.size() - 1;
                    std::string preview_text = last < 1 ? "\xE2\x80\x94" : "LOD" + std::to_string(level);
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(p.x + w - combo_w - S(26), p.y));
                    ImGui::SetNextItemWidth(combo_w);
                    ImGui::BeginDisabled(last < 1 || job.opt.all_lods);
                    ImGui::PushStyleColor(ImGuiCol_Text, choice < 0 ? col::muted : col::text);  // dim: follows the defaults
                    bool combo = ImGui::BeginCombo("##lod", preview_text.c_str(), ImGuiComboFlags_HeightLarge);
                    ImGui::PopStyleColor();
                    if (!combo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(last < 1 ? "%s has no LOD levels" : choice < 0 ? "%s: automatic (Trees and grass LOD)" : "%s: its own LOD level",
                                          plant.name.c_str());
                    if (combo) {
                        if (ImGui::Selectable("Auto (Trees and grass LOD)", choice < 0)) own.erase(plant.name);
                        for (int l = 0; l <= last; ++l) {
                            std::string entry = "LOD" + std::to_string(l) + "  \xC2\xB7  " + with_commas(plant.lod_triangles[l]) + " tris";
                            if (ImGui::Selectable(entry.c_str(), choice >= 0 && level == l)) own[plant.name] = l == last ? 99 : l;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                    ImGui::SetCursorScreenPos(cursor);
                    bool shown = !hidden_plants.count(pi);
                    if (eye("##eye", shown, ImVec2(p.x + w - S(22), p.y), row_h)) {
                        if (shown) hidden_plants.erase(pi);
                        else hidden_plants.insert(pi);
                        filter_dirty = true;
                    }
                    break;
                }
                case Row::MarkerSection:
                case Row::LightSection:
                case Row::CurveSection: {
                    bool markers = row.type == Row::MarkerSection, light_rows = row.type == Row::LightSection;
                    bool& open = markers ? markers_open : light_rows ? lights_open : curves_open;
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                    if (ImGui::Selectable("##added", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, row_h))) open = !open;
                    ImGui::PopStyleColor();
                    arrow(dl, ImVec2(p.x, p.y), row_h, open);
                    const char* name = markers ? "Markers" : light_rows ? "Lights" : "Curves";
                    ImGui::PushFont(fonts.semibold);
                    dl->AddText(ImVec2(p.x + S(16), text_y), ImGui::GetColorU32(col::text), name);
                    float tw = ImGui::CalcTextSize(name).x;
                    ImGui::PopFont();
                    size_t n = !edits ? 0
                               : markers    ? (edits->spawn ? 1 : 0) + edits->bus_stops.size() + edits->audio_volumes.size()
                               : light_rows ? edits->user_lights.size()
                                            : edits->splines.size();
                    dl->AddText(ImVec2(p.x + S(24) + tw, text_y), ImGui::GetColorU32(col::muted), with_commas(n).c_str());
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(26), p.y));
                    if (ImGui::Button("+", ImVec2(S(24), row_h))) ImGui::OpenPopup("add");
                    ImGui::SetItemTooltip(markers      ? "Add a player spawn, bus stop or audio volume"
                                          : light_rows ? "Add a point, spot or area light (the map's own lights are here too, editable)"
                                                       : "Draw a grind curve or an NPC route");
                    if (ImGui::BeginPopup("add")) {
                        if (markers) add_marker_menu();
                        else if (light_rows) add_light_menu();
                        else add_curve_menu();
                        ImGui::EndPopup();
                    }
                    ImGui::SetCursorScreenPos(cursor);
                    break;
                }
                case Row::Hint: {
                    const char* text = row.id == 0 ? "Place the player spawn\xE2\x80\xA6" : "Draw a grind curve\xE2\x80\xA6";
                    if (ImGui::Selectable("##hint", false, 0, ImVec2(w, row_h))) {
                        if (row.id == 0) placing = kPlaceSpawn;
                        else start_curve(false);
                    }
                    dl->AddText(ImVec2(p.x + indent + S(26), text_y), ImGui::GetColorU32(col::accent), text);
                    break;
                }
                case Row::ItemRow: {
                    Item it{row.kind, row.id};
                    if (ImGui::Selectable("##item", item_picked(it), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, row_h)))
                        row_click({it.kind, it.index}, r, rows);
                    row_context({it.kind, it.index});
                    std::string extra;
                    if (row.kind == kSplineItem && item_valid(it)) extra = with_commas(edits->splines[row.id].points.size()) + " pts";
                    else if (row.kind == kLightItem && item_valid(it)) extra = edits->user_lights[row.id].area ? "area" : edits->user_lights[row.id].spot ? "spot" : "";
                    dot_name(item_colour(it), item_name(it), col::text, extra);
                    break;
                }
                case Row::Curves: {
                    bool& on = row.id == 0 ? show_splines : show_autosplines;
                    size_t n = row.id == 0 ? sc.curves.size() : sc.auto_curves.size();
                    if (ImGui::Selectable("##curves", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, row_h))) set_open[row.id] = !set_open[row.id];
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ctx_reset(), ctx_set = row.id, open_context = true;
                    arrow(dl, ImVec2(p.x + indent, p.y), row_h, set_open[row.id]);
                    size_t off = 0;
                    for (int k = 0; k < (int)n; ++k)
                        off += !ripped_grind(row.id == 0 ? sc.curves[k] : sc.auto_curves[k]).enabled;
                    std::string extra = with_commas(n) + (off ? "  (" + with_commas(off) + " off)" : "");
                    dot_name(row.id == 0 ? rgb(255, 90, 200) : rgb(70, 220, 255), row.id == 0 ? "Map splines" : "Auto splines", col::text, extra);
                    eye("##eye", on, ImVec2(p.x + w - S(22), p.y), row_h);
                    break;
                }
                case Row::CurveRow: {
                    Item it{kCurveItem, row.id};
                    if (ImGui::Selectable("##curve", item_picked(it), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, row_h)))
                        row_click({kCurveItem, row.id}, r, rows);
                    row_context({kCurveItem, row.id});
                    bool on = ripped_grind(*ripped_curve(row.id)).enabled;
                    dot_name(item_colour(it), item_name(it), on ? col::text : col::muted, on ? "" : "off", !on);
                    break;
                }
                case Row::Object: {
                    int i = row.id;
                    const OutNode& n = sc.nodes[i];
                    if (ImGui::Selectable("##row", node_picked(i), ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(w, row_h))) {
                        bool on_arrow = ImGui::GetMousePos().x < p.x + indent + S(14);
                        bool branch = !n.children.empty() && q.empty();
                        if (branch && on_arrow) {
                            if (expanded.count(i)) expanded.erase(i);
                            else expanded.insert(i);
                        } else {
                            row_click({kNodeSel, i}, r, rows);
                        }
                    }
                    row_context({kNodeSel, i});
                    if (!n.children.empty() && q.empty()) arrow(dl, ImVec2(p.x + indent, p.y), row_h, expanded.count(i) > 0);
                    uint8_t state = i < (int)edit_state.size() ? edit_state[i] : 0;
                    bool removed = state == 1 || (n.light >= 0 && light_removed(n));
                    bool dim = (nodes[i].flags & kIsInactive) || (!visible(i) && n.light < 0) || removed;
                    dot_name(category_colour(i), n.name.empty() ? "(unnamed)" : n.name, dim ? col::muted : col::text, "", removed);
                    if (state == 2) {
                        float lw = ImGui::CalcTextSize(n.name.empty() ? "(unnamed)" : n.name.c_str()).x;
                        dl->AddText(ImVec2(p.x + indent + S(34) + lw, text_y), ImGui::GetColorU32(rgb(255, 160, 70)), "collision only");
                    }
                    break;
                }
                }
                ImGui::PopID();
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    // ---- copy and paste ------------------------------------------------------------------------
    // The clipboard holds edits as text (so it also pastes into another map, or another window):
    // things placed or drawn here as themselves, or the settings of a map object, map light or map
    // spline under the key "*".

    static constexpr const char* kClipHeader = "Spotbuilder clipboard\n";
    static constexpr const char* kOldClipHeader = "BundleRipper clipboard\n";  // copied by the old name

    static bool clipboard_has_edits() {
        const char* text = ImGui::GetClipboardText();
        return text && (std::strncmp(text, kClipHeader, std::strlen(kClipHeader)) == 0 ||
                        std::strncmp(text, kOldClipHeader, std::strlen(kOldClipHeader)) == 0);
    }

    void copy_selection(bool cut) {
        if (!edits || picked.empty()) return;
        Edits clip;
        size_t things = 0;
        for (auto& [k, i] : picked) {
            if (k == kNodeSel || k == kCurveItem || !item_valid({k, i})) continue;
            if (k == kSpawnItem) clip.spawn = *edits->spawn;
            else if (k == kBusItem) clip.bus_stops.push_back(edits->bus_stops[i]);
            else if (k == kLightItem) clip.user_lights.push_back(edits->user_lights[i]);
            else if (k == kAudioItem) clip.audio_volumes.push_back(edits->audio_volumes[i]);
            else if (k == kSplineItem) clip.splines.push_back(edits->splines[i]);
            ++things;
        }
        std::string what;
        if (things) {
            what = with_commas(things) + (things == 1 ? " thing" : " things");
        } else if (item.kind == kCurveItem && ripped_curve(item.index)) {
            clip.ripped_splines["*"].grind = ripped_grind(*ripped_curve(item.index));
            what = "the spline's grind settings";
        } else if (selected >= 0 && selected < (int)nodes.size()) {
            const OutNode& n = preview.scene.nodes[selected];
            if (n.light >= 0) {
                clip.lights["*"] = edits->lights.count(n.key) ? edits->lights[n.key] : LightEdit{};
                what = "the light's settings";
            } else {
                clip.objects["*"] = edits->objects.count(n.key) ? edits->objects[n.key] : ObjectEdit{};
                what = "the object's settings";
            }
        }
        if (what.empty()) return;
        ImGui::SetClipboardText((std::string(kClipHeader) + edits_text(clip, true)).c_str());
        if (cut && things) {
            delete_selected();
            message = "Cut " + what;
        } else {
            message = "Copied " + what;
        }
    }

    // Pastes things as new copies (their lowest point on `point` when there is one, else a metre
    // over), or copied settings onto the selected map objects, lights or splines.
    void paste(bool at_point, const V3& point) {
        if (!edits) return;
        const char* text = ImGui::GetClipboardText();
        if (!clipboard_has_edits()) {
            message = "Nothing to paste: copy something in Spotbuilder first";
            return;
        }
        Edits clip;
        std::string why;
        if (!parse_edits(std::strchr(text, '\n') + 1, clip, why, true)) {
            message = "The clipboard could not be read: " + why;
            return;
        }
        bool things = clip.spawn || !clip.bus_stops.empty() || !clip.user_lights.empty() || !clip.audio_volumes.empty() ||
                      !clip.splines.empty();
        if (things) {
            V3 delta{1, 0, 1};
            if (at_point) {  // the group's lowest point lands where the mouse is
                V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
                auto add = [&](const V3& p) { lo = vmin(lo, p), hi = vmax(hi, p); };
                if (clip.spawn) add(clip.spawn->position);
                for (auto& m : clip.bus_stops) add(m.position);
                for (auto& l : clip.user_lights) add(l.position);
                for (auto& a : clip.audio_volumes) add(a.center - V3{0, a.half.y, 0});
                for (auto& s : clip.splines)
                    for (auto& p : s.points) add(p);
                if (lo.x <= hi.x) delta = point - V3{(lo.x + hi.x) * 0.5, lo.y, (lo.z + hi.z) * 0.5};
            }
            std::vector<Pick> made;
            if (clip.spawn) {
                Marker m = *clip.spawn;
                m.position = m.position + delta;
                edits->spawn = m;
                made.push_back({kSpawnItem, 0});
            }
            for (Marker m : clip.bus_stops) {
                m.position = m.position + delta;
                edits->bus_stops.push_back(m);
                made.push_back({kBusItem, (int)edits->bus_stops.size() - 1});
            }
            for (UserLight l : clip.user_lights) {
                l.position = l.position + delta;
                l.source.clear();  // a copy is a new light
                edits->user_lights.push_back(l);
                made.push_back({kLightItem, (int)edits->user_lights.size() - 1});
            }
            for (AudioVolume a : clip.audio_volumes) {
                a.center = a.center + delta;
                edits->audio_volumes.push_back(a);
                made.push_back({kAudioItem, (int)edits->audio_volumes.size() - 1});
            }
            for (UserSpline s : clip.splines) {
                for (V3& p : s.points) p = p + delta;
                edits->splines.push_back(s);
                made.push_back({kSplineItem, (int)edits->splines.size() - 1});
            }
            placing = kNotPlacing;
            pick_many(made);
            if (!made.empty()) reveal_item({made.back().first, made.back().second});
            touch();
            message = "Pasted " + with_commas(made.size()) + (made.size() == 1 ? " thing" : " things");
            return;
        }
        if (auto it = clip.objects.find("*"); it != clip.objects.end()) {
            std::vector<int> targets = picked_nodes(false);
            if (targets.empty()) {
                message = "Select map objects to paste the settings onto";
                return;
            }
            for (int t : targets) {
                const std::string& key = preview.scene.nodes[t].key;
                if (key.empty()) continue;
                if (it->second.empty()) edits->objects.erase(key);
                else edits->objects[key] = it->second;
            }
            bump();
            message = "Pasted the settings onto " + with_commas(targets.size()) + (targets.size() == 1 ? " object" : " objects");
            return;
        }
        if (auto it = clip.lights.find("*"); it != clip.lights.end()) {
            std::vector<int> targets = picked_nodes(true);
            if (targets.empty()) {
                message = "Select map lights to paste the settings onto";
                return;
            }
            for (int t : targets) {
                const std::string& key = preview.scene.nodes[t].key;
                if (key.empty()) continue;
                if (it->second.empty()) edits->lights.erase(key);
                else edits->lights[key] = it->second;
            }
            touch();
            message = "Pasted the settings onto " + with_commas(targets.size()) + (targets.size() == 1 ? " light" : " lights");
            return;
        }
        if (auto it = clip.ripped_splines.find("*"); it != clip.ripped_splines.end()) {
            size_t n = 0;
            for (auto& [k, i] : picked) n += k == kCurveItem || (k == kSplineItem && item_valid({k, i}) && !edits->splines[i].npc);
            if (!n) {
                message = "Select splines or grind curves to paste the grind settings onto";
                return;
            }
            GrindSettings g = it->second.grind;
            edit_picked_grinds([&](GrindSettings& x) { x = g; });
            message = "Pasted the grind settings onto " + with_commas(n);
            return;
        }
        message = "Nothing to paste";
    }

    // ---- list clicks and the context menu ------------------------------------------------------

    static Pick row_pick(const Row& row) {
        switch (row.type) {
        case Row::Object: return {kNodeSel, row.id};
        case Row::ItemRow: return {row.kind, row.id};
        case Row::CurveRow: return {kCurveItem, row.id};
        default: return {-1, -1};
        }
    }
    // A list row clicked: Ctrl adds or removes it, Shift selects the rows from the last click to it.
    void row_click(Pick p, int r, const std::vector<Row>& rows) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            toggle_pick(p);
            anchor = p;
            return;
        }
        if (io.KeyShift && anchor.first >= 0) {
            int a = -1;
            for (int k = 0; k < (int)rows.size(); ++k)
                if (row_pick(rows[k]) == anchor) {
                    a = k;
                    break;
                }
            if (a >= 0) {
                std::vector<Pick> list;
                for (int k = std::min(a, r); k <= std::max(a, r); ++k)
                    if (row_pick(rows[k]).first >= 0) list.push_back(row_pick(rows[k]));
                pick_many(list, p);
                return;
            }
        }
        select_one(p, false);
        anchor = p;
        if (ImGui::IsMouseDoubleClicked(0)) frame_selection();
    }
    // Right-click on the row just drawn: selects it (unless it already is) and opens the menu.
    void row_context(Pick p) {
        if (!ImGui::IsItemClicked(ImGuiMouseButton_Right)) return;
        if (!picked.count(p)) select_one(p, false), anchor = p;
        ctx_reset();
        ctx_target = p;
        open_context = true;
    }
    void ctx_reset() {
        ctx_target = {-1, -1};
        ctx_plant = ctx_section = ctx_set = -1;
        ctx_has_point = false;
        context_pos = ImVec2(-1, -1);
    }

    void context_menu() {
        const Scene& sc = preview.scene;
        auto [kind, index] = ctx_target;
        bool group = false;
        auto next_group = [&] {
            if (group) ImGui::Separator();
            group = true;
        };
        if (ctx_plant >= 0 && ctx_plant < (int)plants.size()) {
            next_group();
            if (ImGui::MenuItem(("Select every " + plants[ctx_plant].name).c_str())) expand_selection(plant_things(ctx_plant));
        }
        if (ctx_section >= 0) {
            next_group();
            if (ImGui::MenuItem((std::string("Select all in ") + kCategoryNames[ctx_section]).c_str())) expand_selection(category_things(ctx_section));
        }
        if (ctx_set >= 0) {
            next_group();
            if (ImGui::MenuItem(ctx_set ? "Select all auto splines" : "Select all map splines")) expand_selection(curve_things(ctx_set));
        }
        if (kind == kNodeSel && index >= 0 && index < (int)sc.nodes.size()) {
            next_group();
            node_menu(index);
        } else if (kind == kCurveItem && ripped_curve(index)) {
            next_group();
            curve_menu(index);
        } else if (kind == kSplineItem && item_valid({kind, index})) {
            next_group();
            spline_menu(index);
        } else if (kind >= kSpawnItem && kind <= kAudioItem && item_valid({kind, index})) {
            next_group();
            placed_menu(kind);
        }
        next_group();
        bool cuttable = false;
        for (auto& [k, i] : picked) cuttable = cuttable || (k != kNodeSel && k != kCurveItem);
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !picked.empty())) copy_selection(false);
        ImGui::SetItemTooltip("Placed things are copied as themselves; a map object, light or spline copies its settings");
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, cuttable)) copy_selection(true);
        if (ImGui::MenuItem(ctx_has_point ? "Paste here" : "Paste", "Ctrl+V", false, clipboard_has_edits())) paste(ctx_has_point, ctx_point);
        ImGui::Separator();
        if (ctx_has_point && edits && ImGui::BeginMenu("Add here")) {
            add_here_menu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Select all")) {
            select_all_menu();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame selection", "F", false, !picked.empty())) frame_selection();
        if (ImGui::MenuItem("Deselect", "Esc", false, !picked.empty())) expand_selection({});
    }

    void node_menu(int i) {
        const Scene& sc = preview.scene;
        const OutNode& n = sc.nodes[i];
        int c = category[i];
        if (ImGui::MenuItem((std::string("Select all in ") + kCategoryNames[c]).c_str())) expand_selection(category_things(c), {kNodeSel, i});
        if (n.mesh >= 0 && ImGui::MenuItem("Select all with this mesh")) {
            std::vector<Pick> list;
            for (size_t k = 0; k < sc.nodes.size(); ++k)
                if (sc.nodes[k].mesh == n.mesh) list.push_back({kNodeSel, (int)k});
            expand_selection(list, {kNodeSel, i});
        }
        if (n.mesh >= 0 && n.mesh < (int)meshes.size() && !meshes[n.mesh].parts.empty() && ImGui::BeginMenu("Select all using material")) {
            std::set<int> shown;
            for (const Part& part : meshes[n.mesh].parts) {
                int mi = part.material;
                if (mi < 0 || mi >= (int)sc.materials.size() || !shown.insert(mi).second) continue;
                if (ImGui::MenuItem(sc.materials[mi].name.c_str())) {
                    std::vector<Pick> list;
                    for (size_t k = 0; k < sc.nodes.size(); ++k) {
                        int m = sc.nodes[k].mesh;
                        if (m < 0 || m >= (int)meshes.size()) continue;
                        for (const Part& q : meshes[m].parts)
                            if (q.material == mi) {
                                list.push_back({kNodeSel, (int)k});
                                break;
                            }
                    }
                    expand_selection(list, {kNodeSel, i});
                }
            }
            ImGui::EndMenu();
        }
        if (!n.children.empty() && ImGui::MenuItem("Select it and everything under it")) {
            std::vector<Pick> list;
            std::vector<int> stack = {i};
            while (!stack.empty()) {
                int k = stack.back();
                stack.pop_back();
                if (k < 0 || k >= (int)sc.nodes.size()) continue;
                list.push_back({kNodeSel, k});
                for (int ch : sc.nodes[k].children) stack.push_back(ch);
            }
            expand_selection(list, {kNodeSel, i});
        }
        if (node_plant[i] >= 0 && ImGui::MenuItem(("Select every " + plants[node_plant[i]].name).c_str())) expand_selection(plant_things(node_plant[i]));
        if (int li = light_of(i); li >= 0) {
            static const char* const kinds[] = {"Select all point lights", "Select all spot lights", "Select all suns", "Select all area lights"};
            int kind = light_kind(sc.lights[sc.nodes[li].light]);
            if (ImGui::MenuItem(kinds[kind])) expand_selection(map_lights_of(kind), {kNodeSel, li});
            if (ImGui::MenuItem("Select all the map's lights")) expand_selection(map_lights_of(-1), {kNodeSel, li});
        }
        if (!edits) return;
        std::vector<int> objects = picked_nodes(false), lights = picked_nodes(true);
        std::string many = picked.size() > 1 ? " (" + with_commas(picked.size()) + ")" : "";
        if (!objects.empty()) {
            ImGui::Separator();
            const std::string& key = n.light >= 0 ? sc.nodes[objects.front()].key : n.key;
            ObjectEdit shown = edits->objects.count(key) ? edits->objects[key] : ObjectEdit{};
            if (ImGui::MenuItem(("Include in export" + many).c_str(), nullptr, !shown.removed)) {
                bool remove = !shown.removed;
                edit_picked_objects([&](ObjectEdit& e) { e.removed = remove; });
            }
            if (ImGui::MenuItem(("Collision only (not drawn)" + many).c_str(), nullptr, shown.collision_only)) {
                bool on = !shown.collision_only;
                edit_picked_objects([&](ObjectEdit& e) { e.collision_only = on; });
            }
            if (ImGui::BeginMenu(("Collision" + many).c_str())) {
                const char* modes[] = {"As ripped", "Triangle mesh", "Convex parts", "Convex hull", "None (render only)", "Water"};
                int current = shown.collision ? (int)*shown.collision + 1 : 0;
                for (int m = 0; m < 6; ++m)
                    if (ImGui::MenuItem(modes[m], nullptr, current == m))
                        edit_picked_objects([&](ObjectEdit& e) {
                            if (m == 0) e.collision.reset();
                            else e.collision = (Collision)(m - 1);
                        });
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Make their materials invisible (collision only)")) {
                std::set<int> mats;
                for (int o : objects) {
                    int m = sc.nodes[o].mesh;
                    if (m >= 0 && m < (int)meshes.size())
                        for (const Part& part : meshes[m].parts)
                            if (part.material >= 0 && part.material < (int)sc.materials.size()) mats.insert(part.material);
                }
                for (int mi : mats) {
                    const std::string& mk = sc.materials[mi].key;
                    if (!mk.empty()) edits->materials[mk].invisible = true;
                }
                bump();
            }
            ImGui::SetItemTooltip("Every object using those materials collides but is not drawn");
            if (ImGui::MenuItem(("Reset Studio settings" + many).c_str())) edit_picked_objects([](ObjectEdit& e) { e = ObjectEdit{}; });
        }
        if (!lights.empty()) {
            ImGui::Separator();
            auto found = edits->lights.find(sc.nodes[lights.front()].key);
            bool removed = found != edits->lights.end() && found->second.removed;
            if (ImGui::MenuItem(("Include light in export" + many).c_str(), nullptr, !removed)) {
                for (int l : lights) {
                    const std::string& key = sc.nodes[l].key;
                    if (key.empty()) continue;
                    LightEdit e = edits->lights.count(key) ? edits->lights[key] : LightEdit{};
                    e.removed = !removed;
                    if (e.empty()) edits->lights.erase(key);
                    else edits->lights[key] = e;
                }
                touch();
            }
        }
    }

    void grind_menu_items() {
        std::string many = picked.size() > 1 ? " (" + with_commas(picked.size()) + ")" : "";
        GrindSettings shown;
        if (item.kind == kSplineItem && item_valid(item)) shown = edits->splines[item.index].grind;
        else if (item.kind == kCurveItem && ripped_curve(item.index)) shown = ripped_grind(*ripped_curve(item.index));
        if (ImGui::MenuItem(("Grind curve on" + many).c_str(), nullptr, shown.enabled)) {
            bool on = !shown.enabled;
            edit_picked_grinds([&](GrindSettings& g) { g.enabled = on; });
        }
        ImGui::SetItemTooltip("Off: left out of the export");
        if (ImGui::BeginMenu("Rail radius")) {
            for (int mm : {10, 20, 30, 50, 80, 120, 200})
                if (ImGui::MenuItem((std::to_string(mm) + " mm").c_str(), nullptr, std::lround(shown.radius * 1000) == mm))
                    edit_picked_grinds([&](GrindSettings& g) { g.radius = mm / 1000.0; });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Surface")) {
            for (int s = 0; s < kGrindSurfaceCount; ++s)
                if (ImGui::MenuItem(kGrindSurfaceLabels[s], nullptr, shown.surface == s)) edit_picked_grinds([&](GrindSettings& g) { g.surface = s; });
            ImGui::EndMenu();
        }
    }
    void curve_menu(int index) {
        bool is_auto = curve_is_auto(index);
        if (ImGui::MenuItem(is_auto ? "Select all auto splines" : "Select all map splines")) expand_selection(curve_things(is_auto ? 1 : 0), {kCurveItem, index});
        if (ImGui::MenuItem("Select all splines and grind curves")) {
            std::vector<Pick> list = curve_things(2), mine = spline_things(false);
            list.insert(list.end(), mine.begin(), mine.end());
            expand_selection(list, {kCurveItem, index});
        }
        ImGui::Separator();
        grind_menu_items();
        if (ImGui::MenuItem("Make editable copies")) copy_curves();
        ImGui::SetItemTooltip("Copies them as grind curves you can edit point by point, and switches these off");
        if (picked_curves_moved() && ImGui::MenuItem("Put back where they were")) unmove_picked_curves();
    }
    void spline_menu(int index) {
        bool npc = edits->splines[index].npc;
        if (ImGui::MenuItem(npc ? "Select all NPC routes" : "Select all grind curves")) expand_selection(spline_things(npc), {kSplineItem, index});
        ImGui::Separator();
        if (!npc) grind_menu_items();
        if (ImGui::MenuItem("Reverse direction")) reverse_selected();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selected();
        if (ImGui::MenuItem("Delete", "Del")) delete_selected();
    }
    void placed_menu(int kind) {
        const char* all = kind == kBusItem ? "Select all bus stops" : kind == kLightItem ? "Select all added lights"
                        : kind == kAudioItem ? "Select all audio volumes" : nullptr;
        if (kind == kLightItem && item_valid(Item{ctx_target.first, ctx_target.second})) {
            const UserLight& l = edits->user_lights[ctx_target.second];
            int lk = l.area ? 2 : l.spot ? 1 : 0;
            static const char* const kinds[] = {"Select all added point lights", "Select all added spot lights", "Select all added area lights"};
            if (ImGui::MenuItem(kinds[lk])) expand_selection(user_lights_of(lk), ctx_target);
        }
        if (all && ImGui::MenuItem(all)) expand_selection(item_things(kind), ctx_target);
        if (picked.size() == 1 && ImGui::MenuItem("Move to a clicked spot\xE2\x80\xA6")) placing = kPlaceMove;
        if (kind != kSpawnItem && ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selected();
        if (ImGui::MenuItem("Delete", "Del")) delete_selected();
    }
    void add_here_menu() {
        V3 p = ctx_point;
        auto place = [&](int mode) {
            placing = mode;
            place_at(p);
        };
        if (ImGui::MenuItem(edits->spawn ? "Move the player spawn here" : "Player spawn")) {
            if (edits->spawn) select_item({kSpawnItem}), place(kPlaceMove);
            else place(kPlaceSpawn);
        }
        if (ImGui::MenuItem("Bus stop")) place(kPlaceBus);
        if (ImGui::MenuItem("Point light")) place(kPlaceLight);
        if (ImGui::MenuItem("Spot light")) place(kPlaceSpot);
        if (ImGui::MenuItem("Audio volume")) place(kPlaceAudio);
        ImGui::Separator();
        if (ImGui::MenuItem("Grind curve starting here")) start_curve(false), place_at(p);
        if (ImGui::MenuItem("NPC route starting here")) start_curve(true), place_at(p);
    }
    void select_all_menu() {
        auto entry = [&](const char* label, std::vector<Pick> list) {
            std::string text = std::string(label) + "  (" + with_commas(list.size()) + ")";
            if (ImGui::MenuItem(text.c_str(), nullptr, false, !list.empty())) expand_selection(list);
        };
        entry("Decals", category_things(kDecals));
        entry("Collision objects", category_things(kCollision));
        entry("Terrain", category_things(kTerrainCat));
        if (ImGui::BeginMenu("Map lights")) {
            entry("All", map_lights_of(-1));
            entry("Point lights", map_lights_of(0));
            entry("Spot lights", map_lights_of(1));
            entry("Area lights", map_lights_of(3));
            entry("Suns", map_lights_of(2));
            ImGui::EndMenu();
        }
        entry("Map splines", curve_things(0));
        entry("Auto splines", curve_things(1));
        ImGui::Separator();
        entry("Grind curves", spline_things(false));
        entry("NPC routes", spline_things(true));
        if (ImGui::BeginMenu("Added lights")) {
            entry("All", user_lights_of(-1));
            entry("Point lights", user_lights_of(0));
            entry("Spot lights", user_lights_of(1));
            entry("Area lights", user_lights_of(2));
            ImGui::EndMenu();
        }
        entry("Audio volumes", item_things(kAudioItem));
        entry("Bus stops", item_things(kBusItem));
    }

    // Runs `fn` on the Studio settings of every selected map object (not lights).
    void edit_picked_objects(const std::function<void(ObjectEdit&)>& fn) {
        if (!edits) return;
        for (int i : picked_nodes(false)) {
            const std::string& key = preview.scene.nodes[i].key;
            if (key.empty()) continue;
            ObjectEdit e = edits->objects.count(key) ? edits->objects[key] : ObjectEdit{};
            fn(e);
            if (e.empty()) edits->objects.erase(key);
            else edits->objects[key] = e;
        }
        bump();
    }

    // ---- several selected ------------------------------------------------------------------

    static bool same_audio(const std::optional<AudioOverride>& a, const std::optional<AudioOverride>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a || (a->softness == b->softness && a->smoothness == b->smoothness && a->min_impact_force == b->min_impact_force &&
                      a->impact_cooldown == b->impact_cooldown && a->ignore_player_collisions == b->ignore_player_collisions);
    }
    static bool same_part(const ContactPart& a, const ContactPart& b) {
        return a.dynamic == b.dynamic && a.statik == b.statik && a.restitution == b.restitution;
    }
    static bool same_friction(const std::optional<FrictionOverride>& a, const std::optional<FrictionOverride>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a || (same_part(a->deck, b->deck) && same_part(a->truck, b->truck) && same_part(a->wheel, b->wheel));
    }
    // Copies onto `x` only what changed from `before` to `after`, so each of several objects keeps
    // its other settings.
    static void apply_object_diff(const ObjectEdit& before, const ObjectEdit& after, ObjectEdit& x) {
        if (before.removed != after.removed) x.removed = after.removed;
        if (before.collision_only != after.collision_only) x.collision_only = after.collision_only;
        if (before.collision != after.collision) x.collision = after.collision;
        if (before.surface != after.surface) x.surface = after.surface;
        if (before.round_rail != after.round_rail) x.round_rail = after.round_rail;
        if (before.hide_from_map != after.hide_from_map) x.hide_from_map = after.hide_from_map;
        if (before.behavior.has_value() != after.behavior.has_value()) {
            x.behavior = after.behavior;
        } else if (after.behavior) {
            for (int i = 0; i < kBehaviorCount; ++i)
                if ((*before.behavior)[i] != (*after.behavior)[i]) {
                    if (!x.behavior) x.behavior = std::array<bool, kBehaviorCount>{};
                    (*x.behavior)[i] = (*after.behavior)[i];
                }
        }
        if (!same_audio(before.audio, after.audio)) x.audio = after.audio;
        if (!same_friction(before.friction, after.friction)) x.friction = after.friction;
    }
    static void apply_light_diff(const LightEdit& before, const LightEdit& after, LightEdit& x) {
        if (before.removed != after.removed) x.removed = after.removed;
        if (before.range != after.range) x.range = after.range;
        if (before.times != after.times) x.times = after.times;
    }

    // One group of a mixed selection.
    struct Group {
        const char* one;
        const char* many;
        std::vector<Pick> picks;
    };
    std::vector<Group> selection_groups() const {
        std::vector<Group> g = {{"object", "objects", {}},          {"map light", "map lights", {}},
                                {"map spline", "map splines", {}},  {"auto spline", "auto splines", {}},
                                {"grind curve", "grind curves", {}}, {"NPC route", "NPC routes", {}},
                                {"player spawn", "player spawns", {}}, {"bus stop", "bus stops", {}},
                                {"light", "lights", {}},             {"audio volume", "audio volumes", {}}};
        for (auto& p : picked) {
            auto [k, i] = p;
            int at = -1;
            if (k == kNodeSel && i >= 0 && i < (int)nodes.size()) at = preview.scene.nodes[i].light >= 0 ? 1 : 0;
            else if (k == kCurveItem) at = curve_is_auto(i) ? 3 : 2;
            else if (k == kSplineItem && item_valid({k, i})) at = edits->splines[i].npc ? 5 : 4;
            else if (k == kSpawnItem) at = 6;
            else if (k == kBusItem) at = 7;
            else if (k == kLightItem) at = 8;
            else if (k == kAudioItem) at = 9;
            if (at >= 0) g[at].picks.push_back(p);
        }
        return g;
    }

    void multi_inspector() {
        std::vector<Group> groups = selection_groups();
        std::string breakdown;
        int kinds = 0;
        for (auto& g : groups)
            if (!g.picks.empty()) {
                breakdown += (breakdown.empty() ? "" : ",  ") + with_commas(g.picks.size()) + " " + (g.picks.size() == 1 ? g.one : g.many);
                ++kinds;
            }
        bool deletable = false;
        for (int i = 4; i < 10; ++i) deletable = deletable || !groups[i].picks.empty();
        int pressed = inspector_header(with_commas(picked.size()) + " selected", breakdown, col::accent, false, deletable);
        if (pressed == 1) frame_selection();
        if (pressed == 2) {
            pick_many({});
            return;
        }
        if (pressed == 4) {
            delete_selected();
            return;
        }
        ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
        size_t n = picked.size();
        auto only = [&](std::initializer_list<int> list) {
            size_t c = 0;
            for (int i : list) c += groups[i].picks.size();
            return c == n;
        };
        bool movable = false;
        for (int i = 2; i < 10; ++i) movable = movable || !groups[i].picks.empty();
        if (only({2, 3, 4})) {
            multi_grind(!groups[2].picks.empty() || !groups[3].picks.empty());
        } else if (only({5})) {
            multi_routes();
        } else if (only({0})) {
            note(("Changes apply to all " + with_commas(n) + ". The switches show the active one (" + preview.scene.nodes[selected >= 0 ? selected : groups[0].picks.front().second].name + ").").c_str());
            object_editor(selected >= 0 ? selected : groups[0].picks.front().second);
        } else if (only({1})) {
            note(("Changes apply to all " + with_commas(n) + ".").c_str());
            light_settings(selected >= 0 ? selected : groups[1].picks.front().second);
        } else if (only({8})) {
            multi_user_lights();
        } else if (only({9})) {
            multi_audio();
        } else if (only({7})) {
            bool shelter = edits->bus_stops[groups[7].picks.front().second].shelter;
            if (switch_row("Shelters", shelter)) {
                for (auto& [k, i] : groups[7].picks) edits->bus_stops[i].shelter = shelter;
                touch();
            }
        }
        if (kinds > 1) {
            if (section("Keep only", false)) {
                for (auto& g : groups) {
                    if (g.picks.empty()) continue;
                    std::string label = with_commas(g.picks.size()) + " " + (g.picks.size() == 1 ? g.one : g.many);
                    ImGui::PushID(g.one);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(8), right_edge() - S(90)));
                    if (ImGui::Button("Only these", ImVec2(S(90), 0))) {
                        std::vector<Pick> keep = g.picks;
                        ImGui::PopID();
                        expand_selection(keep);
                        ImGui::EndChild();
                        return;
                    }
                    ImGui::PopID();
                }
            }
        }
        if (movable) note("Drag the arrows in the view to move them together (map objects stay put).");
        ImGui::EndChild();
    }

    // Grind settings of several grind curves and map splines at once.
    void multi_grind(bool has_map) {
        GrindSettings g;
        if (item.kind == kSplineItem && item_valid(item)) g = edits->splines[item.index].grind;
        else if (item.kind == kCurveItem && ripped_curve(item.index)) g = ripped_grind(*ripped_curve(item.index));
        const GrindSettings before = g;
        note(("Changes apply to all " + with_commas(picked.size()) + ".").c_str());
        if (section("Grind (ReSkate Studio)", !default_grind(g))) {
            switch_row("Grind curve on", g.enabled);
            ImGui::SetItemTooltip("Off: left out of the export");
            ImGui::BeginDisabled(!g.enabled);
            double mm = g.radius * 1000;
            if (slider_row("Rail radius", mm, 5, 250, "%.0f mm")) g.radius = mm / 1000;
            std::vector<std::string> names(kGrindSurfaceLabels, kGrindSurfaceLabels + kGrindSurfaceCount);
            combo_row("Surface", g.surface, names, "The sound and feel of the grind");
            ImGui::EndDisabled();
        }
        if (g.enabled != before.enabled || g.radius != before.radius || g.surface != before.surface)
            edit_picked_grinds([&](GrindSettings& x) {
                if (g.enabled != before.enabled) x.enabled = g.enabled;
                if (g.radius != before.radius) x.radius = g.radius;
                if (g.surface != before.surface) x.surface = g.surface;
            });
        if (has_map) {
            ImGui::Dummy(ImVec2(0, S(4)));
            if (ImGui::Button("Make editable copies")) copy_curves();
            ImGui::SetItemTooltip("Copies the map splines as grind curves you can edit point by point, and switches them off");
            if (picked_curves_moved()) {
                ImGui::SameLine();
                if (ImGui::Button("Put back")) unmove_picked_curves();
                ImGui::SetItemTooltip("Undo moving the map splines");
            }
        }
    }

    void multi_routes() {
        NpcSettings r = item.kind == kSplineItem && item_valid(item) ? edits->splines[item.index].route : NpcSettings{};
        const NpcSettings before = r;
        note(("Changes apply to all " + with_commas(picked.size()) + ".").c_str());
        if (section("NPC route", false)) {
            int kind = (int)r.kind;
            if (combo_row("Used by", kind, {"Pedestrians", "Vehicles", "Buses"})) r.kind = (NpcKind)kind;
            slider_row("Width", r.width, 0.5, 20, "%.1f m");
            slider_row("Spacing", r.spacing, 0.5, 50, "%.1f m");
            slider_row("Weight", r.weight, 0, 10, "%.2f");
            int_row("Speed", r.speed, 1, 9);
            switch_row("Both directions", r.bidirectional);
            switch_row("Has stairs", r.stairs);
        }
        bool changed = false;
        for (auto& [k, i] : picked) {
            if (k != kSplineItem || !item_valid({k, i}) || !edits->splines[i].npc) continue;
            NpcSettings& x = edits->splines[i].route;
            if (r.kind != before.kind) x.kind = r.kind, changed = true;
            if (r.width != before.width) x.width = r.width, changed = true;
            if (r.spacing != before.spacing) x.spacing = r.spacing, changed = true;
            if (r.weight != before.weight) x.weight = r.weight, changed = true;
            if (r.speed != before.speed) x.speed = r.speed, changed = true;
            if (r.bidirectional != before.bidirectional) x.bidirectional = r.bidirectional, changed = true;
            if (r.stairs != before.stairs) x.stairs = r.stairs, changed = true;
        }
        if (changed) touch();
    }

    void multi_user_lights() {
        UserLight l = item.kind == kLightItem && item_valid(item) ? edits->user_lights[item.index] : UserLight{};
        const UserLight before = l;
        note(("Changes apply to all " + with_commas(picked.size()) + ".").c_str());
        if (section("Light", false)) {
            int type = l.area ? 2 : l.spot ? 1 : 0;
            if (combo_row("Type", type, {"Point light", "Spot light", "Area light"})) l.spot = type == 1, l.area = type == 2;
            row_label("Colour", control_width());
            ImGui::ColorEdit3("##colour", l.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
            open_row("Intensity", l.intensity, 0.1, 1000000, "%.0f cd");
            open_row("Range", l.range, 0.5, 1000, "%.1f m");
            if (l.spot) slider_row("Cone", l.cone, 1, 179, "%.0f\xC2\xB0");
        }
        if (section("Time of day", l.times != kAllLightTimes)) time_chips(l.times);
        bool changed = false;
        for (auto& [k, i] : picked) {
            if (k != kLightItem || !item_valid({k, i})) continue;
            UserLight& x = edits->user_lights[i];
            if (l.spot != before.spot || l.area != before.area) x.spot = l.spot, x.area = l.area, changed = true;
            if (!std::equal(l.color, l.color + 3, before.color)) std::copy(l.color, l.color + 3, x.color), changed = true;
            if (l.intensity != before.intensity) x.intensity = l.intensity, changed = true;
            if (l.range != before.range) x.range = l.range, changed = true;
            if (l.cone != before.cone) x.cone = l.cone, changed = true;
            if (l.times != before.times) x.times = l.times, changed = true;
        }
        if (changed) touch();
    }

    void multi_audio() {
        AudioVolume a = item.kind == kAudioItem && item_valid(item) ? edits->audio_volumes[item.index] : AudioVolume{};
        const AudioVolume before = a;
        note(("Changes apply to all " + with_commas(picked.size()) + ".").c_str());
        if (section("Sound", false)) {
            std::vector<std::string> presets(kAudioPresetLabels, kAudioPresetLabels + kAudioPresets);
            if (combo_row("Ambience", a.preset, presets) && a.preset == 3 && a.region_tag.empty()) a.region_tag = kDefaultAudioRegion;
            if (a.preset == 3) region_row(a.region_tag);
            if (a.preset == 2) text_row("Behavior", a.behavior, "BehaviorAsset path");
        }
        if (section("Box", false)) {
            const char* names[] = {"Width", "Height", "Length"};
            double* halves[] = {&a.half.x, &a.half.y, &a.half.z};
            for (int i = 0; i < 3; ++i) {
                double full = *halves[i] * 2;
                if (slider_row(names[i], full, 0.5, 400, "%.1f m", ImGuiSliderFlags_Logarithmic)) *halves[i] = full * 0.5;
            }
        }
        if (section("Mixing", false, false)) {
            slider_row("Activation distance", a.activation_distance, 0, 200, "%.0f m");
            slider_row("Indooriness", a.indooriness, 0, 1, "%.2f");
            slider_row("Density", a.density, 0, 1, "%.2f");
            int_row("Priority", a.priority, 0, 10);
            switch_row("Additive", a.additive);
            switch_row("Allow in child regions", a.child_regions);
        }
        bool changed = false;
        for (auto& [k, i] : picked) {
            if (k != kAudioItem || !item_valid({k, i})) continue;
            AudioVolume& x = edits->audio_volumes[i];
            auto copy = [&](auto field) {
                if (!(a.*field == before.*field)) x.*field = a.*field, changed = true;
            };
            copy(&AudioVolume::preset);
            copy(&AudioVolume::region_tag);
            copy(&AudioVolume::behavior);
            copy(&AudioVolume::activation_distance);
            copy(&AudioVolume::indooriness);
            copy(&AudioVolume::density);
            copy(&AudioVolume::priority);
            copy(&AudioVolume::additive);
            copy(&AudioVolume::child_regions);
            if (a.half.x != before.half.x) x.half.x = a.half.x, changed = true;
            if (a.half.y != before.half.y) x.half.y = a.half.y, changed = true;
            if (a.half.z != before.half.z) x.half.z = a.half.z, changed = true;
        }
        if (changed) touch();
    }

    // ---- inspector widgets ----------------------------------------------------------------

    // Label on the left; the next control fills the right part of the row.
    static float control_width() { return std::min(S(190), ImGui::GetContentRegionAvail().x * 0.58f); }
    static void row_label(const char* label, float control_w) {
        float avail = ImGui::GetContentRegionAvail().x;
        control_w = std::min(control_w, avail - ImGui::CalcTextSize(label).x - S(12));  // never over the label
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(avail - control_w);
        ImGui::SetNextItemWidth(control_w);
    }
    static bool combo_row(const char* label, int& current, const std::vector<std::string>& items, const char* tip = nullptr) {
        ImGui::PushID(label);
        row_label(label, control_width());
        bool changed = false;
        if (ImGui::BeginCombo("##c", items[std::clamp(current, 0, (int)items.size() - 1)].c_str())) {
            for (int i = 0; i < (int)items.size(); ++i)
                if (ImGui::Selectable(items[i].c_str(), i == current)) current = i, changed = true;
            ImGui::EndCombo();
        }
        if (tip) ImGui::SetItemTooltip("%s", tip);
        ImGui::PopID();
        return changed;
    }
    static bool slider_row(const char* label, double& v, double lo, double hi, const char* fmt, ImGuiSliderFlags flags = 0) {
        ImGui::PushID(label);
        row_label(label, control_width());
        float f = (float)v;
        bool changed = ImGui::SliderFloat("##s", &f, (float)lo, (float)hi, fmt, ImGuiSliderFlags_AlwaysClamp | flags);
        if (changed) v = f;
        ImGui::PopID();
        return changed;
    }
    static const char* surface_label(int packed) {
        for (auto& s : kStudioSurfaces)
            if (s.packed == packed) return s.label;
        return "(unknown surface)";
    }
    // Studio's surface / sound donor: one of its native collision materials, searchable.
    bool surface_row(const char* label, std::optional<int>& value, const char* tip) {
        ImGui::PushID(label);
        row_label(label, control_width());
        bool changed = false;
        std::string preview_text = value ? surface_label(*value) : "As ripped";
        if (ImGui::BeginCombo("##surface", preview_text.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                surface_filter[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", "Search surfaces\xE2\x80\xA6", surface_filter, sizeof surface_filter);
            if (ImGui::Selectable("As ripped (Studio default)", !value)) value.reset(), changed = true;
            std::string q = lower_copy(surface_filter);
            for (auto& sf : kStudioSurfaces) {
                if (!q.empty() && lower_copy(sf.label).find(q) == std::string::npos) continue;
                if (ImGui::Selectable(sf.label, value && *value == sf.packed)) value = sf.packed, changed = true;
            }
            ImGui::EndCombo();
        }
        if (tip) ImGui::SetItemTooltip("%s", tip);
        ImGui::PopID();
        return changed;
    }

    static bool text_row(const char* label, std::string& s, const char* hint = "") {
        ImGui::PushID(label);
        row_label(label, control_width());
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s", s.c_str());
        bool changed = ImGui::InputTextWithHint("##t", hint, buf, sizeof buf);
        if (changed) s = buf;
        ImGui::PopID();
        return changed;
    }
    static bool position_row(const char* label, V3& p) {
        ImGui::PushID(label);
        row_label(label, control_width());
        float v[3] = {(float)p.x, (float)p.y, (float)p.z};
        bool changed = ImGui::DragFloat3("##p", v, 0.05f, 0, 0, "%.2f");
        if (changed) p = {v[0], v[1], v[2]};
        ImGui::SetItemTooltip("X  Y (up)  Z, in metres; drag or double-click to type");
        ImGui::PopID();
        return changed;
    }
    // A value on a logarithmic slider that Ctrl+click can type past its end (not below zero).
    static bool open_row(const char* label, double& v, double lo, double hi, const char* fmt) {
        ImGui::PushID(label);
        row_label(label, control_width());
        float f = (float)v;
        bool changed = ImGui::SliderFloat("##s", &f, (float)lo, (float)hi, fmt, ImGuiSliderFlags_Logarithmic);
        if (changed) v = std::max(0.0f, f);
        ImGui::SetItemTooltip("Ctrl+click to type any value");
        ImGui::PopID();
        return changed;
    }
    static bool int_row(const char* label, int& v, int lo, int hi) {
        ImGui::PushID(label);
        row_label(label, control_width());
        bool changed = ImGui::SliderInt("##i", &v, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        return changed;
    }
    // Two or more side-by-side buttons, one lit.
    static bool segmented(const char* id, int& value, std::initializer_list<const char*> names) {
        ImGui::PushID(id);
        float w = ImGui::GetContentRegionAvail().x / (float)names.size();
        bool changed = false;
        int i = 0;
        for (const char* name : names) {
            if (i) ImGui::SameLine(0, 0);
            bool on = value == i;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? rgb(96, 142, 255, 0.24f) : col::field);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? rgb(96, 142, 255, 0.32f) : col::field_hover);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? col::accent_hover : col::muted);
            if (ImGui::Button(name, ImVec2(w, 0)) && !on) value = i, changed = true;
            ImGui::PopStyleColor(3);
            ++i;
        }
        ImGui::PopID();
        return changed;
    }
    // A draggable gap between two panels; `sign` +1 widens the panel on its left, -1 the one on
    // its right. Double-click: back to the default width.
    static void splitter(const char* id, float& width, float sign, float gap, float h) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(gap, h));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) width += ImGui::GetIO().MouseDelta.x * sign;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) width = 0;
        if (hot)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y + S(10)), ImVec2(p.x + gap * 0.5f, p.y + h - S(10)),
                                                ImGui::GetColorU32(col::accent), S(2));
    }

    // ---- inspector -------------------------------------------------------------------------

    void heading(const char* text) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PushFont(fonts.caption);
        ImGui::TextColored(col::muted, "%s", text);
        ImGui::PopFont();
        ImGui::Separator();
    }
    void label_value(const char* label, const std::string& value) {
        ImGui::TextColored(col::muted, "%s", label);
        ImGui::SameLine(std::max(S(110), ImGui::CalcTextSize(label).x + S(28)));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    }
    void note(const char* text, ImVec4 colour = col::muted) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(colour, "%s", text);
        ImGui::PopTextWrapPos();
    }

    // A collapsible group of settings; a dot after the title marks one changed from the ripped
    // map. Whether it is open is remembered per title.
    bool section(const char* title, bool changed, bool open_default = true) {
        ImGui::PushID(title);
        bool* open = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("open"), open_default);
        ImGui::Dummy(ImVec2(0, S(3)));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
        if (ImGui::InvisibleButton("##section", ImVec2(w, h))) *open = !*open;
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(hovered ? col::field_hover : rgb(30, 34, 43)), S(6));
        arrow(dl, ImVec2(p.x + S(7), p.y), h, *open);
        ImGui::PushFont(fonts.semibold);
        ImVec2 ts = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2(p.x + S(24), p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(col::text), title);
        ImGui::PopFont();
        if (changed) {
            dl->AddCircleFilled(ImVec2(p.x + S(33) + ts.x, p.y + h * 0.5f), S(3.5f), ImGui::GetColorU32(col::accent), 12);
            if (hovered) ImGui::SetTooltip("Changed from the ripped map");
        }
        ImGui::PopID();
        if (*open) ImGui::Dummy(ImVec2(0, S(1)));
        return *open;
    }

    // The inspector's top: what is selected and its buttons. Returns the button pressed: 0 none,
    // 1 frame, 2 close, 3 move, 4 delete.
    int inspector_header(const std::string& title, const std::string& kind, ImVec4 dot, bool movable, bool deletable) {
        int pressed = 0;
        float close_w = ImGui::GetFrameHeight();
        float right = right_edge();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(fonts.semibold);
        float line_h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(4), p.y + line_h * 0.5f), S(4), ImGui::GetColorU32(dot), 12);
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(14), p.y));
        ImGui::PushTextWrapPos(right - close_w - S(8));
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + right - close_w, p.y - S(3)));
        if (ImGui::Button("\xC3\x97", ImVec2(close_w, close_w))) pressed = 2;
        ImGui::SetItemTooltip("Deselect (Esc in the view)");
        ImGui::SetCursorScreenPos(ImVec2(p.x, after.y));
        if (!kind.empty()) note(kind.c_str());
        ImGui::Dummy(ImVec2(0, S(2)));
        if (ImGui::Button("Frame")) pressed = 1;
        ImGui::SetItemTooltip("Fly the camera to it (F)");
        if (movable) {
            ImGui::SameLine();
            if (ImGui::Button(placing == kPlaceMove ? "Moving\xE2\x80\xA6" : "Move\xE2\x80\xA6")) pressed = 3;
            ImGui::SetItemTooltip("Click a new spot in the view (or drag the arrows there)");
        }
        if (deletable) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col::bad);
            if (ImGui::Button("Delete")) pressed = 4;
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Delete it (Del)");
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::Separator();
        return pressed;
    }

    // Toggles for the times of day a light shines at (Studio's bitmask).
    bool time_chips(int& mask) {
        bool changed = false;
        float avail = ImGui::GetContentRegionAvail().x, x = 0;
        ImGui::PushID("times");
        for (int i = 0; i < kLightTimes; ++i) {
            bool on = (mask >> i) & 1;
            float w = ImGui::CalcTextSize(kLightTimeLabels[i]).x + S(24);
            if (i && x + S(6) + w <= avail) ImGui::SameLine(0, S(6)), x += S(6);
            else x = 0;
            if (chip(kLightTimeLabels[i], on)) mask ^= 1 << i, changed = true;
            x += w;
        }
        if (mask != kAllLightTimes) {
            if (ImGui::SmallButton("All times")) mask = kAllLightTimes, changed = true;
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Night only")) mask = 16 | 64, changed = true;
        if (mask == 0) note("Off at every time of day: it never shines.", col::warn);
        ImGui::PopID();
        return changed;
    }

    // One of Studio's native ambience regions, searchable.
    bool region_row(std::string& tag) {
        ImGui::PushID("region");
        row_label("Region", control_width());
        std::string preview_text = tag.empty() ? std::string("(pick one)") : tag;
        for (auto& r : kStudioAudioRegions)
            if (tag == r.path) preview_text = r.label;
        bool changed = false;
        if (ImGui::BeginCombo("##r", preview_text.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                region_filter[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", "Search regions\xE2\x80\xA6", region_filter, sizeof region_filter);
            std::string q = lower_copy(region_filter);
            for (auto& r : kStudioAudioRegions) {
                if (!q.empty() && lower_copy(r.label).find(q) == std::string::npos && lower_copy(r.path).find(q) == std::string::npos) continue;
                if (ImGui::Selectable(r.label, tag == r.path)) tag = r.path, changed = true;
                ImGui::SetItemTooltip("%s", r.path);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The native Skate ambience it plays: %s", tag.c_str());
        ImGui::PopID();
        return changed;
    }

    // ---- objects ---------------------------------------------------------------------------

    // The Studio settings of one object (all painted copies of a plant share them).
    void object_editor(int node) {
        const Scene& sc = preview.scene;
        const OutNode& n = sc.nodes[node];
        if (!edits || n.key.empty()) {
            note("This object has no stable id in the map, so it cannot be edited.");
            return;
        }
        auto found = edits->objects.find(n.key);
        ObjectEdit e = found != edits->objects.end() ? found->second : ObjectEdit{};
        const ObjectEdit before = e;
        bool changed = false;
        ImGui::PushID("object_edit");
        size_t copies = 0;
        if (node_plant[node] >= 0)
            for (auto& other : sc.nodes) copies += other.key == n.key;
        if (copies > 1) note(("A painted plant part: changes apply to all " + with_commas(copies) + " copies.").c_str());

        bool include = !e.removed;
        if (switch_row("Include in export", include)) e.removed = !include, changed = true;
        ImGui::SetItemTooltip("Off: this object, and everything under it, is left out of the export");
        ImGui::BeginDisabled(e.removed);

        if (section("Collision", e.collision || e.collision_only || e.surface || e.round_rail)) {
            std::string ripped = "triangle mesh";
            for (auto& [k, v] : n.extras.o)
                if (k == "sk8_collision_mode" && v.t == Json::Str) ripped = v.s == "none" ? "none" : v.s == "hull" ? "convex hull" : "triangle mesh";
            std::vector<std::string> modes = {"As ripped (" + ripped + ")", "Triangle mesh", "Convex parts", "Convex hull",
                                              "None (render only)", "Water"};
            int mode = e.collision ? (int)*e.collision + 1 : 0;
            if (combo_row("Collision", mode, modes, "How Studio builds this object's collision")) {
                if (mode == 0) e.collision.reset();
                else e.collision = (Collision)(mode - 1);
                changed = true;
            }
            if (switch_row("Collision only (not drawn)", e.collision_only)) changed = true;
            ImGui::SetItemTooltip("Keeps the collision but draws nothing: its materials export as Studio 'invisible' copies");
            if (e.collision_only && e.collision && *e.collision == Collision::None)
                note("With no collision and nothing drawn, this object does nothing in game.", col::warn);
            if (surface_row("Surface / sound", e.surface, "Studio's base surface: the native material it takes sound, effects and "
                                                           "grip from (this object only; materials have their own)"))
                changed = true;
            if (switch_row("Round rail (smooth grind)", e.round_rail)) changed = true;
            ImGui::SetItemTooltip("Grind it as a smooth surface like the shipped round rails (needed for round tubes)");
        }
        if (section("Map and gameplay", e.hide_from_map || e.behavior.has_value(), false)) {
            if (switch_row("Hide from pause map", e.hide_from_map)) changed = true;
            bool behavior = e.behavior.has_value();
            if (switch_row("Override gameplay", behavior)) {
                if (behavior) e.behavior = std::array<bool, kBehaviorCount>{};
                else e.behavior.reset();
                changed = true;
            }
            if (e.behavior) {
                ImGui::Indent(S(12));
                for (int i = 0; i < kBehaviorCount; ++i)
                    if (switch_row(kBehaviorLabels[i], (*e.behavior)[i])) changed = true;
                ImGui::Unindent(S(12));
            }
        }
        if (section("Impact audio", e.audio.has_value(), false)) {
            bool audio = e.audio.has_value();
            if (switch_row("Override impact audio", audio)) {
                if (audio) e.audio = AudioOverride{};
                else e.audio.reset();
                changed = true;
            }
            if (e.audio) {
                changed |= slider_row("Softness", e.audio->softness, 0, 1, "%.2f");
                changed |= slider_row("Smoothness", e.audio->smoothness, 0, 1, "%.2f");
                changed |= slider_row("Min impact force", e.audio->min_impact_force, 0, 50, "%.2f");
                changed |= slider_row("Impact cooldown", e.audio->impact_cooldown, 0, 5, "%.2f s");
                changed |= switch_row("Ignore player collisions", e.audio->ignore_player_collisions);
            }
        }
        if (section("Contact physics", e.friction.has_value(), false)) {
            bool friction = e.friction.has_value();
            if (switch_row("Override contact physics", friction)) {
                if (friction) e.friction = FrictionOverride{};
                else e.friction.reset();
                changed = true;
            }
            if (e.friction)
                for (auto [name, part] : {std::pair<const char*, ContactPart*>{"Deck", &e.friction->deck},
                                          {"Trucks", &e.friction->truck}, {"Wheels", &e.friction->wheel}}) {
                    ImGui::PushID(name);
                    ImGui::TextColored(col::muted, "%s", name);
                    changed |= slider_row("Dynamic friction", part->dynamic, 0, 2, "%.2f");
                    changed |= slider_row("Static friction", part->statik, 0, 2, "%.2f");
                    changed |= slider_row("Bounce", part->restitution, 0, 1, "%.2f");
                    ImGui::PopID();
                }
        }
        ImGui::EndDisabled();
        if (!e.empty()) {
            ImGui::Dummy(ImVec2(0, S(2)));
            if (ImGui::Button("Reset object")) e = ObjectEdit{}, changed = true;
            ImGui::SetItemTooltip("Back to how it was ripped");
        }
        ImGui::PopID();
        if (changed) {  // what changed goes to every selected object (one, or several)
            std::vector<int> targets = picked.size() > 1 ? picked_nodes(false) : std::vector<int>{node};
            for (int t : targets) {
                const std::string& key = sc.nodes[t].key;
                if (key.empty()) continue;
                ObjectEdit x = edits->objects.count(key) ? edits->objects[key] : ObjectEdit{};
                apply_object_diff(before, e, x);
                if (x.empty()) edits->objects.erase(key);
                else edits->objects[key] = x;
            }
            bump();
        }
    }

    // The Studio settings of one of the map's lights.
    void light_settings(int node) {
        const Scene& sc = preview.scene;
        const OutNode& n = sc.nodes[node];
        if (!edits || n.key.empty() || n.light < 0 || n.light >= (int)sc.lights.size()) return;
        const OutLight& L = sc.lights[n.light];
        auto found = edits->lights.find(n.key);
        LightEdit e = found != edits->lights.end() ? found->second : LightEdit{};
        const LightEdit before = e;
        bool changed = false;
        ImGui::PushID("light_edit");
        bool include = !e.removed;
        if (switch_row("Include in export", include)) e.removed = !include, changed = true;
        if (L.type == 2) note("A sun: Studio leaves sun and sky to the game's time of day, so it is not exported as a light.");
        if (L.area) note(("An area light, " + std::to_string(L.area_size[0]).substr(0, 4) + " x " + std::to_string(L.area_size[1]).substr(0, 4) +
                          " m: the Skate mod gets a real area light.").c_str());
        ImGui::BeginDisabled(e.removed || L.type == 2);
        if (section("Range", e.range.has_value())) {
            bool custom = e.range.has_value();
            if (switch_row("Set its range", custom)) {
                if (custom) e.range = L.range > 0 ? L.range : 10.0;
                else e.range.reset();
                changed = true;
            }
            if (e.range) changed |= slider_row("Range", *e.range, 0.5, 200, "%.1f m", ImGuiSliderFlags_Logarithmic);
            else note(("As ripped: " + std::to_string((int)std::lround(L.range)) + " m").c_str());
        }
        if (section("Time of day", e.times.has_value())) {
            int mask = e.times.value_or(kAllLightTimes);
            if (time_chips(mask)) {
                if (mask == kAllLightTimes) e.times.reset();
                else e.times = mask;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        if (!e.empty()) {
            ImGui::Dummy(ImVec2(0, S(2)));
            if (ImGui::Button("Reset light")) e = LightEdit{}, changed = true;
        }
        ImGui::PopID();
        if (changed) {
            std::vector<int> targets = picked.size() > 1 ? picked_nodes(true) : std::vector<int>{node};
            for (int t : targets) {
                const std::string& key = sc.nodes[t].key;
                if (key.empty()) continue;
                LightEdit x = edits->lights.count(key) ? edits->lights[key] : LightEdit{};
                apply_light_diff(before, e, x);
                if (x.empty()) edits->lights.erase(key);
                else edits->lights[key] = x;
            }
            touch();
        }
    }

    // The Studio settings of a material (every object using it).
    void material_editor(int mi) {
        const Scene& sc = preview.scene;
        const OutMaterial& om = sc.materials[mi];
        if (!edits || om.key.empty()) return;
        auto found = edits->materials.find(om.key);
        MaterialEdit e = found != edits->materials.end() ? found->second : MaterialEdit{};
        bool changed = false;
        ImGui::PushID("material_edit");
        size_t users = 0;
        for (size_t i = 0; i < sc.nodes.size(); ++i) {
            int m = sc.nodes[i].mesh;
            if (m < 0 || m >= (int)meshes.size()) continue;
            for (auto& part : meshes[m].parts)
                if (part.material == mi) {
                    ++users;
                    break;
                }
        }
        note(("Used by " + with_commas(users) + (users == 1 ? " object" : " objects")).c_str());
        bool ripped_invisible = material_invisible_default(om);
        bool invisible = e.invisible.value_or(ripped_invisible);
        if (switch_row("Collision only (invisible)", invisible)) {
            if (invisible == ripped_invisible) e.invisible.reset();
            else e.invisible = invisible;
            changed = true;
        }
        ImGui::SetItemTooltip("Studio's 'Invisible (Collision Only)': surfaces with this material collide but are not drawn");
        if (surface_row("Surface / sound", e.surface, "The native material this one takes sound, effects and grip from"))
            changed = true;
        std::vector<std::string> alphas = {"As ripped", "Opaque", "Cut-out", "Blended"};
        int alpha = e.alpha ? *e.alpha : 0;
        if (combo_row("Transparency", alpha, alphas)) {
            if (alpha == 0) e.alpha.reset();
            else e.alpha = alpha;
            changed = true;
        }
        if (e.alpha && *e.alpha == 2) {
            double cutoff = e.alpha_cutoff.value_or(om.alpha_cutoff);
            if (slider_row("Cut-out threshold", cutoff, 0, 1, "%.2f")) e.alpha_cutoff = cutoff, changed = true;
        }
        std::vector<std::string> domains = {"As ripped", "Surface", "Decal", "Foliage", "Ocean"};
        int domain = !e.domain ? 0 : *e.domain == Domain::Surface ? 1 : *e.domain == Domain::Decal ? 2 : *e.domain == Domain::Foliage ? 3 : 4;
        if (combo_row("Shader type", domain, domains, "Studio's material domain")) {
            static const Domain values[] = {Domain::Surface, Domain::Decal, Domain::Foliage, Domain::Ocean};
            if (domain == 0) e.domain.reset();
            else e.domain = values[domain - 1];
            changed = true;
        }
        if (!e.empty() && ImGui::Button("Reset material")) e = MaterialEdit{}, changed = true;
        ImGui::PopID();
        if (changed) {
            if (e.empty()) edits->materials.erase(om.key);
            else edits->materials[om.key] = e;
            bump();
        }
    }

    // The object's materials, each opening its Studio settings in a card.
    void materials_section(const OutNode& n) {
        const Scene& sc = preview.scene;
        if (n.mesh < 0 || n.mesh >= (int)meshes.size() || meshes[n.mesh].parts.empty()) return;
        const Mesh& m = meshes[n.mesh];
        bool any = false;
        if (edits)
            for (const Part& part : m.parts)
                if (part.material >= 0 && part.material < (int)sc.materials.size()) {
                    auto it = edits->materials.find(sc.materials[part.material].key);
                    any = any || (it != edits->materials.end() && !it->second.empty());
                }
        if (!section("Materials", any)) return;
        note("Shared by every object that uses them. Click one for its settings.");
        char buf[64];
        for (size_t k = 0; k < m.parts.size(); ++k) {
            const Part& part = m.parts[k];
            if (part.material < 0 || part.material >= (int)sc.materials.size()) {
                ImGui::TextColored(col::muted, "(no material)");
                continue;
            }
            const OutMaterial& om = sc.materials[part.material];
            const Material& gm = materials[part.material];
            ImGui::PushID((int)k);
            float thumb = S(34);
            ImVec2 row = ImGui::GetCursorScreenPos();
            bool open = editing_material == part.material;
            if (ImGui::Selectable("##mat", open, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, thumb)))
                editing_material = open ? -1 : part.material;
            ImGui::SetCursorScreenPos(row);
            if (gm.base)
                ImGui::Image((ImTextureID)(uintptr_t)gm.base, ImVec2(thumb, thumb), ImVec2(0, 1), ImVec2(1, 0));
            else
                ImGui::ColorButton("##c", ImVec4(om.base[0], om.base[1], om.base[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(thumb, thumb));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(om.name.c_str());
            static const char* modes[] = {"opaque", "cut-out", "blended"};
            std::string info = modes[std::clamp(om.alpha_mode, 0, 2)];
            if (om.alpha_mode == 1) {
                std::snprintf(buf, sizeof buf, " %.2f", om.alpha_cutoff);
                info += buf;
            }
            if (part.material < (int)material_hidden.size() && material_hidden[part.material]) info += "  \xC2\xB7  collision only";
            if (edits) {
                auto it = edits->materials.find(om.key);
                if (it != edits->materials.end() && !it->second.empty()) info += "  \xC2\xB7  changed";
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(col::muted, "%s", info.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            if (open) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, col::inset);
                ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
                material_editor(part.material);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
        }
    }

    // What the object is: transform, mesh, light and what the rip found on it.
    void details(int node) {
        const Scene& sc = preview.scene;
        const OutNode& n = sc.nodes[node];
        const Node& g = nodes[node];
        char buf[160];
        heading("TRANSFORM");
        V3 t, sc3;
        Quat q;
        V3 world_pos = node_position(node);
        std::snprintf(buf, sizeof buf, "%.2f  %.2f  %.2f", world_pos.x, world_pos.y, world_pos.z);
        label_value("Position", buf);
        if (decompose(g.world, t, q, sc3)) {
            std::snprintf(buf, sizeof buf, "%.3g  %.3g  %.3g", sc3.x, sc3.y, sc3.z);
            label_value("Scale", buf);
        }
        if (n.light >= 0 && n.light < (int)sc.lights.size()) {
            const OutLight& L = sc.lights[n.light];
            heading("LIGHT");
            static const char* types[] = {"point", "spot", "directional (sun)"};
            label_value("Type", types[std::clamp(L.type, 0, 2)]);
            std::snprintf(buf, sizeof buf, "%.2f  %.2f  %.2f", L.color[0], L.color[1], L.color[2]);
            label_value("Colour", buf);
            std::snprintf(buf, sizeof buf, "%.2f", L.intensity);
            label_value("Intensity", buf);
            std::snprintf(buf, sizeof buf, "%.1f m", L.range);
            label_value("Range", buf);
            if (L.type == 1) {
                std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0", L.outer * 2 * 180 / kPi);
                label_value("Cone", buf);
            }
        }
        if (n.mesh >= 0 && n.mesh < (int)meshes.size()) {
            const Mesh& m = meshes[n.mesh];
            heading("MESH");
            label_value("Name", m.name);
            label_value("Triangles", with_commas(m.triangles));
            if (m.buffers) label_value("Vertices", with_commas(m.buffers->vertices));
            size_t copies = 0;
            for (auto& other : sc.nodes) copies += other.mesh == n.mesh;
            if (copies > 1) label_value("Copies", with_commas(copies) + " share this mesh");
            label_value("Materials", with_commas(m.parts.size()));
        }
        if (!n.extras.o.empty()) {
            heading("RIPPED PROPERTIES");
            for (auto& [k, v] : n.extras.o) {
                std::string value;
                if (v.t == Json::Str) value = v.s;
                else v.dump(value);
                label_value(k.c_str(), value);
            }
        }
    }

    void object_inspector() {
        const Scene& sc = preview.scene;
        const OutNode& n = sc.nodes[selected];
        const Node& g = nodes[selected];
        std::string path;
        for (int p = g.parent; p >= 0; p = nodes[p].parent) path = sc.nodes[p].name + (path.empty() ? "" : " / " + path);
        int light = light_of(selected);
        static const char* const light_names[] = {"Point light", "Spot light", "Sun", "Area light"};
        std::string kind = light >= 0 ? light_names[light_kind(sc.lights[sc.nodes[light].light])] : kCategoryNames[category[selected]];
        if (!path.empty()) kind += "  \xC2\xB7  " + path;
        int pressed = inspector_header(n.name.empty() ? "(unnamed)" : n.name, kind, category_colour(selected), false, false);
        if (pressed == 1) frame_selection();
        if (pressed == 2) {
            selected = -1;
            return;
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        segmented("##tabs", inspector_tab, {"Studio settings", "Details"});
        ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
        if (inspector_tab == 0) {
            int light = light_of(selected);
            if (light >= 0 && light_replaced(sc.nodes[light])) {
                int found = -1;
                for (size_t k = 0; k < edits->user_lights.size(); ++k)
                    if (edits->user_lights[k].source == sc.nodes[light].key) found = (int)k;
                if (found >= 0) {
                    note("Its light is editable in Lights.");
                    if (ImGui::Button(("Edit " + item_name({kLightItem, found})).c_str())) {
                        select_item({kLightItem, found});
                        reveal_item(item);
                        ImGui::EndChild();
                        return;
                    }
                } else {
                    note("Its light was deleted (Ctrl+Z brings it back).");
                }
            } else if (light >= 0) {
                light_settings(light);
            }
            if (n.light < 0 && (n.mesh >= 0 || light < 0)) object_editor(selected);
            materials_section(n);
        } else {
            details(selected);
        }
        ImGui::EndChild();
    }

    // ---- placed and drawn things -----------------------------------------------------------

    void marker_settings() {
        Marker* m = marker_of(item);
        if (!m) return;
        bool spawn = item.kind == kSpawnItem, changed = false;
        note(spawn ? "Exported as the empty Studio reads as the spawn point (\"spawn\")."
                   : "Exported as a Studio TravelPoint: a fast-travel bus stop.");
        if (!spawn && section("Bus stop", false)) {
            changed |= text_row("Name", m->name, "shown in game");
            changed |= switch_row("Shelter", m->shelter);
            ImGui::SetItemTooltip("Draw the bus shelter at the stop");
        }
        if (section("Placement", false)) {
            changed |= position_row("Position", m->position);
            changed |= slider_row("Facing", m->yaw, -180, 180, "%.0f\xC2\xB0");
            if (ImGui::Button("Face where the camera looks")) m->yaw = yaw * 180.0 / kPi, changed = true;
        }
        if (changed) touch();
    }

    void user_light_settings() {
        UserLight& l = edits->user_lights[item.index];
        bool changed = false;
        if (!l.source.empty()) {
            UserLight original;
            bool known = map_light_values(l.source, *edits, original);
            note(known ? (same_light(l, original) ? "One of the map's lights, as it was in the map." : "One of the map's lights, changed.")
                       : "One of the map's lights (not in this load of it).");
            if (known && !same_light(l, original)) {
                if (ImGui::Button("Reset to the map's light")) l = original, changed = true;
                ImGui::SetItemTooltip("Back to the values the map had");
            }
        }
        if (section("Light", false)) {
            changed |= text_row("Name", l.name);
            int type = l.area ? 2 : l.spot ? 1 : 0;
            if (combo_row("Type", type, {"Point light", "Spot light", "Area light"}, "Area: a glowing rectangle (Studio area light)"))
                l.spot = type == 1, l.area = type == 2, changed = true;
            row_label("Colour", control_width());
            changed |= ImGui::ColorEdit3("##colour", l.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
            changed |= open_row("Intensity", l.intensity, 0.1, 1000000, "%.0f cd");
            changed |= open_row("Range", l.range, 0.5, 1000, "%.1f m");
            ImGui::SetItemTooltip("How far its light reaches (Studio's light range)");
        }
        if (l.area && section("Area", false)) {
            changed |= slider_row("Width", l.width, 0.1, 20, "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= slider_row("Height", l.height, 0.1, 20, "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= slider_row("Turn", l.yaw, -180, 180, "%.0f\xC2\xB0");
            changed |= slider_row("Tilt", l.pitch, -90, 90, "%.0f\xC2\xB0");
            if (ImGui::Button("Face where the camera looks")) l.yaw = yaw * 180 / kPi, l.pitch = pitch * 180 / kPi, changed = true;
        }
        if (l.spot && section("Spot", false)) {
            changed |= slider_row("Cone", l.cone, 1, 179, "%.0f\xC2\xB0");
            changed |= slider_row("Turn", l.yaw, -180, 180, "%.0f\xC2\xB0");
            changed |= slider_row("Tilt", l.pitch, -90, 90, "%.0f\xC2\xB0");
            if (ImGui::Button("Aim where the camera looks")) l.yaw = yaw * 180 / kPi, l.pitch = pitch * 180 / kPi, changed = true;
        }
        if (section("Time of day", l.times != kAllLightTimes)) changed |= time_chips(l.times);
        if (section("Placement", false)) changed |= position_row("Position", l.position);
        if (changed) touch();
    }

    void audio_settings() {
        AudioVolume& a = edits->audio_volumes[item.index];
        bool changed = false;
        if (section("Sound", false)) {
            changed |= text_row("Name", a.name);
            std::vector<std::string> presets(kAudioPresetLabels, kAudioPresetLabels + kAudioPresets);
            int preset = a.preset;
            if (combo_row("Ambience", preset, presets, "What plays inside the box")) {
                a.preset = preset;
                if (preset == 3 && a.region_tag.empty()) a.region_tag = kDefaultAudioRegion;
                changed = true;
            }
            if (a.preset == 3) changed |= region_row(a.region_tag);
            if (a.preset == 2) changed |= text_row("Behavior", a.behavior, "BehaviorAsset path");
        }
        if (section("Box", false)) {
            const char* names[] = {"Width", "Height", "Length"};
            double* halves[] = {&a.half.x, &a.half.y, &a.half.z};
            for (int i = 0; i < 3; ++i) {
                double full = *halves[i] * 2;
                if (slider_row(names[i], full, 0.5, 400, "%.1f m", ImGuiSliderFlags_Logarithmic)) *halves[i] = full * 0.5, changed = true;
            }
            changed |= slider_row("Turn", a.yaw, -180, 180, "%.0f\xC2\xB0");
            ImGui::SetItemTooltip("Studio turns audio volumes about the vertical only");
            changed |= position_row("Centre", a.center);
            if (ImGui::Button("Face where the camera looks")) a.yaw = yaw * 180.0 / kPi, changed = true;
        }
        if (section("Mixing", false, false)) {
            changed |= slider_row("Activation distance", a.activation_distance, 0, 200, "%.0f m");
            changed |= slider_row("Indooriness", a.indooriness, 0, 1, "%.2f");
            changed |= slider_row("Density", a.density, 0, 1, "%.2f");
            changed |= int_row("Priority", a.priority, 0, 10);
            changed |= switch_row("Additive", a.additive);
            changed |= switch_row("Allow in child regions", a.child_regions);
            changed |= text_row("Density group", a.density_group);
        }
        if (changed) touch();
    }

    void spline_settings() {
        UserSpline& s = edits->splines[item.index];
        bool changed = false;
        if (section("Curve", false)) {
            changed |= text_row("Name", s.name);
            int type = s.npc ? 1 : 0;
            if (combo_row("Type", type, {"Grind curve", "NPC route"},
                          "A grind curve is an invisible rail to grind along; an NPC route is a path people or cars follow"))
                s.npc = type == 1, changed = true;
            changed |= switch_row("Closed loop", s.closed);
            char buf[96];
            std::snprintf(buf, sizeof buf, "%zu points  \xC2\xB7  %.1f m", s.points.size(), polyline_length(spline_samples(s)));
            note(buf);
        }
        if (!s.npc) {
            if (section("Grind", !default_grind(s.grind))) {
                changed |= switch_row("Grindable", s.grind.enabled);
                ImGui::SetItemTooltip("Off: kept here but left out of the export");
                double mm = s.grind.radius * 1000;
                if (slider_row("Rail radius", mm, 5, 250, "%.0f mm")) s.grind.radius = mm / 1000, changed = true;
                std::vector<std::string> names(kGrindSurfaceLabels, kGrindSurfaceLabels + kGrindSurfaceCount);
                int surface = s.grind.surface;
                if (combo_row("Surface", surface, names, "The sound and feel of the grind")) s.grind.surface = surface, changed = true;
            }
        } else if (section("NPC route", false)) {
            int kind = (int)s.route.kind;
            if (combo_row("Used by", kind, {"Pedestrians", "Vehicles", "Buses"})) s.route.kind = (NpcKind)kind, changed = true;
            changed |= slider_row("Width", s.route.width, 0.5, 20, "%.1f m");
            changed |= slider_row("Spacing", s.route.spacing, 0.5, 50, "%.1f m");
            changed |= slider_row("Weight", s.route.weight, 0, 10, "%.2f");
            ImGui::SetItemTooltip("How likely NPCs pick this route over others");
            changed |= int_row("Speed", s.route.speed, 1, 9);
            changed |= switch_row("Both directions", s.route.bidirectional);
            changed |= switch_row("Has stairs", s.route.stairs);
        }
        if (section("Points", false)) {
            bool drawing = placing == kAddPoints;
            if (drawing) {
                if (accent_button("Done", ImVec2(0, 0))) {
                    finish_drawing();
                    if (changed) touch();
                    return;  // the curve may be gone
                }
                ImGui::SetItemTooltip("Stop adding points (Enter)");
            } else {
                if (ImGui::Button("Add points\xE2\x80\xA6")) placing = kAddPoints, insert_after = -1;
                ImGui::SetItemTooltip("Click in the view to add points at the end");
                ImGui::SameLine();
                ImGui::BeginDisabled(point < 0);
                if (ImGui::Button("Insert after")) placing = kAddPoints, insert_after = point;
                ImGui::SetItemTooltip("Click in the view to add points after the selected one");
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(point < 0);
            if (ImGui::Button("Delete point")) delete_point();
            ImGui::EndDisabled();
            if (ImGui::Button("Reverse")) {
                std::reverse(s.points.begin(), s.points.end());
                if (point >= 0) point = (int)s.points.size() - 1 - point;
                changed = true;
            }
            ImGui::SetItemTooltip("Flip its direction (NPC one-way routes follow it)");
            slider_row("New points lift", draw_lift, 0, 3, "%.2f m");
            ImGui::SetItemTooltip("Clicked points go this far above the surface (rails: the top of the rail)");
            float line = ImGui::GetTextLineHeightWithSpacing();
            float list_h = std::min(S(200), (float)std::max<size_t>(1, s.points.size()) * line + S(10));
            ImGui::BeginChild("points", ImVec2(0, list_h), ImGuiChildFlags_Borders);
            if (s.points.empty()) ImGui::TextColored(col::muted, "No points yet: click in the view.");
            for (size_t k = 0; k < s.points.size(); ++k) {
                char label[128];
                std::snprintf(label, sizeof label, "%3zu    %8.2f  %8.2f  %8.2f", k + 1, s.points[k].x, s.points[k].y, s.points[k].z);
                if (ImGui::Selectable(label, point == (int)k, ImGuiSelectableFlags_AllowDoubleClick)) {
                    point = (int)k;
                    if (ImGui::IsMouseDoubleClicked(0)) frame_selection();
                }
            }
            ImGui::EndChild();
            if (point >= 0 && point < (int)s.points.size()) changed |= position_row("Point", s.points[point]);
        }
        if (changed) touch();
    }

    void curve_settings() {
        const OutCurve& c = *ripped_curve(item.index);
        char buf[128];
        std::snprintf(buf, sizeof buf, "%zu points  \xC2\xB7  %.1f m  \xC2\xB7  %s", c.points.size(), polyline_length(curve_samples(c, 6)),
                      c.bezier ? "Bezier" : "poly line");
        note(buf);
        if (c.key.empty()) {
            note("This spline has no stable id, so its settings cannot be kept.", col::warn);
            return;
        }
        GrindSettings g = ripped_grind(c);
        bool changed = false;
        if (section("Grind (ReSkate Studio)", !default_grind(g))) {
            changed |= switch_row("Export as a grind curve", g.enabled);
            ImGui::SetItemTooltip("Off: left out of the mod build and the .obj curves");
            ImGui::BeginDisabled(!g.enabled);
            double mm = g.radius * 1000;
            if (slider_row("Rail radius", mm, 5, 250, "%.0f mm")) g.radius = mm / 1000, changed = true;
            std::vector<std::string> names(kGrindSurfaceLabels, kGrindSurfaceLabels + kGrindSurfaceCount);
            changed |= combo_row("Surface", g.surface, names, "The sound and feel of the grind");
            ImGui::EndDisabled();
        }
        if (changed) set_ripped_grind(c, g);
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ImGui::Button("Make an editable copy")) {
            copy_curves();
            return;
        }
        ImGui::SetItemTooltip("Copies it as a grind curve you can edit point by point, and switches this one off");
    }

    void item_inspector() {
        bool movable = item.kind != kCurveItem && item.kind != kSplineItem;
        bool deletable = item.kind != kCurveItem;
        int pressed = inspector_header(item_name(item), item_kind_text(item), item_colour(item), movable, deletable);
        if (pressed == 1) frame_selection();
        if (pressed == 2) {
            select_item({});
            return;
        }
        if (pressed == 3) placing = placing == kPlaceMove ? kNotPlacing : kPlaceMove;
        if (pressed == 4) {
            delete_selected();
            return;
        }
        ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
        switch (item.kind) {
        case kSpawnItem:
        case kBusItem: marker_settings(); break;
        case kLightItem: user_light_settings(); break;
        case kAudioItem: audio_settings(); break;
        case kSplineItem: spline_settings(); break;
        case kCurveItem: curve_settings(); break;
        }
        ImGui::EndChild();
    }

    void scene_summary() {
        const Scene& sc = preview.scene;
        char buf[128];
        ImGui::PushFont(fonts.semibold);
        ImGui::TextUnformatted(preview.name.c_str());
        ImGui::PopFont();
        note("Nothing selected: click an object in the view or the list.");
        ImGui::Separator();
        ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
        if (edits && section("Add to the map", false)) {
            float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            struct Add {
                const char* label;
                const char* tip;
                int what;
            };
            const Add adds[] = {
                {edits->spawn ? "Move the spawn" : "Player spawn", "Where the skater starts", 1},
                {"Bus stop", "A fast-travel point", 2},
                {"Point light", "A light shining all around", 3},
                {"Spot light", "A light shining in a cone", 4},
                {"Area light", "A glowing rectangle", 8},
                {"Audio volume", "A box of ambience sound", 5},
                {"Grind curve", "An invisible rail you draw point by point", 6},
                {"NPC route", "A path pedestrians or cars follow", 7},
            };
            for (int i = 0; i < IM_ARRAYSIZE(adds); ++i) {
                if (i % 2) ImGui::SameLine();
                if (ImGui::Button(adds[i].label, ImVec2(bw, 0))) {
                    switch (adds[i].what) {
                    case 1:
                        if (edits->spawn) select_item({kSpawnItem}), placing = kPlaceMove;
                        else placing = kPlaceSpawn;
                        break;
                    case 2: placing = kPlaceBus; break;
                    case 3: placing = kPlaceLight; break;
                    case 4: placing = kPlaceSpot; break;
                    case 5: placing = kPlaceAudio; break;
                    case 6: start_curve(false); break;
                    case 7: start_curve(true); break;
                    case 8: placing = kPlaceArea; break;
                    }
                }
                ImGui::SetItemTooltip("%s", adds[i].tip);
            }
        }
        if (section("Scene", false)) {
            size_t stored = 0;
            for (auto& m : meshes) stored += m.triangles;
            label_value("Objects", with_commas(sc.nodes.size()));
            label_value("Meshes", with_commas(sc.meshes.size()));
            label_value("Materials", with_commas(sc.materials.size()));
            label_value("Textures", with_commas(sc.images.size()));
            label_value("Triangles", with_commas(stored) + " stored");
            label_value("", with_commas(drawn_triangles) + " drawn");
            label_value("Splines", std::to_string(sc.curves.size()) + " map, " + std::to_string(sc.auto_curves.size()) + " auto");
            V3 ext = scene_hi - scene_lo;
            std::snprintf(buf, sizeof buf, "%.0f x %.0f x %.0f m", ext.x, ext.y, ext.z);
            label_value("Size", buf);
            std::snprintf(buf, sizeof buf, "%.1f s", preview.seconds);
            label_value("Loaded in", buf);
        }
        if (edits && section("Edits", edits->count() > 0)) {
            size_t changed = edits->count();
            for (auto& l : edits->user_lights) changed -= unchanged_import(*edits, l);
            label_value("Changed", with_commas(changed) + (changed == 1 ? " thing" : " things"));
            label_value("Spawn", edits->spawn ? "placed" : "not placed");
            size_t routes = 0;
            for (auto& s : edits->splines) routes += s.npc;
            label_value("Placed", with_commas(edits->bus_stops.size()) + " bus stops, " + with_commas(edits->user_lights.size()) + " lights, " +
                                      with_commas(edits->audio_volumes.size()) + " audio volumes");
            label_value("Drawn", with_commas(edits->splines.size() - routes) + " grind curves, " + with_commas(routes) + " NPC routes");
            note("Saved beside the map; every export applies them.");
        }
        ImGui::EndChild();
    }

    void properties(ImVec2 size) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
        ImGui::BeginChild("properties", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (item.kind != kNoItem && !item_valid(item)) item = {}, point = -1;
        if (selected >= (int)nodes.size()) selected = -1;
        for (auto it = picked.begin(); it != picked.end();) {  // drop what no longer exists
            bool ok = it->first == kNodeSel ? it->second >= 0 && it->second < (int)nodes.size() : item_valid({it->first, it->second});
            if (ok) ++it;
            else it = picked.erase(it), ++sel_version;
        }
        if (picked.size() > 1) multi_inspector();
        else if (item.kind != kNoItem) item_inspector();
        else if (selected >= 0) object_inspector();
        else scene_summary();
        ImGui::EndChild();
    }

    // The thing under a point of the view: something placed or drawn, a map light, else the
    // object the ray meets (whose surface point comes back in `hit`).
    Pick thing_at(ImVec2 mouse, ImVec2 origin, Hit& hit) {
        int node = -1;
        Item it = item_at(mouse, origin, node);
        hit = pick((int)(mouse.x - origin.x), (int)(mouse.y - origin.y));
        if (it.kind != kNoItem) return {it.kind, it.index};
        if (node >= 0) return {kNodeSel, node};
        if (hit.node >= 0) return {kNodeSel, hit.node};
        return {-1, -1};
    }

    void viewport(ImVec2 size, const SceneViewStatus& status) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("view", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        int w = std::max(16, (int)avail.x), h = std::max(16, (int)avail.y);
        ensure_targets(w, h);
        ImGuiIO& io = ImGui::GetIO();

        // Input: an invisible button over the whole view owns the mouse while a button is held.
        ImGui::InvisibleButton("##viewport", ImVec2((float)w, (float)h),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        double dt = std::min(0.1, (double)io.DeltaTime);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) right_flew = false;
        if (active && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // Fly: look with the mouse, move with WASD/QE, Shift for speed, wheel sets the speed.
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            yaw += io.MouseDelta.x * 0.0035;
            pitch = std::clamp(pitch - io.MouseDelta.y * 0.0035, -1.55, 1.55);
            V3 move;
            if (ImGui::IsKeyDown(ImGuiKey_W)) move = move + forward();
            if (ImGui::IsKeyDown(ImGuiKey_S)) move = move - forward();
            if (ImGui::IsKeyDown(ImGuiKey_D)) move = move + right();
            if (ImGui::IsKeyDown(ImGuiKey_A)) move = move - right();
            if (ImGui::IsKeyDown(ImGuiKey_E)) move = move + V3{0, 1, 0};
            if (ImGui::IsKeyDown(ImGuiKey_Q)) move = move - V3{0, 1, 0};
            double boost = io.KeyShift ? 4 : 1;
            if (length(move) > 0) cam = cam + normalize(move) * (speed * boost * dt), right_flew = true;
            if (io.MouseWheel != 0) speed = std::clamp(speed * std::pow(1.25, io.MouseWheel), 0.5, 2000.0), right_flew = true;
        } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            double k = orbit * std::tan(fov * 0.5) * 2 / std::max(1, h);
            cam = cam - right() * (io.MouseDelta.x * k) + up() * (io.MouseDelta.y * k);
        } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyAlt) {
            V3 pivot = cam + forward() * orbit;
            yaw += io.MouseDelta.x * 0.005;
            pitch = std::clamp(pitch - io.MouseDelta.y * 0.005, -1.55, 1.55);
            cam = pivot - forward() * orbit;
        } else if (hovered && io.MouseWheel != 0) {
            double step = std::max(orbit * 0.15, 0.5) * io.MouseWheel;
            cam = cam + forward() * step;
            orbit = std::max(0.5, orbit - step);
        }

        // The move gizmo, at the middle of what is selected: drag an arrow to slide along that
        // axis, the centre to slide over the surfaces under the mouse. Everything selected that
        // can move goes with it (map splines by an offset).
        V3 pivot;
        bool has_pivot = !placing && selection_pivot(pivot);
        int handle = has_pivot && hovered && drag_axis < 0 ? gizmo_handle(pivot, origin, io.MousePos) : -1;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
            press_on_gizmo = handle >= 0;
            if (handle >= 0) {
                drag_axis = handle;
                drag_from = pivot;
                drag_mouse = io.MousePos;
                drag_dir = ImVec2(0, 0);
                drag_targets = move_targets();
                drag_starts.clear();
                for (V3* p : drag_targets) drag_starts.push_back(*p);
                drag_curves = false;
                for (auto& [k, i] : picked) drag_curves = drag_curves || k == kCurveItem;
                // One marker, light, box or point lands on the surface; groups and whole splines
                // follow the surface by how far the point under the mouse moves.
                drag_pivot = pivot_thing();
                drag_snap = drag_pivot.kind != kNoItem;
                drag_has_hit = false;
                if (handle == 0 && !drag_snap) {
                    Hit h0 = pick((int)(io.MousePos.x - origin.x), (int)(io.MousePos.y - origin.y));
                    if (h0.surface) drag_hit_from = h0.point, drag_has_hit = true;
                }
                if (handle > 0) {
                    double len = gizmo_length(pivot);
                    ImVec2 a, b;
                    if (project(pivot, origin, a) && project(pivot + axis_of(handle) * len, origin, b))
                        drag_dir = ImVec2((float)((b.x - a.x) / len), (float)((b.y - a.y) / len));
                }
            }
        }
        if (drag_axis >= 0) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || drag_targets.empty()) {
                drag_axis = -1;
                drag_targets.clear();
                if (drag_curves) cleanup_ripped(), curve_lines_dirty = true;
            } else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
                V3 delta;
                bool ok = false;
                if (drag_axis == 0) {
                    Hit hit = pick((int)(io.MousePos.x - origin.x), (int)(io.MousePos.y - origin.y));
                    if (hit.surface) {
                        if (drag_snap) delta = hit.point + V3{0, surface_offset(drag_pivot), 0} - drag_from, ok = true;
                        else if (drag_has_hit) delta = hit.point - drag_hit_from, ok = true;
                        else drag_hit_from = hit.point, drag_has_hit = true;  // began over the sky: follow from here
                    }
                } else {
                    float l2 = drag_dir.x * drag_dir.x + drag_dir.y * drag_dir.y;
                    if (l2 > 1e-4f) {
                        double t = ((io.MousePos.x - drag_mouse.x) * drag_dir.x + (io.MousePos.y - drag_mouse.y) * drag_dir.y) / l2;
                        delta = axis_of(drag_axis) * t, ok = true;
                    }
                }
                if (ok) {
                    for (size_t k = 0; k < drag_targets.size(); ++k) *drag_targets[k] = drag_starts[k] + delta;
                    if (drag_curves) curve_lines_dirty = true;
                    touch();
                }
            }
        }
        if (handle == 0 || drag_axis == 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else if (handle > 0 || drag_axis > 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        bool clicked = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt && !press_on_gizmo &&
                       io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < S(5) * S(5);
        bool right_clicked = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !right_flew &&
                             io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < S(5) * S(5);
        if (placing && hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        bool typing = io.WantTextInput;
        bool finish = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (placing && !typing && finish) {
            if (placing == kAddPoints) finish_drawing();
            else placing = kNotPlacing;
        } else if (!placing && hovered && !typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pick_many({});
        }
        if (placing == kAddPoints && !typing && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) remove_drawn_point();
        if (!placing && !typing && (hovered || active)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                if (picked.size() == 1 && item.kind == kSplineItem && point >= 0) delete_point();
                else delete_selected();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selected();
        }
        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (!placing && !typing && !any_popup && io.KeyCtrl && !io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_selection(false);
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) copy_selection(true);
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {  // over the view: where the mouse points
                Hit hit;
                if (hovered) hit = pick((int)(io.MousePos.x - origin.x), (int)(io.MousePos.y - origin.y));
                paste(hovered && hit.surface, hit.point);
            }
        }
        if (clicked && placing && edits) {
            Hit hit = pick((int)(io.MousePos.x - origin.x), (int)(io.MousePos.y - origin.y));
            if (hit.surface) place_at(hit.point);
        } else if (clicked) {
            int handle_point = picked.size() == 1 && !io.KeyCtrl ? spline_point_at(io.MousePos, origin) : -1;
            if (handle_point >= 0) {
                point = handle_point;
            } else {
                Hit hit;
                Pick p = thing_at(io.MousePos, origin, hit);
                if (io.KeyCtrl) {
                    toggle_pick(p);
                } else {
                    select_one(p, true);
                    if (p.first > kNoItem && p.first != kNodeSel) reveal_item({p.first, p.second});
                    if (p.first == kNodeSel && preview.scene.nodes[p.second].light >= 0) cat_open[kLights] = true;
                }
            }
        }
        if (right_clicked) {
            if (placing == kAddPoints) {
                finish_drawing();
            } else if (placing) {
                placing = kNotPlacing;
            } else {  // the context menu, for what is under the mouse (selected first if it was not)
                Hit hit;
                Pick p = thing_at(io.MousePos, origin, hit);
                if (p.first >= 0 && !picked.count(p)) select_one(p, true);
                ctx_reset();
                ctx_target = p;
                ctx_has_point = hit.surface;
                ctx_point = hit.point;
                open_context = true;
            }
        }
        if ((hovered || active) && !typing && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();

        if (test_marker && edits && edits->spawn) {  // SPOTBUILDER_SNAPSHOT_MARKER=1: select and frame the spawn
            test_marker = false;
            select_item({kSpawnItem});
            frame_selection();
        }
        if (test_item.kind != kNoItem && item_valid(test_item)) {  // SPOTBUILDER_SNAPSHOT_ITEM=<kind>,<index>
            select_item(test_item);
            reveal_item(test_item);
            test_item = {};
            frame_selection();
        }
        if (test_light >= 0 && test_light < (int)light_nodes.size()) {
            select(nodes[light_nodes[test_light]].parent >= 0 ? nodes[light_nodes[test_light]].parent : light_nodes[test_light], true);
            test_light = -1;
            V3 p = node_position(light_nodes.empty() ? 0 : light_nodes[0]);
            frame_bounds(p - V3{6, 6, 6}, p + V3{6, 6, 6}, false);
        }
        if (test_select_all >= 0) {  // SPOTBUILDER_SNAPSHOT_SELECTALL=<category>
            bool had = !picked.empty();  // an item picked first: expand like the menu does, the view stays
            std::vector<Pick> all = test_select_all == 99 ? curve_things(2) : test_select_all == 98 ? item_things(kLightItem)
                                                                                                    : category_things(test_select_all);
            expand_selection(all);
            test_select_all = -1;
            if (!had) frame_selection();
        }
        if (test_pick) {  // SPOTBUILDER_SNAPSHOT_PICK=<x>,<y> (fractions of the view): see gui.cpp
            test_pick = false;
            select(pick((int)(test_pick_x * w), (int)(test_pick_y * h)).node, true);
        }
        if (test_context) {  // SPOTBUILDER_SNAPSHOT_CONTEXT=<x>,<y>: right-click there
            test_context = false;
            Hit hit;
            ImVec2 m(origin.x + test_context_x * w, origin.y + test_context_y * h);
            Pick p = thing_at(m, origin, hit);
            if (p.first >= 0 && !picked.count(p)) select_one(p, true);
            ctx_reset();
            ctx_target = p;
            ctx_has_point = hit.surface;
            ctx_point = hit.point;
            open_context = true;
            context_pos = m;
        }
        render();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImage((ImTextureID)(uintptr_t)color_srv, origin, ImVec2(origin.x + w, origin.y + h));

        // Overlays: stats and controls
        ImGui::PushFont(fonts.caption);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s triangles  \xC2\xB7  %s draws  \xC2\xB7  %.0f fps  \xC2\xB7  speed %.1f m/s",
                      with_commas(drawn_triangles).c_str(), with_commas(draw_calls).c_str(), io.Framerate, speed);
        auto label = [&](ImVec2 at, const char* text, float alpha) {  // text on a dark pill so it reads on any scene
            ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddRectFilled(ImVec2(at.x - S(8), at.y - S(4)), ImVec2(at.x + ts.x + S(8), at.y + ts.y + S(4)),
                              ImGui::GetColorU32(rgb(12, 14, 19, 0.72f)), S(6));
            dl->AddText(at, ImGui::GetColorU32(rgb(225, 230, 240, alpha)), text);
        };
        label(ImVec2(origin.x + S(16), origin.y + h - S(28)), buf, 0.9f);
        const char* help = "RMB fly / menu  \xC2\xB7  Wheel zoom  \xC2\xB7  MMB pan  \xC2\xB7  Alt+LMB orbit  \xC2\xB7  Ctrl+click add  \xC2\xB7  F frame";
        ImVec2 hs = ImGui::CalcTextSize(help);
        if (hs.x + ImGui::CalcTextSize(buf).x + S(64) < w) label(ImVec2(origin.x + w - hs.x - S(16), origin.y + h - S(28)), help, 0.65f);
        ImGui::PopFont();
        // The selected spline's points, and the move gizmo
        if (picked.size() == 1 && item.kind == kSplineItem && item_valid(item)) {
            const auto& pts = edits->splines[item.index].points;
            for (size_t k = 0; k < pts.size(); ++k) {
                ImVec2 s;
                if (!project(pts[k], origin, s)) continue;
                bool sel = (int)k == point;
                float r = sel ? S(6) : S(4.5f);
                dl->AddCircleFilled(s, r, ImGui::GetColorU32(sel ? col::accent : rgb(255, 255, 255, 0.95f)), 16);
                dl->AddCircle(s, r, ImGui::GetColorU32(rgb(0, 0, 0, 0.7f)), 16, S(1.2f));
            }
        }
        if (!placing && selection_pivot(pivot)) draw_gizmo(dl, pivot, origin, drag_axis >= 0 ? drag_axis : handle);

        if (placing) {
            std::string text;
            switch (placing) {
            case kPlaceSpawn: text = "Click where the skater should start  \xC2\xB7  it faces the way the camera looks"; break;
            case kPlaceBus: text = "Click where the bus stop goes  \xC2\xB7  it faces the way the camera looks"; break;
            case kPlaceLight: text = "Click a surface: the light goes 2.5 m above it"; break;
            case kPlaceSpot: text = "Click a surface: the spot light goes 4 m above it, shining down"; break;
            case kPlaceArea: text = "Click a surface: the area light goes 3 m above it, facing down"; break;
            case kPlaceAudio: text = "Click the floor of the area the sound fills"; break;
            case kPlaceMove: text = "Click the new spot"; break;
            case kAddPoints:
                text = insert_after >= 0 ? "Click to insert points after point " + std::to_string(insert_after + 1)
                                         : std::string("Click along the rail or path to add points");
                text += "  \xC2\xB7  Backspace takes one back  \xC2\xB7  Enter or right-click finishes";
                break;
            }
            if (placing != kAddPoints) text += "  \xC2\xB7  Esc cancels";
            ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            ImVec2 a(origin.x + (w - ts.x) * 0.5f - S(16), origin.y + S(16)), b(a.x + ts.x + S(32), a.y + ts.y + S(16));
            dl->AddRectFilled(a, b, ImGui::GetColorU32(rgb(96, 142, 255, 0.92f)), S(8));
            dl->AddText(ImVec2(a.x + S(16), a.y + S(8)), ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), text.c_str());
        } else if (status.loading) {
            const char* text = "Reloading\xE2\x80\xA6";
            ImVec2 ts = ImGui::CalcTextSize(text);
            ImVec2 a(origin.x + (w - ts.x) * 0.5f - S(16), origin.y + S(16)), b(a.x + ts.x + S(32), a.y + ts.y + S(16));
            dl->AddRectFilled(a, b, ImGui::GetColorU32(rgb(30, 34, 44, 0.92f)), S(8));
            dl->AddText(ImVec2(a.x + S(16), a.y + S(8)), ImGui::GetColorU32(col::text), text);
        }
        ImGui::EndChild();
    }

    SceneAction draw(const SceneViewStatus& status, Job& job, Edits& map_edits) {
        SceneAction action = SceneAction::None;
        if (edits != &map_edits) {
            edits = &map_edits;
            bump();
            edits_changed = false;
        }
        update_replaced();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (!error.empty()) {
            ImGui::TextColored(col::bad, "The scene view cannot start: %s", error.c_str());
            return action;
        }
        if (!has_scene) {
            // Empty state: a big drop target
            ImGui::BeginChild("empty", avail, ImGuiChildFlags_Borders);
            ImVec2 region = ImGui::GetContentRegionAvail();
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float inset = S(24);
            dl->AddRect(ImVec2(origin.x + inset, origin.y + inset), ImVec2(origin.x + region.x - inset, origin.y + region.y - inset),
                        ImGui::GetColorU32(rgb(96, 142, 255, 0.35f)), S(14), 0, S(1.5f));
            float block_w = std::min(region.x - S(80), S(480));
            ImGui::SetCursorPos(ImVec2((region.x - block_w) * 0.5f, region.y * 0.36f));
            ImGui::BeginGroup();
            ImGui::PushFont(fonts.title);
            const char* title = status.loading ? "Loading the map\xE2\x80\xA6" : "Drop a Unity map here";
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + block_w);
            if (status.loading) {
                ImGui::TextColored(col::muted, "%s", status.step.c_str());
            } else {
                ImGui::TextColored(col::muted, "A Skater XL map bundle or any Unity scene bundle, or a folder of them. It opens here "
                                               "to look around and edit, then Export writes the .glb.");
                ImGui::Dummy(ImVec2(0, S(8)));
                ImGui::BeginDisabled(status.busy);
                if (accent_button("Open map\xE2\x80\xA6", ImVec2(S(150), S(38)))) action = SceneAction::OpenFile;
                ImGui::SameLine();
                if (ImGui::Button("Open folder\xE2\x80\xA6", ImVec2(S(140), S(38)))) action = SceneAction::OpenFolder;
                ImGui::EndDisabled();
            }
            if (!status.error.empty()) {
                ImGui::Dummy(ImVec2(0, S(6)));
                ImGui::TextColored(col::bad, "%s", status.error.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::EndChild();
            return action;
        }
        apply_live_lods(job);

        // Toolbar
        if (status.stale) {
            ImGui::BeginDisabled(status.busy);
            if (accent_button("Reload", ImVec2(0, 0))) action = SceneAction::Load;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Load the map again with the current options");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col::warn, "Options changed that need a reload");
        } else if (status.loading) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col::accent, "%s", status.step.empty() ? "Reloading\xE2\x80\xA6" : status.step.c_str());
        } else if (!status.error.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col::bad, "Reload failed: %s", status.error.c_str());
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("");
        }
        bool hidden_before = show_hidden;
        bool rails_before = show_rails;
        const char* labels[] = {"Grind rails", "Textures", "Wireframe", "Hidden objects"};
        bool* values[] = {&show_rails, &show_textures, &wireframe, &show_hidden};
        const char* tips[] = {"Show the collision rail ReSkate Studio builds under every grind curve (selected ones always show it)",
                              "Base colour textures", "Draw triangle edges", "Objects that are disabled in the map"};
        float chips_w = ImGui::CalcTextSize("Lighting: Afternoon").x + S(24) + S(6);
        for (const char* l : labels) chips_w += ImGui::CalcTextSize(l).x + S(24) + S(6);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(12), right_edge() - chips_w));
        lighting_menu();
        for (int i = 0; i < IM_ARRAYSIZE(labels); ++i) {
            ImGui::SameLine(0, S(6));
            chip(labels[i], *values[i], tips[i]);
        }
        if (rails_before != show_rails) curve_lines_dirty = true;
        if (hidden_before != show_hidden) {
            filter_dirty = true;
            if (selected >= 0 && !visible(selected)) selected = -1;
        }

        float gap = S(12);
        float body_h = ImGui::GetContentRegionAvail().y;
        if (left_w <= 0) left_w = std::clamp(avail.x * 0.22f, S(270), S(380));
        if (right_w <= 0) right_w = std::clamp(avail.x * 0.26f, S(300), S(440));
        float min_view = S(320);
        left_w = std::clamp(left_w, S(200), std::max(S(200), avail.x - right_w - min_view - gap * 2));
        right_w = std::clamp(right_w, S(250), std::max(S(250), avail.x - left_w - min_view - gap * 2));
        hierarchy(ImVec2(left_w, body_h), job);
        ImGui::SameLine(0, 0);
        splitter("##split_left", left_w, 1.f, gap, body_h);
        ImGui::SameLine(0, 0);
        viewport(ImVec2(std::max(S(80), avail.x - left_w - right_w - gap * 2), body_h), status);
        ImGui::SameLine(0, 0);
        splitter("##split_right", right_w, -1.f, gap, body_h);
        ImGui::SameLine(0, 0);
        properties(ImVec2(right_w, body_h));
        if (open_context) {
            if (context_pos.x >= 0) ImGui::SetNextWindowPos(context_pos);
            ImGui::OpenPopup("##scene_context");
            open_context = false;
        }
        if (ImGui::BeginPopup("##scene_context")) {
            context_menu();
            ImGui::EndPopup();
        }
        return action;
    }
};

SceneView::SceneView(ID3D11Device* device, ID3D11DeviceContext* context, const Fonts& fonts)
    : impl_(std::make_unique<Impl>(device, context, fonts)) {}
SceneView::~SceneView() = default;
void SceneView::set(Preview&& preview) { impl_->set(std::move(preview)); }
void SceneView::clear() { impl_->clear(); }
bool SceneView::loaded() const { return impl_->has_scene; }
SceneAction SceneView::draw(const SceneViewStatus& status, Job& job, Edits& edits) { return impl_->draw(status, job, edits); }
bool SceneView::take_edits_changed() {
    bool c = impl_->edits_changed;
    impl_->edits_changed = false;
    return c;
}
void SceneView::edits_restored() {
    impl_->bump();
    impl_->edits_changed = false;
    impl_->curve_lines_dirty = true;
    impl_->point = -1;
    impl_->drag_axis = -1;
    impl_->drag_targets.clear();
    if (impl_->placing == Impl::kAddPoints && !impl_->item_valid(impl_->item)) impl_->placing = Impl::kNotPlacing;
}
bool SceneView::import_map_lights(Edits& e) {
    Impl& v = *impl_;
    if (!v.has_scene) return false;
    std::set<std::string> have(e.dropped_lights.begin(), e.dropped_lights.end());
    for (auto& l : e.user_lights)
        if (!l.source.empty()) have.insert(l.source);
    size_t before = e.user_lights.size();
    const Scene& sc = v.preview.scene;
    for (int i : v.light_nodes) {
        const OutNode& n = sc.nodes[i];
        const OutLight& L = sc.lights[n.light];
        if (L.type == 2 || n.key.empty() || have.count(n.key)) continue;  // suns stay the map's (the game lights its own)
        auto le = e.lights.find(n.key);
        if (le != e.lights.end() && le->second.removed) continue;  // left out before: stays out
        e.user_lights.push_back(light_from_map(n, L, v.nodes[i].world, le != e.lights.end() ? &le->second : nullptr));
        have.insert(n.key);
    }
    if (e.user_lights.size() > 40) v.lights_open = false;
    return e.user_lights.size() > before;
}
void SceneView::drop_unchanged_imports(Edits& e) const {
    e.user_lights.erase(std::remove_if(e.user_lights.begin(), e.user_lights.end(),
                                       [&](const UserLight& l) { return impl_->unchanged_import(e, l); }),
                        e.user_lights.end());
}
size_t SceneView::unchanged_imports(const Edits& e) const {
    size_t n = 0;
    for (auto& l : e.user_lights) n += impl_->unchanged_import(e, l);
    return n;
}
std::string SceneView::take_message() {
    std::string m = std::move(impl_->message);
    impl_->message.clear();
    return m;
}
void SceneView::edits_replaced() {
    impl_->bump();
    impl_->edits_changed = false;
    impl_->item = {};
    impl_->point = -1;
    impl_->picked.clear();
    impl_->pivot_set.clear();
    ++impl_->sel_version;
    impl_->placing = 0;
}

} // namespace xl
