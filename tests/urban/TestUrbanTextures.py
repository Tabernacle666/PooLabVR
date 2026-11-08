#!/usr/bin/env python3
"""Validate urban PBR batch output without image libraries."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib
import struct

source = Path(__file__).resolve().parents[2]/"labs"/"labovr"/"tools"/"UrbanTextures.py"
with tempfile.TemporaryDirectory() as temp:
    result = Path(temp)
    subprocess.run([sys.executable,str(source),str(result),"32"],check=True)
    manifest=json.loads((result/"manifest.json").read_text())
    assert len(manifest["materials"])>=16
    assert "wet_asphalt" in manifest["materials"]
    for entry in manifest["materials"].values():
        assert set(entry["maps"])=={"basecolor","normal","orm","height"}
        for name in entry["maps"].values():
            raw=(result/name).read_bytes()
            assert raw[:8]==b"\x89PNG\r\n\x1a\n"
            assert struct.unpack(">II",raw[16:24])==(32,32)
            assert raw[-12:]==b"\x00\x00\x00\x00IEND\xaeB\x60\x82"
print("Urban texture manifest and PNG structure: PASS")
