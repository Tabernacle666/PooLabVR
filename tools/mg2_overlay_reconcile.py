#!/usr/bin/env python3
"""Bring new authored MG2 assets into an existing PBR overlay without erasing CC0.
The overlay is untracked: it may hold newer PBR material revisions than main.
Authoring source wins for newer geometry; valid acquired CC0 wins for unchanged
geometry. All writes are local and the pack manifest is published last.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

from mg2_hero_repair import inspect_glb


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=path.parent, suffix=".tmp",
                                     delete=False, encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
        temp = f.name
    os.replace(temp, path)


def copy_if_exists(a, b):
    if a.is_file():
        b.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(a, b)
    elif b.exists():
        b.unlink()


def main(source, overlay):
    source, overlay = Path(source).resolve(), Path(overlay).resolve()
    if not (overlay / "pack.json").exists():
        return
    if source == overlay or source in overlay.parents:
        raise ValueError("MG2 source and overlay cannot overlap")
    source_data = (source / "pack.json").read_bytes()
    runtime_data = (source / "runtime.json").read_bytes()
    profiles_data = (source / "room_profiles_v1.json").read_bytes()
    placement_data = (source / "placement_recipe_v2.json").read_bytes()
    fingerprint = hashlib.sha256(
        source_data + runtime_data + profiles_data + placement_data).hexdigest()
    stamp = overlay / ".mg2_source_revision.json"
    if stamp.exists():
        try:
            previous = json.loads(stamp.read_text(encoding="utf-8"))
            if previous.get("source_fingerprint") == fingerprint:
                return
        except (OSError, ValueError):
            pass
    canonical = json.loads(source_data)
    previous = json.loads((overlay / "pack.json").read_text(encoding="utf-8"))
    originals = {row["asset_key"]: row for row in previous["assets"]}
    new = copy.deepcopy(canonical)
    preserved = changed = 0
    for i, asset in enumerate(new["assets"]):
        old = originals.get(asset["asset_key"])
        keep = False
        if old and old["asset_revision"] >= asset["asset_revision"]:
            old_glb = overlay / old["glb"]
            old_prov = overlay / old["provenance"]
            if old_glb.is_file() and old_prov.is_file():
                try:
                    old_source = json.loads(old_prov.read_text(encoding="utf-8"))
                    if "pbr_lot1" in old_source:
                        inspect_glb(old_glb.read_bytes())
                        keep = True
                except (ValueError, KeyError, OSError):
                    keep = False
        if keep:
            new["assets"][i] = copy.deepcopy(old)
            preserved += 1
            continue
        for key in ("glb", "metadata", "provenance"):
            copy_if_exists(source / asset[key], overlay / asset[key])
        changed += 1
    # Runtime/recipes are authored source and can advance independently of
    # older acquired PBR textures. Never hide a new desk candidate.
    for name in ("runtime.json", "room_profiles_v1.json",
                 "placement_recipe_v2.json"):
        copy_if_exists(source / name, overlay / name)
    recipes = source / "recipes"
    if recipes.is_dir():
        shutil.copytree(recipes, overlay / "recipes", dirs_exist_ok=True)
    new["pack_revision"] = max(previous["pack_revision"],canonical["pack_revision"])+1
    write_json(overlay / "pack.json", new)
    write_json(stamp, {"source_fingerprint": fingerprint,
                       "preserved_pbr": preserved,"new_or_changed": changed})
    print(f"[MG2] PBR overlay reconciled: {preserved} CC0 assets preserved, "
          f"{changed} sources updated; pack {new['pack_revision']}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python mg2_overlay_reconcile.py SOURCE_PACK LOCAL_PACK")
    main(sys.argv[1], sys.argv[2])
