# FrameHands: SteamVR dashboard control and tracking improvements

This is a source-code patch for:
https://github.com/AlistairBishop06/frametop-hands-only

## Changes

- Native ARM64 OpenVR dashboard tab, with a single large ON/OFF button.
- SteamVR overlay application registration using `binary_path_linux_arm`.
- Separate dashboard systemd service that continues running while the hand-tracking
  services are switched off.
- Adaptive filtering for the dashboard pointer ray, with an optional
  `HANDS_POINTER_SMOOTH=0` setting to switch it off.
- Slightly steadier palm and finger tracking filters.
- A more forgiving default index-pinch threshold (25 mm), adjustable using
  `HANDS_PINCH_BEGIN=0.025` in `~/.config/framehands/config`.
- Installer, release-packaging, ARM64 build-workflow and uninstall integration.
- A regression test for dashboard availability when hand tracking is disabled.
- Preservation of the right-hand virtual-controller overlay fix if it is already applied.

## Apply on your PC

Open a terminal in a fresh clone of the repository (or an up-to-date clean
working tree), then:

    git checkout -b feature/dashboard-controls-and-smoother-hands
    python3 /path/to/framehands_feature_update.py
    git status
    git diff --check
    python3 -m unittest discover -s tests -v
    git add -A
    git commit -m "Add SteamVR hand tracking dashboard and smooth pointer/gestures"
    git push -u origin feature/dashboard-controls-and-smoother-hands

Replace `/path/to/` with the actual script location. Review the diff before
committing, then merge the branch into main. A tag beginning with `v` triggers
the ARM64 release workflow. Publish the draft release after CI succeeds.

## Install the new release on the Steam Frame

    cd ~/frametop-hands-only
    git pull --ff-only origin main
    ./install.sh

Restart SteamVR normally and check:

    ~/.local/bin/framehands doctor
    ~/.local/bin/framehands status

The FrameHands dashboard tab should become available within SteamVR.
Depending on the SteamVR build, its application entry may also be selectable
under the dashboard's + menu.

## Important input limitation

When hand tracking is OFF, the virtual hand pointer cannot interact with the
dashboard, so a physical SteamVR controller or SSH terminal is needed to turn
tracking ON again. Via SSH:

    ~/.local/bin/framehands enable

## Validation

The patcher passed Python syntax checking and a replacement smoke test. The
new native dashboard C++ was syntax-checked against a local OpenVR interface
stub. A real ARM64 release build and headset interaction test have **not** been
performed. GitHub repository writes were not authorised by the connected
GitHub integration, so the changes are supplied locally rather than pushed.
