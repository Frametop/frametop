# Frame Testbench

A development tool for Frametop, built from Donn Green's ([JakeGreen2145](https://github.com/JakeGreen2145)) [frame-testbench](https://github.com/JakeGreen2145/frame-testbench) at commit 1d39528, MIT licensed ([LICENSE](LICENSE)). It isn't part of a Frametop install: get.sh never installs it, the release zip and the image leave it out, and `uninstall.sh` doesn't remove it. Install and remove it with `testbench/install.sh`.

Headset and controller pose tools for closed-loop development on Steam Frame. A Python CLI and stdio MCP server drive native OpenVR adapters locally or over SSH. CLI commands return JSON; MCP captures also return image content for agent vision. This is not a replacement compositor.

## What it controls

- The real HMD's worn/unworn proximity input.
- The real HMD's own buttons (its other boolean inputs, listed in `status`).
- The real HMD's translation and rotation, including valid synthetic tracking while physical tracking is unavailable.
- Independent left/right synthetic controller translation and rotation, without requiring powered physical controllers.
- SteamVR stereo screenshots, with runtime pose/frame metadata and optional downloads to the agent machine.

Input overrides stay active across CLI calls. `release` restores physical input. There is no per-screenshot activation/rollback cycle. Do not wear the headset while another person or agent is driving its pose.

The input adapter wraps the existing `cv` driver at public OpenVR interface boundaries. It forwards display components, unrelated devices, and unrelated inputs. It does not add a second HMD. This interception is experimental, not a Valve-supported extension mechanism.

## Build and install

Like the rest of Frametop, the native parts build in the `dev` container (`testbench/build.sh`) against the OpenVR headers pinned in `scripts/openvr.sh`. They run on the SteamOS host, so the build fails if one needs a newer glibc than the host's 2.39. Root access and changes to `/opt` are not needed. From the repo root, on the Frame:

```sh
testbench/install.sh install --restart   # build, MCP venv, stage SteamVR's preload, restart SteamVR
testbench/frame-testbench status
```

`./ft dev test` runs the testbench's tests with Frametop's (`just test-testbench` in the image).

Installation copies the native libraries to a content-addressed directory under `~/.local/share/frame-testbench/`, removes earlier builds' folders there, and writes only:

```text
~/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf
```

`install.sh install --restart` (or `uninstall --restart`) restarts the headset's `steamvr.service` and interrupts VR: only with nobody in the headset. It does not restart SSH or alter networking. Library updates get new paths rather than overwriting loaded shared objects. Without `--restart`, the change applies when SteamVR next starts. The CLI's `install` and `uninstall` only stage it: restart SteamVR through `install.sh`, which checks for the crash loop below. The CLI commands below run in `testbench/`.

## Development loop

```sh
# Keep rendering even when testing the unworn state.
./frame-testbench compositor awake
./frame-testbench worn on

# Standing space, metres. Rotation is yaw/pitch/roll in degrees.
./frame-testbench pose set --position 0 1.6 0 --rotation 0 0 0
./frame-testbench capture --output artifacts/front

# Move 25 cm right, then look 30 degrees left around world up.
./frame-testbench pose move --translation .25 0 0 --rotation 30 0 0
./frame-testbench capture --output artifacts/moved
./frame-testbench status

# Test app behavior when the HMD is removed.
./frame-testbench worn off
./frame-testbench status

# Return control to the physical headset when done.
./frame-testbench release
./frame-testbench compositor auto
```

`pose reset` releases only pose. `worn physical` releases only proximity. `hmd-button PATH on|off|physical` presses, releases or restores one HMD button, such as `/input/system/click`; `status` lists them under `input.hmd_buttons`. A held button is one long press to SteamVR until it's released. Worn state and pose are independent. `compositor awake` sets `power.pauseCompositorOnStandby=false`; `compositor auto` sets it to true. That setting persists, and `release` deliberately does not change it. HMD standby and whole-machine suspend are separate.

Coordinates are right-handed: +X right, +Y up, forward -Z. Positive yaw turns left, positive pitch looks up, positive roll rotates around +Z. Euler composition is Y * X * Z. Relative moves use the chosen world's axes, not head-local axes. `--space raw` is available on `pose set` and `pose move`; the default Standing space uses the runtime's actual raw-to-standing transform, not an assumed floor offset. A relative move requires an active absolute pose first.

## Controller poses

```sh
./frame-testbench controller left set --position -.25 1.2 -.5 --rotation 0 0 0
./frame-testbench controller right set --position .25 1.2 -.5 --rotation 0 0 0
./frame-testbench controller left move --translation .1 0 -.1 --rotation 30 0 0
./frame-testbench status
./frame-testbench capture --output artifacts/controllers
./frame-testbench controller left reset
./frame-testbench release
```

Controller coordinates and rotations use the same conventions as the HMD, including `--space raw`. Setting a pose connects that synthetic controller; resetting disconnects it. Global `release` disconnects both synthetic controllers and releases HMD pose, proximity and button overrides. HMD `pose reset` and `worn physical` remain selective.

These are additional devices with stable serials `frame_testbench_left` and `frame_testbench_right`, not overrides of your physical controllers. Physical controller inputs pass through unchanged. Keep physical controllers off during synthetic left/right tests to avoid competing role assignments. Check the exact synthetic device indices and downstream poses in `status`.

The controllers expose the Frame profile's buttons, trigger/grip values, thumbstick axes, and click/touch components. See [controller input commands and layout](docs/controller-inputs.md). `inputs` provides separate action-based runtime readback; application bindings can transform or reserve inputs. Skeletal input and haptics are not simulated.

