# OASIS — Animated characters v1
These are **actual glTF 2.0 skinned and animated GLB sources**; unlike the MG2 architecture/props pack, they contain joint hierarchies, inverse bind matrices, JOINTS_0 / WEIGHTS_0, and named animation samplers. Generated from `tools/generate_oasis_characters.mjs` without third-party models.

## Human adult 01
19 bones, 8,372 triangles, 7 original in-place clips:
`idle`, `walk_in_place`, `stairs_up_in_place`, `stairs_down_in_place`, `sit_down`, `seated_idle`, `stand_up`.

## Grey raccoon 01
22 bones, 9,824 triangles, 4 clips:
`idle`, `walk_in_place`, `sniff`, `tail_sway`.
A grey coat, facial mask and dark banded tail are modeled with separate materials.

## Engine hand-off
**No runtime integration is implied and no engine C++ files are changed.** The static MG2 GLB loader does not yet provide a character animation system. These two actors must be imported with joint matrices and skinning support; transforms are local to parents. All locomotion is in place; the engine supplies world displacement, slope/step height, foot contact and chair/seat target. The first pass uses *rigid, single-joint vertex weights*, so elbows/knees and fur still need deforming skin weights, shape cleanup, anatomical polish, photoreal materials and runtime IK. The provided motions are procedural reference clips, not motion-captured, and should not be called final animation.

Characters are independent of the architectural pack to avoid imposing the cost on the office and forest static geometry.
