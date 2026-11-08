#!/usr/bin/env python3
"""Verify the entire glTF buffer/index contract without a 3D package."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

source=Path(__file__).resolve().parents[2]/"labs"/"labovr"/"tools"/"UrbanMeshes.py"
with tempfile.TemporaryDirectory() as tmp:
    directory=Path(tmp)
    subprocess.run([sys.executable,str(source),str(directory)],check=True)
    gltf=json.loads((directory/"urban.gltf").read_text())
    data=(directory/"urban.bin").read_bytes()
    assert gltf["asset"]["version"]=="2.0"
    assert len(gltf["meshes"])==24
    ground = next(mesh for mesh in gltf["meshes"] if mesh["name"]=="BlockTerrain")
    limits = gltf["accessors"][ground["primitives"][0]["attributes"]["POSITION"]]
    assert limits["min"][0]==-50 and limits["max"][0]==50
    assert limits["min"][2]==-50 and limits["max"][2]==50
    wall = next(mesh for mesh in gltf["meshes"] if mesh["name"]=="LowBrickWall")
    primitive = wall["primitives"][0]
    wall_max = gltf["accessors"][primitive["attributes"]["POSITION"]]["max"]
    assert 3.19 <= wall_max[1] <= 3.21, wall_max
    assert wall_max[0] >= 49.9, wall_max
    assert gltf["buffers"][0]["byteLength"]==len(data)
    assert len(data)>10000
    for mesh in gltf["meshes"]:
        assert len(mesh["primitives"])==1
        primitive=mesh["primitives"][0]
        assert primitive["mode"]==4
        for field in ("POSITION","NORMAL","TEXCOORD_0"):
            a=gltf["accessors"][primitive["attributes"][field]]
            assert a["count"]>0
        positions=gltf["accessors"][primitive["attributes"]["POSITION"]]
        assert len(positions["min"])==len(positions["max"])==3
        for lo,hi in zip(positions["min"],positions["max"]):
            assert hi>=lo
        indices=gltf["accessors"][primitive["indices"]]
        assert indices["count"]%3==0
        assert indices["count"]>=3
    for view in gltf["bufferViews"]:
        assert view["byteOffset"]>=0
        assert view["byteLength"]>0
        assert view["byteOffset"]+view["byteLength"]<=len(data)
print("Urban meshes glTF and buffer ranges: PASS")
