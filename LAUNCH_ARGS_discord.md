# Spotbuilder launch args (formerly BundleRipper)
Double-click `Spotbuilder.exe` for the editor window: the map in 3D, live LODs, ReSkate Studio settings (collision, surfaces/sound, collision-only, player spawn, bus stops, lights, audio volumes, grind curves, NPC routes...) and every option below under **Settings**.
Drag a map onto the exe to rip it with the defaults, or run:
`Spotbuilder <map file or folder> [options]`
`Spotbuilder --gui [map] [options]` opens the window with those settings filled in.

## Skate mod (needs ReSkate Studio + Blender)
- `--build-mod`: build a Skate mod instead of a .glb (rip, Blender .blend, Studio compile-map)
- `--mod-name <name>`: mod folder name (default: the map's name)
- `--no-deploy`: build it without installing into Skate's Mods folder (installing needs Skate closed)
- `--pause-map <3d|2d>`, `--time-of-day <default|morning|noon|afternoon|evening|night>`
- `--no-gi`: skip baking bounced light (faster builds)
- `--streaming <auto|on|off>`: load big maps in cells around the player
- `--no-mod-lods`: no Studio mesh LODs
- `--keep-blend`: also save `<map>.blend` next to the map
- `--package <folder>`: where Studio builds the mod
- `--blender <exe>`, `--studio-cli <exe>`, `--game <folder>`: only if not found from Studio's settings

## Output
- `-o <file.glb>` / `--output`: where to write the .glb (default: next to the map)
- `--external-textures`: PNGs in a `<name>_textures` folder instead of inside the .glb
- `--max-texture-size <px>`: use smaller mips for textures bigger than this
- `--no-textures`: geometry and material colours only (quick tests)

## What gets exported
- `--include-inactive`: also export disabled objects (tagged `xl_inactive`)
- `--all-lods`: every LOD level, not just LOD0
- `--lod <n>`: use LOD level n of every LOD group instead of LOD0
- `--tree-lod <n>`: LOD level for trees/grass painted on terrains
- `--plant-lod "<name>=<n>"`: LOD level for one painted tree/bush/grass type (`n` = a number, `lowest` or `default`); repeat per type. `--list` prints the type names and their LOD triangle counts
- `--no-colliders`: skip the `_col` collision objects; visible meshes take over their collision
- `--triggers`: include trigger colliders
- `--decals <project|quad|none>`: decals cut onto surfaces (default), flat quads, or skipped
- `--no-trees`: skip trees painted on Unity terrains
- `--terrain-resolution <n>`: max grid squares per terrain side (default 1024)
- `--no-lights`: skip lights

## Mesh optimisation (on by default)
- `--simplify <mm>`: drop vertices only where nothing moves more than this many mm (default 1); every result is checked against the original, including normals and UVs
- `--no-simplify`: lossless clean-up only (merge duplicate vertices, drop zero-area triangles)
- `--no-optimize`: meshes exactly as stored in the map

## Grind splines
- `--no-splines`: skip the map's own splines (default: curves in `<name>_splines.obj`)
- `--spline-meshes`: also put them in the .glb as line meshes
- `--no-autosplines`: skip `<name>_autosplines.obj` (grind lines found on rails, copings, ledges)
- `--autospline-all`: look for grind lines on every collidable object

## Blender / materials
- `--keep-hierarchy`: keep Unity's object tree (default: flat world-space objects)
- `--vertex-colors`: keep vertex colours (adds Color Attribute nodes)
- `--standard-alpha`: cut-outs as glTF MASK (adds Alpha Clip nodes)
- `--occlusion`: link ambient occlusion (adds a glTF node group)

## Map edits
- `--edits <file.json>`: apply these edits (default: `<map>.spotbuilder.json` beside the map, which the window saves)
- `--no-edits`: ignore saved edits

## Info
- `--list`: print what the map contains and exit
- `--dump-textures <folder>`: also save every texture as a PNG
- `-v` / `--verbose`: more detail
- `-h` / `--help`: show all args

-# `--colliders` and `--autosplines` still work but do nothing; both are on by default.
