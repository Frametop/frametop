# Frametop maintainability refactor: the full plan

Asked by Daenon 2026-11-01: make the whole codebase easier to maintain and less sloppy, with
expected regressions, how to make up for them, and the cost against more precise changes now.
Planning only: nothing here is built yet. The first pass (SOLID-PLAN.md, PROGRESS.md) is on
branch `solid-refactor`.

Numbers are measured at experimental 2058f75 (2026-11-01) unless marked "this branch". "Code"
means Python, C, C++, headers, QML and shell, without third_party, tests or docs: **128.5k
lines**, plus 54.4k lines of tests. Churn is commits touching the file since 2026-10-01. Copied
lines are lines in identical blocks of 5 or more (stripped) found in both files.

## The short version

- **Recommendation: the hybrid (option C below).** Now: fix the gate and the hygiene, delete
  the pass-through wrappers, finish the safe-write work, share one glyph helper across five
  panels, and take the remaining copies out of the gaze recorder. Drop the first pass's two
  dispatch tables before it merges (keep its tests and shared modules). Refactor each other
  hotspot only when a feature next touches it, with a characterization test first. Split
  `screens/vr.cpp` only after DEE-208 and DEE-322 land.
- **Big line savings aren't available.** The cleanup threads already shared the recorder pages,
  the window shell, the conf parser and the IPC clients. What's left copied is about 800 lines in
  the recorders and about 300 in the glyph code. The hot files (vr.cpp, the relay, Input
  Settings) are big because they do a lot, not because they're copied: splitting them makes
  smaller files, not fewer lines.
- **Expected result of the hybrid now:** about −0.8k production lines from phases 1–3, about
  −0.7k after the first pass merges without its tables. That costs about 3 agent runs and about
  20 minutes of Daenon's headset time. The full refactor reaches about −1.05k and much smaller hot
  files, for about 7.5 runs, about 70–85 minutes in the headset, and steady merge conflicts in
  the files with the most churn.

## Rules for every phase

- **Shrink first.** Each phase must end with production code no bigger than it started, tests
  aside. A phase that only moves code says so and proves the move is worth it (a file under
  1,000 lines, or a piece that can now be tested).
- **No abstraction without two users.** No interface or base class with one implementation, no
  class where a dict or a function does, no plugin or registry machinery, no dependency
  injection framework. Pass the runner as an argument, as `conf/ft_vrsettings.py` does.
- **Characterize, then move.** Record the current behavior (golden outputs, recorded traces)
  against the old code before changing it, as the relay and ft-floatd tests did.
- Formats stay as they are: CLI flags, socket messages, config and rules files, recorded data,
  unit names, install layout. The standalone Hand Recorder (shipped without `conf/`) keeps
  working.
- Short branches: one branch per phase off experimental, merged back (merge commit) before the
  next starts, so no refactor branch lives longer than about a day.

## 1. Inventory

### Hotspots

