---
name: verify-fake-worn-guard
description: Model-check the testbench's fake-worn guard (testbench/frame_testbench/hostinput.py guard, cli.py `worn`, drop_unsafe_worn, release_or_keep_flag) and ft-stream's lease decision (stream/ft-stream.cpp HostInput::Tick, stream/hostinput.h) with TLA+, and replay the testbench's known bugs against the real code. Use before calling done any change to how the testbench fakes the wearer, starts or stops the host-input guard, writes or clears frametop-fake-worn.json, takes a host's input lock, or handles a failed proxy request, or to when ft-stream sends a lease, and when asked whether a faked wearer can take a streamed host's keyboard and mouse.
---

# Verify the fake-worn guard

**The promise.** A wearer faked by the testbench (`frame-testbench worn on`) never lends Frametop a
streamed host's keyboard and mouse, and whenever a testbench command has finished with the wearer
still faked, `$XDG_RUNTIME_DIR/frametop-fake-worn.json` says so.

**The harm.** ft-stream leases a host's input while it holds the host's input lock and the pointer
helper says "worn", and the helper can't tell `worn on` from a real head. A break takes the keyboard
and mouse from whoever sits at that computer (2026-10-30: oma7, mid-sentence; see
`docs/host-input.md`).

Tests can't reach this: it depends on the order in which a guard unit, a stream, the pointer
helper's answers, a stalled input proxy and a kill signal act. The model checks every order of
those steps.

## Run

From the repo root, on the Frame (`check` takes about 18 s, `replay` a few):

```sh
frame-job --local -- .claude/skills/verify-fake-worn-guard/scripts/check
.claude/skills/verify-fake-worn-guard/scripts/replay
```

- `scripts/check` runs TLC 1.7.4 on `docs/charts/fake-worn-guard/FakeWornGuard.tla`, once per config and rule, and
  compares each result and each shortest counterexample with the baseline in the script. Exit 0:
  matches. Exit 1: something differs. Exit 2: TLC couldn't run. TLC comes from `TLA2TOOLS` or is
  downloaded once into `~/.cache/frametop-formal` and checked against its SHA-256. Java comes from
  `JAVA`, then `java` on PATH, then mise's `java@temurin-25`. `--model DIR` checks another copy of
  the model.
- `scripts/replay` drives the real `cli.py` and `hostinput.py` offline (temp runtime dir, fake
  proxy, made-up host; it touches no live proxy, stream, lock or flag) and says whether findings
  A to F still happen. E is ft-stream's: it builds a small program against `stream/hostinput.h`
  with g++ (`CXX`) in its temp dir and runs ft-stream's two ticks. Exit 0 means the baseline: A, C,
  D, E and F no longer happen, and B still does at the testbench level but leaves a flag that
  doesn't allow host input, which stops ft-stream's lease.

Read every result, and draw the chart, with the pstack `/statechart` skill. Give it
`docs/charts/fake-worn-guard/FakeWornGuard.tla` with `docs/charts/fake-worn-guard/Kills.cfg` (264 states: it draws F's window, the guard gone
before `worn 1`) or `docs/charts/fake-worn-guard/NoF.cfg` (480 states: the proxy hangs), and `docs/charts/fake-worn-guard/view.mjs` as its
view. `All` (632 states) is over its 500-state chart budget, so it would draw only bug paths. It
turns each violation into the shortest path in plain words. Rendered charts are in `work/` if
that folder is still there (not committed): `fake-worn-guard.png` (Kills, no bug paths),
`fake-worn-guard-small.png` (the same, under 100 KB), `fake-worn-guard-nof.png` (NoF) and
`fake-worn-guard-with-B.png` (before ft-stream read the flag).

## The rules and configs

Rules (INVARIANTs in the model):

- `NoHostTakeover`: ft-stream never leases the host. Nobody wears the headset in the model, so
  any lease comes from a faked wearer, or from a "worn" answer given while one was faked.
- `FakeIsNoticed`: when no testbench command is running and the wearer is faked, a flag is there:
  one naming a live guard (which holds the locks), or one naming no live guard (which the next
  command's `drop_unsafe_worn` and the guard unit's `after_guard` release). In the model that is
  `flag # "none"`; `"cli"` is a flag a command wrote after a release it couldn't confirm.
  Mid-command, the command's own post-check covers it.
