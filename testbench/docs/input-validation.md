# Button, analog and touch verification

Verified on Steam Frame with SteamVR 2.17.10 on aarch64, 2026-10-06. This extends the [pose/MCP verification](controller-mcp-validation.md).

## Live results

The installed Frame controller profile has 25 boolean/scalar input components per hand, 21 boolean and 4 scalar. The synthetic devices registered exactly those side-specific paths and types. Neither the installed Valve profile nor its render models are redistributed; the proxy references SteamVR's existing profile and identifies as `frame_controller` for bindings.

The live test used an official Python MCP SDK client, the real stdio server, SSH, the native adapter, and a separate public `IVRInput` observer. It did not substitute the proxy's state for application readback.

1. Rebuilt and installed the adapter with one SteamVR restart. Set HMD pose/proximity and connected both synthetic controllers. Readback confirmed input readiness and valid per-hand device poses.
2. Initial application actions were all inactive. Probes showed `IsInputAvailable()==false` while Steam's dashboard held focus. A synthetic right system-button press/release closed the dashboard. `IsInputAvailable()` became true and actions became active. This verified system-click behavior despite its reservation from ordinary application action readback.
3. Through MCP, held all non-system buttons and their touch components on both hands, plus system touch and thumbrest touch. Set left trigger/grip to 0.8/0.7 and stick to `[-0.6, 0.5]`; right trigger/grip to 0.65/0.4 and stick to `[0.3, -0.4]`. Set trigger/grip/stick click and touch explicitly.
4. Compared every accessible action with the published driver state. All 48 accessible components matched within `1e-5`, with active action origins attributed to the exact synthetic serial for that hand. This included thumbrest touch on this runtime build. System-click action remained inactive and was not misreported as false.
5. Reset left inputs only. All left accessible actions read neutral, both poses remained active, and every right input retained its previous value.
6. Reset right controller pose. Its buttons, touches and analog values became neutral and the device disconnected. The left controller stayed connected.
7. Released all overrides and restored automatic compositor standby. Final driver readback showed every input neutral, both controller pose overrides false, HMD override false, and worn override unset.

The MCP server listed 18 tools. Input commands persisted across separate calls; no per-command install/revert cycle was used. Raw snapshots and probe output remain in ignored private artifacts.

## Automated coverage

The integrated suite tests the complete profile registration, finite/range/sidedness validation, all-or-nothing command validation, independent analog/click/touch fields, correct OpenVR scalar units, error reporting for partial runtime publication, neutralization failures and retry, selective/global reset, deactivation/reactivation and cleanup, and physical-device forwarding.

CLI/MCP tests cover all input operations, strict typed schemas, pre-I/O validation and real stdio SDK calls. The observer's fake SDK tests cover original application bindings, source identity, inactive/null values, per-component failures, delayed bindings, process deadlines and the runtime input-availability flag. The availability regression failed before the field was implemented, then passed.

## Limits

- Inputs act on real applications. Keep tests out of purchase, deletion, or other consequential dialogs.
- Dashboard focus can suppress application input without indicating a publication failure. `inputs.input_available` and each action's `active` field distinguish this from neutral values.
- Trigger/grip analog values and explicit click/touch are separate. A game's own bindings may apply thresholds or transformations.
- Physical controllers are not overridden. Keep them off while testing synthetic handed devices to avoid role competition.
- These results do not establish game-specific compatibility, haptics, skeletal input, hardware latency, or physical-camera alignment.
