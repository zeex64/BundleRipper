# Spotbuilder's Blender step: turns a ripped map (.glb plus a curves file) into a .blend that
# ReSkate Studio builds into a Skate mod. Spotbuilder runs it as
#
#   blender --background --factory-startup --python-exit-code 1 --python make_blend.py --
#           <map.glb> <curves.json> <out.blend>
#
# The .glb already carries Studio's settings as custom properties (sk8_object, sk8_material,
# sk8_* keys, spawn / TravelPoint empties, audio volume empties). Here they are also copied into
# the Skate Map add-on's property groups when this Blender has the add-on (that is how its
# panels show them, and grind curves are only read from there in Blender 5), and the curves the
# .glb cannot hold are made: grind curves and NPC routes.
import json
import math
import sys
import traceback

import addon_utils
import bpy

GRIND_SURFACES = [  # Studio's Sk8GrindCurveSettings.surface, in its order
    ("material_37225248", "Concrete"),
    ("material_37226144", "Metal Thin"),
    ("material_37226528", "Metal"),
    ("material_37227424", "Metal Rail"),
    ("material_37228128", "Wood Thick Rough"),
]
LIGHT_TIMES = ["morning", "noon", "afternoon", "evening", "night", "weatherday", "weathernight"]


def progress(fraction, message):
    print(f"SB_PROGRESS\t{fraction:.3f}\t{message}", flush=True)


def register_grind_settings():
    """Studio's grind curve settings, when its add-on is not installed here: same names and
    values, so Studio reads them from the saved file."""
    from bpy.props import BoolProperty, EnumProperty, FloatProperty, PointerProperty

    class Sk8GrindCurveSettings(bpy.types.PropertyGroup):
        enabled: BoolProperty(name="Grind Curve", default=False)
        radius: FloatProperty(name="Rail Radius", default=0.03, min=0.005, max=0.25)
        surface: EnumProperty(name="Surface", items=[(i, l, "", n) for n, (i, l) in enumerate(GRIND_SURFACES)],
                              default="material_37227424")

    bpy.utils.register_class(Sk8GrindCurveSettings)
    bpy.types.Object.sk8_grind_curve = PointerProperty(type=Sk8GrindCurveSettings)


def fix_light(obj):
    """Brightness the way the game reads it. Blender's glTF importer makes every light
    W = cd x 4pi / 683; ReSkate Studio sends W x 683 lumens, which Skate reads as lumens / 4pi
    candela for a point light but lumens / pi for a spot or a rectangle, so spots are cut to a
    quarter. Spotbuilder's area lights arrive as wide spots with their size and become real
    rectangles."""
    light = obj.data
    size = obj.get("xl_area_size")
    if size is not None and len(size) == 2:
        cd = float(obj.get("xl_area_intensity", 0.0)) or light.energy * 683.0 / (4.0 * math.pi)
        area = bpy.data.lights.new(light.name, "AREA")
        area.shape = "RECTANGLE"
        area.size = float(size[0])
        area.size_y = float(size[1])
        area.color = light.color
        area.energy = cd * math.pi / 683.0
        if hasattr(light, "use_custom_distance"):
            area.use_custom_distance = light.use_custom_distance
            area.cutoff_distance = light.cutoff_distance
        obj.data = area
        obj["sk8_light_area_mode"] = "AREA"
    elif light.type == "SPOT":
        light.energy /= 4.0


def as_dict(value):
    if value is None:
        return None
    if hasattr(value, "to_dict"):
        return value.to_dict()
    return value if isinstance(value, dict) else None


def set_group(group, values, what):
    """Copies raw settings into a registered property group (enums arrive as their numbers)."""
    if group is None or not values:
        return
    for key, value in values.items():
        prop = group.bl_rna.properties.get(key)
        if prop is None:
            try:
                group[key] = value  # internal markers such as _sk8_behavior_override_initialized
            except Exception:
                pass
            continue
        if prop.is_readonly:
            continue
        try:
            if prop.type == "ENUM" and isinstance(value, (int, float)) and not isinstance(value, bool):
                if prop.is_enum_flag:
                    value = {item.identifier for item in prop.enum_items if int(value) & item.value}
                else:
                    match = [item.identifier for item in prop.enum_items if item.value == int(value)]
                    if not match:
                        continue
                    value = match[0]
            setattr(group, key, value)
        except Exception as error:
            print(f"{what}: could not set {key} = {value!r}: {error}")


def collection(name, parent):
    c = bpy.data.collections.new(name)
    parent.children.link(c)
    return c


