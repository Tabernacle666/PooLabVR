# OASIS MG2 v1 — source pack

54 original procedural GLB 2.0 models and 11 placement recipes. Right-handed glTF Y-up, meters. Pack revision 4.

## PBR material pass

Selected close-view models use deterministic procedural texture seeds: embedded sRGB base-color PNG, linear tangent normal PNG and linear ORM PNG (occlusion R, roughness G, metalness B). 64/128px tileable source patterns, not final artist-authored 4K surfacing. Masked leaf alpha and BLEND glazing remain separate. **Only used material slots are exported per asset**; no change to engine-side instance data.

Regenerate GLBs using `node tools/generate_oasis_mg2_assets.mjs`. The generator is the editable authoring master. This pack is source geometry; engine team owns MG2 cluster cooking, DXR, import acceptance, host alignment and runtime residency. Do not describe the content as visually final AAA.
