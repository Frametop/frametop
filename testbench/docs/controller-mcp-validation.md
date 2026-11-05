# Controller and MCP hardware verification

Verified on a development Steam Frame running SteamVR 2.17.10 on aarch64, 2026-10-06. This extends the [original HMD validation](validation.md).

## Automated tests

The combined source built on Linux x86_64 and on the Frame. All 88 tests passed on the development host with the optional MCP SDK installed. The Frame ran 75 tests successfully and skipped 13 MCP-dependent tests because its system Python did not have that optional SDK. MCP was separately exercised from the development host against the actual Frame over SSH.

The native tests cover synthetic device registration and activation, initially disconnected poses, independent left/right set and release, global release, continued publication without an active HMD, malformed commands, cleanup, concurrent commands, and existing physical-input forwarding. Observer tests verify full-array device readback with noncontiguous indices and disconnected devices. MCP tests include an actual stdio SDK session, schema validation, backend errors, PNG image content, serialization, cancellation, and stdout isolation.

## Actual MCP-to-headset loop

A Python MCP SDK client launched the real `frame_testbench.mcp_server` subprocess with `--host frame`. It initialized the protocol, listed all 11 tools, and invoked every tool against the live headset. No fake runtime or substituted screenshots were used in this loop.

1. Installed the rebuilt adapter with one SteamVR restart. The synthetic controllers registered at runtime indices 2 and 3, initially disconnected. A separate pre-existing pointer device at index 1 remained distinguishable.
2. Set compositor awake, worn on, and the HMD to Standing position `[0, 1.6, 0]`.
3. Set left controller to `[-0.25, 1.2, -0.5]` and right to `[0.25, 1.2, -0.5]`. Independent OpenVR readback reported connected, valid poses and left/right roles 1 and 2. Both pose matrices matched the targets within `4.8e-8` per element.
4. Captured a real compositor screenshot through MCP. The response contained both PNG image blocks and runtime metadata. Decoded image bytes were written locally and inspected.
5. Moved left by `[0.1, 0.2, -0.15]` metres and rotated yaw/pitch/roll `[35, -15, 20]` degrees. Moved right independently by `[-0.05, 0.05, 0.1]` and rotated `[-25, 10, -10]`. Readback matched both composed target matrices within `3e-8` per element.
6. Captured again through MCP. The image showed a generic controller model over the Steam dashboard. The original lower controller poses were outside the useful screenshot crop; the visible model alone is not evidence for both controllers. Independent indexed runtime readback verified both controllers.
7. Reset left only. OpenVR reported the left device disconnected. Right retained its exact commanded pose, and HMD pose/proximity overrides remained active.
8. Exercised HMD move and reset, then worn off. Released all overrides and restored automatic compositor standby. Final readback showed both synthetic controllers disconnected and invalid, HMD override disabled, worn override unset, and pause-on-standby true.
9. Read back `sshd.service` and user `steamvr.service` as active. SSH/network configuration and stock driver files were not modified.

Captures and raw runtime logs remain ignored private artifacts, not repository contents.

## Scope

- These are additional synthetic controllers, not overrides of physical controllers. Keep physical controllers off during handed tests to avoid competing role assignments.
- Translation, orientation, handedness, connection/release, and compositor image transport were verified. Button, trigger, stick, haptic, skeleton, and game-specific action binding behavior were not implemented or claimed.
- The server is stdio-only, opens no network port, and uses operator-configured SSH access. It does not expose installation or SteamVR restart tools.
- Overrides persist across MCP calls and client disconnects. No per-test rollback is necessary. Explicit reset/release commands end the input session.
- Physical cameras do not move with synthetic poses. These tests do not establish optical passthrough alignment or real-motion latency.
