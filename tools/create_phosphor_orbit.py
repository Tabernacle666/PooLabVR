import bpy, math
from mathutils import Vector

bpy.ops.wm.read_factory_settings(use_empty=True)

def mat(name, color, metallic=0.0, rough=0.45, emission=None):
    m=bpy.data.materials.new(name); m.diffuse_color=(*color,1)
    m.use_nodes=True; bs=m.node_tree.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value=(*color,1)
    bs.inputs['Metallic'].default_value=metallic; bs.inputs['Roughness'].default_value=rough
    if emission:
        bs.inputs['Emission Color'].default_value=(*emission,1); bs.inputs['Emission Strength'].default_value=3.0
    return m

metal=mat('Military olive metal',(0.08,0.10,0.07),.82,.28)
edge=mat('Edge steel',(0.18,0.20,0.16),.9,.22)
glass=mat('Recessed imperfect glass',(0.02,0.16,0.10),.1,.18,(0.02,0.35,0.16))
green=mat('Phosphor green',(0.08,0.8,0.28),0,.3,(0.08,1.0,0.35))
orange=mat('Amber controls',(0.8,0.25,0.02),.1,.3,(1.0,0.25,0.02))
yellow=mat('Set button',(0.9,0.55,0.08),.2,.25,(1.0,0.65,0.1))

def cube(name,loc,scale,material,bev=.12):
    bpy.ops.mesh.primitive_cube_add(location=loc); o=bpy.context.object; o.name=name; o.scale=scale; bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    if bev:
        mod=o.modifiers.new('Machined radius','BEVEL'); mod.width=bev; mod.segments=4
    o.data.materials.append(material); return o

def cyl(name,loc,radius,depth,material,rot=(math.pi/2,0,0),verts=48):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts,radius=radius,depth=depth,location=loc,rotation=rot); o=bpy.context.object; o.name=name; o.data.materials.append(material)
    mod=o.modifiers.new('Edge radius','BEVEL'); mod.width=.06; mod.segments=3; return o

# Thick outer watch body, recessed window and raised bezel.
cube('PhosphorOrbit_Housing',(0,0,0),(3.8,.55,2.15),metal,.32)
cube('PhosphorOrbit_Bezel',(0,-.62,.15),(3.35,.10,1.72),edge,.22)
cube('PhosphorOrbit_Glass',(0,-.76,.15),(3.02,.035,1.43),glass,.18)

# Two left controls with real depth and illuminated rings.
for z in (1.05,-1.05):
    cyl('PhosphorOrbit_Knob',(-3.15,-.9,z),.48,.34,edge)
    cyl('PhosphorOrbit_KnobRing',(-3.15,-1.1,z),.58,.06,green)
    cube('KnobGrip',(-3.15,-1.29,z),(.06,.04,.28),metal,.03)

# Top-edge accent button and right-side sound/haptic toggle.
cube('AccentCycleButton',(1.55,-.82,2.04),(.42,.12,.16),edge,.08)
cube('SoundHapticButton',(3.42,-.88,-.95),(.20,.10,.36),orange,.08)

# Recessed S button on right, with surrounding well.
cube('SetButtonWell',(3.25,-.78,.55),(.42,.08,.62),edge,.12)
cube('SetButton',(3.25,-.91,.55),(.30,.07,.46),yellow,.10)

# Tachometer ring, center hub, and two needles beneath the glass.
for r in (1.18,1.28):
    bpy.ops.mesh.primitive_torus_add(major_radius=r,minor_radius=.045,major_segments=64,minor_segments=8,location=(.75,-.88,.18),rotation=(math.pi/2,0,0)); bpy.context.object.data.materials.append(green)
cyl('GaugeHub',(.75,-.98,.18),.14,.08,edge)
for angle,material in ((math.radians(38),green),(math.radians(-42),orange)):
    o=cube('GaugeNeedle',(.75+math.cos(angle)*.62,-1.02,.18+math.sin(angle)*.62),(.035,.035,.68),material,.02); o.rotation_euler[1]=-angle

# Radio-band frame and phosphor tick marks.
cube('ParameterBand',(.75,-.91,-1.12),(1.72,.04,.18),green,.04)
for x in [ -1.0,-.5,0,.5,1.0,1.5,2.0 ]: cube('BandTick',(.75+x,-.98,-1.12),(.018,.02,.09),green,.01)

# Export one coherent asset, preserving named parts for later control wiring.
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath='assets/phosphor_orbit.glb',export_format='GLB',use_selection=True,export_materials='EXPORT',export_apply=True)
