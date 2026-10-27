# Frame Testbench

Headset and controller pose tools for closed-loop development on Steam Frame. A Python CLI and stdio MCP server drive native OpenVR adapters locally or over SSH. CLI commands return JSON; MCP captures also return image content for agent vision. This is not a replacement compositor.

## What it controls

- The real HMD's worn/unworn proximity input.
- The real HMD's translation and rotation, including valid synthetic tracking while physical tracking is unavailable.
- Independent left/right synthetic controller translation and rotation, without requiring powered physical controllers.
- SteamVR stereo screenshots, with runtime pose/frame metadata and optional downloads to the agent machine.

Input overrides stay active across CLI calls. `release` restores physical input. There is no per-screenshot activation/rollback cycle. Do not wear the headset while another person or agent is driving its pose.

The input adapter wraps the existing `cv` driver at public OpenVR interface boundaries. It forwards display components, unrelated devices, and unrelated inputs. It does not add a second HMD. This interception is experimental, not a Valve-supported extension mechanism.

## Build and install

Requires Linux, Python 3, a C++17 compiler, and the existing SteamVR installation. Build natively on the Frame host to match its libc. Root access and changes to `/opt` are not needed.

```sh
make
make test
./frame-testbench install --restart
./frame-testbench status
```

Installation copies the native libraries to a content-addressed directory under `~/.local/share/frame-testbench/` and writes only:

```text
~/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf
```

`--restart` restarts the headset's `steamvr.service` and interrupts VR. It does not restart SSH or alter networking. Library updates get new paths rather than overwriting loaded shared objects. Installation without `--restart` stages the change for the next SteamVR restart.

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

`pose reset` releases only pose. `worn physical` releases only proximity. Worn state and pose are independent. `compositor awake` sets `power.pauseCompositorOnStandby=false`; `compositor auto` sets it to true. That setting persists, and `release` deliberately does not change it. HMD standby and whole-machine suspend are separate.

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

Controller coordinates and rotations use the same conventions as the HMD, including `--space raw`. Setting a pose connects that synthetic controller; resetting disconnects it. Global `release` disconnects both synthetic controllers and releases HMD pose/proximity overrides. HMD `pose reset` and `worn physical` remain selective.

These are additional devices with stable serials `frame_testbench_left` and `frame_testbench_right`, not overrides of your physical controllers. Physical controller inputs pass through unchanged. Keep physical controllers off during synthetic left/right tests to avoid competing role assignments. Check the exact synthetic device indices and downstream poses in `status`.

The controllers expose the Frame profile's buttons, trigger/grip values, thumbstick axes, and click/touch components. See [controller input commands and layout](docs/controller-inputs.md). `inputs` provides separate action-based runtime readback; application bindings can transform or reserve inputs. Skeletal input and haptics are not simulated.

## MCP server

The optional stdio server exposes the same operations as typed MCP tools. It returns capture metadata and a PNG image block, so an agent can inspect the result without a separate file-reading tool. It opens no network listener. Host selection is fixed at startup, not controlled by tool arguments.

See [MCP setup and tools](docs/mcp.md) for installation and client configuration. Installation/restarts remain explicit CLI operations rather than agent-exposed MCP tools.

## Remote agent use

Sync the repository without builds, captures, or `.git`, then compile on Frame:

```sh
rsync -az --exclude=.git --exclude=build --exclude=artifacts --exclude=.venv --exclude=__pycache__ ./ frame:dev/frame-testbench/
ssh frame 'cd dev/frame-testbench && make'
./frame-testbench --host frame install --restart
./frame-testbench --host frame compositor awake
./frame-testbench --host frame worn on
./frame-testbench --host frame pose set --position 0 1.6 0
./frame-testbench --host frame capture --fetch artifacts/remote-front
```

`frame` is an SSH alias configured by the operator. The CLI requires existing SSH authentication and strict host-key checking. `--remote-root` selects a different remote checkout. `--output` is a headset-side directory when using SSH; `--fetch` creates a new local directory containing `preview.png`, `stereo.png`, and `result.json`. It never overwrites an existing capture directory.

For an agent, the loop is: set input, read `status`, capture, inspect `local_files["stereo.png"]`, change code, repeat. A successful input command acknowledges the proxy state. Check runtime pose and activity in `status` for downstream behavior. A successful screenshot verifies complete PNG containers, not correct application rendering.

## Capture limits

The output is SteamVR's public stereo screenshot, not a desktop screenshot. It can include compositor overlays but has cropped field of view and is not a full optical lens image. Camera-layer inclusion must be verified for the application being tested. A virtual pose does not move physical cameras or validate passthrough motion alignment.

Each capture records runtime state before and after the screenshot. Invalid tracking, frozen frame counters, or dark pixels remain evidence to inspect, not proof of a working overlay. Failed output directories are retained. Room imagery, device identifiers, and captured desktop content belong in ignored `artifacts/`, not Git.

## Recovery over SSH

First try:

```sh
ssh frame 'cd dev/frame-testbench && ./frame-testbench release'
ssh frame 'cd dev/frame-testbench && ./frame-testbench uninstall --restart'
```

If SteamVR is crash-looping and the CLI is unavailable, remove the single override directly:

```sh
ssh frame 'rm -f ~/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf; systemctl --user daemon-reload; systemctl --user restart steamvr.service'
```

This leaves the stock runtime and its driver files untouched. The proxy starts with overrides disabled after restart. `uninstall` leaves immutable library versions on disk so it cannot remove code still mapped by a running process.

## Native commands

`build/frame-observe` provides `status`, `capture ABS_NEW_DIRECTORY`, `setting true|false`, and `debug DEVICE_INDEX REQUEST`. It uses Background OpenVR initialization and emits JSON on stdout; SDK chatter goes to stderr. `OPENVR_API_LIBRARY` selects another runtime library. `FRAME_OBSERVE_TIMEOUT_SECONDS` bounds a command from 1 to 120 seconds, default 20.

`FRAME_TESTBENCH_OBSERVER` selects a different observer executable. `FRAME_TESTBENCH_SOCKET` selects the local input socket, normally `/run/user/<uid>/frame-testbench.sock`. Input control uses a user-only UNIX socket. There is no listening network service.

## Verification

`make test` builds and exercises the public SDK boundaries with fake runtimes, checks pose mathematics and the CLI, and tests input forwarding and loader selection without starting SteamVR. With `.[mcp]` installed in the active Python environment, it also exercises the actual stdio MCP protocol. These tests do not substitute for the live headset checks in the [original HMD validation](docs/validation.md), [controller/MCP validation](docs/controller-mcp-validation.md), and [button/analog/touch validation](docs/input-validation.md).

OpenVR headers retain Valve's license under `vendor/`. The remaining project code is MIT licensed.
