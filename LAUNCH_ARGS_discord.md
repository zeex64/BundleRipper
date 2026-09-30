# BundleRipper launch args
Drag a map onto `BundleRipper.exe` for the defaults, or run:
`BundleRipper <map file or folder> [options]`

## Output
- `-o <file.glb>` / `--output`: where to write the .glb (default: next to the map)
- `--external-textures`: PNGs in a `<name>_textures` folder instead of inside the .glb
- `--max-texture-size <px>`: use smaller mips for textures bigger than this
- `--no-textures`: geometry and material colours only (quick tests)

## What gets exported
- `--include-inactive`: also export disabled objects (tagged `xl_inactive`)
- `--all-lods`: every LOD level, not just LOD0
- `--no-colliders`: skip the `_col` collision objects; visible meshes take over their collision
- `--triggers`: include trigger colliders
- `--decals <project|quad|none>`: decals cut onto surfaces (default), flat quads, or skipped
- `--no-trees`: skip trees painted on Unity terrains
- `--terrain-resolution <n>`: max grid squares per terrain side (default 1024)
- `--no-lights`: skip lights

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

## Info
- `--list`: print what the map contains and exit
- `--dump-textures <folder>`: also save every texture as a PNG
- `-v` / `--verbose`: more detail
- `-h` / `--help`: show all args

-# `--colliders` and `--autosplines` still work but do nothing; both are on by default.
