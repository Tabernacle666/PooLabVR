# OASIS MG2 — Production assets / pack v6

82 original, reproducibly generated glTF 2.0 GLB models and 19 recipes. Y-up, metres, project-owned materials and stable metadata keys. Editable source: `tools/generate_oasis_mg2_assets.mjs`. To update production metadata after regeneration, use `node tools/sync_oasis_mg2_catalog.mjs --write --revision=N` for a changed binary (or omit revision for metadata-only fixes).

## Visual and geometry density

The regular kit contains three office desk styles, three chairs, IT peripherals, doors/partitions, services/sanitary equipment, eight forest canopy silhouettes plus undergrowth. PBR material seeds supply baseColor, tangent normal and ORM for selected near-field models. Newly added **dense reference** sources (`tree_maple_hero01`, `tree_fir_hero01`, `forest_patch_dense01`) provide about 91k extra triangles: branching crowns, masked leaf groups, and a tileable 72x72 microrelief ground with vegetation, stones and dead wood. They are **not enabled by default**. Integrators determine CLAS and LOD pages, local cache/streaming, ray hit and wind behavior before admission into the active forest.

No engine/runtime files are modified. Textures, branching geometry and masks remain procedural first-pass assets, not final photoreal content. Validate actual import, exposure, alpha and in-headset legibility before treating this as visually approved.

## v7 — office close-view and shared-space assets

19 additional native glTF models bring the catalogue to **101 GLB** and **24 placement recipes**. The existing single-office recipe now references three near-field desk elements (hollow ceramic mug, articulated lamp, compact note stack). The engine's current worktop loader already reads these `socket_desktop` entries, so these three can be visible in the active desk setup without engine source changes. The five new rooms (executive, creative, lounge, break nook, corridor) are *placement data*, not guaranteed active room generation. Asset creation does not imply runtime adoption.

New materials continue to use the existing pack palette and embedded PBR seed patterns selectively. Dense props, particularly laptop keys, speaker grilles and planting, must be residency-budgeted by the engine team before broad instancing. The generator is the editable source. Original assets only; no third-party meshes or licensed logos.

## v8 — furnished meeting, library and kitchenette source

15 new original glTF assets: bookcase stocked with varied volumes, upholstered armchair, wall display, mobile whiteboard, conference camera, coffee machine, kitchenette cabinet with water/drain sockets, lounge rug, radiator, bonsai, coat rail, bar stool, cork pinboard, ceiling fan and rolling drawer unit. The catalogue now has **116 GLB models, 28 recipes**. Existing lounge, large meeting and creative-office staging recipes are enriched; new room recipes remain inactive until the engine team's placement/runtime adoption. Materials are source-level procedural, not final AAA.
