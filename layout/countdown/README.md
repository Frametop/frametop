# Reset countdown

`ft-layout reset` shows a head-following **3–2–1** so you can release the reset
control and face forward before Frametop samples the head pose. The overlay is
non-interactive and closes after three seconds. Repeated reset requests are
rejected while that reset is pending or applying.

With ft-screens, reset moves only existing unpinned local and connected remote
panels. It reads the active profile's poses without restoring apps, fullscreen
state, visibility, sizes, or connections, and does not save configuration.
Disconnected and pinned panels are left alone. The legacy gamescope backend
still reopens the active profile after its countdown. Direct `apply` and `use`
commands remain immediate; use `ft-layout use NAME` to restore a full profile.

## Build and run

```sh
layout/countdown/build.sh
```

The recipe follows the other components: from a PC it syncs and builds in the
Frame's dev container; on the Frame it builds in that container; `FRAME_IN_BOX=1`
uses the current container for release-image builds. The binary is
`layout/countdown/build/ft-reset-timer`, and `ft-layout` runs it through
`scripts/in-box` when called on the host. The build's `--check` is offline and
does not initialize OpenVR.

The build fetches the SDK pinned in `scripts/openvr.sh`. `deps.sh` fetches the
STB headers at the same commit as `screens/build.sh`, with SHA-256 checks. The
renderer uses the installed Noto Sans font. There are no vendored binaries or
headers.

If the helper is missing or fails, reset still waits at least three seconds
before sampling the head. A missing helper does not start a container or access VR.
A helper timeout is bounded at six seconds; a failure does not cancel the reset.
The repo helper also receives an absolute `CLOCK_MONOTONIC` deadline at six
seconds from the request. The host and its Distrobox share this clock. Because
`scripts/in-box` cannot forward the host timeout to its container child, this
deadline prevents a container that starts late from showing a stale countdown.
Expiry is checked before VR initialization, after initialization and overlay
creation, before showing, and between frames; any existing overlay is cleaned
up.

## Offline tests

```sh
python3 layout/countdown/tests/test_reset.py
python3 layout/countdown/tests/test_geometry_reset.py
layout/countdown/deps.sh
python3 layout/countdown/tests/test_lifecycle.py
```

The Python tests load the current `layout/ft_layout.py` and replace headset,
configuration, and helper operations with isolated mocks. The C++ tests compile
the current helper against `tests/cpp/openvr.h`; they do not link OpenVR and
cannot contact SteamVR. Their temporary binary is kept under `build/` and
removed afterward. A C++17 compiler and a local TrueType font are required.
`CXX`, `STB_INCLUDE_DIR`, and `FT_COUNTDOWN_TEST_FONT` can override the detected
compiler, header directory, and font. Linux Noto Sans/DejaVu Sans and macOS Arial
are recognized for tests.

Only three images are uploaded in a short-lived process, avoiding SteamVR's
per-process `SetOverlayRaw` upload limit. The `frametop.guide.reset-countdown.*`
key keeps Frametop's pointer ray from hitting the display-only panel. After a
SteamVR or pointer change, check that the countdown remains readable as you
turn, does not intercept input, disappears at completion, and samples the final
head pose.
