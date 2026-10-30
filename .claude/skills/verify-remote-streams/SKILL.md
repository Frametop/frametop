---
name: verify-remote-streams
description: Model-check how ft-screens starts, stops and restarts a remote display's ft-stream (screens/remote.c start_stream, stop_stream, host_busy, ft_remote_tick, ft_remote_child, ft_remote_command) and when ft-stream lends or gives back a streamed host's keyboard and mouse (stream/ft-stream.cpp HostInput, stream/hostinput.h Decide and Fresh) with TLA+, and replay the known findings against the real code offline. Use before calling done any change to remote stream start, stop, restart or settings changes, to how ft-stream reads the input switch or the pointer helper's headset answer, or to when it sends engage, release or drop, and when asked whether two ft-streams can overlap for one display or whether a host's input can be lent while nobody wears the headset.
---

# Verify remote streams and the host-input lease

**The promise.** A remote display has at most one ft-stream at a time, and a host does at most one
start or release at a time. Frametop holds a host's keyboard and mouse only while the person at the
Frame is wearing the headset, on an answer about the headset that is fresh. The host's escape
hotkey stays in force until the person takes the headset off for real and puts it back on.

**The harm.** A lease sent on an old "worn" answer takes the computer's keyboard and mouse from
whoever is sitting at it. An escape that never lifts leaves Frametop unable to type on that
computer until something else resets it. Two ft-streams for one virtual display overlap Vibepollo's
Disconnect Monitor with the next launch, so the new stream can fail and wait for a restart.

## Run

Both scripts are cheap (`check` well under a minute, `replay` about 35 s, most of it the host
paths running on real time), offline and safe while the headset is worn. Run them through
`frame-job --local` so they get the Frame's CPU cap; `replay`'s container work runs outside that
cgroup, so it pins itself with `taskset` to as many CPUs as `frame-job --status` allows:

```sh
cd .claude/skills/verify-remote-streams
frame-job --local -- python3 scripts/check            # TLC: every model and rule vs BASELINE
frame-job --local -- python3 scripts/replay           # the findings against this checkout's code
frame-job --local -- python3 scripts/replay /path/to/other/checkout
```

- `check` exits 0 when every result matches the baseline, 1 when one differs, and 2 when TLC can't
  run. `--model DIR` checks another copy of `model/`.
- `replay` exits 0 when every finding behaves as recorded, 1 when one changed, and 2 when the
  harness can't build. It needs the `dev` podman container (g++, nlohmann-json, wlroots-0.20,
  wayland-server). `VERBOSE=1` prints each step and what was sent. `KEEP=1` keeps the build
  directory.

`replay` never reaches the live system. HostInput is copied out of `stream/ft-stream.cpp` with
the relay's and the pointer helper's socket names swapped for `fttest_*_<pid>`, `XDG_RUNTIME_DIR`
points at a scratch directory (so the input lock is private), and the host is a stub that plays the
escape rules from `docs/host-input.md`. HostInput runs on real time, because answers count from the
kernel's send stamp. The remote.c harness runs the real file with SteamVR
stubbed and `FT_STREAM` pointing at a fake ft-stream that takes 1 s to exit after SIGTERM. Never
point either harness at the real relay, helper or a Vibepollo host.

