#!/usr/bin/env python3
"""Exterior baseline regression guards for the actual source paths."""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
read=lambda p:(root/p).read_text(encoding="utf-8")
shader=read("shaders/scene.hlsl")
scene=read("labs/labovr/UrbanScene.cpp")
world=read("labs/labovr/HomeWorld.cpp")
header=read("src/core/Core.h")
meshes=read("labs/labovr/tools/UrbanMeshes.py")
assert "m.unlit=2" in scene
assert "if(s.extra.w==2)" in shader and "if(s.extra.w==1)" in shader
assert "FarZ=100.0f" in header
assert "tuple(98.*q for q in n)" in meshes
assert "if(((tile.x^tile.z)&1ll)!=0)break;" in scene
assert "diagnosticCamera_.p.x,diagnosticCamera_.p.z" in world
assert "result->features=urban::GenerateTile(key);" in world
assert "for(const auto& feature:tile->features)" in world
assert "r.objects.push_back(steam)" not in world, "solid spheres reintroduced as steam"
assert "SetFogWind(terminalGpu_->serviceContext,&urbanFogWind_)" in world
assert "restored_lab=1" in world
assert "MeshId::BlockTerrain" in scene, "ground missing below winter forest"
assert "WetAsphalt" in read("labs/labovr/UrbanScene.h")
assert "SurfaceMaterial::WetAsphalt))m.colorRoughness.w=.55f" in scene, "wet asphalt cannot pass glossy reflection roughness gate"
assert "light.power=132.0f" in scene
assert "std::cos(yaw)*1.50f" in scene, "street lamp still lit at pole instead of diffuser"
assert "settings.lightIndex=1" in world, "outside volume still lit by indoor bulb"
assert "settings.height=8.8f" in world, "outdoor fog does not reach tree crowns"
assert "dx*dx+dz*dz<=10.*10." in world, "wet road overlays render beyond 10m"
assert "SurfaceMaterial::WetAsphalt" in world, "dynamic wet surface disabled"
atmos=read("shaders/atmosphere.hlsl")
assert "firstExtra=index==1u?2u:4u" in atmos, "street light fog slots missing"
assert "spread=1.45+.50*rise" in atmos, "outdoor rising plume envelope missing"
assert "closeExterior=s.extra.w!=2" in shader, "near-field reflection cap missing"
assert "meshes.append(snow_drift(" in meshes, "rectangular snow patch restored"
assert "BlockTerrain\",1,1" in meshes, "wasteful ground tessellation restored"
import csv
import io
import math
views=list(csv.DictReader(io.StringIO(read("tools/diagnostic_cameras.csv"))))
aerial=next(v for v in views if v["name"]=="aerial_100x100")
cx,cy,cz=(float(aerial[k]) for k in ("x","y","z"))
# The cloud dome's radius is 98 m. All four 100x100-m parcel
# corners must remain in front of the sky dome in the overhead view.
for x in (-6.,94.):
    for z in (23.,123.):
        assert math.dist((cx,cy,cz),(x,0.,z))<96., "aerial camera clips the parcel"
assert abs(cx-44.)<.01 and abs(cz-73.)<.01
assert "f.position.z+faceOffset" in scene, "vines still behind street wall"
assert "brick({-4.14f" in world, "black side facade not clad"
assert "float hemisphere=.27+.49" in shader, "daylight skylight restored"
assert "3.05,.03" in meshes, "vines LOD rises above the low wall"
assert "brick({2.20f,.45f,4.10f}" in world, "window sill missing"
assert "centerY>.84f" in world, "masonry covers the new window"
assert "GridOffsetX" in read("labs/labovr/UrbanLayout.h")
assert "brick-window-lintel" in read("labs/labovr/HomePhysics.cpp")
cameras=read("tools/diagnostic_cameras.csv").strip().splitlines()
assert len(cameras)>=6 and cameras[-1].startswith("aerial_100x100,")
assert "Diagnostic CSV requires at least five viewpoints" in read("labs/labovr/main.cpp")
print("Exterior skylight, sky range, forest, fog and diagnostic camera: PASS")
