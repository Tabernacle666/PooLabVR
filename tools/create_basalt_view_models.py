"""Build the three static OASIS VR view models with Blender.

Run from any directory: blender -b --python tools/create_basalt_view_models.py
Authoring dimensions are in mm. Blender +Y maps to engine +Z on import.
The equipped assembly is exported with its origin at the optic exit.
"""

from math import cos, pi, sin
from pathlib import Path

import bpy
from mathutils import Vector


bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, color, roughness, metallic=0.0, emission=0.0):
    surface = bpy.data.materials.new(name)
    surface.diffuse_color = (*color, 1)
    surface.use_nodes = True
    shader = surface.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1)
    shader.inputs["Roughness"].default_value = roughness
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Emission Color"].default_value = (*color, 1)
    shader.inputs["Emission Strength"].default_value = emission
    return surface


fabric = material("Glove Fabric", (.033, .036, .034), .88)
binding = material("Glove Binding", (.018, .021, .020), .73)
pad = material("Glove Armor", (.055, .058, .052), .58)
thread = material("Glove Stitching", (.080, .083, .073), .83)
olive = material("Basalt Olive", (.157, .172, .102), .55, .55)
enamel = material("Basalt Deep Olive", (.076, .089, .057), .49, .51)
graphite = material("Basalt Graphite", (.056, .060, .059), .43, .68)
steel = material("Basalt Worn Alloy", (.20, .19, .16), .34, .85)
grip_rubber = material("Basalt Rubber", (.022, .026, .026), .76)
optic_red = material("Basalt Optic Red", (.48, .014, .008), .26, .23, .45)
optic_dark = material("Basalt Optic Dark", (.16, .012, .009), .30, .36, .12)
amber = material("Basalt Amber", (.84, .30, .047), .45, .18, .40)
ink = material("Basalt Marking", (.53, .54, .43), .61)


def anchor(name, point=(0, 0, 0), parent=None):
    obj = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(obj)
    obj.parent = parent
    obj.location = Vector(point) * .001
    return obj


def section_mesh(name, rings, surface, parent, *, closed=False, smooth=False, mirror=1):
    # A loop per cross-section gives the fingers a continuous silhouette.
    count = len(rings[0])
    verts = [(mirror * p[0] * .001, p[1] * .001, p[2] * .001)
             for ring in rings for p in ring]
    faces = []
    uvs = []
    spans = len(rings) if closed else len(rings) - 1
    for row in range(spans):
        following = (row + 1) % len(rings)
        for col in range(count):
            j = (col + 1) % count
            faces.append((row * count + col, following * count + col,
                          following * count + j, row * count + j))
            u, u1 = col / count, (col + 1) / count
            v, v1 = row / spans, (row + 1) / spans
            uvs.append(((u, v), (u, v1), (u1, v1), (u1, v)))
    if not closed:
        faces.extend((tuple(range(count)),
                      tuple(reversed(range(spans * count, (spans + 1) * count)))))
        uvs.extend((((.5 + .5 * cos(2 * pi * i / count),
                      .5 + .5 * sin(2 * pi * i / count)) for i in order)
                    for order in (range(count), reversed(range(count)))))
    if mirror < 0:
        faces = [tuple(reversed(face)) for face in faces]
        uvs = [tuple(reversed(tuple(face))) for face in uvs]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    uv = mesh.uv_layers.new(name="Surface")
    for polygon, coordinates in zip(mesh.polygons, uvs):
        polygon.use_smooth = smooth and polygon.index < spans * count
        for loop, point in zip(polygon.loop_indices, coordinates):
            uv.data[loop].uv = point
    mesh.materials.append(surface)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.parent = parent
    return obj


def tube(name, profile, surface, parent, *, mirror=1, sides=12, cross_axis=(1, 0, 0)):
    centers = [Vector(point) for point, _, _ in profile]
    rings = []
    for i, (_, wide, high) in enumerate(profile):
        axis = (centers[min(i + 1, len(centers) - 1)] - centers[max(0, i - 1)]).normalized()
        side = Vector(cross_axis)
        side = (side - axis * axis.dot(side)).normalized()
        up = side.cross(axis)
        rings.append([centers[i] + side * (wide * cos(2 * pi * j / sides))
                      + up * (high * sin(2 * pi * j / sides)) for j in range(sides)])
    return section_mesh(name, rings, surface, parent, smooth=True, mirror=mirror)