| Area | Size | Churn | Tests today | Problem |
|---|---|---|---|---|
| `pointer/helper/ft-pointer.cpp` `main()` | 3,266 of 4,248 lines | 74 | pieces only (`pointer/tests/`, 11 files); `main()` none | one function; **DEE-322 owns its split** |
| `screens/vr.cpp` | 6,350 | 120 | vrcmd, posemath, levelpin tests | `ft_vr_command` is now a 20-line dispatcher (cleanup did it); `ft_vr_poll` 267 lines; about 54 file-scope variables; DEE-208 is working here |
| `screens/handinput.cpp` + `screens/tests/handinput_test.cpp` | 2,788 + 5,613 | 39 + 48 | yes | the test file carries analysis tools; `Poke` 328 lines |
| Input Settings `Backend` (`input-settings/ft_input_settings.py`) | 1,676-line class, 185 methods; `input-settings/main.qml` 2,113 | 60 + 39 | input-settings/test | Qt god object: rules, conf, host commands, gaze, hands, voice, Bluetooth, travel mode |
| `input/input-relay.py` `Relay` | 1,197-line class, 48 methods; file 2,492 (2,536 on this branch) | 67 | keys, hostin (+ control golden on this branch) | typing, Full Keyboard link, host devices, pause, devices in one class |
| Recorders: hands `Session` (`hands/rec/session.py`), gaze `Session` (`gaze/rec/session.py`), `RecorderBackend` (`recorder/window.py`), hands `Backend` (`hands/rec/ft_handrec.py`) | 1,582 / 896 / 856 / 797 | 45 + 36 on the hands side; low on gaze | test_session and others | the shared pages (`recorder/qml/`) and window shell (`RecorderBackend`) already exist; still copied: 514 of gaze `main.qml`'s 1,081 lines, 148 lines of the two sessions, 136 of the two windows' Backends |
| `gaze/gazecheck.py` `Checks` | 1,181, 57 methods | 30 | gaze/test | one class of independent checks |
| `float/ft_floatd.py` `Daemon` | 1,002; `on_event` 83 | low | float/test (+ request golden on this branch) | fine as is (see "not worth it") |
| `layout/ft_layout.py` | 2,171 (1,988 on this branch); `remote_command` 210, `main` 203 | 30 | layout/tests | remote displays and CLI in the same module |
| `display-settings/ft_display_settings.py` `Backend` | 819, 91 methods | low | 1 import test | thin, but only a smoke test |
| Native mains: `hands/track/main.cpp`, `hands/track/replay.cpp`, `hands/rec/panel/ft-handpanel.cpp`, `gaze/panel/ft-gazepanel.cpp` | 1,001 / 751 / 604 / 418-line `main()`s | main.cpp 27, the rest low | replay gate on dody | long, but the pipeline is already in shared headers; main and replay share only 18 copied lines |
| Also hot, not ranked above | `screens/compositor.c` 1,484 (34), `gaze/ft-gazed` 1,139 (26), `scripts/update-check.py` 1,000 (41), `stream/ft-stream.cpp` 1,072 (21) | | | on touch only |

### Copies

- Glyph code: `STB_TRUETYPE_IMPLEMENTATION` in 5 files (`frunner/ft-spotpanel.cpp`,
  `hands/rec/panel/ft-handpanel.cpp`, `gaze/panel/ft-gazepanel.cpp`,
  `layout/countdown/ft-reset-timer.cpp`, `screens/label.cpp`); four are near-identical, about 75
  lines each. The reset timer tries a different font first.
- Pass-through wrappers over `ft_conf`/`ft_store`: `read_conf` x6, `write_conf_value` x3,
  `write_value` x1, `write_json` x3 (voice settings, hands takes, recorder common).
  `recorder/common.py`'s `store_json` is not one of them: it is the fallback the standalone Hand
  Recorder needs without `conf/`.
- `host()` in Input Settings next to the shared `fthost.host_command`.
- Fixed `.tmp` names instead of `mkstemp`: 11 writers, including `gaze/rec/eyerec.py`,
  `gaze/rec/ft_eyerec.py`, `gaze/rec/sounds.py`, `recorder/age.py`, `recorder/contributions.py`,
  `tour/ft_tour.py`, `hands/handsflag.py`, `float/ft_apps.py`, `float/ft_floatd.py`,
  `models/catalog.py` and the gaze bench recorder. Two writers of one file at once can lose a
  write; most of these have one writer, so it matters for the shared ones first.
- `ft_store.update_json` (a locked read-modify-write) exists, but nothing outside
  `conf/ft_store.py` uses it; Input Settings and voice settings still lock only the write.

### Sloppiness from the 2026-10-31 re-audit, checked again at 2058f75

