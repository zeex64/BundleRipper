# Generates XLMapRipper/src/studio_surfaces.h from ReSkate Studio's add-on tables, the same way
# its _native_collision_material_items() builds the "Base Surface / Audio Donor" enum.
import ast, re
src = open(r'C:\Users\Zee\Documents\GitHub\ReSkateStudio\blender_addon\sk8_map_export.py', encoding='utf-8').read()

def tuple_literal(name):
    m = re.search(r'^' + name + r'\s*=\s*\(', src, re.M)
    start = m.end() - 1
    depth = 0
    for i in range(start, len(src)):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                return ast.literal_eval(src[start:i + 1])
    raise SystemExit('no end for ' + name)

rows = tuple_literal('NATIVE_COLLISION_MATERIAL_ROWS')
packed_list = tuple_literal('NATIVE_COLLISION_NETWORK_PACKED')
items = []
for network_id, network_packed in enumerate(packed_list):
    material_slot = (network_packed >> 6) & 0x1fff
    property_slot = (network_packed >> 19) & 0x1fff
    _, material_label, _ = rows[material_slot]
    if property_slot:
        _, property_label, _ = rows[property_slot]
        label = f"{material_label} + {property_label} behavior"
    else:
        label = material_label
    items.append((network_packed | 0x20, f"{network_id:03d} - {label}"))

out = ['// Generated from ReSkate Studio\'s blender_addon/sk8_map_export.py (NATIVE_COLLISION_MATERIAL_ROWS,',
       '// NATIVE_COLLISION_NETWORK_PACKED): the "Base Surface / Audio Donor" choices. Regenerate with',
       '// tools/gen_studio_surfaces.py when Studio\'s table changes.',
       '#pragma once',
       '',
       'namespace xl {',
       '',
       'struct StudioSurface {',
       '    int packed;         // MaterialDecl.Packed, what sk8 collision_material stores',
       '    const char* label;  // as Studio lists it',
       '};',
       '',
       'inline constexpr StudioSurface kStudioSurfaces[] = {']
for packed, label in items:
    out.append('    {%d, "%s"},' % (packed, label.replace('\\', '\\\\').replace('"', '\\"')))
out += ['};', '', 'inline constexpr int kDefaultStudioSurface = 32;', '', '} // namespace xl', '']
open(r'C:\Users\Zee\Documents\GitHub\XLMapRipper\src\studio_surfaces.h', 'w', encoding='utf-8').write('\n'.join(out))
print(len(items), 'surfaces; first', items[0], 'metal rail?', [i for i in items if 'Metal Rail' in i[1]][:2])

# ---- audio region tags (Sk8AudioSettings.region_tag) -> src/studio_audio.h
import hashlib
m = re.search(r'^AUDIO_TAG_PATHS\s*=\s*"""(.*?)"""', src, re.M | re.S)
paths = [line.strip() for line in m.group(1).splitlines() if line.strip()]
prefix = 'audio/_systems/audiotag/region/'
rows = []
for path in paths:
    if not path.startswith(prefix):
        continue
    leaf = path[len(prefix):]
    label = leaf.removeprefix('dgo_tag_').replace('_', ' ').title()
    rows.append((path, label))
out = ['// Generated from ReSkate Studio\'s blender_addon/sk8_map_export.py (AUDIO_TAG_PATHS, region group):',
       '// the "Native Region" choices of an audio volume. Regenerate with tools/gen_studio_surfaces.py.',
       '#pragma once', '', 'namespace xl {', '',
       'struct StudioAudioRegion {', '    const char* path;   // what sk8_audio_region_tag stores', '    const char* label;', '};', '',
       'inline constexpr StudioAudioRegion kStudioAudioRegions[] = {']
for path, label in rows:
    out.append('    {"%s", "%s"},' % (path, label))
out += ['};', '', 'inline constexpr const char* kDefaultAudioRegion = "audio/_systems/audiotag/region/dgo_tag_reg_mpr_beach";',
        '', '} // namespace xl', '']
open(r'C:\Users\Zee\Documents\GitHub\XLMapRipper\src\studio_audio.h', 'w', encoding='utf-8').write('\n'.join(out))
print(len(rows), 'audio region tags')
