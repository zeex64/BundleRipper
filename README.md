# Spotbuilder

*(formerly BundleRipper)*

A map editor for bringing levels from other games into Skate. It opens whole levels from Unity
asset bundles (meshes, PBR materials, textures, terrain and terrain trees, colliders, decals,
grind splines and lights), lets you set them up in 3D (ReSkate Studio settings, spawn, bus stops,
lights, audio volumes, grind curves, NPC routes, a time-of-day lighting preview), and exports a
single binary glTF (`.glb`) for Blender or, with ReSkate Studio installed, builds a Skate mod
directly. Built for Skater XL mod maps (HDRP, URP and built-in shaders, Unity 2019-2022), and it
works on scene bundles from other Unity games too.

## Use

**Window:** double-click `Spotbuilder.exe`. It is a scene editor: open or drop a map (a bundle
file, or a folder of bundles) and it loads in 3D with the current options, the way the export
will build it. **Export...** (Ctrl+Enter) opens the Export window: pick a **.glb file** or a
**Skate mod** (below), each with its own options, and go. **Settings** pulls out how the map is
ripped (textures, objects, LODs, optimisation, splines, terrain plants), which applies to both.
The window remembers its settings (`%APPDATA%\Spotbuilder\settings.txt`, stored as
command-line arguments) and reopens the last map.

- **View:** hold the right mouse button with WASD/QE to fly (Shift faster, wheel sets the
  speed); wheel to zoom, middle mouse to pan, Alt + left mouse to orbit, F to frame. Toggles
  for textures, lighting, wireframe and hidden objects.
- **Object list:** Markers and Curves (what you added: spawn, bus stops, lights, audio volumes,
  grind curves, NPC routes; **+** adds one), then the map's Level (Unity's hierarchy), Terrain,
  Terrain plants (by type), Collision, Decals, Lights and Splines (each spline listed), each with
  an eye to show or hide it. Drag the gaps beside the list and the inspector to resize them
  (double-click resets).
- **Live LODs:** every LOD level is loaded, so the LOD settings and each plant type's own LOD
  (the dropdown on its row) switch instantly.
- **Inspector:** click anything in the view or the list. Objects get two tabs: *Studio
  settings* (collision, gameplay, impact audio, contact physics and the materials, in sections
  that fold; a blue dot marks one you changed) and *Details* (transform, mesh, ripped
  properties). With nothing selected it has buttons to add each kind of marker or curve.
- **Selecting several:** Ctrl+click adds or removes (in the view or the list), Shift+click
  selects a range of list rows, and right-click opens a menu for what is under the mouse:
  select all of its kind (decals, collision objects, map/auto splines, lights...), all with the
  same mesh or material, everything under it, or every copy of a plant; switch settings for the
  whole selection (include in export, collision, collision only, invisible materials, grind on,
  rail radius, surface); duplicate (Ctrl+D), delete, or add a marker or curve right where you
  clicked. The inspector edits a selection together: what you change goes to all of them and
  their other settings stay.
- **Moving things:** selected markers, lights, audio volumes, curves (whole, or one point) and
  the map's own splines show arrows: drag one to slide along that axis, or drag the centre to
  slide over the surfaces under the mouse. Several move together. The arrows sit on what you
  picked by hand: after "select all of a kind" (or keep only) they stay there, so you can line
  everything up from the one you were working on. Move... puts one on the next spot you click;
  Del deletes; Esc deselects.
- **Undo, copy and paste:** Ctrl+Z undoes any map edit (Ctrl+Y or Ctrl+Shift+Z redoes; also the
  Undo and Redo buttons). A drag, a slider or a typed name is one step. Ctrl+C copies the
  selection: placed and drawn things as themselves, a map object's, light's or spline's settings
  as settings. Ctrl+V pastes things as new copies (over the view: their lowest point where the
  mouse points; right-click has Paste here), or pastes copied settings onto the selected map
  objects, lights or splines. Ctrl+X cuts. The clipboard is text, so it pastes into another map
  too.
- **Grind rails:** the collision rail ReSkate Studio builds under a grind curve (an 8-sided prism
  of the rail radius hanging from the line) is drawn on selected curves, or on all of them with
  the Grind rails toggle.