| Item | Now |
|---|---|
| ruff report-only | **fixed**: `just lint` fails on a finding |
| committed TLC traces (docs/charts/G7-release-traces) | **fixed**: gone |
| stale stream spike (`iris.h` copy) | **fixed**: gone |
| `handasset.cpp` and `.frame-job` tracked | **fixed**: untracked |
| `ft_vr_command` 404 lines | **fixed**: 20-line dispatcher plus handlers |
| recorder Takes, Viewer, Export, Upload pages per window | **fixed**: shared in `recorder/qml/` (DEE-319, DEE-323) |
| no CI before a release | **open**: `.github/workflows/release.yml` runs on `v*` tags and by hand only (Depot runners are paid) |
| HANDOFF.md and python-split-plan.md in the repo | **open**: moved to the `dev/maintainability` folder, 1,103 lines, 94 DEE refs, local paths |
| DEE refs in code | **open, small**: 10 outside that folder (`screens/vr.cpp` x2, levelpin, travel mode, display settings, 5 tests) |
| one-test `test_import.py` smoke files | **open**: 5 (display-settings, framedrop, remote-displays, remote, steam) |
| a file with zero tests passes; no test timeouts | **open** (not rechecked in the gate code) |
| `sys.path.insert` in non-test code | 38 files; left as is (see "not worth it") |

## 2. Target per area

Line counts are estimates; "−" is production code removed net.

