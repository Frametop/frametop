# Frame controller inputs

The virtual controllers use the installed Steam Frame controller profile, rather than mapping generic gamepad inputs onto guessed button names. The native adapter references `{frame_controller}/input/frame_controller_profile.json` from SteamVR; the project does not redistribute that runtime file or its render models.

## Input layout

| Hand | Buttons |
| --- | --- |
| Both | `system`, `bumper`, trigger click, grip click, thumbstick click |
| Left | `view`, `dpad_up`, `dpad_right`, `dpad_down`, `dpad_left` |
| Right | `menu`, `a`, `b`, `x`, `y` |

Each listed button also has a touch component. `thumbrest` has touch only, no click. Triggers and grips have independent normalized values from 0 to 1. Thumbsticks have X and Y values from -1 to 1. This is 25 boolean/scalar components per hand. Left ABXY is not a supported alias for the D-pad: use the actual profile names.

The profile examined on the development headset had SHA-256 `0cbb52f75d041a1e1d5a642f1d56373170c54b0862bd1391ce16aa49b0859be6`. Runtime updates can change the profile, so recheck component definitions when updating SteamVR.

## CLI examples

Connect the selected synthetic controller by setting its pose before driving inputs:

```sh
./frame-testbench controller right set --position .25 1.2 -.5
./frame-testbench controller right button a on --touch on
./frame-testbench controller right button a off --touch off
./frame-testbench controller right trigger .7 --click on --touch on
./frame-testbench controller right grip .4 --touch on
./frame-testbench controller right thumbstick .3 -.6 --click off --touch on
./frame-testbench controller right touch thumbrest on
./frame-testbench inputs

# Neutralize buttons, analog values, and touches while retaining this pose.
./frame-testbench controller right inputs-reset

# Disconnect the synthetic controller and clear its inputs.
./frame-testbench controller right reset
```

Add `--host frame` before `controller` or `inputs` to use SSH. Inputs persist between commands. A press is an explicit on followed by off, not an implicit timed pulse. Analog value, click, and touch are independent: omitted optional flags keep their previous values. A trigger value of 1 does not automatically set its explicit click component. Applications may apply their own binding thresholds to an analog value.

Invalid side/control combinations, nonfinite numbers, and out-of-range values fail before mutation. A multi-component command validates the complete batch before calling the runtime. The OpenVR component API itself is not transactional; runtime publication failures must be treated as partial failures, not rolled-back input.

Global `release` clears both controllers' inputs and poses, plus HMD pose/proximity. HMD-only reset commands remain selective. Keep physical controllers off for synthetic handed tests to avoid competing role assignments. Virtual button presses can activate real application UI, so use a disposable test scene rather than a purchase or destructive confirmation dialog.

## MCP

The same operations are available as `controller_button`, `controller_trigger`, `controller_grip`, `controller_thumbstick`, `controller_touch`, and `controller_inputs_reset`. Use `controller_input_status` for independent runtime input readback. See [MCP tool reference](mcp.md) for argument names and schemas.

## Readback and limits

`status` reports native input state and device poses. The separate `inputs` command uses an original application action manifest and bindings to read values through public SteamVR input APIs, independently of the proxy's requested-state cache. An inactive or unbound action is not evidence of a released button; check activity and source identity as well as the value.

Application bindings can transform analogs or synthesize clicks. The installed profile marks thumbrest touch as SteamVR-internal, and system-button handling can be reserved by the runtime. These restrictions should remain visible in readback rather than be replaced with the proxy's requested values.

Haptics, skeletal hand input, battery/radio behavior, and physical controller firmware are outside this implementation. Virtual poses do not move the physical cameras.