## MCP server

The optional stdio server exposes the same operations as typed MCP tools. It returns capture metadata and a PNG image block, so an agent can inspect the result without a separate file-reading tool. It opens no network listener. Host selection is fixed at startup, not controlled by tool arguments.

See [MCP setup and tools](docs/mcp.md) for installation and client configuration. Installation/restarts remain explicit CLI operations rather than agent-exposed MCP tools.

## Remote agent use

From a PC, `testbench/install.sh` syncs the repo to `~/dev/frametop` on the Frame, builds and stages the preload there, and makes the MCP venv on the PC, like Frametop's other installers (see "Developing from a PC" in Frametop's README). The CLI's `--remote-root` defaults to that copy, `dev/frametop/testbench`:

```sh
testbench/install.sh install
testbench/frame-testbench --host frame compositor awake
testbench/frame-testbench --host frame worn on
testbench/frame-testbench --host frame pose set --position 0 1.6 0
testbench/frame-testbench --host frame capture --fetch testbench/artifacts/remote-front
```

Restarting SteamVR (`install.sh install --restart`) ends whatever runs in VR: ask whoever uses the headset first. `frame` is an SSH alias configured by the operator. The CLI requires existing SSH authentication and strict host-key checking. `--remote-root` selects a different remote checkout. `--output` is a headset-side directory when using SSH; `--fetch` creates a new local directory containing `preview.png`, `stereo.png`, and `result.json`. It never overwrites an existing capture directory.

For an agent, the loop is: set input, read `status`, capture, inspect `local_files["stereo.png"]`, change code, repeat. A successful input command acknowledges the proxy state. Check runtime pose and activity in `status` for downstream behavior. A successful screenshot verifies complete PNG containers, not correct application rendering.

## Capture limits

The output is SteamVR's public stereo screenshot, not a desktop screenshot. It can include compositor overlays but has cropped field of view and is not a full optical lens image. Camera-layer inclusion must be verified for the application being tested. A virtual pose does not move physical cameras or validate passthrough motion alignment.

Each capture records runtime state before and after the screenshot. Invalid tracking, frozen frame counters, or dark pixels remain evidence to inspect, not proof of a working overlay. Failed output directories are retained. Room imagery, device identifiers, and captured desktop content belong in ignored `artifacts/`, not Git.

## Recovery over SSH

First try:

```sh
ssh frame 'cd dev/frametop && testbench/frame-testbench release'
ssh frame 'cd dev/frametop && testbench/install.sh uninstall --restart'
```

A SteamVR that keeps crashing is more than a nuisance on the Frame. `steamvr.service` restarts itself (`Restart=always`), and SteamOS's health check (`/usr/share/deckard/steamvr-health-check`) counts every session that fails or lasts under 10 seconds: after three in a row it repairs Steam, after four it sets the next boot to the other OS slot. Three things keep the testbench out of that loop:

- The loader reads the health check's file (`$XDG_RUNTIME_DIR/steamvr-short-session-tracker`). A SteamVR that starts while it counts a failure runs without the proxy, so a proxy that crashes vrserver costs one failed session. That one session has no testbench, whatever the failure was (stopping SteamVR in its first 10 seconds counts too); the next start after a good session has it again.
- The drop-in sets `RestartSec=30`: while the testbench is installed, a SteamVR that exits, crashed or not, comes back after 30 seconds.
- `install.sh install --restart` and `uninstall --restart` won't restart while a failure is counted, or while SteamVR is still starting. Afterwards it watches that file, the unit's `InvocationID` and state for 30 seconds, then checks that vrserver runs, and takes the preload back out at the first failure.

If SteamVR is crash-looping and the CLI is unavailable, remove the single override directly:

```sh
ssh frame 'rm -f ~/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf; systemctl --user daemon-reload; systemctl --user restart steamvr.service'
```

This leaves the stock runtime and its driver files untouched. The proxy starts with overrides disabled after restart. `install.sh uninstall` also deletes the library folders; a running process keeps a deleted library mapped.

## Native commands

`build/frame-observe` provides `status`, `capture ABS_NEW_DIRECTORY`, `setting true|false`, and `debug DEVICE_INDEX REQUEST`. It uses Background OpenVR initialization and emits JSON on stdout; SDK chatter goes to stderr. `OPENVR_API_LIBRARY` selects another runtime library. `FRAME_OBSERVE_TIMEOUT_SECONDS` bounds a command from 1 to 120 seconds, default 20.

`FRAME_TESTBENCH_OBSERVER` selects a different observer executable. `FRAME_TESTBENCH_SOCKET` selects the local input socket, normally `/run/user/<uid>/frame-testbench.sock`. Input control uses a user-only UNIX socket. There is no listening network service.

## Verification

`just test-testbench` (part of `just test`, so `./ft dev test` and CI run it) builds and exercises the public SDK boundaries with fake runtimes, checks pose mathematics and the CLI, and tests input forwarding and loader selection without starting SteamVR. The image has no MCP SDK, so the MCP tests skip there. `install.sh install` runs them in `testbench/.venv` against the pinned packages, exercising the actual stdio MCP protocol. These tests do not substitute for the live headset checks in the [original HMD validation](docs/validation.md), [controller/MCP validation](docs/controller-mcp-validation.md), and [button/analog/touch validation](docs/input-validation.md).

The testbench is MIT licensed, copyright Donn Green ([LICENSE](LICENSE)). The OpenVR headers it builds against come from `scripts/openvr.sh` and keep Valve's license.
