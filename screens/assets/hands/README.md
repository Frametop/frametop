# The rendered hands' models

ft-handview draws your hands with one of these (`HANDS_VIEW_MODEL` in `~/.config/frametop.conf`; hands/README.md, "Rendered hands"). Each folder has its source, version, licence and how it was converted in its README, and its licence's text in LICENSE:

| `HANDS_VIEW_MODEL` | Files | From | Licence | Per hand |
|---|---|---|---|---|
| `asset` (the default) | `left.glb`, `right.glb` (this folder) | WebXR Input Profiles' `generic-hand` | MIT, (c) 2019 Amazon | 1360 vertices, 2314 triangles |
| `babylon` | [babylon/](babylon/README.md) | Babylon.js's hand meshes, by Patrick Ryan | CC BY 4.0 (attribution) | 1070, 1808 |
| `mrtk3` | [mrtk3/](mrtk3/README.md) (right; the left mirrored) | MRTK3's OpenXR hand | BSD-3-Clause, (c) 2025 Mixed Reality Toolkit Contributors | 1399, 2794 |
| `godot` | [godot/](godot/README.md) | Godot XR Tools' hands, by DigitalN8m4r3 (Miodrag Sejic) | CC0 1.0 | 1764, 3052 |
| `mrtk2` | [mrtk2/](mrtk2/README.md) | MRTK2's rigged hand | MIT, (c) Microsoft Corporation | 1334, 2664 |

`screens/tools/hand-asset.py` maps each one's skeleton onto WebXR's 25 hand joints (the names it takes are in its MODELS) and turns them all into `screens/handasset.h`, which ft-handview compiles in (`screens/handskin.h` poses them). Run it again after changing these files. `screens/tools/hand-glb-clean.py` rewrote the converted ones (each README says how).

`HANDS_VIEW_MODEL=robot` isn't here: it's built in code from the tracked skeleton (`screens/handrobot.cpp`).

Robot hands looked at on 2026-10-09, none taken. MuJoCo Menagerie's `shadow_hand` (Apache-2.0, from Shadow Robot Company; a rigid OBJ part per finger segment, five fingers) has the right licence and parts, but about 51,000 triangles without the forearm (each fingertip about 5,000), so it would need decimating to a tenth, its MJCF body transforms turned into bone-bound parts, and its proportions aren't a human hand's. Menagerie's `tetheria_aero_hand_open` and `sharpa_wave` (Apache-2.0) are segmented too but heavier (STL, no normals); `leap_hand` (MIT) and `wonik_allegro` (BSD) have four fingers. Quaternius' 2018 "Animated LowPoly Robot" (CC0 on OpenGameArt) has a mitten hand with three finger bones, and Quaternius' current licence forbids redistributing its packs. Kenney, Poly Pizza, OpenGameArt and itch.io had no finger-rigged robot hand; Sketchfab needs a login to download; InMoov is non-commercial.

Not taken: Ultraleap's hands (Apache-2.0; GenericHand is left only with a forearm, 4350 triangles; LowPoly is faceted), MRTK2's hand coach (21 joints, no tips, unshared vertices), Meta's hands (as in A-Frame's and three.js's older examples) and MANO (non-commercial).

## asset: generic-hand

`left.glb` and `right.glb` are the `generic-hand` profile of the WebXR Input Profiles, unchanged:

- Source: https://github.com/immersive-web/webxr-input-profiles, `packages/assets/profiles/generic-hand/`
- Fetched at commit `f4992299601614adbfefd398dc8e281556bb7444` (2026-04-23) of `main`; the files last changed in `c5356912d0` (2022-12-02, "Adjust bone placement for proper animation"). Package `@webxr-input-profiles/assets` 1.0.19.
- Licence: MIT, Copyright (c) 2019 Amazon: `packages/assets/LICENSE.md` at that commit, copied here as [LICENSE](LICENSE). The repository's top-level licence (W3C Software and Document License) applies "unless otherwise specified in individual packages", and the assets package specifies MIT; its README says "Assets are available under MIT license in .glTF, or .glb formats."
- sha256: `bc67783144944ea1cda54d9247885825ea5fb9d4651469fe7d00be517a5c2b87` left.glb, `291790c14f7f88a7f9bd35330c47392ed8e8d395ae6728f4bb7089f1bc1f2b96` right.glb

Each is one skinned mesh (1360 vertices, 2314 triangles, up to four joints per vertex) on the 25 WebXR hand joints (wrist, thumb metacarpal to tip, and metacarpal to tip for each finger), in metres. The two are separate meshes, not one mirrored, and their nodes' default poses differ from the bind pose (the bind pose comes from the inverse bind matrices).

Not used: the other hand profiles in that repository (`oculus-hand`, `generic-fixed-hand`).