def block(name, center, size, bevel, surface, parent, rotation=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=.002)
    obj = bpy.context.object
    obj.name = name
    obj.scale = Vector(size) / 2
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.parent = parent
    obj.location = Vector(center) * .001
    obj.rotation_euler = rotation
    obj.data.materials.append(surface)
    rounded = obj.modifiers.new("Soft machined edge", "BEVEL")
    rounded.width = bevel * .001
    rounded.segments = 2
    rounded.harden_normals = True
    weighted = obj.modifiers.new("Weighted face normals", "WEIGHTED_NORMAL")
    weighted.keep_sharp = True
    return obj


def frame(name, profile, surface, parent, *, closed=False):
    # Rounded rectangular cross-sections along the forward axis.
    rings = []
    for depth, width, height, center_z, radius in profile:
        ring = []
        for corner, (x, z) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
            for step in range(4):
                angle = (corner + step / 3) * pi / 2
                ring.append((x * (width / 2 - radius) + radius * cos(angle),
                             depth,
                             center_z + z * (height / 2 - radius) + radius * sin(angle)))
        rings.append(ring)
    return section_mesh(name, rings, surface, parent, closed=closed)


def glove(root, *, left=False, wrapped=False):
    mirror = -1 if left else 1
    tube("Gauntlet", [((0, -128, -9), 31, 21), ((0, -121, -9), 34, 22),
                     ((0, -71, -6), 28, 19), ((0, -47, -4), 28, 17)],
         fabric, root, mirror=mirror, sides=16)
    tube("Cuff binding", [((0, -89, -8), 31, 20), ((0, -86, -7), 32, 22),
                          ((0, -71, -6), 30, 21), ((0, -68, -6), 28, 19)],
         binding, root, mirror=mirror, sides=16)
    if wrapped:
        # Palm sits on the outside of the grip, with four fingers curled
        # around the front of the rubber overmold.
        tube("Wrapped palm", [((0, -64, -7), 26, 17), ((10, -45, -10), 30, 19),
                              ((20, -29, -20), 26, 22), ((22, -12, -24), 20, 24),
                              ((17, 5, -23), 13, 23)], fabric, root, sides=16)
        for digit, z in enumerate((9, -10, -27, -43)):
            y = 19 - 5 * digit
            radius = 8.8 - .55 * digit
            tube(f"Wrapped finger {digit + 1}",
                 [((23, -6 - 4 * digit, z - 3), radius, radius),
                  ((22, y - 5, z), radius, radius * .88),
                  ((13, y + 3, z), radius * .91, radius * .84),
                  ((-1, y + 4, z - 1), radius * .83, radius * .77),
                  ((-13, y - 1, z - 4), radius * .68, radius * .65),
                  ((-16, y - 6, z - 5), radius * .38, radius * .34)], fabric, root,
                 cross_axis=(0, 0, 1))
            block(f"Grip knuckle {digit + 1}", (23, y - 7, z + 1),
                  (15, 21 - digit, 11 - digit), 5, pad, root)
        tube("Wrapped thumb", [((-12, -38, -1), 12, 11), ((-25, -27, 9), 12, 10),
                               ((-25, -8, 17), 9.5, 8), ((-17, 5, 19), 8, 7),
                               ((-12, 9, 17), 4.4, 3.7)], fabric, root)
        return

    tube("Palm", [((0, -54, -4), 26, 16), ((0, -36, -3), 33, 20),
                  ((1, -8, 0), 39, 21), ((0, 17, 1), 37, 17),
                  ((0, 27, -1), 32, 12)], fabric, root, mirror=mirror, sides=16)
    block("Dorsal guard", (0, -2, 17), (64, 48, 13), 9, pad, root)
    for digit, (x, length, radius) in enumerate(((-28, 47, 10), (-9, 53, 10.5),
                                                (10, 49, 9.2), (27, 39, 8.0))):
        tip_x = x + (x / 28) * 4
        tube(f"Finger {digit + 1}",
             [((x, 16, 2), radius, radius * .84),
              ((x, 29, 4), radius, radius * .87),
              ((tip_x, 27 + length * .53, 2), radius * .89, radius * .81),
              ((tip_x, 26 + length * .78, -7), radius * .79, radius * .73),
              ((tip_x, 26 + length * .83, -19), radius * .67, radius * .63),
              ((tip_x, 25 + length * .78, -25), radius * .37, radius * .34)],
             fabric, root, mirror=mirror)
        block(f"Knuckle guard {digit + 1}", (mirror * x, 21, 11),
              (radius * 1.55, 19, 10), 4, pad, root)
        block(f"Finger seam {digit + 1}", (mirror * tip_x, 27 + length * .57, 7),
              (radius * 1.13, 2.6, 2.9), 1.1, binding, root, (-.35, 0, 0))
    tube("Thumb", [((-28, -18, -5), 13, 11), ((-45, -7, -4), 13, 11),
                   ((-54, 10, -7), 10, 9.5), ((-55, 28, -15), 8.8, 7.7),
                   ((-48, 38, -24), 7.4, 6.5), ((-44, 38, -26), 4, 3.6)],
         fabric, root, mirror=mirror)
    for x in (-20, 20):
        block("Cuff seam", (x, -94, 8), (2, 30, 1.8), .7, thread, root)


