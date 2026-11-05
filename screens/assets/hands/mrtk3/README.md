# mrtk3: MRTK3's OpenXR hand

`HANDS_VIEW_MODEL=mrtk3`. `right.glb` is MRTK3's rigged hand visualizer's mesh, converted; the left hand is the right one mirrored across x (`screens/tools/hand-asset.py` does it), as MRTK3 does.

- Source: https://github.com/MixedRealityToolkit/MixedRealityToolkit-Unity, `org.mixedrealitytoolkit.input/Visualizers/RiggedHandVisualizer/openxr_hand.fbx`, at commit `acdec2d5a8e290892be4d6ceceaed51fa5563700`. sha256 of the FBX: `3ca41046a672db55068c218e2876a1282dbb7fb51a3d1ce24c8bf1028dba06ce`.
- Licence: BSD 3-Clause, Copyright (c) 2025, Mixed Reality Toolkit Contributors: `org.mixedrealitytoolkit.input/LICENSE.md` at that commit (the same as the repository's), copied here as [LICENSE](LICENSE). Keep the notice with any copy, source or binary.
- Converted (2026-10-09): `assimp export openxr_hand.fbx openxr_hand.glb` (Open Asset Import Library 6.0.5, glTF 2 binary), then `screens/tools/hand-glb-clean.py openxr_hand.glb right.glb --identity Armature --rest-from-bind` (assimp's helper nodes folded in, the armature's 100x scale dropped, the rest pose made the bind pose, weights renormalised; the vertices and the skin as they were). sha256 of right.glb: `c0c712da462ccb43613ae8c5a91b9d6d239814d261f209e3b0330866e58c8941`.

One skinned mesh of 1399 vertices and 2794 triangles on 24 joints with OpenXR's names, each named for the bone that starts at it, so a joint along from WebXR's: its `index_proximal` sits at the base of the index metacarpal (WebXR's `index-finger-metacarpal`), `index_intermediate` at the knuckle, and so on, and the bones' end nodes (`index_tip_end`, not joints) are the tips. Its `*_metacarpal` bones run from the wrist and give their weights to it; the wrist is where they start.
