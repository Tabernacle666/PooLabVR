# OASIS original dressing source pack v1

**100 original posters** (5 GLB atlas sheets, 20 named selectable node meshes per sheet), **80 restrained shades of white** and **7 micro-detail wear meshes**. Source generators: `tools/generate_oasis_posters.mjs` and `tools/generate_oasis_wear.mjs`. This pack is original authored source data, not runtime-instanced decoration yet.

## Why five atlas sheets rather than 100 separately loaded materials?
All 20 poster variants in one sheet reference one shared 800x896 RGBA embedded PNG; a scene integrator selects exactly one node (`poster_001` etc.) and reuses the shared material/texture. **Never draw all 20 nodes on top of one another.** Generated poster designs are low-resolution graphic illustration and text rather than final high-resolution photographed artwork. They can be replaced without changing poster identities.

## Where wear comes from
`wear_rules.json` defines local physical causes, probabilities per eligible host and persistent procedural seeds. Examples: floor traffic paths, chair-back contact, cabinet and door handles, cracked glaze, rare protruding nails and cart dents. `white_shades_80.json` exposes subtle per-object sRGB whites as data values, not 80 standalone GPU materials. The engine should pass a stable per-object seed and material variation scalars; a worn surface should be determined by its use, not uniform independent random noise.

## Integration boundary
This pack is **not included in `oasis_mg2_v1/pack.json`** because the current static MG2 importer does not offer submesh selection or contextual material variants. The future engine-side asset registration can connect the named node or physical host surface on demand. Decal geometry needs bias and collision/portal-aware attachment. No renderer or world logic was modified by this asset pass.
