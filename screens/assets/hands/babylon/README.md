# babylon: Babylon.js's hand meshes

`HANDS_VIEW_MODEL=babylon`. `left.glb` and `right.glb` are Babylon.js's optimized hand meshes, unchanged but for their names:

- Source: https://github.com/BabylonJS/Assets, `core/HandMeshes/l_hand_rhs.glb` and `r_hand_rhs.glb` (the right-handed versions; the `_lhs` ones are for Babylon's left-handed space), at commit `c81f5d744fe8d41acfebd89eaeb7008c6f9659e3`. By Patrick Ryan (PatrickRyanMS), added 2021-06-09 ("optimized hand meshes").
- Licence: Creative Commons Attribution 4.0 International (CC BY 4.0), copied here as [LICENSE](LICENSE) from that repository's `LICENSE` at that commit. Its README: "This work is licensed under a Creative Commons Attribution 4.0 International License (Unless specified otherwise in the asset folder)"; `core/HandMeshes` specifies nothing else. Attribution: "Hand meshes by Patrick Ryan for Babylon.js (https://github.com/BabylonJS/Assets), CC BY 4.0". Frametop's README and docs/reference.md carry it.
- sha256: `68504332a99ca3f9885bbe6f660377257b6b2a7abef249eceb7fc0ae414342b4` left.glb (l_hand_rhs.glb), `7bb924b5520059ab962c4b79357e7fbb9d436fd3b3abf3b1d0317fcbe8878608` right.glb (r_hand_rhs.glb)

Each is one skinned mesh of 1070 vertices and 1808 triangles on 26 joints (`wrist_R`, the thumb's and fingers' `metacarpal`, `proxPhalanx`, `intPhalanx`, `distPhalanx` and `tip`, and a `root_R`), all children of the root, with WebXR's meanings; `root_R`'s weights go to the wrist. The palm is weighted to the wrist alone, and its own gradient texture isn't used.
