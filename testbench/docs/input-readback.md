# Independent controller input readback

Build with `sh scripts/build-observe.sh`, then run on the SteamVR host:

```sh
build/frame-observe inputs "$PWD/resources/input-actions.json"
```

Use an absolute path to an existing manifest. Keep `frame-controller-bindings.json` beside it. The observer uses the public `IVRInput` API, not the input proxy socket, requested values, or cached proxy state. It initializes as an OpenVR Background application and does not change controller input, compositor settings, or scene focus. SteamVR may persist this application's action bindings.

The original resources target `controller_type: frame_controller`. Synthetic devices must publish that controller type and the matching installed Frame input profile. The resources are application bindings, not a replacement driver profile. No installed Valve profile or binding file is redistributed here.

## Coverage

The manifest defines 48 actions. Each hand reports 25 boolean/scalar component entries:

- Both hands: system and bumper click/touch, trigger and grip value/click/touch, thumbstick x/y/click/touch, thumbrest touch.
- Left only: view and dpad_up/right/down/left click/touch.
- Right only: menu and a/b/x/y click/touch.

Trigger-mode `pull` binds to a `vector1` action. Joystick-mode `position` binds to a `vector2` action, whose x/y values appear as separate component entries sharing the same action path. Click/touch slots bind to boolean actions. Every action is optional so missing or reserved bindings remain reportable rather than making the entire manifest unusable.

The Frame profile marks thumbrest as `InputValueVisibility_SteamVRInternal`. An application action may never expose it. System inputs can also be reserved. Both are attempted and reported normally, but excluded from the readiness condition. An inactive action is not evidence of a released button. Publication evidence at the driver boundary is distinct from application input verification. Pose, skeleton and haptic outputs are outside this command's scope.

## Output contract

One JSON object on stdout. SDK chatter goes to stderr. A successful snapshot has these fields:

| Field | Type and meaning |
| --- | --- |
| `ok` | `true` means a snapshot was obtained, not that every input is available |
| `command` | `"inputs"` |
| `manifest` | Supplied absolute manifest path |
| `action_set` | `"/actions/observe"` |
| `input_available` | Public `IVRSystem::IsInputAvailable()` result; false when dashboard or another runtime state suppresses application input |
| `binding_ready` | Every non-system/non-thumbrest entry on both hands is active with no action error |
| `wait_expired` | Readiness deadline elapsed without satisfying that condition |
| `updates` | Number of `UpdateActionState` calls |
| `wait_ms` | Elapsed milliseconds from the first update attempt through the final snapshot |
| `inputs` | Object with `left` and `right` objects, each keyed by exact driver component path |

Each entry under `inputs.left` or `inputs.right` has exactly these fields:

| Field | Type and meaning |
| --- | --- |
| `action` | `/actions/observe/in/{side}_{component}_{slot}`, with `thumbstick_position` shared by x/y |
| `type` | `"boolean"`, `"vector1"`, or `"vector2"` |
| `error` | Numeric `EVRInputError` from source lookup, action lookup, or action-data read, zero on success |
| `active` | Runtime `bActive`, or `null` on API error |
| `value` | Boolean or scalar for an active successful action; `null` if inactive, failed, or nonfinite |
| `origin` | Source identity object for active successful actions, otherwise `null` |

An origin object has exactly these fields:

| Field | Type and meaning |
| --- | --- |
| `handle` | Decimal string containing the action's runtime `activeOrigin` handle |
| `error` | Numeric error from `GetOriginTrackedDeviceInfo` |
| `device_path_handle` | Decimal-string runtime device-path handle, or `null` if identity lookup is unavailable |
| `device_index` | Actual tracked device index, or `null` if identity lookup fails or index is invalid |
| `serial` | Device serial, or `null` if unavailable |
| `serial_error` | Numeric `ETrackedPropertyError`, or `null` when no valid origin device could be queried |
| `synthetic` | True only for exact serial `frame_testbench_left` or `frame_testbench_right` |

Handles are strings to preserve all 64 bits in JSON consumers. To verify synthetic input, require the exact expected serial as well as `error == 0`, `active == true`, and the expected value. Hand restriction alone does not identify a synthetic device when physical controllers are also connected. An origin error does not erase a successfully read action value, but it prevents attributing that value to a device.

Fatal initialization, manifest, action-set, or update errors produce `{"ok":false,"error":"..."}` and exit 1. A process alarm produces the same error shape and exit 124. Component errors and inactive bindings do not abort the other hand's readback and exit 0 with explicit per-entry results. Consumers must inspect these fields rather than rely on exit status alone.

## Startup readiness and limits

Fresh observer processes can initially see inactive actions while SteamVR loads the bindings. The observer retries updates until both hands' nonreserved actions are active or the readiness deadline expires. `VRInputError_NoData` from `UpdateActionState` is retried within the same deadline. Other update errors fail immediately. Missing source/action handles are retained as explicit component errors for the snapshot.

- `FRAME_OBSERVE_INPUT_WAIT_MS`: readiness wait, default `2000`, allowed `0` through `10000`. Zero requests one snapshot without waiting.
- `FRAME_OBSERVE_TIMEOUT_SECONDS`: existing whole-process limit, default `20`, allowed `1` through `120`. It bounds initialization, SDK calls, binding wait, and shutdown, including hung calls.

An absent controller, partial binding or lost application focus can leave `binding_ready` false. The final snapshot still reports what SteamVR returned. `binding_ready` is a convenience condition, not proof that reserved inputs are accessible or that device identity matches the expected serial.

On the tested Frame, the open Steam dashboard suppressed every application action. Check `input_available` before diagnosing binding failure. With a connected synthetic controller, a deliberate `controller right button system on` followed by `controller right button system off` toggles the dashboard. Do not blindly toggle it on every read: that can close or open real UI. Once the dashboard was closed, 24 components per hand were active with the expected synthetic origins, including thumbrest and system touch. System click itself remained reserved. The observer reports this limitation; it does not seize focus or echo proxy values.

## Verification

`python3 -m unittest tests.test_observe -v` compiles the real observer and a fake shared library implementing the public SDK interfaces. The tests cover all component paths and resource mappings, different handed values, source identity, inactive/null semantics, individual errors, delayed binding readiness, transient update errors, nonfinite analog values, and process timeouts. Existing status, capture, debug and settings tests run unchanged.

These CPU tests do not establish that a live SteamVR version accepts the binding or exposes system/thumbrest inputs. Live verification must compare actual action values and source serials after controller publication, then verify release independently.
