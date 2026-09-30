# BundleRipper

Rips whole levels out of Unity asset bundles to a single binary glTF (`.glb`) that imports
straight into Blender: meshes, PBR materials, textures, terrain and terrain trees,
colliders, decals, grind splines and lights. Built for Skater XL mod maps (HDRP, URP and
built-in shaders, Unity 2019-2022), and it works on scene bundles from other Unity games too.

## Use

Drag a map file (e.g. `BerlinXL V2 by GyOm`) onto `BundleRipper.exe`. It writes
`<map>.glb` next to the map. From a terminal:

```
BundleRipper <map file or folder> [options]

  -o, --output <file.glb>   output path (default: <map>.glb next to the map)
  --external-textures       write PNGs to <name>_textures\ instead of inside the .glb
  --max-texture-size <px>   use smaller mip levels for textures bigger than this
  --no-textures             geometry and material colours only
  --include-inactive        also export disabled GameObjects (tagged xl_inactive)
  --all-lods                export every LOD level, not just LOD0
  --no-colliders            leave out the collision-only <name>_col objects; the visible
                            meshes then take over their collision (sk8_collision_mode)
  --triggers                include trigger colliders
  --keep-hierarchy          keep Unity's GameObject tree instead of flat world-space objects
  --vertex-colors           keep mesh vertex colours (Blender multiplies them into base colour)
  --standard-alpha          write cut-outs as glTF MASK (Blender adds Alpha Clip nodes)
  --occlusion               link ambient occlusion too (Blender adds a glTF side node group)
  --decals <mode>           project (default) | quad | none
  --no-splines              leave out grind splines
  --spline-meshes           also put the splines in the .glb as line meshes
  --no-autosplines          don't write <name>_autosplines.obj
  --autospline-all          look for grind lines on every collidable object
  --terrain-resolution <n>  most grid squares per Unity terrain side (default 1024)
  --no-trees                leave out trees painted on Unity terrains
  --no-lights               leave out lights
  --list                    print what the map contains and exit
  --dump-textures <folder>  also save every texture as a PNG
  -v, --verbose             more detail
```

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

Needs CMake and Visual Studio 2022 (C++). `build.bat` produces `bin\BundleRipper.exe`,
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
(`third_party/t2d`, MIT). Licences are in `third_party/`.
