#!/usr/bin/env python3
"""OASIS MG2: acquire real CC0 PBR maps and embed them into existing pilot GLBs.

Authoring-time only; no runtime network requests or scene/procedural regeneration.
Requires Pillow (`python -m pip install Pillow`). The pack's GLBs are modified
in place, with metadata/provenance/digests updated after successful processing.
"""
import argparse
import copy
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import urllib.request
import zipfile

try:
    from PIL import Image, ImageOps
except ImportError as exc:
    raise SystemExit("Pillow manquant: python -m pip install Pillow") from exc

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "assets/packs/oasis_mg2_v1"
CACHE = PACK / ".pbr_cache"
UA = "OASIS-PBR-Pilot/1.0 (Poly Haven and ambientCG attribution in HANDOFF_PBR_LOT1.md)"
SOURCES = {
    "wood_table_worn": ("Poly Haven", "wood_table_worn", "https://polyhaven.com/a/wood_table_worn"),
    "WoodFloor003": ("ambientCG", "WoodFloor003", "https://ambientcg.com/view?id=WoodFloor003"),
    "Metal010": ("ambientCG", "Metal010", "https://ambientcg.com/view?id=Metal010"),
    "Porcelain001": ("ambientCG", "Porcelain001", "https://ambientcg.com/view?id=Porcelain001"),
    "Fabric010": ("ambientCG", "Fabric010", "https://ambientcg.com/view?id=Fabric010"),
    "Concrete003": ("ambientCG", "Concrete003", "https://ambientcg.com/view?id=Concrete003"),
    "PaintedPlaster017": ("ambientCG", "PaintedPlaster017", "https://ambientcg.com/view?id=PaintedPlaster017"),
    "Carpet001": ("ambientCG", "Carpet001", "https://ambientcg.com/view?id=Carpet001"),
    "Plastic006": ("ambientCG", "Plastic006", "https://ambientcg.com/view?id=Plastic006"),
    "Plastic011": ("ambientCG", "Plastic011", "https://ambientcg.com/view?id=Plastic011"),
}
# Each (source, repeat, metallic factor): never replace similarly named materials
# indiscriminately across architectural objects or dark plastic electronics.
TARGETS = {
    "desk_office01": {"wood_oiled": ("wood_table_worn", 1.0, 0.0), "wood_light": ("wood_table_worn", 1.0, 0.0)},
    "chair_ergonomic01": {"fabric_dark": ("Fabric010", 2.0, 0.0), "fabric_blue": ("Fabric010", 2.0, 0.0)},
    "meeting_table01": {"wood_oiled": ("wood_table_worn", 1.5, 0.0), "steel_brushed": ("Metal010", 1.5, 1.0)},
    "door_office_solid01": {"wood_oiled": ("wood_table_worn", 2.0, 0.0), "steel_brushed": ("Metal010", 2.0, 1.0)},
    "slab_floor01": {"wood_oiled": ("WoodFloor003", 3.0, 0.0), "stone": ("Concrete003", 3.0, 0.0)},
    "partition_solid01": {"ceramic": ("PaintedPlaster017", 2.0, 0.0)},
    "sink_wall01": {"ceramic": ("Porcelain001", 1.0, 0.0)},
    "toilet_floor01": {"ceramic": ("Porcelain001", 1.0, 0.0)},
    "coffee_mug02": {"ceramic": ("Porcelain001", 1.0, 0.0)},
    "lounge_rug01": {"fabric_dark": ("Carpet001", 2.0, 0.0)},
    "desk_lamp_arch02": {"steel_brushed": ("Metal010", 1.0, 1.0)},
}


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


# Only original UV-bearing material slots are eligible. Screens, labels,
# alpha-cut foliage, lights and other emissive materials are not overwritten.
BULK_SLOTS = {
    "wood_oiled": ("wood_table_worn", 1.0, 0.0),
    "wood_light": ("wood_table_worn", 1.2, 0.0),
    "steel_brushed": ("Metal010", 1.0, 1.0),
    # Painted steel retains its authored coating colour. A broad scanned
    # painted metal map made computer housings and stands bright red.
    # Do not remap the coloured coating through a generic material family.
    "chrome": ("Metal010", 1.0, 1.0),
    "plastic_dark": ("Plastic006", 1.0, 0.0),
    "plastic_mid": ("Plastic011", 1.0, 0.0),
    "fabric_dark": ("Fabric010", 1.2, 0.0),
    "fabric_blue": ("Fabric010", 1.2, 0.0),
    "ceramic": ("Porcelain001", 1.0, 0.0),
}
BULK_FAMILIES = ("furniture", "electronics", "props", "lighting", "restroom", "plants")


