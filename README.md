# FrameHands

FrameHands tracks both hands with the Steam Frame's built-in cameras and renders posed 3D hands in SteamVR. It is derived from Frametop's `ft-camd`, `ft-hands`, and `ft-handview`. It runs as three user services with normal SteamVR; it does not start Frametop Desktop, KDE, Distrobox, or Podman.

The camera broker borrows XRService's camera buffers, the ncnn tracker fits 21 joints and a skeleton for each hand, and the renderer presents left and right OpenVR overlays. Tracking files under `/run/user/$UID/framehands/` publish hands, fitted joints, pinches, middle pinches, and grips for other applications. The files are sequence locked; see `hands/include/fh_hands.h` and `fh_gestures.h` for their binary layouts. The tracker retains the original camera calibration, model scheduling, side detection, and smoothing.

## Installation on a Steam Frame

The fork must have a **published ARM64 release** from `.github/workflows/release.yml`. Then:

```bash
git clone <your-fork-url>
cd <repo-folder>
./install.sh
~/.local/bin/framehands doctor
```

The installer checks ARM64 SteamOS, SteamVR, and the Frame's camera calibration files. If binaries are not in the checkout, it downloads the latest release archive from that checkout's GitHub origin and verifies its SHA256SUMS. It copies only the tracker binaries, models, renderer, CLI, and FrameHands service files into `~/.local/share/framehands/app`. It creates a private config file at `~/.config/framehands/config` if none exists. It asks for `sudo setcap` once for the camera broker's existing `cap_sys_ptrace`, `cap_perfmon`, and `cap_dac_read_search` requirements; the broker drops those capabilities after attaching to XRService. No system packages or immutable-root changes are made on the headset.

If Frametop hand services are currently running, installation stops with an explanation. It does not stop or alter Frametop or SteamVR services. Stop those Frametop services yourself before retrying, when safe to do so.

Launch SteamVR after installation. With the default `AUTO_START=1`, the FrameHands user services start with `steamvr.service`, continue through games, and stop when SteamVR stops. SteamVR's dashboard remains the normal dashboard.

## Commands

```text
framehands start       Start tracking and rendering while SteamVR is running
framehands stop        Stop FrameHands services
framehands restart     Apply config changes and restart
framehands status      Show SteamVR, services, and visible hands
framehands enable      Start automatically with SteamVR
framehands disable     Stop and disable automatic startup
framehands sync        Apply startup and render enable settings
framehands gestures    Show both hands' current pinch, grip and middle-pinch data
framehands logs        Show the last 100 FrameHands journal lines
framehands doctor      Check installation, permissions, libraries and services
```

Config is `~/.config/framehands/config`. `TRACKING=0` disables tracking; `RENDER=0` disables the overlay; `AUTO_START=0` prevents SteamVR startup. Run `framehands restart` after changing tracker or renderer settings, and `framehands sync` after changing startup settings. `HANDS_MODEL=latest` uses the pinned fine-tuned model, with bundled MediaPipe ncnn models as a fallback; `stock` selects stock explicitly. `HANDS_VIEW_MODEL` accepts `asset`, `babylon`, `mrtk3`, `godot`, `mrtk2`, `robot`, and `capsules`. `HANDS_VIEW_STYLE` accepts `ghost` or `solid`. The tracker also accepts its original camera settings such as `HANDS_CAMERAS`, `HANDS_CPUS`, and `HANDS_BRIGHT`. `PERFORMANCE=balanced` keeps the original three-thread, full-accuracy behavior; `quality` uses four threads; `power` uses two threads and stock INT8 models, which reduce accuracy. The default disables the optional recording buffer.

`DEBUG=1` increases service status logging and logs gesture transitions. `GESTURE_INPUT=0` and `CONTROLLER_EMULATION=0` are reserved configuration switches. The current standalone release publishes gesture data but does **not** send pinch clicks, ray events, or controller input into SteamVR. It does not expose native SteamVR skeletal input to games. Games must integrate the shared tracking data themselves; the visual overlay does not imply game input support. The renderer uses SteamVR's OpenVR overlay and DMA-BUF import interface already used by the original Frametop hand view. Room View detection is best effort, and a SteamVR update may change the private camera or shared-texture behavior.

## Verification on the headset

Only test with nobody wearing the headset, following the original project's testbench and safety guidance if it is available. Without that setup, observe without forcing poses:

```bash
systemctl --user status steamvr.service framehands-camd.service framehands-track.service framehands-view.service
framehands doctor
framehands status
framehands gestures
framehands logs
getcap ~/.local/share/framehands/app/hands/build/fh-camd
```

Success means all three FrameHands services are active while SteamVR is running, `doctor` reports no service or permission errors, `status` reports the left and right hands when both are in the cameras' view, `gestures` updates on pinch and grip, and both posed overlays appear in the headset. Stop SteamVR normally and verify all three FrameHands units stop. No hardware verification has been performed as part of this refactor.

If the broker fails, check the camera calibration files and capabilities. If the tracker fails, check `framehands logs` for a camera-ring heartbeat or model error. If rendering fails, check for a missing SteamVR OpenVR interface or GPU library with `framehands doctor` and the renderer's journal. The installer requires a published release; a clone without one cannot install prebuilt binaries.

## Build and release

CI builds on a native GitHub ARM64 Ubuntu runner with an older glibc baseline, pinned ncnn source and models, pinned OpenVR SDK files, and pinned stb. `scripts/build.sh` builds and runs the offline C++ tests; the release workflow packages only required runtime assets into `framehands-aarch64.tar.gz` with a checksum. The installer never invokes `make` on SteamOS. Build locally on a compatible ARM64 Linux machine with `build-essential`, `cmake`, `ninja-build`, `curl`, `unzip`, `pkg-config`, `libjsoncpp-dev`, `libgbm-dev`, `libdrm-dev`, `libegl1-mesa-dev`, and `libgles2-mesa-dev`; set `OPENVR_LIB` to a folder containing the pinned ARM64 `libopenvr_api.so`, then run `bash scripts/build.sh`.

CI proves ARM64 compilation and offline tests, not compatibility with every SteamOS image. Verify the resulting release on a Frame before publishing it for others.

## Uninstallation

```bash
./uninstall.sh
# optional, with confirmation:
./uninstall.sh --models --config
```

The uninstaller stops and disables only FrameHands units, removes its camera binary's capabilities, its app folder and CLI symlink, and preserves config by default. It does not remove Podman containers, SteamVR, Frametop, desktop files, or other users' files.

## Attribution and licenses

FrameHands retains Frametop's camera broker, tracker, and hand renderer. Frametop is copyright (c) 2026 DeeJanuz, under the repository's MIT `LICENSE`. The camera feed notice is in `hands/camd/LICENSE.FrameEyeCameraFeed`; bundled stock MediaPipe/ncnn models have notices in `hands/models/NOTICE`. Fine-tuned models are downloaded from `DeeJanuz/frametop-hands-models` at the pinned commit in `hands/models.pin`; their `LICENSE` and `NOTICE` are distributed with the release. Rendered hand assets and their licenses are under `renderer/assets/hands/` in the source tree.