- `TypeOK`: the lock has one holder, only a running guard or a stream holds it, and only a stream
  that holds it asks the helper, keeps an answer, remembers a test's file or leases.

Configs (constants):

`GuardCanDieMidFake` lets the guard die (SIGTERM or SIGKILL) between `start_guard` seeing it live
and the post-check after `worn 1`, which was finding F's window.

| Config | `GuardCanDie` | `ProxyCanHang` | `GuardCanDieMidFake` | Baseline |
| --- | --- | --- | --- | --- |
| `Clean` | FALSE | FALSE | FALSE | every rule holds: the design is right when nothing fails |
| `Kills` | TRUE | FALSE | TRUE | every rule holds (`NoHostTakeover` broke by finding B, then F, before their fixes) |
| `Hangs` | FALSE | TRUE | FALSE | every rule holds (both broke, by finding A, before the fix) |
| `All` | TRUE | TRUE | TRUE | every rule holds (`NoHostTakeover` broke as in `Kills`, and `FakeIsNoticed` by finding D, before their fixes) |
| `NoF` | TRUE | TRUE | FALSE | every rule holds: `All` without F's window, which guarded B, D and E while F was open, and is small enough to chart |

## Findings

A, B and C were real, replayed against the code by `scripts/replay` at `68d3f83`. A and C are
fixed: `cli.py` `release_or_keep_flag` and `drop_unsafe_worn` release through
`hostinput.drop_wearer`, so the flag goes only once the proxy confirms the release (or isn't
there), and a flag naming the command is written if none is left. D, which the fix for C
uncovered, is fixed too. B is fixed in ft-stream (oma7's `bd606ce`, merged in `81a5c82`), and E,
which that fix uncovered, in `38d6257`. F is fixed in `1f485fe`. Every config now holds.

B and E are fixed only in the code on this branch. The installed ft-stream may predate them:
`strings` on it should show "while %s says a test may not take them".

- **A (fixed). `worn on` dropped the flag after a timeout.** The proxy stalls. `worn on` starts the guard,
  which takes the lock. `bench.control('worn 1')` acts but its reply times out.
  `release_quietly` sent `worn-release`, which also timed out and was swallowed, and then
  `clear_flag()` ran anyway (`cli.py`, the `except BaseException` after `worn 1`). Then the wearer
  was faked with no flag. The guard saw the flag gone and closed the lock, so a stream took the
  lock and leased the host. The post-check branch after `worn 1` had the same
  `release_quietly` + `clear_flag` shape.
- **B (fixed in ft-stream, `bd606ce`). A killed guard frees the lock before anything releases
  the wearer.** SIGKILL or OOM of the guard unit makes the kernel drop its `flock` at once.
  `ExecStopPost` (`after_guard`) and the next command's `drop_unsafe_worn` release the wearer only
  later. A stream that took the lock in that gap leased the host. The testbench still leaves that
  gap (`scripts/replay` says B happens), but the flag stays, and ft-stream now reads it every tick
  (`hostinput::TestForbidding`) and sends no lease while it doesn't allow host input
  (`StreamTick`). `stream/test/hostinput-test.cpp` guards it in the code; dropping the flag test
  from `StreamTick` brings B's path back in `Kills`, `All` and `NoF`.
- **C (fixed). `drop_unsafe_worn` dropped the flag after a timeout.** A command finds a dead
  guard's flag. `bench.control('worn-release')` times out. `transport.request` raises
  `socket.timeout`, an `OSError`, which `except OSError` treated as "no proxy". `clear_flag()` then
  ran with the wearer still faked, so nothing noticed any more. `drop_wearer` in `hostinput.py`
  gets this right: it treats only `FileNotFoundError` and `ConnectionRefusedError` as "no proxy"
  and keeps the flag otherwise, and `drop_unsafe_worn` now uses it. The model's `YouRunOther`
  held C. It was never TLC's shortest path for any rule, so `scripts/replay` checks it.
- **D (fixed in `0c7eb9e`). `worn on` let `start_guard` clear a flag whose release was
  unconfirmed.** It surfaced once C kept the flag, and only in `All` (a hang and a guard death in
  one run): a timed-out release leaves a flag, the next `worn on` can't confirm its release
  either, `start_guard` runs `clear_flag()`, and the new guard dies before it takes the lock, so
  `worn on` is refused with the wearer faked and no flag. `worn on` now refuses while such a flag
  is there (`OnCheck`). `scripts/check` guards D in `All` and `NoF`, and the unit test
  `test_worn_on_is_refused_while_an_unconfirmed_release_left_a_flag` guards it in the code.
- **E (fixed in `38d6257`). ft-stream leased on a "worn" answer from while the wearer was
  faked.** A guard dies after `worn on`, a stream takes the lock and asks the helper, which says
  "worn" (the fake). The flag stops the lease. Then the next command releases the wearer and
  clears the flag, and the stream's next tick leased on that answer, which counts as fresh for
  3 s. ft-stream now forgets the answer when a test's file goes away (`hostinput::AfterTest`) and
  drops answers already queued (`TakeAnswers(now, false)`), then asks again (`StreamTick`'s
  `TestGone`). `hostinput-test.cpp` guards it; dropping `TestGone` brings E back in `NoF`.
- **F (fixed in `1f485fe`). The guard is stopped between `start_guard` and the post-check.**
  `start_guard` sees a live guard, then the guard gets SIGTERM (or SIGKILL) before `worn 1`: it
  released a wearer that wasn't faked yet, cleared the flag and freed the lock. `worn 1` faked the
  wearer with no flag, and before the post-check noticed, a stream could take the lock, hear
  "worn" from the helper and lease. The window was the few milliseconds before the post-check,
  against ft-stream's 3 s lock retry and 1 s helper question. Now `guard` and `after_guard` release
  the wearer but leave the flag (`release_confirmed`, the model's `GuardReleases`); the post-check
  or the next testbench command releases again and clears it (`drop_wearer`). The cost, accepted
  by Daenon: host input stays blocked after an outside stop or kill until the next testbench
  command. `test_a_guard_stopped_before_worn_1_leaves_the_flag_while_the_wearer_is_faked` and the
  guard tests guard it in the code; `GuardReleases` clearing the flag again brings F's path back
  (`YouRunWornOn OnCheck OnStartGuard GuardStarts OnWait GuardStopped OnFake StreamStarts
  StreamTakesLock StreamTick HelperAnswers StreamTick`) in `Kills` and `All`.

## Model to code

Each model action is one step of the code. Change the model when the code it names changes.

| Model action | Code (at the sync commit) |
| --- | --- |
| `DropFaked`/`DropFlag` in `YouRunWornOn`, `YouRunWornOff`, `YouRunOther` | `cli.py` `drop_unsafe_worn` (l. 413), through `drop_wearer` (`hostinput.py` l. 220), run before every command (`main`, l. 462) |
| `OnCheck` | `cli.py` `dispatch`, `worn` branch (l. 287): `hostinput.refusal(state())`, then the live-guard test, then the left-flag refusal (l. 305) |
| `OnStartGuard`, `StopOldGuard`, `OnWait` | `hostinput.py` `start_guard` (l. 245): `systemctl stop`, `clear_flag`, `systemd-run`, wait for a live guard's flag |
| `OnFake`, `KeepFlag` | `cli.py` `bench.control('worn 1')` (l. 313) and its `except BaseException`: `release_or_keep_flag` (l. 401) |
| `OnVerify` | `cli.py` the check after `worn 1` (l. 317): no live guard -> `release_or_keep_flag` |
| `OffSend` | `cli.py` `worn off|physical` (`control`, then `clear_flag`) and `release` (l. 360) |
| `GuardStarts`, `GuardLetsGo` | `hostinput.py` `guard` (l. 168): `take_locks` (l. 151), then `set_flag`; leaves once the flag stops naming it |
| `GuardTerm`, `GuardStopped`, `GuardReleases` | `guard`'s SIGTERM path (l. 197) and `release_confirmed` (l. 204): the wearer goes, the flag stays |
| `GuardKilled`, `GuardCleanup` | SIGKILL/OOM, then the unit's `ExecStopPost`: `after_guard` (l. 230), `release_confirmed` for any unallowed flag naming no live guard |
| `MayDieNow` | the window `GuardCanDieMidFake` opens: `pc` in `on_fake`/`on_verify` is after `start_guard` (l. 245) and before the post-check (l. 317) |
| `GuardAlive` | `hostinput.py` `guard_alive` (l. 276) |
| `Released`, the proxy's `hung` | `transport.py` `request` (l. 9): a 4 s timeout raises `socket.timeout`, after the proxy may have acted |
| `StreamStarts`, `StreamEnds` | a stream to a host with `"input": true` opens or closes (`HostInput` in `stream/ft-stream.cpp`) |
| `StreamTakesLock` | `HostInput::Tick` (l. 443): `flock` on `frametop-input-HOST.lock`, retried every 3 s |
| `StreamTick` (`asked`, `saidTest`, `seen`, `lease`) | `HostInput::Tick`: ask `gaze ? headset` once a second, `TakeAnswers` (l. 417), `hostinput::TestForbidding` (`stream/hostinput.h` l. 170) on the flag every tick, `AfterTest` (l. 181) and `TakeAnswers(now, false)` when it goes, then `Decide` (l. 121): a lease only with no test's file and a fresh "worn" |
| `HelperAnswers` | the pointer helper's answer to `gaze ? headset`: "worn" while the testbench fakes the wearer |

## Scope

What the check covers: every interleaving of one testbench command at a time, one guard unit, one
stream to one host with `"input": true`, a proxy that may stall (`Hangs`), and a guard that may be
killed or sent SIGTERM (`Kills`). It covers that model, not the whole system.

Left out:
- `--allow-host-input`, which the user approves.
- A host that gains `"input": true` mid-run. That gap is documented in `docs/host-input.md`.
- Several hosts, and two testbench commands at once.
- The testbench command itself being killed mid-command.
- The verify skill's fake viewer and `verify-frametop guard`.
- The proxy going away entirely (`FileNotFoundError`).
- In ft-stream: `verify-frametop.json` (the verify skill's file, which `TestForbidding` reads like
  the flag), the input relay's pause answer, answers going stale after 3 s, the lease's release
  handshake, and nested desktops' second runtime dir. The model leases at once on a "worn" answer
  and stops leasing at once when it may not.
- Someone really wearing the headset.

## When a result differs

- **A rule that held now breaks, or a known finding breaks by another path.** Read the path with
  `/statechart`, then replay it against the code step by step: one model action, one code step
  from the table above. If the code does it, it's a real bug. Report it in those words, and don't
  weaken the rule or the model to pass. If the code can't do a step, the model is wrong: fix the
  model to match the code, and say what was wrong.
- **A known finding now holds.** Either a fix landed (`scripts/replay` should then say "no longer
  happens" for it), or the model drifted from the code. Confirm which before you touch the
  baseline.
- **An intended change** (a fix, a new step): change the model actions it touches, run `check`,
  replay any new path against the code, then update `BASELINE` in `scripts/check`, the findings
  above, and `scripts/replay` for any finding that is gone.

## Coverage and sync

Covered code:
- `testbench/frame_testbench/hostinput.py`: `read_flag`, `set_flag`, `clear_flag`, `take_locks`,
  `guard`, `release_confirmed`, `drop_wearer`, `after_guard`, `release_wearer`, `start_guard`, `guard_alive`, `refusal`,
  and `state`'s `input_streams`
- `testbench/frame_testbench/cli.py`: `dispatch` (`worn` and `release` branches),
  `release_or_keep_flag`, `drop_unsafe_worn`, `main`
- `testbench/frame_testbench/transport.py`: `request`
- `stream/ft-stream.cpp`: `HostInput::Tick`, `TakeAnswers`, the input lock
- `stream/hostinput.h`: `Decide`, `Fresh`, `TestForbidding`, `AfterTest`
- `docs/host-input.md`: the lease rule and the guard's description

**Sync commit: `1f485fe`** (experimental, 2026-10-30, after the fixes for A to F). The model describes the covered code as of
this commit. To see whether it needs another look:

```sh
git diff 1f485fe -- testbench/frame_testbench/hostinput.py testbench/frame_testbench/cli.py testbench/frame_testbench/transport.py stream/ft-stream.cpp stream/hostinput.h docs/host-input.md
```

Move the sync commit forward only after you have read that diff, confirmed that every model action
still matches the code (and fixed the model where it didn't), and seen `check` and `replay` match
the baseline at the new commit. Bring a drifted model back in line with the pstack
`/maintain-formal-verification-skill` skill.