def collect_targets(pack):
    result = {name: dict(maps) for name, maps in TARGETS.items()}
    for row in pack["assets"]:
        family = row["asset_key"].split(".")[1]
        if family not in BULK_FAMILIES:
            continue
        meta = read_json(PACK / row["metadata"])
        slots = set(meta.get("materials", {}))
        planned = {}
        for name, source in BULK_SLOTS.items():
            if name not in slots or (family == "electronics" and name == "ceramic"):
                continue
            if family == "plants" and name != "ceramic":
                continue
            planned[name] = source
        if planned:
            # Fail-open for authoring compatibility, not for runtime: silently
            # leave existing good textures on assets without usable UV0.
            doc, _ = parse_glb((PACK / row["glb"]).read_bytes())
            active = {}
            for index, material in enumerate(doc.get("materials", [])):
                slot = material.get("name")
                if slot not in planned:
                    continue
                primitives = [primitive for mesh in doc.get("meshes", [])
                    for primitive in mesh.get("primitives", [])
                    if primitive.get("material") == index]
                if primitives and all("TEXCOORD_0" in primitive.get("attributes", {})
                    for primitive in primitives):
                    active[slot] = planned[slot]
            if active:
                model = Path(row["glb"]).stem
                result.setdefault(model, {}).update({
                    slot: source for slot, source in active.items()
                    if slot not in result.get(model, {})
                })
    return result


def write_json_atomic(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tmp", delete=False, mode="w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
        tmp = Path(f.name)
    os.replace(tmp, path)


def request(url, max_bytes=150_000_000):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=75) as resp:
        data = resp.read(max_bytes + 1)
        if len(data) > max_bytes:
            raise ValueError(f"Download exceeds size limit: {url}")
    return data


