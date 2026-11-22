# OASIS MG2 v1 — asset authoring pack

54 original, procedural GLB 2.0 models. glTF right-handed Y-up, meters. Named mesh nodes, PBR materials, movable door subcomponents, embedded masked foliage images, light emitters, service sockets, closed structural reference volumes. 11 deterministic pilot/room/forest placement recipes. Metadata uses the engine team's MG2 catalogue v1 fields: `asset_key`, `asset_revision`, `glb`, `metadata`, `geometry.bytes` and `geometry.triangles`; `pack.axes=right_handed_y_up`.

Regenerate all GLBs: `node tools/generate_oasis_mg2_assets.mjs`. The source master is the generator and project-authored parameters. GLBs are source geometry, not pre-cooked CLAS. `pack.json` is designed for the MG2 importer but runtime visual/physics/DXR/portal activation is the engine team's responsibility. No engine files are changed by this package.

Scope: architectural partitions/doors/slabs/stairs, furniture and office electronics, lighting and building services, toilets/washbasins/accessories, four canopy tree species, undergrowth and landscape, and pilot fixtures. All geometry is an initial parametric reference: realistic AAA surfacing, unique high-resolution material detail, exact letter-profile fittings, wind simulation and final gameplay collision are still the next artist/integrator passes. Do not assert those are finished.
