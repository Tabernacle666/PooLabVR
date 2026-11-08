#!/usr/bin/env python3
"""Guard the independently staged runtime DLL against regression."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
cm=(root/"CMakeLists.txt").read_text(encoding="utf-8")
tag='copy_if_different "$<TARGET_FILE:ktx>" "$<TARGET_FILE_DIR:LaboVR>/ktx.dll"'
assert cm.count(tag)==2, "ktx.dll must be staged in POST_BUILD and StageLaboVR"
post=cm.split("add_custom_command(TARGET LaboVR POST_BUILD",1)[1].split("add_custom_target(StageLaboVR",1)[0]
stage=cm.split("add_custom_target(StageLaboVR",1)[1].split("add_custom_target(LaboVRPackage",1)[0]
assert tag in post and tag in stage
assert re.search(r"target_link_libraries\(Core PUBLIC[^\n]*\bktx\b",cm)
assert re.search(r"target_link_libraries\(LaboVRCook PRIVATE[^\n]*\bktx\b",cm)
diag=(root/"tools/DiagnosticCycle.ps1").read_text(encoding="utf-8")
assert "'ktx.dll'" in diag
assert "Runtime package incomplete" in diag
print("Runtime staging ktx.dll: PASS")
