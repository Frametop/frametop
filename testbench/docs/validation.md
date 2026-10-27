# Initial HMD validation

This records the original HMD-only version. See [controller and MCP validation](controller-mcp-validation.md) for the later extension.

## Live Steam Frame, 2026-10-06

Tested on a development Steam Frame running SteamVR 2.17.10, aarch64. Exact driver fingerprint and machine-readable measurements are in [hardware-verification.json](hardware-verification.json). These observations cover this runtime build, not every future SteamOS update.

The complete 55-test offline suite passed on both the development host and the Frame's native host compiler/runtime. Tests include real UNIX sockets, dynamically loaded fake cv and OpenVR libraries, forwarding of the public interfaces, malformed input, concurrent input/control, activation/deactivation, and release behavior.

The live sequence used the public CLI over SSH:

1. Build both native libraries and observer on the Frame, then install the user-service override and restart SteamVR once. Read back the override, mapped driver control socket, real HMD index 0, and captured proximity handle. SSH and SteamVR remained active.
2. Set compositor awake and worn on. Set Standing pose to position `[0, 1.6, 0]` with identity rotation. Public OpenVR readback returned valid tracking result 200 at that exact pose while the cached physical pose was invalid and physical proximity was false. Runtime activity changed from standby 3 to in-use 1.
3. Capture and download the compositor stereo image. Unlike the dark, untracked baseline, it contained the Steam dashboard in both eyes.
4. Move by `[0.25, 0.10, -0.15]` metres and rotate by yaw 25, pitch -10, roll 5 degrees. The public HMD Standing matrix matched the composed target within `4.8e-8` per matrix element. A new capture showed the dashboard displaced and tilted. These were actual compositor pixels, not a rendered fixture.
5. Set worn off and wait six seconds. Runtime activity became standby 3. The virtual HMD pose remained valid and unchanged, independently of the invalid physical tracking sample.
6. Set worn on again and restore the front-facing pose. A fresh capture showed the dashboard level again. No SteamVR restart occurred between these input/capture commands.
7. Release both overrides. Public tracking returned to the physical invalid state rather than retaining the synthetic pose. Restore automatic compositor standby. Readback confirmed pose override false, proximity override null, and pause-on-standby true. SSH and the same vrserver/vrcompositor processes remained running.

The adapter remains installed with overrides released. Original `/opt/steamvr/drivers/cv/bin/linuxarm64/driver_cv.so` bytes remained unchanged. Captures and full status logs are private, ignored artifacts. No room images or device serial were included in the repository.

## Bugs caught by integration and hardware testing

- The Python capture wrapper and native observer initially both attempted to create the output directory. The native observer now exclusively owns its atomic creation.
- Status transforms use nested 3x4 matrices. The Python Standing-space conversion now consumes the actual observer schema.
- SteamVR appends `.png` to screenshot filename prefixes on this build. Passing `preview.png` produced `preview.png.png`. The observer now passes extensionless prefixes, and the fake SDK reproduces this behavior.

Each issue was reproduced before its fix and covered by a regression test.

## Evidence boundaries

- Input commands acknowledge the proxy state; `status` supplies independent public runtime readback. Do not equate an accepted command with a rendered application change.
- Captures are 1920x1080 side-by-side stereo on this build. They are cropped compositor screenshots, not full optical lens coverage. PNG container validation checks completion and CRCs; live verification additionally decoded the compressed pixel data and visually inspected the images.
- Virtual headset movement does not move the physical cameras. This tool cannot validate physical passthrough alignment, comfort, camera calibration, or real motion latency.
- SteamVR activity transitions were verified. No claim is made that every DSP/XRService user-presence consumer or whole-machine suspend policy is virtualized.
- Release returns the latest physical sample, which can be invalid or stale in standby. It does not manufacture valid physical tracking.
- Controller inputs are forwarded, not virtualized. MCP integration is deferred.
- Removal of the override is the recovery path. SSH survived installation and all live tests. A forced compositor crash or power-loss recovery was not induced.

## Retesting after runtime updates

Run `make test`, then the README's live loop with an unused output directory. Check the actual HMD index/model, proxy readiness, public pose matrix, both worn transitions, frame advancement, visible image changes, release, and SSH access. Compare the driver fingerprint. Unsupported interface versions pass through rather than receiving an incorrectly sized proxy vtable; absent control readiness must not be called a successful override.