def label(text, parent):
    curve = bpy.data.curves.new(text, "FONT")
    curve.body = text
    curve.align_x = 'CENTER'
    curve.size = .011
    curve.resolution_u = 4
    obj = bpy.data.objects.new(text, curve)
    bpy.context.collection.objects.link(obj)
    obj.parent = parent
    obj.location = (.0345, .057, .049)
    # Text x faces forward, text y faces up, text normal faces outward.
    obj.rotation_euler = (pi / 2, 0, pi / 2)
    obj.data.materials.append(ink)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.convert(target='MESH')


def basalt(root):
    housing = anchor("Basalt.Housing", parent=root)
    frame("Armored chassis", [(-63, 41, 38, 49, 8), (-52, 58, 49, 51, 8),
                              (115, 61, 50, 52, 8), (139, 54, 41, 52, 7)], olive, housing)
    frame("Chassis underside seal", [(-55, 49, 16, 28, 6), (-43, 58, 13, 26, 5),
                                    (126, 55, 13, 26, 5)], graphite, housing)
    frame("Rear graphite cap", [(-68, 39, 33, 49, 7), (-64, 50, 43, 49, 7),
                                (-55, 53, 43, 49, 8)], graphite, housing)
    frame("Control spine", [(-54, 34, 10, 76, 4), (-32, 43, 12, 77, 4),
                            (105, 43, 12, 77, 4), (130, 32, 8, 76, 3)], graphite, housing)
    block("Side armor plate", (28, 56, 49), (11, 102, 31), 6, enamel, housing)
    block("Inlaid nameplate", (33, 56, 50), (2.5, 82, 22), 4, olive, housing)
    label("BASALT", housing)
    block("Opposite armor plate", (-29, 52, 49), (9, 98, 27), 6, enamel, housing)
    for side in (-1, 1):
        for y in (-37, 104):
            block("Captive alloy fastener", (side * 30, y, 39), (3, 8, 8), 2, steel, housing)
            block("Fastener recess", (side * 31.8, y, 39), (1, 4, 1.2), .4, graphite, housing)
        for rib in range(3):
            block("Inlaid vent", (side * 27, -40 + 7 * rib, 58),
                  (3.5, 3, 14), 1.1, graphite, housing, (.22, 0, 0))
        block("Chassis rail lip", (side * 26, 41, 32), (8, 98, 6), 2.2, steel, housing)
    for y in (77, 83, 89):
        block("Amber side inlay", (34.5, y, 44), (.7, 3.6, 5), .4, amber,
              housing, (-.35, 0, 0))

    handle = anchor("Basalt.Grip", parent=root)
    block("Grip core", (0, -15, -15), (38, 50, 92), 10, graphite, handle, (-.20, 0, 0))
    block("Grip overmold", (0, -19, -25), (42, 48, 67), 9, grip_rubber, handle, (-.20, 0, 0))
    for rib in range(5):
        block("Grip rib", (0, -22 + 1.6 * rib, -53 + rib * 9),
              (44, 48, 3.3), 1.3, grip_rubber, handle, (-.20, 0, 0))
    block("Grip heel", (0, -25, -61), (44, 55, 12), 4, graphite, handle, (-.20, 0, 0))

    command = anchor("Basalt.Command", (0, 11, 77), root)
    block("Thumb control socket", (0, 0, 0), (29, 34, 9), 4, graphite, command)
    block("Raised thumb control", (0, 1, 5), (24, 27, 6), 3, grip_rubber, command)
    for y in (-6, 1, 8):
        block("Control tread", (0, y, 8), (17, 3, 1.5), .5, steel, command)
    block("Status lamp carrier", (0, 33, 86), (20, 13, 5), 2, graphite, housing)
    for x in (-5.5, 0, 5.5):
        block("Amber status lamp", (x, 33, 89), (3, 7, 1.7), .8, amber, housing)

    cover = anchor("Basalt.ServiceCover", (-21, -24, 81), root)
    block("Service cover", (21, -17, 2), (48, 43, 10), 5, olive, cover, (.10, 0, 0))
    for y in (-29, -14):
        block("External hinge", (0, y, 1), (10, 11, 8), 3, steel, cover)
    block("Cover latch", (45, -18, 4), (8, 15, 5), 2, graphite, cover)
    for y in (-25, -19, -13):
        block("Non-slip latch ridge", (45, y, 7), (5, 2, 1.6), .6, steel, cover)

    sleeve = anchor("Basalt.SlidingShroud", (0, 111, 52), root)
    frame("Sliding olive armor", [(-20, 64, 54, 0, 7), (-14, 68, 55, 0, 8),
                                  (33, 68, 55, 0, 8), (39, 60, 48, 0, 7),
                                  (36, 55, 42, 0, 6), (-21, 55, 42, 0, 6)], olive, sleeve, closed=True)
    for side in (-1, 1):
        block("Shroud slide", (side * 32, 7, -11), (7, 45, 13), 4, graphite, sleeve)
        block("Shroud wear insert", (side * 36, 7, -11), (2, 32, 4), 1.6, steel, sleeve)
    block("Shroud arch", (0, 7, 27), (43, 25, 8), 4, olive, sleeve)

    optics = anchor("Basalt.Optics", (0, 144, 52), root)
    frame("Recessed optic bezel", [(0, 55, 44, 0, 7), (7, 62, 50, 0, 8),
                                    (34, 60, 48, 0, 8), (38, 54, 44, 0, 7),
                                    (38, 44, 34, 0, 6), (33, 41, 31, 0, 5),
                                    (19, 39, 28, 0, 5), (0, 39, 28, 0, 5)], graphite, optics, closed=True)
    frame("Red optic surround", [(18, 39, 28, 0, 5), (28, 42, 31, 0, 6),
                                  (28, 37, 25, 0, 4), (18, 34, 23, 0, 4)], optic_red, optics, closed=True)
    frame("Red glass optic", [(16, 35, 23, 0, 4), (19, 36, 24, 0, 5),
                               (22, 33, 21, 0, 4)], optic_dark, optics)
    block("Optical core", (0, 22, 0), (16, 1.4, 5), 1.9, optic_red, optics)


def export_selected(root, name, destination):
    bpy.ops.object.select_all(action='DESELECT')
    root.select_set(True)
    for obj in root.children_recursive:
        obj.select_set(True)
    bpy.ops.export_scene.gltf(filepath=str(destination / name), export_format='GLB',
                             use_selection=True, export_apply=True, export_materials='EXPORT')


assets = Path(__file__).resolve().parents[1] / "assets"
left = anchor("Glove.Left")
glove(left, left=True)
export_selected(left, "glove_left.glb", assets)

right = anchor("Glove.Right")
glove(right)
export_selected(right, "glove_right.glb", assets)

# The in-game pose sits at the beam exit on the red optic's centerline.
equipped = anchor("Basalt.OpticExit")
held = anchor("Basalt.GripSpace", (0, -182, -52), equipped)
basalt(held)
glove(anchor("Glove.Right.Equipped", parent=held), wrapped=True)
export_selected(equipped, "basalt_equipped.glb", assets)