- **Lighting:** the Lighting menu previews the map lit like Skate at morning, noon, afternoon,
  evening or night, using the game's own values (sun direction, colour and lux, exposure range,
  fog distance, from its environment assets), with sun shadows and every light (yours and the
  map's) that shines at that time, with Studio's falloff. Lights are sorted into screen tiles
  by depth each frame, so a pixel only adds the few lights that reach it (Shred Cavern's 590
  lights: 16 ms down to under 3 ms). Exposure is approximate; Flat and Editor are the plain views.
  The map's own point, spot and area lights load as editable lights in the Lights section, with
  the map's values (place, aim, colour, intensity, range, cone or size): move, edit, copy or
  delete them like added ones, and "Reset to the map's light" puts one back. Exports use them
  in place of the originals; the file beside the map only keeps the ones you changed or deleted.
  Suns stay the map's (the game lights its own). Right-click selects every light of a type.
  Intensity goes to 1,000,000 cd and range to 1 km (Ctrl+click a slider to type any value).
- **Sky:** the map's own skybox (a cubemap, panorama or six-sided material, or an HDRP HDRI sky)
  shows behind it in the lighting preview, and a .glb export writes it as `<map>_sky.png`. It is
  never put in a mod: ReSkate Studio cannot change Skate's sky, which comes with each time of day.
- **Curves:** draw a grind curve or NPC route by clicking points along it (Backspace takes one
  back, Enter finishes). Select a point by clicking its dot; Add points / Insert after / Delete
  point / Reverse are in the inspector. The map's own and auto splines can be switched off per
  spline, given Studio grind settings, or turned into an editable copy.
- **Bottom strip:** the Output tab shows what is running across the whole width (step, a
  progress bar with the newest message, time); click it for the steps, the result and the log.

**Map edits (ReSkate Studio settings):** the inspector edits are saved beside the map
(`<map>.spotbuilder.json`; edits saved under the old name, `<map>.bundleripper.json`, are still
read and move to the new name on the next save) and baked into every export, from the window or the command line
(`--edits <file>` uses another file, `--no-edits` ignores them). They become the glTF extras
that ReSkate Studio reads after the .glb goes through Blender (Studio's converter reads the raw
custom properties, so both the nested `sk8_object` / `sk8_material` dicts and the flat `sk8_*`
keys are written):

| Edit | Where | Exported as |
|---|---|---|
| Include in export (off) | object | the object and its children are left out |
| Collision: triangle mesh / convex parts / convex hull / none (render only) / water | object | `sk8_object.collision_mode` (0-4) + `sk8_collision_mode` |
| Collision only (not drawn) | object | its materials become invisible copies (`sk8_material.invisible`) |
| Surface / sound (one of Studio's 279 native collision materials) | object or material | `collision_material` (packed) + `sk8_collision_material_packed` |
| Round rail, hide from pause map | object | `sk8_object.round_rail`, `hide_from_pause_map` + `sk8_hide_from_pause_map` |
| Override gameplay (jump pad, wipeout, stairs...), impact audio, contact physics | object | `sk8_object.custom_*` fields + `sk8_surface_profile_json` |
| Collision only (invisible), transparency, shader type | material | `sk8_material.invisible`, `alpha`, `alpha_cutoff`, `domain` |
| Player spawn | Markers | an empty named `spawn` facing the chosen way |
| Bus stops | Markers | empties named `TravelPoint`, `TravelPoint.001`... with `bus_stop_name` / `bus_stop_shelter` |
| Lights you add (point / spot / area: colour, intensity, range, cone or size, aim, times of day) | Markers | KHR punctual lights with `sk8_light_range`, `sk8_light_tod`; an area light goes out as a wide spot with `xl_area_size` and the mod build makes it a Blender area light |
| The map's lights (made editable on load: everything an added light has) | Lights | added lights in place of the originals (unchanged ones export as they were) |
| Audio volumes (tunnel / drips / custom / native region, box size, turn, mixing) | Markers | empties scaled to the box with `sk8_audio_*` keys |
| Grind curves you draw (grindable, radius, surface) | Curves | mod build: curves with Studio's grind settings; .glb: `<name>_splines.obj` |
| NPC routes (pedestrians / vehicles / buses, width, spacing, weight, speed, one way, stairs) | Curves | mod build only: curves with `sk8_npc_*` settings |
| The map's splines: off, grind radius and surface, moved, editable copy | Splines | mod build: grind curves; .glb: left out of the .obj when off, moved in it |
| Collider-only objects (`_col`) | Collision | their XL_Collision material is invisible in Studio by default (collision, nothing drawn); the material's switch can turn it back on |

Place markers with the **+** on the Markers row: click a surface in the view; spawns, bus stops
and audio volumes face the way the camera looks. Ripped empties that Studio would misread (named
`spawn`, `TravelPoint...` or `..._prefab`) get a ` (ripped)` suffix.

**Build a Skate mod:** pick **Skate mod** in the Export window (or `--build-mod`). The map is
ripped into a staging folder (`%LOCALAPPDATA%\Spotbuilder\Builds\<name>`), Blender turns the
.glb into a .blend (the edits become the Skate Map add-on's settings, and the grind curves and
NPC routes become real curves), and ReSkate Studio's `reskate_cli compile-map` builds the mod
and, with **Install into Skate** on, puts it in Skate's `Mods` folder (Skate must be closed).
On the way, spot lights are cut to a quarter of Blender's power: Studio's lumens are read by the
game as lumens / pi candela for a spot, so a glTF spot would otherwise come out 4x too bright.
Blender, `reskate_cli.exe` and the game folder are found from Studio's settings
(`%LOCALAPPDATA%\ReSkateStudio\settings.json`), a running ReSkate Studio and the usual install
places; the Output card shows what was found, and the Skate mod options can point at them.
The Skate mod options also set the mod name, pause map (3D/2D), time of day, global
illumination, world streaming, Studio's mesh LODs and whether to keep `<map>.blend`.

**Drop:** drag a map file (e.g. `BerlinXL V2 by GyOm`) onto `Spotbuilder.exe` to rip it with
the defaults. It writes `<map>.glb` next to the map.

**Command line:**

```
Spotbuilder <map file or folder> [options]
Spotbuilder                        opens the window
Spotbuilder --gui [map] [options]  opens the window with these settings filled in
```

Any arguments other than `--gui` run the command line, which works in scripts and prints
progress to the console. It applies the map edits saved beside the map too, and
`--build-mod` builds the Skate mod instead of the .glb. `Spotbuilder --help` lists every
option. The GUI's controls, `--help`, and the saved settings all come from one option table
(`src/options.cpp`), so they always match.

A folder input loads every bundle in it together (cross-bundle references resolve).

## What ends up in the .glb

| Unity | glTF / Blender |
|---|---|
| GameObject hierarchy | flat: one root object per mesh/light with its world transform (`--keep-hierarchy` keeps the tree); inactive objects and LOD1+ skipped by default |
| MeshRenderer / SkinnedMeshRenderer | mesh, one primitive per submesh; shared meshes stay shared |
| Static-batched renderers | cut back out of the combined mesh and returned to local space |
| Unity built-in Cube/Plane/Quad/Sphere/Capsule/Cylinder | rebuilt (Plane/Quad vertex-exact) |
| HDRP/Lit, LayeredLit (layer 0), Unlit, Standard, URP | glTF PBR; mask map -> ORM texture, DXT5nm -> RGB normal map, tiling baked into UVs, tints baked into textures |
| HDRP planar / triplanar mapping | UVs baked from world position x `_TexWorldScale` (triplanar as a per-triangle box projection) |
| Custom Shader Graphs | textures picked by slot and texture name when slots have generated names; separate metallic/roughness/AO maps packed to ORM; alpha test from keywords, or from a cut-out alpha in the base texture |
| Graph tiling property (`<slot>_TILING`, `_TILING`, ...) | graph named triplanar/planar: world projection at that tiling; otherwise world triplanar when the tiling is below 1 or the mesh UVs are only a world-XZ projection, else UV x tiling |
| Vertex-colour layer graphs (`_TEXTURE_01`..`_05` + `_NORMAL_0N`) | one plain material per layer (`<name> L2`...); each area goes to its strongest layer (base, then vertex R, G, B, A), with triangles cut along the layer borders |
| HDRP/URP DecalProjector | real geometry: scene triangles clipped to the projector box with the decal's UVs, child `<name>_decal` |
| Dreamteck SplineComputer (grinds) | curves in `<map>_splines.obj` next to the .glb (File > Import > Wavefront in the same scene; they line up with the map). Linear -> poly curve, Bezier/Catmull-Rom/B-spline -> exact cubic NURBS with Bezier knots. `--spline-meshes` also puts them in the .glb as line meshes |
| Auto grind splines (always, unless `--no-autosplines`) | grind lines found on the geometry, in `<map>_autosplines.obj` (same curve format): top creases (ledge/box/square-rail edges, ridges) and the top line of round tubes up to 30 cm radius, on objects on Skater XL's Grindable (12) / Coping (16) layers or named rail, coping, ledge, curb, bench, pipe, hubba, manny... `--autospline-all` looks at every collidable object. On Modern Lines it reproduces 94.5% of the author's splines |
| Terrain trees | every painted tree is its prototype prefab (LOD0) at the terrain position, Y rotation and width/height scale; meshes shared between copies; trunk colliders follow the TerrainCollider's tree-collider switch; `--no-trees` leaves them out |
| Unity Terrain | heightmap grid mesh (`<name>_terrain`, up to `--terrain-resolution` squares per side), one material per TerrainLayer tiled by its tile size/offset; each area goes to its strongest (bilinearly sampled) splat layer, with triangles cut along the layer borders so they run smoothly instead of stepping along the grid; collides when there is a TerrainCollider |
| MeshCollider sharing the render mesh | custom property `sk8_collision_mode = triangle_mesh` (or `hull` if convex) on the render object |
| Other Mesh/Box/Sphere/Capsule colliders | default: separate `<name>_col` objects with the translucent `XL_Collision` material; with `--no-colliders`: the object's render mesh gets `sk8_collision_mode = triangle_mesh`, and so do visible child meshes inside the collider's bounds |
| Colliders on invisible objects (a `COLLIDER` child or sibling) | default: `<name>_col` objects; with `--no-colliders`: the visible meshes the collider overlaps take its collision (searched in the collider's parent group first, widening up the tree only if nothing there overlaps) |
| Light | KHR_lights_punctual (HDRP intensities: candela / lux) |

Blender shading stays flat: every texture is linked straight into the Principled BSDF (only
the Normal Map node and a Separate Color for the metal/roughness channels sit in between).
To get there the tool bakes colour tints into the textures and texture tiling into the UVs,
leaves vertex colours and the occlusion link out, and writes cut-out alpha as glTF BLEND
(so Blender links alpha directly instead of building clip nodes). Those cut-out materials
carry `sk8_material.alpha = 2` (mask) and `alpha_cutoff`, which ReSkate Studio honours, plus
`xl_alpha_mode`/`xl_alpha_cutoff`. In Blender they show the Blended render method.

## Mesh optimisation

On by default; every shared mesh is processed once:

1. **Lossless clean-up.** Vertices identical in everything written are merged, and zero-area
   and repeated triangles are dropped. Unity's extra UV sets (usually lightmap UVs) are dropped
   when no material samples them, and vertex colours are dropped unless `--vertex-colors` is on.
   Triangles and vertices are then reordered for the GPU vertex cache.
2. **Simplification within `--simplify` millimetres** (default 1), measured in world space for
   the largest placed copy. Open edges, material borders and UV/normal seams are locked, so
   nothing cracks or slides. meshoptimizer proposes each result. Every vertex it removed, and
   the centre of every triangle it replaced, must then lie within the limit of the new surface,
   with the normal within 3° and the UVs within one texel of a 1024 texture (after tiling).
   Spots that fail are pinned and the submesh is simplified again; a submesh that never passes
   stays as it was. Decals (2 mm above their surface) are only cleaned up, not simplified.

Checked against the unoptimised meshes, with a brute-force closest-point test on the worst
samples: the surface stays within about 1.3 mm everywhere and 1 mm at 99.9% of points.
Flat and over-tessellated geometry (terrain grids, bowls, sculpted rocks, jump lines) often
halves. Foliage cards and trees barely change, because every blade and leaf is an open card
already at its minimum. For foliage, the lever is the author's own lower LODs: `--tree-lod 1`
takes The Lost Loop's 11,000 grass clumps from 2,592 to 864 triangles each.

Custom properties on objects (glTF extras): `xl_layer`, `xl_tag` (Unity layer and tag
numbers), `xl_collider`, `xl_trigger`, `xl_decal`, `xl_spline*`, `xl_inactive`, and
`sk8_collision_mode` (`none` / `triangle_mesh` / `hull`), which ReSkate Studio reads.
Materials carry `xl_shader`, and `xl_uv_mapping` when HDRP used planar/triplanar mapping.

Axes: Unity is left-handed Y-up; the writer mirrors X (positions, normals, rotations and
winding), so the scene reads the right way round (text on signs and decals is not mirrored).

## Not handled yet

- terrain grass/detail objects (painted details, not trees) and holes-texture compression (holes are read when stored raw)
- custom Shader Graph effects beyond textures and tiling (height-blended layer edges become hard
  triangle edges; water, hue/saturation controls and overlays are not reproduced)
- detail maps, lightmaps, skyboxes, particles, animation
- crunched ETC textures (mobile only)

## Build

Needs CMake and Visual Studio 2022 (C++). `build.bat` produces `bin\Spotbuilder.exe`,
a single exe with the CRT linked statically.

## Format notes

- **UnityFS bundle**: `"UnityFS"`, u32 format, player/engine version strings, i64 size,
  u32 packed/unpacked block-info size, u32 flags (low 6 bits compression: 0 none,
  1 LZMA, 2 LZ4, 3 LZ4HC; 0x80 block info at the end; 0x200 padding before blocks on
  2020.3.34+/2021.3.2+/2022.1.1+, earlier the same bit meant UnityCN encryption). Format
  >= 7 (and 2019.4.15+) aligns the header to 16. Block info (big endian): 16-byte hash,
  blocks {u32 usize, u32 csize, u16 flags}, nodes {i64 offset, i64 size, u32 flags,
  path}. Node flag 4 = serialized file. LZMA blocks are 5 property bytes + raw stream.
- **Serialized file** (formats 9..22+): header, then metadata in the file's endianness:
  version string, platform, type-tree flag, types (blob type trees from format 12:
  nodes {u16 ver, u8 level, u8 flags, u32 type, u32 name, i32 size, i32 index,
  u32 meta, +u64 ref hash from 19}; string offsets with bit 31 index Unity's common
  string table), objects {path id aligned i64, offset, size, type index}, script types,
  externals. Fields with meta flag 0x4000 align to 4 afterwards; strings always do.
- **Mesh** (2019+): channels {stream, offset, format, dimension & 0xF}; streams packed
  back to back, each 16-byte aligned; 2019 vertex formats 0 float, 1 half, 2 unorm8,
  3 snorm8, 4 unorm16, 5 snorm16, 6.. integers; channels 0 pos, 1 normal, 2 tangent,
  3 colour, 4-11 uv0-7. Submesh firstByte / index size = first index, plus baseVertex.
- **Static batching**: `m_StaticBatchInfo {firstSubMesh, subMeshCount}` on the renderer;
  the MeshFilter then points at a world-space "Combined Mesh (root: scene)".
- **HDRP 7 mask map**: R metallic, G occlusion, B detail mask, A smoothness; smoothness
  and AO remapped by `_SmoothnessRemapMin/Max`, `_AORemapMin/Max`. With a mask map the
  red channel alone gives metallic (the `_Metallic` slider is hidden and left at 0).
- **HDRP 7 DecalProjector**: projects along local +Z; box = `m_Offset` +/- `m_Size`/2
  in projector space; uv = ((x, y) - offset) / size + 0.5, then `* m_UVScale + m_UVBias`.
- **Built-in resource ids** (`Library/unity default resources`): 10202 Cube,
  10206 Cylinder, 10207 Sphere, 10208 Capsule, 10209 Plane, 10210 Quad.

## Third-party code

LZ4 (BSD-2), LZMA SDK LzmaDec (public domain), miniz (MIT), bcdec (MIT/Unlicense),
Unity crunch decoder `crn_decomp.h` (zlib), ASTC/ETC/EAC decoders from texture2ddecoder
(`third_party/t2d`, MIT), Dear ImGui 1.91.9b (`third_party/imgui`, MIT), meshoptimizer 1.3 (`third_party/meshoptimizer`, MIT; its
simplifier's quadrics are changed to double precision: in float, centimetre-sized error on a
mesh hundreds of metres across rounds to zero). Licences are in `third_party/`.
