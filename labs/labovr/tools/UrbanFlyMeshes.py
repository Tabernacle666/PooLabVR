#!/usr/bin/env python3
"""Extract only the CUDA insect meshes from the legacy home_physics glTF.
The source remains untouched; the build stages a compact independent library.
"""
import json
import sys
from pathlib import Path

src=Path(__file__).resolve().parent.parent/"assets"/"home_physics.gltf"
dest=Path(sys.argv[1]) if len(sys.argv)>1 else src.parent/"urban"/"flies"
dest.mkdir(parents=True,exist_ok=True)
data=json.loads(src.read_text(encoding="utf-8"))
blob=(src.parent/data["buffers"][0]["uri"]).read_bytes()
if len(data["meshes"])!=17: raise RuntimeError("Home fly mesh source order changed")
# Original 0..2 are chain/socket/bulb, 15..16 are pot/rock.
selected=data["meshes"][3:15]
expected=["SculptedThorax","HeadCarapace","SegmentedAbdomen",
          "FacetedCompoundEye","LeftMembrane","LeftWingVeins",
          "RightMembrane","RightWingVeins","JointedLeg",
          "TarsalHook","Antennae","PopulationProxy"]
if [mesh["name"] for mesh in selected]!=expected:
    raise RuntimeError("Fly mesh names/order changed; refusing wrong index mapping")
result={"asset":{"version":"2.0","generator":"PooLabVR UrbanFlyMeshes"},
        "buffers":[],"bufferViews":[],"accessors":[],"meshes":[]}
output=bytearray()
views={}
accessors={}
def copy_accessor(old):
    if old in accessors:return accessors[old]
    accessor=dict(data["accessors"][old])
    old_view=accessor["bufferView"]
    if old_view not in views:
        view=dict(data["bufferViews"][old_view])
        while len(output)%4:output.append(0)
        begin=len(output)
        offset=view.get("byteOffset",0)
        size=view["byteLength"]
        output.extend(blob[offset:offset+size])
        view["byteOffset"]=begin
        view["buffer"]=0
        views[old_view]=len(result["bufferViews"])
        result["bufferViews"].append(view)
    accessor["bufferView"]=views[old_view]
    accessors[old]=len(result["accessors"])
    result["accessors"].append(accessor)
    return accessors[old]
for mesh in selected:
    primitives=[]
    for primitive in mesh["primitives"]:
        p=dict(primitive)
        p["attributes"]={key:copy_accessor(old) for key,old in primitive["attributes"].items()}
        p["indices"]=copy_accessor(primitive["indices"])
        primitives.append(p)
    result["meshes"].append({"name":mesh["name"],"primitives":primitives})
result["buffers"]=[{"uri":"flies.bin","byteLength":len(output)}]
(dest/"flies.bin").write_bytes(output)
(dest/"flies.gltf").write_text(json.dumps(result,separators=(",",":"))+"\n",encoding="utf-8")
print(f"extracted {len(selected)} insect meshes, {len(output)} bytes")