def cache_bytes(path, url, md5=None):
    if path.exists():
        b = path.read_bytes()
        if not md5 or hashlib.md5(b).hexdigest().lower() == md5.lower():
            return b
    b = request(url)
    if md5 and hashlib.md5(b).hexdigest().lower() != md5.lower():
        raise ValueError(f"Source checksum mismatch: {url}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b)
    return b


def key_name(name):
    return "".join(c for c in name.lower() if c.isalnum())


def get_ambient(asset, resolution):
    url = f"https://ambientcg.com/get?file={asset}_{resolution}-JPG.zip"
    raw = cache_bytes(CACHE / f"{asset}_{resolution}-JPG.zip", url)
    if not zipfile.is_zipfile(io.BytesIO(raw)):
        raise ValueError(f"Non-ZIP response for {url}; verify the download entitlement and URL")
    images = {}
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        for info in z.infolist():
            if info.file_size > 55_000_000 or info.filename.lower().endswith(('.exr', '.sbsar')):
                continue
            name = Path(info.filename).name
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            role = key_name(Path(name).stem.split("_")[-1])
            for kind, aliases in {
                "color": {"color", "albedo", "basecolor"},
                "normalgl": {"normalgl", "normalopengl"},
                "normaldx": {"normaldx", "normaldirectx"},
                "rough": {"roughness", "rough"},
                "metal": {"metalness", "metallic", "metal"},
                "ao": {"ambientocclusion", "ao"},
            }.items():
                if role in aliases and kind not in images:
                    images[kind] = z.read(info)
    return images, {"archive_url": url, "archive_sha256": digest(raw), "files": sorted(images)}


def get_polyhaven(asset, resolution):
    api_url = f"https://api.polyhaven.com/files/{asset}"
    entry = json.loads(request(api_url, 7_000_000))
    aliases = {
        "color": ("diffuse", "albedo"),
        "normalgl": ("norgl", "normalgl"),
        "normaldx": ("nordx", "normaldx"),
        "orm": ("arm", "aoroughmetal"),
        "rough": ("rough", "roughness"),
        "ao": ("ao",),
        "metal": ("metal", "metallic"),
    }
    images, files = {}, []
    for role, names in aliases.items():
        family = next((v for k, v in entry.items() if key_name(k) in names), None)
        if not isinstance(family, dict):
            continue
        variant = family.get(resolution.lower(), {})
        fmt = next((variant.get(k) for k in ("png", "jpg") if isinstance(variant.get(k), dict)), None)
        if not fmt or not fmt.get("url"):
            continue
        raw = cache_bytes(CACHE / f"{asset}_{resolution}_{role}.{fmt['url'].split('?')[0].split('.')[-1]}", fmt["url"], fmt.get("md5"))
        images[role] = raw
        files.append({"map": role, "url": fmt["url"], "sha256": digest(raw), "md5": fmt.get("md5")})
    return images, {"api_url": api_url, "files": files}


def image_rgb(raw):
    with Image.open(io.BytesIO(raw)) as im:
        return im.convert("RGB")


def grey(raw, size):
    if not raw:
        return None
    with Image.open(io.BytesIO(raw)) as im:
        return im.convert("L").resize(size, Image.Resampling.LANCZOS)


def to_png(img):
    o = io.BytesIO()
    img.save(o, format="PNG", optimize=True)
    return o.getvalue()


def to_jpeg(img):
    o = io.BytesIO()
    img.save(o, format="JPEG", quality=94, subsampling=0, optimize=True)
    return o.getvalue()


def build_maps(images):
    if not images.get("color") or not images.get("rough") and not images.get("orm"):
        raise ValueError("PBR source missing color or roughness/ARM")
    color = image_rgb(images["color"])
    size = color.size
    nkey = "normalgl" if images.get("normalgl") else "normaldx"
    if not images.get(nkey):
        raise ValueError("PBR source missing tangent normal map")
    normal = image_rgb(images[nkey]).resize(size, Image.Resampling.LANCZOS)
    if nkey == "normaldx":
        r, g, b = normal.split()
        normal = Image.merge("RGB", (r, ImageOps.invert(g), b))
    if images.get("orm"):
        orm = image_rgb(images["orm"]).resize(size, Image.Resampling.LANCZOS)
    else:
        rough = grey(images.get("rough"), size)
        ao = grey(images.get("ao"), size) or Image.new("L", size, 255)
        metal = grey(images.get("metal"), size) or Image.new("L", size, 255)
        orm = Image.merge("RGB", (ao, rough, metal))
    return {"base": (to_jpeg(color), "image/jpeg"), "normal": (to_png(normal), "image/png"), "orm": (to_png(orm), "image/png")}


def parse_glb(raw):
    if len(raw) < 28 or struct.unpack_from("<III", raw, 0) != (0x46546c67, 2, len(raw)):
        raise ValueError("Invalid GLB 2.0")
    offset = 12
    chunks = []
    while offset < len(raw):
        length, kind = struct.unpack_from("<II", raw, offset)
        offset += 8
        chunks.append((kind, raw[offset:offset + length]))
        offset += length
    if offset != len(raw) or len(chunks) != 2 or [c[0] for c in chunks] != [0x4E4F534A, 0x004E4942]:
        raise ValueError("Expected packed JSON+BIN GLB")
    return json.loads(chunks[0][1]), chunks[1][1]


def make_glb(doc, binary):
    def padded(raw, fill):
        return raw + fill * ((-len(raw)) % 4)
    j = padded(json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8"), b" ")
    b = padded(binary, b"\0")
    length = 12 + 8 + len(j) + 8 + len(b)
    return struct.pack("<III", 0x46546C67, 2, length) + struct.pack("<II", len(j), 0x4E4F534A) + j + struct.pack("<II", len(b), 0x004E4942) + b


def inject_glb(raw, planned, cooked, signature):
    doc, binary = parse_glb(raw)
    extra = doc.setdefault("extras", {})
    old = extra.pop("oasis_pbr_pilot", None)
    if old:
        if old.get("signature") == signature:
            return None
        baseline = old["baseline"]
        binary = binary[:baseline["bin_length"]]
        for key in ("images", "textures", "bufferViews"):
            doc[key] = doc.get(key, [])[:baseline[key]]
        doc["buffers"][0]["byteLength"] = baseline["bin_length"]
        for index, material in old["original_materials"].items():
            doc["materials"][int(index)] = material
    original_materials = {}
    used_sources = {}
    lookup = {}
    baseline = {k: len(doc.get(k, [])) for k in ("images", "textures", "bufferViews")}
    baseline["bin_length"] = len(binary)

    def texture(source, role):
        nonlocal binary
        token = (source, role)
        if token in lookup:
            return lookup[token]
        data, mime = cooked[source][role]
        padding = (-len(binary)) % 4
        binary += b"\0" * padding
        offset = len(binary)
        binary += data
        view = len(doc.setdefault("bufferViews", []))
        doc["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
        image = len(doc.setdefault("images", []))
        doc["images"].append({"bufferView": view, "mimeType": mime, "name": f"oasis_pbr_{source}_{role}"})
        tx = len(doc.setdefault("textures", []))
        doc["textures"].append({"source": image})
        lookup[token] = tx
        return tx

    for index, material in enumerate(doc["materials"]):
        name = material.get("name")
        if name not in planned:
            continue
        source, repeat, metallic = planned[name]
        referenced = [p for mesh in doc.get("meshes", []) for p in mesh.get("primitives", []) if p.get("material") == index]
        if not referenced or any("TEXCOORD_0" not in p.get("attributes", {}) for p in referenced):
            raise ValueError(f"Missing UV0 on active material {name}")
        original_materials[str(index)] = copy.deepcopy(material)
        used_sources[name] = source
        pbr = material.setdefault("pbrMetallicRoughness", {})
        pbr["roughnessFactor"] = 1.0
        # glTF baseColorFactor multiplies the scanned image. The obsolete
        # parametric tint otherwise darkens even a perfect photographed map.
        original_alpha = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])[3]
        pbr["baseColorFactor"] = [1.0, 1.0, 1.0, original_alpha]
        pbr["metallicFactor"] = metallic
        def info(role):
            tex = {"index": texture(source, role), "texCoord": 0}
            if repeat != 1:
                tex["extensions"] = {"KHR_texture_transform": {"scale": [repeat, repeat]}}
            return tex
        pbr["baseColorTexture"] = info("base")
        pbr["metallicRoughnessTexture"] = info("orm")
        material["normalTexture"] = {**info("normal"), "scale": 1.0}
        material["occlusionTexture"] = {**info("orm"), "strength": 0.8}
    if not original_materials:
        raise ValueError("No pilot materials found in GLB")
    if any(x[1] != 1 for x in planned.values()):
        ext = doc.setdefault("extensionsUsed", [])
        if "KHR_texture_transform" not in ext:
            ext.append("KHR_texture_transform")
    doc["buffers"][0]["byteLength"] = len(binary)
    extra["oasis_pbr_pilot"] = {"schema": 1, "signature": signature, "baseline": baseline,
                                "original_materials": original_materials, "materials": used_sources}
    return make_glb(doc, binary), used_sources


def git_sha1(raw):
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolution", choices=("1K", "2K", "4K"), default="2K")
    parser.add_argument("--audit", action="store_true", help="Report pilot targets without downloading or modifying files")
    parser.add_argument("--download-only", action="store_true")
    args = parser.parse_args()
    pack_path = PACK / "pack.json"
    pack = read_json(pack_path)
    assets = {Path(row["glb"]).stem: row for row in pack["assets"]}
    targets = collect_targets(pack)
    print(f"PBR bulk: {len(targets)} existing GLBs planned; shared scans", flush=True)
    for name in targets:
        if name not in assets:
            raise ValueError(f"Missing existing asset {name}")
    if args.audit:
        for name, plan in targets.items():
            doc, _ = parse_glb((PACK / assets[name]["glb"]).read_bytes())
            actual = {m.get("name") for m in doc["materials"]}
            missing = set(plan) - actual
            if missing:
                raise ValueError(f"{name}: missing material(s) {missing}")
            print(name, ", ".join(f"{m}={source}" for m, (source, _, _) in plan.items()))
        print("Audit OK; no files modified")
        return
    needed = {v[0] for p in targets.values() for v in p.values()}
    cooked, source_meta = {}, {}
    for name in sorted(needed):
        provider, asset, url = SOURCES[name]
        print("Acquiring", name, "from", provider, flush=True)
        if provider == "Poly Haven":
            maps, meta = get_polyhaven(asset, args.resolution)
        else:
            maps, meta = get_ambient(asset, args.resolution)
        cooked[name] = build_maps(maps)
        source_meta[name] = {"provider": provider, "asset_id": asset, "source_url": url,
                             "license": "CC0-1.0", "license_url": "https://polyhaven.com/license" if provider == "Poly Haven" else "https://docs.ambientcg.com/license/",
                             "download_date": dt.datetime.now(dt.timezone.utc).date().isoformat(),
                             "maps_present": sorted(maps), "outputs_sha256": {k: digest(v[0]) for k, v in cooked[name].items()}, **meta}
    if args.download_only:
        print("Sources acquired in .pbr_cache; no GLB modified")
        return
    signature = digest(json.dumps({"source_outputs": {k: v["outputs_sha256"] for k, v in source_meta.items()},
                                   "targets": targets, "resolution": args.resolution}, sort_keys=True).encode())
    pending = []
    new_revision = pack["pack_revision"] + 1
    for name, planned in targets.items():
        row = assets[name]
        path = PACK / row["glb"]
        raw = path.read_bytes()
        change = inject_glb(raw, planned, cooked, signature)
        if change is None:
            continue
        updated, used = change
        meta_path = PACK / row["metadata"]
        meta = read_json(meta_path)
        provenance_path = PACK / row["provenance"]
        provenance = read_json(provenance_path)
        new_doc, _ = parse_glb(updated)
        row["asset_revision"] = new_revision
        row["git_blob_sha1"] = git_sha1(updated)
        meta["asset_revision"] = new_revision
        meta["geometry"].update({"bytes": len(updated), "embedded_textures": len(new_doc.get("images", [])),
                                 "material_slots": len(new_doc["materials"])})
        provenance["pbr_lot1"] = {"sources_by_material": used, "resolution": args.resolution,
                                  "signature": signature, "source_manifest": "pbr_acquisitions_lot1.json"}
        pending.append((path, updated, meta_path, meta, provenance_path, provenance))
    if not pending:
        print("PBR pilot already current; no changes")
        return
    catalog_path = PACK / "sources_reelles_candidates.json"
    catalog = read_json(catalog_path)
    # Register acquired CC0 providers without requiring a separate catalogue
    # commit that might conflict with locally downloaded materials.
    next_id = max([int(row.get("catalog_id", "source_000").removeprefix("source_"))
                   for row in catalog["sources"]] + [0])
    for source_id in sorted(source_meta):
        provider, asset_id, url = SOURCES[source_id]
        if any(item.get("url") == url for item in catalog["sources"]):
            continue
        next_id += 1
        catalog["sources"].append({
            "catalog_id": f"source_{next_id:03d}", "id": source_id,
            "title": asset_id, "category": "material_PBR",
            "provider": provider, "url": url, "license": "CC0-1.0",
            "purpose": "OASIS bulk scanned/PBR material source",
            "selection_status": "source_acquired", "ingest_status": "embedded_pilot_glb",
            "file_sha256": source_meta[source_id].get("archive_sha256"),
            "authored_at": dt.datetime.now(dt.timezone.utc).date().isoformat()
        })
    for source in catalog["sources"]:
        name = source.get("title", "")
        url = source.get("url", "")
        match = next((k for k, v in SOURCES.items() if v[2] == url and k in source_meta), None)
        if match:
            source.update({"selection_status": "source_acquired", "ingest_status": "embedded_pilot_glb",
                           "file_sha256": source_meta[match].get("archive_sha256") or digest(json.dumps(
                               source_meta[match]["outputs_sha256"], sort_keys=True).encode()),
                           "download_date": source_meta[match]["download_date"]})
    pack["pack_revision"] = new_revision
    # Produce all GLBs before touching the pack manifest to make failure non-destructive.
    for glb_path, glb, _, _, _, _ in pending:
        with tempfile.NamedTemporaryFile(dir=glb_path.parent, suffix=".tmp", delete=False) as f:
            f.write(glb)
            tmp = f.name
        os.replace(tmp, glb_path)
    for _, _, meta_path, meta, provenance_path, provenance in pending:
        write_json_atomic(meta_path, meta)
        write_json_atomic(provenance_path, provenance)
    write_json_atomic(PACK / "pbr_acquisitions_lot1.json", {"schema_version": 1, "resolution": args.resolution,
                       "status": "embedded_pilot_glb_pending_visual_approval", "sources": source_meta,
                       "glbs": [row[0].name for row in pending]})
    write_json_atomic(catalog_path, catalog)
    write_json_atomic(pack_path, pack)
    print(f"Embedded real CC0 PBR maps in {len(pending)} GLBs (pack revision {new_revision}).")
    print('Run: node tools/sync_oasis_mg2_catalog.mjs')
    print('Then validate: commit_and_loop.bat "mg2"')


if __name__ == "__main__":
    main()
