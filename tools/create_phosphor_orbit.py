import math
from pathlib import Path

import bpy


bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, color, roughness, metallic=0.0, emission=0.0, alpha=1.0):
    result = bpy.data.materials.new(name)
    result.diffuse_color = (*color, alpha)
    result.use_nodes = True
    shader = result.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, alpha)
    shader.inputs["Roughness"].default_value = roughness
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Alpha"].default_value = alpha
    shader.inputs["Emission Color"].default_value = (*color, 1.0)
    shader.inputs["Emission Strength"].default_value = emission
    return result


shell = material("Orbit Housing", (.105, .112, .068), .62, .48)
bezel = material("Orbit Bezel", (.045, .047, .033), .36, .68)
steel = material("Orbit Machined Steel", (.20, .19, .15), .33, .85)
rubber = material("Orbit Rubber", (.023, .025, .018), .78)
glass = material("Orbit Glass", (.11, .25, .19), .19, .08, alpha=.16)
display = material("Orbit Display", (.019, .054, .026), .49, emission=.18)
amber = material("Orbit Amber", (.92, .29, .037), .30, .15, .9)
phosphor = material("Orbit Phosphor", (.11, .77, .32), .28, .12, .65)
set_key = material("Orbit Set Key", (.46, .27, .061), .37, .21)
ink = material("Orbit Engraving", (.65, .60, .44), .65)


def mesh(name, positions, faces, surface, smooth=False, uv_size=None):
    geometry = bpy.data.meshes.new(name)
    geometry.from_pydata(positions, [], faces)
    geometry.update()
    obj = bpy.data.objects.new(name, geometry)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(surface)
    for polygon in geometry.polygons:
        polygon.use_smooth = smooth
    if uv_size is not None:
        width, height, center_x, center_z = uv_size
        uv = geometry.uv_layers.new(name="Display")
        for loop in geometry.loops:
            x, _, z = positions[loop.vertex_index]
            uv.data[loop.index].uv = ((x-center_x)/width+.5, (z-center_z)/height+.5)
    return obj


def rounded_rect(width, height, radius):
    result = []
    for corner, (sign_x, sign_z) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        x = sign_x * (width/2-radius)
        z = sign_z * (height/2-radius)
        for point in range(7):
            angle = (corner + point/6) * math.pi/2
            result.append((x + math.cos(angle)*radius, z + math.sin(angle)*radius))
    return result


def connect_rings(rings, closed=False):
    size = len(rings[0])
    faces = []
    for n in range(len(rings) if closed else len(rings)-1):
        current = n * size
        following = (n+1) % len(rings) * size
        for k in range(size):
            j = (k+1) % size
            faces.append((current+k, current+j, following+j, following+k))
    return faces


def profile(name, layers, surface, center=(0, 0), closed=False):
    x, z = center
    rings = [
        [(x+u, depth, z+v) for u, v in rounded_rect(width, height, radius)]
        for width, height, radius, depth in layers
    ]
    faces = connect_rings(rings, closed)
    if not closed:
        size = len(rings[0])
        faces.extend((tuple(reversed(range(size))), tuple(range((len(rings)-1)*size, len(rings)*size))))
    return mesh(name, [vertex for ring in rings for vertex in ring], faces, surface)


def panel(name, width, height, radius, depth, surface, center=(0, 0)):
    x, z = center
    vertices = [(x+u, depth, z+v) for u, v in rounded_rect(width, height, radius)]
    return mesh(name, vertices, [tuple(range(len(vertices)))], surface, uv_size=(width, height, x, z))


def disc(name, x, z, radius, back, front, surface, knurled=False):
    segments = 64 if knurled else 32
    rings = []
    bevel = min(.06, (back-front)*.28)
    for depth, scale_factor in ((back, .89), (back-bevel, 1), (front+bevel, 1), (front, .87)):
        profile_ring = []
        for i in range(segments):
            angle = 2 * math.pi * i / segments
            notch = .92 if knurled and i % 2 else 1.0
            r = radius * scale_factor * notch
            profile_ring.append((x+r*math.cos(angle), depth, z+r*math.sin(angle)))
        rings.append(profile_ring)
    faces = connect_rings(rings)
    faces.append(tuple(reversed(range(segments))))
    faces.append(tuple(range(3*segments, 4*segments)))
    return mesh(name, [vertex for ring in rings for vertex in ring], faces, surface)


def engraving(name, label, x, y, z, size):
    shape = bpy.data.curves.new(name, "FONT")
    shape.body = label
    shape.align_x = 'CENTER'
    shape.size = size
    shape.resolution_u = 3
    obj = bpy.data.objects.new(name, shape)
    bpy.context.collection.objects.link(obj)
    obj.rotation_euler[0] = math.pi/2
    obj.location = (x, y, z)
    obj.data.materials.append(ink)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.ops.object.convert(target='MESH')


profile("Painted housing", [(7.49, 3.43, .48, .49), (8.10, 3.81, .52, .24),
                            (8.16, 3.86, .53, -.29), (7.94, 3.66, .46, -.51)], shell)
