# OASIS MG2 / source production pack v5

79 original parameterized GLB 2.0 models and 18 placement recipes in glTF Y-up/right-handed meters. Includes three distinct desks, three office/meeting chairs, eight structural tree silhouettes (two each for spruce/fir/birch/maple), three shrubs, forest understory and office / sanitary / utility fixtures. Higher-detail near-field models use bevel meshes, distinct functional subparts, sockets and optional embedded PBR base/normal/ORM textures.

`tools/generate_oasis_mg2_assets.mjs` is the editable master. Run it at repository root with Node 22 or newer to regenerate GLB files. `tools/sync_oasis_mg2_catalog.mjs` updates the pack's per-model byte/triangle metadata and SHA-1 only when explicitly invoked with `--write` after review; it does not edit runtime code. Asset paths, keys and sockets remain stable as the catalog grows.

This is an expanding **source reference pack**, not the final AAA art pass. Engine integration must accept source bounds, attachments, world transforms, light emission, collision, alpha class and per-room placement; RTX MG2 pages/CLAS and runtime residency are engine-owned. The models are not yet certified in-game.
