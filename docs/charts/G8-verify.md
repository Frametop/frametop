# G8 Verify, data and diagnostics

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers where each testbench fake enters the real pipeline, and how long it lasts. Fake 1 is the input proxy inside vrserver: the `90-frame-testbench.conf` drop-in preloads `interpose.cpp`, which wraps `driver_cv.so` and takes worn, head pose, HMD button and synthetic controller overrides on `RUN/frame-testbench.sock`. Fake 2 is the fake trackers (`frame-testbench-fakes`, `fakes.py`): stand-in eye files for ft-gaze, and a marker that parks ft-hands while fake hands are published. Fake 3 is Frametop's own sockets (the relay's testbench devices, the pointer helper, ft-powerd). Fake 4 is the fake-worn guard (`frametop-fake-worn-guard`, `hostinput.py`). The chart also draws the verify run's lease and cleanup (`verify-frametop` launch, guard, snapshot, cleanup), the MCP server, the Hand and Eye Recorders with their export and Hugging Face upload, `report.sh`, `doctor.sh`, the logs and the tour. Arrows into G1, G3, G4, G5 and G6 show where each fake feeds those groups.

Models:

- `G8-verify.tla` (module `G8Verify`): one verify run, its lease, the proxy's overrides, the fake trackers, an agent that can die mid-run, status polls from later agents, and a wearer. Configs: `Code` (the code as it is), `NoDeath` (the agent always lives to clean up), `Overrides` (only the overrides' bug), `OverridesFix` (overrides bound to the lease), `Polls` (only the idle hour's bug) and `Fixed` (both fixes).
- `G8-verify-Guard.tla` (module `G8Guard`): extends verify-fake-worn-guard's `FakeWornGuard.tla`, read from `fake-worn-guard/` without changes. It adds fairness for the system's own steps and the rule `GuardRestoresReal`. Configs: `LiveClean` (no faults), `LiveHangs` (the proxy can stall), `LiveKills` (the guard can die), `LiveNoF` (stalls and deaths, but not mid-fake) and `LiveAll` (every fault). The skill's own configs and rules still hold at `78e93b1`: Clean 88, Hangs 188, Kills 264, NoF 480 and All 632 states.

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 11 configs |
| LaunchClean | A run starts on a clean headset (launch's doctor) | Holds in all 6 Verify configs |
| NoDriveOnWearer | Drive steps happen only with nobody in the headset (`verify-frametop guard`) | Holds in all 6 Verify configs |
| OverridesEnd | The proxy's overrides end once nobody drives the run | Breaks in Code and Overrides, 5 to 6 steps then a loop (V1). Holds in NoDeath, OverridesFix, Polls and Fixed |
| FakesEnd | The fake trackers end once nobody drives the run | Breaks in Code and Polls, 6 to 9 steps then a loop (V2). Holds in NoDeath, Overrides, OverridesFix and Fixed |
| WearerGetsReal | Someone who puts the headset on gets their own head, proximity and trackers back | Breaks in Code, Overrides and Polls (V1, V2). Holds in NoDeath, OverridesFix and Fixed |
| GuardRestoresReal | Once the wearer is faked with no live guard and no command running, the fake ends with no command needed | Breaks in LiveNoF and LiveAll (V3). Holds in LiveClean, LiveHangs and LiveKills |

States per run: 26 (NoDeath) to 98 for Verify, 88 (LiveClean) to 632 (LiveAll) for Guard.

Results with the fixes (`be6480e`): every rule holds in all 11 configs, 56 runs. States per run: 26 (NoDeath) to 98 for Verify, 96 (LiveClean) to 644 (LiveAll) for Guard. The Verify model now describes the fixed code: `TtlEnds` (the proxy's override TTL) replaces `LeaseEnds`, and `StatusPoll` restarts nothing. `FixLease` and `FixPoll` are no longer used, so Code, Overrides, OverridesFix, Polls and Fixed check the same code. `FixLease` was a different fix (overrides bound to the verify run's lease); the code took a TTL inside the proxy, which needs no lease and also covers runs that never took one. The Guard wrapper adds `TtlEnds` to the skill's `Next` with strong fairness (the TTL's clock runs through the proxy's stalls, so a proxy that is up again and again acts on it), and its configs now also check the skill's `NoHostTakeover` and `FakeIsNoticed`. `FakeWornGuard.tla` is unchanged. To see the old breaks, check out `f85dff9` and run its copy of the models.

Undoing a fix in a copy of the models brings its break back: without `TtlEnds` (and its fairness), OverridesEnd and WearerGetsReal break in every Verify config but NoDeath. With a status poll restarting the hour again, FakesEnd and WearerGetsReal break in Code, Polls and Fixed. Without `TtlEnds` in the Guard wrapper, GuardRestoresReal breaks in LiveNoF and LiveAll, and it breaks there too with weak fairness in place of strong: a proxy that stalls just before each repeated `worn off` never stays up long enough for weak fairness to apply.

V4 and V5 aren't modelled. `input/test/hostin-test.py` checks V4 (a `hostbye` mid-drag sends `btn trigger 0`), and `hands/rec/tests/test_validate.py` checks V5. Both fail on the old code.

The chart (`G8-verify.mmd`, `.svg`) still draws V1 to V5 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| V1 | DEE-281 | OverridesEnd, WearerGetsReal | The input proxy's overrides (worn on or off, head pose, HMD buttons, synthetic controllers) had no time limit, and a real wearer didn't end them (`testbench/native/input_state.cpp`). Only `frame-testbench release`, `verify-frametop cleanup` or a SteamVR restart did. A run whose agent died kept them, and someone who put the headset on later got a frozen head pose, or standby under worn off | Fixed in `be6480e`. The proxy ends every override after 300 s with no command other than `status` (`FRAME_TESTBENCH_OVERRIDE_TTL` in vrserver's environment changes it). A release the proxy refuses is tried again every second. `status` reports `override_ttl` and `override_idle` |
| V2 | DEE-282 | FakesEnd, WearerGetsReal | Any command restarted the fake trackers' idle hour, `status` too (`fakes.py`), so a later agent's doctor and status checks kept a dead run's fake eyes and parked hands alive | Fixed in `be6480e`. A `status` question no longer counts as a command for the idle hour |
| V3 | DEE-283 | GuardRestoresReal (minor) | A guard that died while the input proxy stalled. `after_guard` tried the release 3 times 0.2 s apart, then gave up (`hostinput.py`), and the faked wearer stayed until the next `frame-testbench` command | Fixed in `be6480e` through V1: the proxy's TTL ends the faked wearer once it runs again. `after_guard`'s retries and the flag are unchanged, so NoHostTakeover still holds |
| V4 | DEE-284 | not modelled (minor, G3 side) | In pointer mode the relay's drop of a host mouse let go of its buttons only on the virtual mouse (`release_held`, `input/input-relay.py`). A testbench killed mid-drag, or a release read after `hostbye`, left the 3D mouse's button down until the next click | Fixed in `be6480e`. Each device remembers the pointer-mode actions its buttons hold, and its drop releases them through the same path, unless another device holds the same action. `testbench/docs/desktop.md` Limits now says so |
| V5 | DEE-285 | not modelled (minor) | The hand upload's draft gate opened on `FT_HANDREC_ALLOW_UPLOAD=1` alone (`hands/rec/hub.py`), while the eye upload's opened only for another dataset (`gaze/rec/hub.py`), and a missing text counted as final (`recorder/hub.py` `is_draft`). All five consent texts are final today, so nothing could leak | Fixed in `be6480e`. The hand gate opens a rehearsal only with the kind's dataset set to a test one (`FT_HANDREC_DATASET`, or `FT_HANDREC_TEST_DATASET` for testing), and a missing consent or upload text counts as a draft. `rehearse.sh` passes its `--repo` to the check, and the rehearsal banner shows only when uploads are allowed |

Design choices in these fixes (Daenon can override each):

- V1: the TTL is 300 s. It covers an agent's normal gaps between proxy commands, a build or long thinking included, and a wearer who puts the headset on after a dead run gets their head and proximity back within 5 minutes instead of never. Status polls don't renew it, so a doctor or the MCP's status tool can't keep a dead run's overrides alive. An agent that stops for longer than 5 minutes loses its overrides and sets them again with its next command. A looped replay renews it through its pose commands.
- V1: the TTL lives in the proxy, not in `verify-frametop`'s lease, so it also covers `frame-testbench` and MCP use outside a verify run.
- V2: the fakes keep their idle hour; only `status` stops restarting it.
- V3: no new retry loop in `after_guard`. The TTL bounds the stalled case at 300 s after the proxy runs again.
- V4: a dropped device's held actions go up only when no other device holds the same action, so a real mouse's drag survives the testbench mouse leaving.
- V5: only the hand gate changed. `recorder/hub.py` `is_draft` still reads a missing file as not a draft, since the shared window uses it for its banners; the Eye Recorder already checks that its texts exist.

Fixed before `78e93b1`, drawn green: the fake-worn guard's A to F (`37d0fbf`, ft-stream `bd606ce` in merge `81a5c82`, `0c7eb9e`, `38d6257`, `1f485fe`), which the skill's model checks.
