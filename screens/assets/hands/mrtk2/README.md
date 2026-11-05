# mrtk2: MRTK2's rigged hand

`HANDS_VIEW_MODEL=mrtk2`. `left.glb` and `right.glb` are MRTK2's rigged hand visualizer's meshes, converted:

- Source: https://github.com/microsoft/MixedRealityToolkit-Unity, `Assets/MRTK/SDK/Features/UX/Prefabs/RiggedHandVisualizer/L_Hand_MRTK_Rig.fbx` and `R_Hand_MRTK_Rig.fbx`, at commit `5c6ab1f6fc064c9a4d9c786ae966c98c03d8b500`. sha256: `c4db0e45bc352601a69836ad95f018fbd7eee5425cd1c415e321a889dea0da25` L, `32fb64b8a9963b0751958d0023178cfc29fb2c07edfabc64e83ad7f2b7e9f04d` R.
- Licence: MIT, Copyright (c) Microsoft Corporation: `Assets/MRTK/SDK/License.txt` at that commit (the same as the repository's `LICENSE.md`), copied here as [LICENSE](LICENSE). The repository's `NOTICE.md` lists nothing for these files.
- Converted (2026-10-09): `assimp export L_Hand_MRTK_Rig.fbx L_Hand_MRTK_Rig.glb` (Open Asset Import Library 6.0.5, glTF 2 binary), then `screens/tools/hand-glb-clean.py L_Hand_MRTK_Rig.glb left.glb --scale 0.01 --rest-from-bind` (centimetres to metres, assimp's helper nodes folded in, the rest pose made the bind pose, weights renormalised), the same for the right. sha256: `f14ca55e30867e8e72587ec83990ca2dc3ba9dc8c853090512f750eff9d43ff5` left.glb, `42a9e73bf93fd3d727b9438ede6e540eddb466a2413e5833a643910adf651eb0` right.glb.

Each is one skinned mesh of 1334 vertices and 2664 triangles on 22 joints: `L_Wrist`, `L_Thumb_1` .. `_3` and `_end`, three bones and an end for each finger (`L_Pointer_1` .. `L_Pointer_end`: knuckle, middle and last joints, tip), and `L_WristPadding`, the little finger's metacarpal. The other fingers have no metacarpal joints: hand-asset.py puts WebXR's a third of the way from the wrist to the knuckle, with no weights.