To look at a path or redraw a chart, use the pstack `/statechart` skill on the files in `model/`
(`host-view.mjs` and `remote-view.mjs` hold the reader's labels). The host model has about 3,600
states, over the tool's 500, so it draws only the paths to rules that break. To draw the host model
with every rule holding, use its slice: `HostChart.tla` with `HostChart.cfg` (the switch stays on,
and the helper counts only offs of 3 s or more, 345 states) and `host-chart-view.mjs`. The remote
model is drawn with `RemoteStreams.cfg` as is.

## The models and rules

**`model/HostInputLease.tla`**: one stream to one input host, from ft-stream's start to its
exit, plus the person, the switch file, the pointer helper and the host's escape. `MaxInbox = 2`
answers can wait unread, each marked `old` once 3 s have passed since it was given. Time is
abstract: `Age` stands for 3 s passing (answers go stale and the switch gets read again) and only
fires once ft-stream has caught up. `mark` is the escape marker, which outlives the stream, and
`onAfter` the pointer helper's on-at being after the escape; each answer carries it as `c`.

| Rule | Meaning |
|---|---|
| `TypeOK` | Variables stay in range. |
| `AwayGivesBack` | Once a live stream holds the lock and has read a fresh "away", nothing is lent. |
| `NoStaleLease` | Whenever input is lent while nobody wears the headset, the answer it rests on is not older than the headset change. |
| `OffOnLiftsEscape` | After the host escaped and the person took the headset off long enough and put it back on, ft-stream doesn't lease into a host that is still escaped. |
| `EscapeNeedsOff` | A release that lifts the escape follows a headset off or a switch off since the host last lent its input: never a Wi-Fi drop, a crash or a quiet helper alone. |

**`model/RemoteStreams.tla`**: one remote display with two ft-stream slots (`Slots = {s1, s2}`)
and two settings (`Settings = {a, b}`), with `SYMMETRY` on both. A stream is `none`, `starting`,
`live`, `releasing` (after SIGTERM, running Disconnect Monitor) or `exited` (not reaped yet).
`Leaving(o)` is a slot whose process is still there after a stop or its own end; `HostBusy` counts
it, as `host_busy` does.

| Rule | Meaning |
|---|---|
| `TypeOK` | At most one slot is in use for the display. |
| `OneStreamPerDisplay` | At most one ft-stream process exists for the display. |
| `OneHostOperation` | At most one start or release is in flight for the host. |
| `StoppedStaysStopped` | A stopped display never restarts and runs no stream. |
| `NeverStranded` | A display in use with no stream always has a restart due. |

## Findings

All three were confirmed by `replay` at `676c8c7`, and all three are fixed.

**H1: a lease could rest on an old "worn" answer** (`NoStaleLease`; fixed in `76121c7`). The
path was 11 steps: `StreamStarts SwitchOff StreamLive TickLive HelperAnswers HeadsetOff ReadSwitch
Age SwitchOn ReadSwitch TickLive`. ft-stream asked the helper, then saw the switch off and unlocked
before it read the answer. The "worn" answer waited in the socket, and when the switch came back on
`TakeAnswers` stamped it with the read time, so `Fresh` called it new and ft-stream engaged with
nobody wearing the headset (in the replay, on a 9 s old answer). Now the socket has
`SO_TIMESTAMPNS`, and `TakeAnswers` dates each answer (worn and paused) from the kernel's send
stamp with `hostinput::GivenAt`; an answer without the stamp is ignored. In the model a waiting
answer turns `old` on `Age`, and `TickLive` takes its freshness from that. Reverting only the worn
stamp doesn't bring H1 back in the replay, because the relay's pause answer waited too and blocks
the lease; reverting both does.

**H2: an escape survived an off-on across two streams** (`OffOnLiftsEscape`; fixed in
`2c783dd`). The path was 15 steps: `StreamStarts StreamLive TickLive HelperAnswers HeadsetOff
TickLive HostEscape StreamEnds StreamStarts Age HeadsetOn StreamLive TickLive HelperAnswers
TickLive`. The first stream ended with `Stop(kDrop)`, which keeps the escape. The next one started
with the headset off and wasn't live, so it never asked the helper, and it engaged once the headset
was back on: no release crossed the off-on. The Vibepollo fork keeps one process-wide escape flag
that only a client `release` (or a Vibepollo restart) clears. Now the stream that hears the escape
writes `$XDG_RUNTIME_DIR/frametop-escape-ID` (`Got`, `hostinput::EscapeMark`), and a release that
goes removes it. The next stream picks it up (`Recall`, at `Open` and when it takes the lock) and
asks `gaze ? headset on-at`. The pointer helper notes when the headset last went back on after an
off of 1.5 s or more (`headsetOnAt`, CLOCK_BOOTTIME), and an on-at after the escape owes a release
(`OffOnSince`), which goes before the first engage. The plain `gaze ? headset` reply is unchanged,
because gazecheck takes only two or three words. `replay` keeps a Wi-Fi drop with the headset on as
`H2-control`: the escape must stand. `EscapeNeedsOff` checks the same in the model.

**R1: a stop and a start overlapped two ft-streams for one display** (`OneStreamPerDisplay` and
`OneHostOperation`; fixed in `e15b65f`). The path was 5 steps: `YouStart TickStarts YouStop
YouStart TickStarts`. `stop` cleared `used` but kept the pid until it was reaped, and `host_busy`
only counted used slots, so a new slot's ft-stream connected while the old one was still
releasing its Remote Monitor (ft-layout's route change, or Connected off and on). Now `host_busy`
also counts a slot that is `leaving`: a pid not yet reaped whose stream was stopped (`fd < 0`) or
said `lost`. A stream asked to end gets SIGKILL `FT_REMOTE_QUIT_MS` (20 s) after SIGTERM
(`quit_at`), so a hung one can't hold the display forever. `replay` keeps the in-place settings
change as `R1-control`, which must not overlap. `screens/tests/remote-test.c` (in `just test-c`)
tests the same cases on the real remote.c, plus another host (no wait) and a hung ft-stream.

The doc drift found with these (ft-screens neither stops streams on headset off nor sends
`input engage|release`) was fixed in `docs/host-input.md` at `8ee0a21`.

## Model to code

| Model action | Code |
|---|---|
| `StreamStarts`, `StreamLive`, `StreamEnds` | `stream/ft-stream.cpp` `HostInput::Open` (`Recall` reads the marker), `Tick(now, live)`, `Stop(kDrop)` on exit |
| `ReadSwitch` | `Tick`: `checkAt` every `kStaleNs`, `HostFlag(host, "input")`; switch off -> `Apply(kRelease)` then `Unlock` |
| `TickIdle` | `Tick` with the switch off or not live: `Apply(Decide)`, `Unlock` |
| `TickLive` | `Tick`: take the flock (`Recall`), ask (`gaze ? headset on-at`), `TakeAnswers` (last one wins, dated by `GivenAt` from the `SO_TIMESTAMPNS` stamp; `OffOnSince` owes a release), `Apply(Decide)` |
| `TickWith` | `Apply`, `Stop`, the `releaseOwed` and `last` edges, `Engage`/`Release`/`Drop`; a release that went removes the marker (`Send`) |
| `Decide` | `stream/hostinput.h` `Decide` (switch, live, fresh away -> release; not fresh -> drop) |
| `Age` | 3 s passing: `Fresh` (`kStaleNs`) on the given-at time, waiting answers turn `old`, and the next switch read |
| `HelperAnswers` | the pointer helper's reply to `gaze ? headset on-at` |
| `HeadsetOn` | `pointer/helper/ft-pointer.cpp`: `headsetOff` going false sets `headsetOnAt` (after `HEADSET_OFF_AFTER`, 1.5 s, of idle: an off of 3 s always counts, a shorter one may) |
| `HostEscape` | the host's escape hotkey (`Got` reads the record and writes the marker) |
| `YouStart(v)` | `screens/remote.c` `ft_remote_command` `start` (slot pick, same-settings check, `stop_stream` for new settings) |
| `YouStop` | `ft_remote_command` `stop`: `used = false`, `stop_stream` (SIGTERM, `quit_at`, pid kept) |
| `TickStarts` | `ft_remote_tick`: `restart_at` due, `pid == 0`, `fd < 0`, `!host_busy` -> `start_stream` |
| `HostBusy`, `Leaving` | `host_busy`: a used slot of the host still starting (20 s), or any slot `leaving` of the display or host |
| `StreamLive`, `CantConnect`, `StreamEnds` | the stream's messages on its fd (`readable`, `message`), `schedule_restart` |
| `ReleaseDone`, `Reaped` | ft-stream's exit (Disconnect Monitor for `monitor`), `ft_remote_child` (`pid = 0`, `quit_at = 0`); the SIGKILL in `ft_remote_tick` ends a hung one |

## Scope

Left out of the host model: the relay's pause answer (taken as fresh and not paused), the test
files (the `verify-fake-worn-guard` skill covers them), LiSend failures, two streams to one host and
the lock handoff between them (`Recall` at the lock is for that), the host fork's own code, timing
finer than the 3 s `Age`, and the 500 ms lease cadence. A helper restarted since the escape says
on-at 0 and lifts nothing, and a marker left by a Vibepollo restart costs one release that does
nothing: neither is modeled.

Left out of the remote model: more than one display or host (`remote-test.c` covers another
host), the 20 s `host_busy` timeout, the backoff timing (2 s to 30 s), the SIGKILL timing (a hung
ft-stream simply ends in the model; `remote-test.c` covers the kill), spawn failure, panels, rings
and input routing. Not escalated in the code either: an ft-stream that said `lost` on its own and
then hangs on the way out isn't killed, since nothing asked it to end. Its Remote Monitor release
is bounded by curl's 15 s timeout (`stream/host.cpp`), and the display waits until it's reaped.

`replay` only tracks H1, H2 and R1 (and their controls). A change that breaks something else, for
example `Decide` no longer releasing on "away", shows up in `check` once the model follows the code,
not in `replay`.

## When a result differs

- **A rule that held now breaks, or a known finding breaks by another path.** Read the path with
  `/statechart`, then replay it against the code step by step: one model action, one code step from
  the table above. If the code does it, it's a real bug. Report it in those words, and don't weaken
  the rule or the model to pass. If the code can't do a step, the model is wrong: fix the model to
  match the code, and say what was wrong.
- **A known finding now holds, or `replay` says it no longer happens.** Either a fix landed, or the
  model or the harness drifted from the code. Confirm which before you touch the baseline.
- **`replay` can't build.** HostInput or remote.c changed shape (a renamed member, the socket names
  moved). Update `replay-src/` to match, and say what changed.
- **An intended change** (a fix, a new step): change the model actions it touches, run `check`,
  replay any new path against the code, then update `BASELINE` in `scripts/check` and
  `scripts/replay`, and the findings above.

## Coverage and sync

Covered code:
- `stream/ft-stream.cpp`: `struct HostInput` (`Open`, `Got`, `Stop`, `Apply`, `Unlock`,
  `TakeAnswers`, `Recall`, `Tick`), and the exit path's Disconnect Monitor for `monitor`
- `stream/hostinput.h`: `Decide`, `Fresh`, `GivenAt`, `EscapeMark`, `ReadMark`, `WriteMark`, `OnAt`,
  `OffOnSince`, `Engage`, `Release`, `Drop`
- `pointer/helper/ft-pointer.cpp`: `headsetOff`, `headsetOnAt`, the `gaze ? headset [on-at]` reply
- `screens/remote.c`: `start_stream`, `stop_stream`, `leaving`, `host_busy`, `schedule_restart`, `message`,
  `ft_remote_tick`, `ft_remote_child`, `ft_remote_command` (`start`, `stop`)
- `layout/ft_layout.py`: `start_remote`, `remote_command` (`host NAME route=/direct=`)
- `docs/host-input.md`: the lease rule, the escape and the headset section

**Sync commit: `2c783dd`** (experimental, 2026-10-30). The models describe the covered code as of
this commit. To see whether they need another look:

```sh
git diff 2c783dd -- stream/ft-stream.cpp stream/hostinput.h pointer/helper/ft-pointer.cpp screens/remote.c layout/ft_layout.py docs/host-input.md
```

Move the sync commit forward only after you have read that diff, confirmed that every model action
still matches the code (and fixed the model where it didn't), and seen `check` and `replay` match
the baseline at the new commit. Bring a drifted model back in line with the pstack
`/maintain-formal-verification-skill` skill.
