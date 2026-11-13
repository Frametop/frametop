# Working on FrameHands

This repository is the standalone Steam Frame hand-tracking fork of Frametop. Read `README.md` and `hands/README.md` before changing the runtime. Preserve the camera broker's capability dropping, the tracker's calibration and model scheduling, and the shared-file sequence locks.

Build on ARM64 Linux with `bash scripts/build.sh`. `hands/Makefile` builds the broker and tracker and runs offline camera, skeleton, side, pinch, and shared-memory tests. `renderer/build.sh` builds the SteamVR overlay and its offline CPU tests. CI uses the same commands and packages only required binaries, models, service files, and installer.

On the Steam Frame, install from a published release with `./install.sh`. Do not build or install system packages on the headset. Never modify SteamOS's immutable root or weaken security settings. The only privileged install operation is `setcap` on FrameHands' own camera broker binary; `uninstall.sh` removes those capabilities before deleting it. Keep FrameHands paths and services separate from Frametop.

The headset may be in use. Do not stop or restart SteamVR, the game session, the dashboard, Frametop, or unrelated services without asking the wearer. Do not drive a test pose while anyone wears the headset. Do not claim headset verification unless performed on the hardware. Run `framehands doctor` and `scripts/update-check.py` after a SteamOS update; add a check or explicit retest hint when code begins depending on a new host interface.

Keep all third-party model and rendered-asset licenses and notices. The fine-tuned model is pinned in `hands/models.pin`; never fetch an unpinned model as part of the release. Preserve ARM64 compatibility and test both install and uninstall idempotence on a Frame before release.
