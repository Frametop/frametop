# G6 Remote displays and host input

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers the Remote Displays app and ft-layout, remote.c's stream slots in ft-screens, one ft-stream per display, the host's audio and input locks, the host-input lease, the relay's network devices, the Frame mic, and the testbench's fake-worn guard.

Model: `G6-remote.tla` (module `G6RemoteExit`): how an ft-stream ends and whether something always reaps it. Configs: `Exit` (the code), `Live` (liveness of asked and stuck ends), `PreFix` (before `e15b65f` added the SIGKILL), `PreR23` (the code at `78e93b1`, before the R2 and R3 fixes). Since those fixes the model tracks `clock` (`quit_at` is set: a SIGKILL is coming) apart from `quit` (the socket is closed), and HungGetsKilled and HungLostGetsKilled read `clock`.

The rest of G6 is checked by two more models here, which matched their baselines at `78e93b1`. The verify skills (`verify-remote-streams`, `verify-fake-worn-guard`) run their checks:

- [remote-streams/](remote-streams/): `HostInputLease.tla` (3,561 states), `HostChart.tla` (345), `RemoteStreams.tla` (164).
- [fake-worn-guard/](fake-worn-guard/): `FakeWornGuard.tla` (264 to 632 states).

Results at `78e93b1`:

| Config | Rule | Result |
| --- | --- | --- |
| Exit | TypeOK, OneStreamPerDisplay, OneHostOperation, StoppedStaysStopped, NeverStranded | Hold (312 states) |
| Exit | HungGetsKilled | Breaks, 5 steps (R2) |
| Exit | HungLostGetsKilled | Breaks, 6 steps (R3) |
| Live | AskedEnds | Holds (1,232 states) |
| Live | StuckEnds | Breaks (R2, R3 as liveness) |
| PreFix | AskedEnds, StuckEnds | Break: the slot leak `e15b65f` fixed |
| PreFix | OneStreamPerDisplay | Holds |

Fixed and drawn green: H1 (`76121c7`), H2 (`2c783dd`), R1 and the slot leak (`e15b65f`), and the fake-worn bugs A and C (`37d0fbf`), D (`0c7eb9e`), B (`bd606ce`), E (`38d6257`) and F (`1f485fe`).

Results with the fixes (`144bd31`): Exit holds every rule (332 states) and Live holds AskedEnds and StuckEnds (1,312). PreR23 still breaks HungGetsKilled in 5 steps and HungLostGetsKilled in 6 (312 states), as Exit did at `78e93b1`. PreFix breaks as before. The chart still draws R2 to R5 red and is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| R2 | DEE-259 | HungGetsKilled | An ft-stream the host ended that hung in LiStopConnection was never killed. Its panel froze and still said live | Fixed in `144bd31`. ft-stream says `state lost` before LiStopConnection, and ft-screens starts the SIGKILL clock on it |
| R3 | DEE-260 | HungLostGetsKilled | One that hung after it said lost (decoder close, Disconnect Monitor) was never killed, and held every display of its host | Fixed in `144bd31`. ft-screens sets `quit_at` on `lost` or a closed socket, so FT_REMOTE_QUIT_MS (20 s) later it's killed |
| R4 | DEE-261 | From the code | A desktop stop SIGTERMed its ft-streams and exited. One that hung was an orphan nobody killed | Fixed in `144bd31`. ft-stream ends itself 25 s after its way out starts (`alarm`). Not modelled. An orphan can still overlap the next desktop's stream of its display for up to 25 s |
| R5 | DEE-262 | From the logs (minor) | A 404 that never clears retried every 30 s for good | Fixed in `144bd31`. A stream that ended with a 404 backs off up to 5 minutes. It still retries, and connecting it again tries at once |

A stuck ft-stream keeps `frametop-input-HOST.lock` and `frametop-audio-HOST.lock` until it dies, so the host's other streams get no keyboard, mouse or sound.