profile("Rear gasket", [(7.45, 3.34, .43, .52), (7.61, 3.43, .46, .46)], rubber)
profile("Left protective rail", [(.48, 2.99, .16, .35), (.60, 3.17, .20, -.14),
                                 (.44, 2.98, .16, -.50)], bezel, (-3.68, -.02))
profile("Right protective rail", [(.53, 2.99, .16, .35), (.64, 3.17, .20, -.14),
                                  (.48, 2.98, .16, -.50)], bezel, (3.68, -.02))
profile("Machined frame", [(6.22, 3.37, .30, -.49), (6.14, 3.28, .28, -.67),
                           (5.80, 2.99, .23, -.714), (5.70, 2.89, .22, -.55)],
        steel, (.12, .00), closed=True)
profile("Lens gasket", [(5.88, 3.05, .24, -.575), (5.86, 3.01, .24, -.685),
                        (5.69, 2.88, .22, -.689), (5.63, 2.82, .20, -.57)],
        bezel, (.12, .00), closed=True)
panel("Recessed holographic plane", 5.68, 2.90, .22, -.552, display, (.12, .00))

profile_points = rounded_rect(5.77, 2.97, .24)
lens_rings = []
for radius in (1.0, .82, .58, .32, .13):
    lens_rings.append([
        (.12+x*radius, -.622-.081*(1-radius**2)+.006*math.sin(x*2.2+z)*radius, z*radius)
        for x, z in profile_points
    ])
lens_faces = connect_rings(lens_rings)
lens_faces.append(tuple(range((len(lens_rings)-1)*len(profile_points), len(lens_rings)*len(profile_points))))
mesh("Crowned lens", [vertex for ring in lens_rings for vertex in ring], lens_faces, glass,
     smooth=True, uv_size=(5.77, 2.97, .12, 0))

for name, z, ring_surface in (("Select", .66, steel), ("Adjust", -.80, phosphor)):
    disc(f"{name} socket", -3.37, z, .525, -.43, -.62, rubber)
    disc(f"{name} knurl", -3.37, z, .442, -.59, -.94, steel, knurled=True)
    disc(f"{name} inlay", -3.37, z, .358, -.91, -.975, ring_surface)
    disc(f"{name} cap", -3.37, z, .301, -.955, -.991, bezel)
    profile(f"{name} index", [(.042, .127, .013, -.988), (.043, .121, .012, -1.013)],
            ink, (-3.36, z+.238))

disc("Amber indicator bezel", -3.37, 1.34, .152, -.49, -.616, steel)
disc("Amber indicator lens", -3.37, 1.34, .110, -.608, -.660, amber)
profile("Accent button well", [(.83, .35, .10, -.40), (.82, .34, .10, -.60)], bezel, (1.90, 1.63))
profile("Accent cycle switch", [(.67, .25, .08, -.54), (.63, .22, .07, -.73)], steel, (1.90, 1.63))
for i, color in enumerate(((.13, .82, .33), (.12, .73, .93), (.47, .27, .98), (.93, .26, .79), (.96, .43, .09))):
    lamp = material(f"Orbit Accent Lamp {i+1}", color, .32, emission=.68)
    disc(f"Accent lamp {i+1}", 2.56+i*.234, 1.63, .083, -.51, -.576, lamp)

profile("Set socket", [(.81, 1.18, .18, -.49), (.77, 1.14, .17, -.71)], rubber, (3.49, .39))
profile("Set surround", [(.72, 1.02, .14, -.64), (.69, .99, .14, -.74),
                         (.60, .87, .11, -.77), (.60, .87, .11, -.61)], steel, (3.49, .39), closed=True)
profile("Raised set key", [(.57, .84, .11, -.65), (.56, .81, .10, -.87),
                           (.47, .70, .08, -.92)], set_key, (3.49, .39))
engraving("Set engraving", "S", 3.49, -.925, .23, .45)
profile("Feedback socket", [(.66, .41, .09, -.49), (.65, .39, .09, -.63)], rubber, (3.49, -.74))
profile("Sound and haptic toggle", [(.42, .24, .07, -.60), (.41, .22, .07, -.76)], steel, (3.49, -.74))
engraving("Feedback label", "AUX", 3.49, -.55, -1.12, .17)

for x in (-3.42, 3.49):
    for z in (-1.35, 1.33):
        if x < 0 and z > 0:
            continue
        disc("Recessed screw", x, z, .108, -.48, -.55, steel)
        profile("Screw slot", [(.12, .025, .01, -.549), (.12, .025, .01, -.558)], rubber, (x, z))
engraving("Select label", "SELECT", -3.38, -.53, .04, .137)
engraving("Adjust label", "ADJUST", -3.38, -.53, -1.44, .137)
engraving("Accent label", "COLOR", 1.16, -.533, 1.61, .15)

for x in (-2.85, 2.76):
    profile("Brace lug", [(.72, .55, .17, 1.00), (.82, .60, .18, .31)], steel, (x, -.14))
profile("Wrist saddle", [(4.36, 1.93, .45, 1.01), (4.68, 2.09, .49, .46)], rubber, (-.53, -.16))

destination = Path(__file__).resolve().parents[1] / "assets" / "phosphor_orbit.glb"
bpy.ops.export_scene.gltf(filepath=str(destination), export_format='GLB',
                         export_materials='EXPORT', export_apply=True)
