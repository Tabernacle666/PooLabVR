#!/usr/bin/env python3
"""Verify the entire glTF buffer/index contract without a 3D package."""
import json
import math
import struct
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
    assert len(gltf["meshes"])==34
    ground = next(mesh for mesh in gltf["meshes"] if mesh["name"]=="BlockTerrain")
    limits = gltf["accessors"][ground["primitives"][0]["attributes"]["POSITION"]]
    assert limits["min"][0]==-50 and limits["max"][0]==50
    assert limits["min"][2]==-50 and limits["max"][2]==50
    assert limits["count"]==41*41, "graded forest surface missing"
    ground_indices=gltf["accessors"][ground["primitives"][0]["indices"]]
    assert ground_indices["count"]==40*40*6
    base=next(m for m in gltf["meshes"] if m["name"]=="ContinuousGround")
    bpos=gltf["accessors"][base["primitives"][0]["attributes"]["POSITION"]]
    assert bpos["count"]==49*49
    assert bpos["min"][0]==-60 and bpos["max"][0]==60
    assert bpos["min"][2]==-60 and bpos["max"][2]==60
    bview=gltf["bufferViews"][bpos["bufferView"]]
    points=list(struct.iter_unpack("<fff",data[bview["byteOffset"]:
                                             bview["byteOffset"]+bpos["count"]*12]))
    assert abs(points[24*49+24][1]-
               (points[24*49][1]+points[24*49+48][1])*.5)>.03
    snow = next(mesh for mesh in gltf["meshes"] if mesh["name"]=="IrregularSnowPatch")
    snow_pos=gltf["accessors"][snow["primitives"][0]["attributes"]["POSITION"]]
    snow_data=gltf["bufferViews"][snow_pos["bufferView"]]
    start=snow_data["byteOffset"]
    points=list(struct.iter_unpack("<fff",data[start:start+snow_pos["count"]*12]))
    assert snow_pos["count"]==1+5*36
    rim=points[-36:]
    radii={round(math.hypot(x/1.15,z/.82),3) for x,y,z in rim}
    assert len(radii)>12, "snow must not have a square or circular stamped perimeter"
    assert max(abs(y-.004) for x,y,z in rim)<1e-5, "snow perimeter must meet the ground"
    snow_ix=gltf["accessors"][snow["primitives"][0]["indices"]]
    ix_data=gltf["bufferViews"][snow_ix["bufferView"]]
    indices=struct.unpack("<"+"I"*snow_ix["count"],
        data[ix_data["byteOffset"]:ix_data["byteOffset"]+snow_ix["count"]*4])
    for k in range(0,len(indices),3):
        a,b,c=(points[indices[k+j]] for j in range(3))
        upward=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2])
        assert upward>0, "snow triangle has reversed winding or zero area"
    lab=next(m for m in gltf["meshes"] if m["name"]=="ContinuousGroundLab")
    position=gltf["accessors"][lab["primitives"][0]["attributes"]["POSITION"]]
    view=gltf["bufferViews"][position["bufferView"]]
    vertices=list(struct.iter_unpack("<fff",data[view["byteOffset"]:
                                view["byteOffset"]+position["count"]*12]))
    assert all(not (11.75<x+60<20.25 and 102.75<z+60<111.25)
               for x,y,z in vertices), "laboratory floor covered by soil"
    sidewalk=next(m for m in gltf["meshes"] if m["name"]=="Sidewalk108")
    a=gltf["accessors"][sidewalk["primitives"][0]["attributes"]["POSITION"]]
    assert a["count"]>=3000, "slab joints lost"
    assert any(m["name"]=="SidewalkNorthNegative" for m in gltf["meshes"])
    assert any(m["name"]=="CurbNorthPositive" for m in gltf["meshes"])
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