def make_curve(entry, target, addon):
    data = bpy.data.curves.new(entry["name"], "CURVE")
    data.dimensions = "3D"
    points = entry["points"]
    bezier = bool(entry.get("bezier")) and "left" in entry
    spline = data.splines.new("BEZIER" if bezier else "POLY")
    if bezier:
        spline.bezier_points.add(len(points) - 1)
        for i, bp in enumerate(spline.bezier_points):
            bp.co = points[i]
            bp.handle_left_type = bp.handle_right_type = "FREE"
            bp.handle_left = entry["left"][i]
            bp.handle_right = entry["right"][i]
    else:
        spline.points.add(len(points) - 1)
        for i, p in enumerate(spline.points):
            p.co = (*points[i], 1.0)
    spline.use_cyclic_u = bool(entry.get("closed"))
    obj = bpy.data.objects.new(entry["name"], data)
    target.objects.link(obj)
    if entry["kind"] == "npc":
        route = entry["npc"]
        for key, value in route.items():
            obj["sk8_npc_" + key] = value
        obj["sk8_npc_enabled"] = True
        if addon and hasattr(obj, "sk8_npc_route"):
            set_group(obj.sk8_npc_route, dict(route, enabled=True), obj.name)
    else:
        grind = entry["grind"]
        settings = obj.sk8_grind_curve
        settings.enabled = bool(grind["enabled"])
        settings.radius = float(grind["radius"])
        try:
            settings.surface = grind["surface"]
        except Exception as error:
            print(f"{obj.name}: surface {grind['surface']}: {error}")
    return obj


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    glb, curves_path, out_blend = argv[:3]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon = False
    try:
        addon_utils.enable("sk8_map_export", default_set=False)
        addon = hasattr(bpy.types.Object, "sk8_object")
    except Exception as error:
        print("ReSkate Studio's Skate Map add-on is not installed in this Blender:", error)
    print("Studio add-on:", "found" if addon else "not found (settings stay as custom properties)")
    if not hasattr(bpy.types.Object, "sk8_grind_curve"):
        register_grind_settings()

    progress(0.05, "importing the map into Blender")
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=glb)
    imported = [o for o in bpy.data.objects if o not in before]
    scene = bpy.context.scene
    root = scene.collection
    map_collection = collection("Map", root)
    markers = collection("Markers", root)
    for obj in imported:
        target = map_collection
        marker = obj.get("xl_marker")
        if obj.type == "EMPTY" and (marker or obj.name.split(".")[0] in ("spawn", "TravelPoint")):
            target = markers
        for c in list(obj.users_collection):
            c.objects.unlink(obj)
        target.objects.link(obj)

    progress(0.6, "applying Studio settings")
    for obj in imported:
        if addon:
            set_group(getattr(obj, "sk8_object", None), as_dict(obj.get("sk8_object")), obj.name)
        marker = obj.get("xl_marker")
        if obj.type == "EMPTY":
            if marker == "audio_volume":
                obj.empty_display_type = "CUBE"
                obj.empty_display_size = 1.0  # the box is its scale (half sizes)
                obj.show_in_front = True
                if addon and hasattr(obj, "sk8_audio"):
                    values = {k[len("sk8_audio_"):]: obj[k] for k in obj.keys() if k.startswith("sk8_audio_")}
                    set_group(obj.sk8_audio, values, obj.name)
            elif marker or obj.name.split(".")[0] in ("spawn", "TravelPoint"):
                obj.empty_display_type = "SINGLE_ARROW"
                obj.empty_display_size = 1.5
                obj.show_in_front = True
        if obj.type == "LIGHT":
            fix_light(obj)
        if obj.type == "LIGHT" and addon and hasattr(obj, "sk8_light"):
            values = {}
            if "sk8_light_range" in obj:
                values["attenuation_radius"] = float(obj["sk8_light_range"])
            if "sk8_light_tod" in obj:
                mask = int(obj["sk8_light_tod"])
                values["time_of_day"] = {t for i, t in enumerate(LIGHT_TIMES) if mask & (1 << i)}
            if "sk8_light_area_mode" in obj:
                values["area_mode"] = str(obj["sk8_light_area_mode"])
            for key, value in values.items():
                try:
                    setattr(obj.sk8_light, key, value)
                except Exception as error:
                    print(f"{obj.name}: light {key}: {error}")
    if addon:
        for material in bpy.data.materials:
            set_group(getattr(material, "sk8_material", None), as_dict(material.get("sk8_material")), material.name)

    progress(0.75, "making grind curves and NPC routes")
    with open(curves_path, encoding="utf-8") as f:
        curves = json.load(f).get("curves", [])
    grind = collection("Grind curves", root)
    routes = collection("NPC routes", root)
    for entry in curves:
        if len(entry.get("points", [])) < 2:
            continue
        make_curve(entry, routes if entry["kind"] == "npc" else grind, addon)

    progress(0.9, "saving the .blend")
    try:
        bpy.ops.file.pack_all()
    except Exception as error:
        print("pack_all:", error)
    bpy.ops.wm.save_as_mainfile(filepath=out_blend, compress=False)
    progress(1.0, "saved " + out_blend)


try:
    main()
except Exception:
    traceback.print_exc()
    sys.exit(1)
