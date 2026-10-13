# godot: Godot XR Tools' hands

`HANDS_VIEW_MODEL=godot`. `left.glb` and `right.glb` are Godot XR Tools' hand models, rewritten as GLB:

- Source: https://github.com/GodotVR/godot-xr-tools, `addons/godot-xr-tools/hands/model/hand_l.gltf` and `hand_r.gltf` (buffers embedded), at commit `6fc5361db295c12b5e51ee5b9fee09751a4458e7`. sha256: `ec4dbb0686088b0fde8f8c27403b03bd200e41bd8a5771bfafe25cd48ac40e56` hand_l.gltf, `eb8d9765dcb7dcb67928f90e5fbad6ae1f7705043ab900dbe4a43b68a2721d37` hand_r.gltf.
- Licence: CC0 1.0 (public domain dedication): `addons/godot-xr-tools/hands/License.md` at that commit, copied here as [LICENSE](LICENSE): "To the extent possible under law, DigitalN8m4r3 aka Miodrag Sejic has waived all copyright and related or neighboring rights to Hand Models for the Godot XR Tools." Its `About.md`: the mesh was made with MakeHuman Community Edition, whose output MakeHuman claims nothing over (CC0 assets). The repository's MIT licence covers its code; the hands have their own.
- Converted (2026-10-09): `screens/tools/hand-glb-clean.py hand_l.gltf left.glb` and the same for the right (no geometry or skin changes; one plain material, the textures left out). sha256: `b183e9fe043125feb063b0b5edac1c330ebf7a38400d487d257221876157ec57` left.glb, `dc6e48ab1edb6f4f0bb952da2a83e39a13632eb857de901b16a4f500cfeb5322` right.glb.

Each is one skinned mesh of 1764 vertices and 3052 triangles on 26 joints with OpenXR's names and meanings (`Wrist_L`, `Thumb_Metacarpal_L` .. `Little_Tip_L`) and a `Palm_L`, whose weights go to the wrist. Its wrist runs longer than the others'.
