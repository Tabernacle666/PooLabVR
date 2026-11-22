# OASIS MG2 source pack v1

Original GLB 2.0 geometry. Meters, glTF right-handed Y-up. Each model has named material primitives and socket nodes; metadata sidecars define gameplay roles, collision candidates and bounds. Geometry is reference-grade procedural source, not pre-cooked CLAS and not enabled in OASIS by this commit.

Regenerate all: `node tools/generate_oasis_mg2_assets.mjs` from repository root. The engine team owns schema acceptance, meshlet/cluster cooking, and runtime admission. Preserve `asset_key` and node names across revisions. Structural mounts for letter profiles require integration-side alignment. Do not copy these sidecars into runtime registries without the importer.
