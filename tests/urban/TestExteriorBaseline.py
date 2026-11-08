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
assert "SetFogWind(terminalGpu_->serviceContext,&inlet)" in world
assert "restored_lab=1" in world
print("Exterior skylight, sky range, forest, fog and diagnostic camera: PASS")