| # | Area | Target | Deleted or merged | Change |
|---|---|---|---|---|
| A | Wrappers and safe writes | Callers use `ft_conf`/`ft_store` directly (except `store_json`, kept for the standalone recorder); Input Settings and voice settings write the rules file with `update_json`; the `.tmp` writers use `ft_store` where `conf/` ships, or `store_json` | 13 wrappers; `host()` → `fthost.host_command` | about −100 |
| B | Recorders | Take the remaining copies out: the gaze window's copied QML blocks into components in `recorder/qml/`, the shared session and Backend parts into the existing `recorder/` modules | 514 + 148 + 136 copied lines, less the shared pieces they become | about −450 |
| C | Glyph code | One font and label helper (a header next to `screens/label.cpp`) with the font path as a parameter, used by the 5 panels | 4 copies of the stb glyph setup | about −250 |
| D | First pass's tables | Before the first pass merges: the relay's control commands and ft-floatd's requests back to their if-chains, keeping the golden tests that pin them | `Form`, `Datagram`, `Request` and the guards | about −170 against this branch |
| E | Relay (on touch) | `Relay` keeps devices and the event loop; typing plus the Full Keyboard link, host devices, and the pause move to modules it owns | nothing copied: a move | about 0; `Relay` 1,197 → about 600 |
| F | Input Settings (on touch) | `Backend` stays the one object QML sees (no QML renames), but forwards to plain modules: rules (extending `input-settings/input_rules.py`), gaze, hands, voice (reusing `voice/voice_settings.py`), Bluetooth, travel mode | its own conf and host helpers | about −150; `Backend` 1,676 → under 600 |
| G | `screens/vr.cpp` (last) | The file-scope state in one struct; `ft_vr_poll` and the overlay update in their own file; the command handlers in their own file; no new classes | dead state found while moving | about −200; 6,350 → 3–4 files under 2,500 |
| H | Tests | Analysis tools out of `screens/tests/handinput_test.cpp` into a tool; the 5 import-only tests become one real test each or go; the gate fails a file with zero tests and has a per-file timeout | padding | tests about −1,500 lines, no lost coverage |
| I | Hygiene | The handoff and split-plan docs leave the repo (where to is Daenon's call); DEE refs in code become plain words | | docs −1,103 |
| J | ft-pointer `main()` | **DEE-322's**, not this plan's | | not counted |

Totals, production code:

| | Phases | Lines |
|---|---|---|
| Targeted | A, D, H, I | about −100 now; the first pass then merges at about +120 instead of +294 |
| Hybrid now | A, B, C, D, H, I | about −800; net of the first pass about −700 |
| Full | everything | about −1,150; net of the first pass about −1,050 |

### Where abstraction is not worth it

- **ft-floatd `on_event`**: 83 lines whose branch order carries meaning (the window record and
  launches first). A table there would hide that order. Leave it.
- **The first pass's dispatch tables**: they added about 170 lines for about 50 short branches,
  and the golden tests already guard those if-chains. Undo them (D).
- **ft_layout's `remote_command` and `main`**: plain argument handling read top to bottom.
  Moving remote displays out of `ft_layout` is fine as a plain move when that code next changes;
  a command table is not.
- **The hand tracker mains**: the pipeline is already in shared headers; what's left is each
  tool's own arguments and loop. Leave them.
- **`gaze/gazecheck.py` `Checks`**: 57 independent checks in one class is a list, not a god
  object. Leave it.
- **One import bootstrap instead of 38 `sys.path.insert`s**: it would change how every installed
  tool finds its modules, for no change in behavior. Leave it.
- **A base class for the Input and Display Settings backends**: they share almost nothing but
  Qt. Leave them separate.
- **C++ classes around vr.cpp's globals**: a struct is enough; anything more grows the file.
- **`tour/ft_tour.py` `pages()`**: long, but it's content. Leave it.

## 3. Phases

Each unit is one local commit, behavior-preserving, with its gate passing first; full
`just test` at the end of each phase, one full run at a time across all worktrees.

| Phase | Units | Gate | Waits for |
|---|---|---|---|
| 0 First pass | D on `solid-refactor` (the golden tests must still pass), then merge it into experimental by merge commit; the known conflict is 3 hunks in Input Settings (keep theirs, drop the two moved definitions) | control and request goldens; full `just test` on the merge | cleanup 3 done; Daenon's go (decision 1) |
| 1 Hygiene and gate | I; H (zero-test fail, timeouts, import tests, handinput tools); A, one wrapper or writer per commit | testgate per area; a golden of each wrapper's file output first; the standalone recorder's tests without `conf/` | 0 |
| 2 Gaze recorder copies | B: the copied QML blocks into `recorder/qml/` components one at a time, then the session and Backend copies | hands/rec, gaze/rec, recorder tests; a golden of a scripted fake session's output files (manifest, takes, voice log) for both recorders, recorded first | 1; no open hands/rec branch (it changes often) |
| 3 Glyph helper | C, one panel per commit | ctest; a new offscreen text-render test per panel (budgeted here: about half a run), pixel-exact before and after | 1 |
| 4 Relay (on touch) | E: one module per commit | keys-test, hostin-test, control golden | 0 |
| 5 Input Settings (on touch) | F: one page's logic per commit behind the unchanged `Backend`; a test that every name `main.qml` uses still exists on `Backend` | input-settings/test; rules-file golden per slot call | cleanup's travel mode merged |
| 6 vr.cpp (last) | G: state struct, then the poll split, then the handlers file | screens tests, vrcmd, testbench recorded traces | DEE-208 and DEE-322 merged; no other screens branch open |

Coordination with cleanup 3 and DEE-322: this plan never touches `ft-pointer.cpp`, and touches
`screens/vr.cpp` only in phase 6, after both are merged. Before each phase, read experimental's
log and open worktrees, and leave any file another thread has changed in the last day.

## 4. Expected regressions and how to make up for them

| Phase | What could break | Caught off the headset? | Compensation |
|---|---|---|---|
| 0 | a control command or float request answering differently after the tables go; Input Settings defaults after the conflict | yes | the control (136 cases) and request (94) goldens; testgate input-settings |
| 1 | a conf or rules write losing a key or changing format; a file written under a different name; the standalone Hand Recorder failing to import | yes | golden output per wrapper, recorded first; `update_json` tests with two writers; run the recorder tests with `conf/` off the path |
| 2 | a take saved with a different manifest; a gaze window control missing or misplaced; a hands change landing at the same time | mostly (goldens); the UI no | goldens of both recorders' outputs; HEADSET-TESTS: one eye take, view and export (about 15 min) |
| 3 | a panel's text shifted, clipped or in another font (the reset timer) | yes, with the new render test | pixel-exact render goldens; a look at each panel in the headset (about 5 min) |
| 4 | key timing (taps, held modifiers), typing through the Full Keyboard, host devices joining | partly (keys-test covers the timing logic, not real devices) | control golden; keys-test; HEADSET-TESTS: Meta tap, Meta+J/K, typing in Full Keyboard, Ctrl+Alt+Shift+Esc (about 10 min); no host streams in agent tests |
| 5 | a QML binding to a missing name (silent in Qt), a page that stops saving | partly | the `main.qml` name test; rules-file goldens; HEADSET-TESTS: change one setting on each page (about 10 min) |
| 6 | overlay order, pose timing, frame pacing, pins | **no**: these need SteamVR and the headset | one commit per moved group for bisecting; testbench recorded traces of overlay transforms with the headset not worn (then `release`, `compositor auto`); HEADSET-TESTS: pins, spin, float, dock, recenter (about 30–45 min); perf measured passively in real use |

Rollback points: each phase is one merge commit on experimental, so `git revert -m 1` undoes a
phase. No feature flags or kept-old-copy shims: they would be new code to delete later, and the
goldens plus one commit per unit make a bad unit easy to find and revert.

## 5. Cost and benefit

A "run" is one agent session the size of the 10/31 night run (about 13 commits, one review, two
full test runs).

| | A. Full refactor | B. Targeted now | **C. Hybrid (recommended)** |
|---|---|---|---|
| Phases | 0–6 | 0–1 | 0–3 now; 4–6 when a feature touches the file |
| Agent runs | about 7.5 | about 1.5 | about 3 now; about 1–2 more per hotspot when touched |
| Daenon's headset time | about 70–85 min | none | about 20 min now; 10–45 min per hotspot later |
| Production lines, net of the first pass | about −1,050 | about +20 | about −700 now |
| Largest files after | none over 2,500 but ft-pointer (DEE-322) | unchanged | unchanged until touched |
| Risk | medium-high (phase 6 can only be judged in the headset) | very low | low now (phase 2 meets busy hands/rec code) |
| Merge-conflict cost | high: vr.cpp (120 commits in a month), ft-pointer (74), the relay (67) and Input Settings (60) are where features land | low | low to medium: the hands recorder changes often |
| Payoff | the hot files become readable in pieces | a cleaner gate and repo; no smaller code | that, plus the remaining copies gone; hot files shrink as they're worked on, when the test is cheapest to write |

Verdict: the full refactor buys smaller hot files, not much less code, and it pays for them in
the files where every other thread works, with the riskiest part (vr.cpp) only checkable in the
headset. Targeted alone leaves the copies in. The hybrid takes the copies and the hygiene now,
for 3 runs and 20 minutes in the headset, and splits each hot file on the branch that already
has to change it.

## 6. Open decisions for Daenon

1. The first pass (`solid-refactor`, +294 production lines): drop its two dispatch tables and
   merge the rest (tests, shared modules) after cleanup 3? Recommended: yes, about +120 then.
2. HANDOFF.md and python-split-plan.md (1,103 lines, 94 DEE refs, local paths): delete from the
   repo, or move to private notes outside it? Recommended: move out.
3. A free CI check on pushes (GitHub-hosted runner: lint plus the Python tests), next to the
   Depot release on tags? Today nothing runs before a tag or a manual run.
4. The 5 one-test `test_import.py` files: replace each with one real test, or delete them?
   Recommended: replace for display-settings (819-line Backend), delete the others.
5. `screens/tests/handinput_test.cpp` analysis tools: move them out of the gate into a tool?
   Recommended: yes.
6. Recorder data format: confirm it must not change, even file names inside a take.
   Recommended: no change, so old takes and uploads stay valid.
7. vr.cpp split: schedule it after DEE-208 and DEE-322 with about a day's screens freeze, or
   leave it to on-touch refactoring? Recommended: on touch.
8. Headset test budget: about 20 minutes for the hybrid now; OK?

## 7. Status (2026-11-01)

Daenon chose the hybrid, with decisions 1-8 answered (1: drop the tables, merge after cleanup
3; 3: write CI, don't push; 6: no recorder format change; 7: vr.cpp on touch; 8: headset time
OK, kept small). Done on `solid-refactor`, not yet merged: phase 0's drop, phase 1 (H's zero-test
failure and timeouts were already in the gate), phase 2's Welcome page only, phase 3. Each step,
its checks and the deviations are in [PROGRESS.md](PROGRESS.md), steps 12-20. Phase 3's
compensation turned out fully offline: the panels' text is byte-identical before and after, so
its headset look isn't needed.

