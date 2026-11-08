#!/usr/bin/env python3
"""Verify the separate insect glTF contains only the expected ordered meshes."""
import json
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

script=Path(__file__).resolve().parents[2]/"labs"/"labovr"/"tools"/"UrbanFlyMeshes.py"
with TemporaryDirectory() as tmp:
    dest=Path(tmp)
    subprocess.run([sys.executable,str(script),str(dest)],check=True)
    doc=json.loads((dest/"flies.gltf").read_text())
    blob=(dest/"flies.bin").read_bytes()
    assert len(doc["meshes"])==12
    assert doc["meshes"][0]["name"]=="SculptedThorax"
    assert doc["meshes"][-1]["name"]=="PopulationProxy"
    assert doc["buffers"][0]["byteLength"]==len(blob)
    assert len(blob)>5000
    for view in doc["bufferViews"]:
        assert view["byteOffset"]+view["byteLength"]<=len(blob)
    for mesh in doc["meshes"]:
        assert mesh["primitives"][0]["mode"]==4
print("Insect glTF extraction: PASS")
