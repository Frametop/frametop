# The rendered hands' model

`left.glb` and `right.glb` are the `generic-hand` profile of the WebXR Input Profiles, unchanged:

- Source: https://github.com/immersive-web/webxr-input-profiles, `packages/assets/profiles/generic-hand/`
- Fetched at commit `f4992299601614adbfefd398dc8e281556bb7444` (2026-04-23) of `main`; the files last changed in `c5356912d0` (2022-12-02, "Adjust bone placement for proper animation"). Package `@webxr-input-profiles/assets` 1.0.19.
- Licence: MIT, Copyright (c) 2019 Amazon: `packages/assets/LICENSE.md` at that commit, copied here as [LICENSE](LICENSE). The repository's top-level licence (W3C Software and Document License) applies "unless otherwise specified in individual packages", and the assets package specifies MIT; its README says "Assets are available under MIT license in .glTF, or .glb formats."
- sha256: `bc67783144944ea1cda54d9247885825ea5fb9d4651469fe7d00be517a5c2b87` left.glb, `291790c14f7f88a7f9bd35330c47392ed8e8d395ae6728f4bb7089f1bc1f2b96` right.glb

Each is one skinned mesh (1360 vertices, 2314 triangles, up to four joints per vertex) on the 25 WebXR hand joints (wrist, thumb metacarpal to tip, and metacarpal to tip for each finger), in metres. The two are separate meshes, not one mirrored, and their nodes' default poses differ from the bind pose (the bind pose comes from the inverse bind matrices).

`screens/tools/hand-asset.py` turns them into `screens/handasset.h`, which ft-handview compiles in (`screens/handskin.h` poses it; hands/README.md, "Rendered hands"). Run it again after changing these files.

Not used: the other hand profiles in that repository (`oculus-hand`, `generic-fixed-hand`).
