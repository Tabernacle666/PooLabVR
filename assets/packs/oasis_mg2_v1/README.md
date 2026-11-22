# OASIS MG2 — Production assets / pack v6

82 original, reproducibly generated glTF 2.0 GLB models and 19 recipes. Y-up, metres, project-owned materials and stable metadata keys. Editable source: `tools/generate_oasis_mg2_assets.mjs`. To update production metadata after regeneration, use `node tools/sync_oasis_mg2_catalog.mjs --write --revision=N` for a changed binary (or omit revision for metadata-only fixes).

## Visual and geometry density

The regular kit contains three office desk styles, three chairs, IT peripherals, doors/partitions, services/sanitary equipment, eight forest canopy silhouettes plus undergrowth. PBR material seeds supply baseColor, tangent normal and ORM for selected near-field models. Newly added **dense reference** sources (`tree_maple_hero01`, `tree_fir_hero01`, `forest_patch_dense01`) provide about 91k extra triangles: branching crowns, masked leaf groups, and a tileable 72x72 microrelief ground with vegetation, stones and dead wood. They are **not enabled by default**. Integrators determine CLAS and LOD pages, local cache/streaming, ray hit and wind behavior before admission into the active forest.

No engine/runtime files are modified. Textures, branching geometry and masks remain procedural first-pass assets, not final photoreal content. Validate actual import, exposure, alpha and in-headset legibility before treating this as visually approved.
