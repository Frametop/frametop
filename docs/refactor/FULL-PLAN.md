# Frametop maintainability refactor: the full plan

Asked by Daenon 2026-11-01: make the whole codebase easier to maintain and less sloppy, with
expected regressions, how to make up for them, and the cost against more precise changes now.
Planning only: nothing here is built yet. The first pass (SOLID-PLAN.md, PROGRESS.md) is on
branch `solid-refactor`.

Numbers are measured at experimental 2058f75 (2026-11-01) unless marked. "Code" means Python,
C, C++, headers, QML and shell, without third_party, tests or docs: **128.5k lines**, plus 54.4k
lines of tests. Churn is commits touching the file since 2026-10-01.

## The short version

- **Recommendation: the hybrid (option C below).** Do the cheap, safe, shrinking work now: the
  hygiene and gate fixes, the recorder deduplication, one font helper, and deleting the
  pass-through wrappers. Refactor each remaining hotspot only when a feature next touches it,
  with a characterization test first. Split `screens/vr.cpp` last, after DEE-208 and DEE-322
  land, in a week with no other screens work.
- SOLID by itself won't shrink this codebase. The first pass proved it: dispatch tables and
  moved modules made production code 294 lines bigger. Lines come out where code is copied
  (recorders, glyph code, wrappers, the two hand-tracker mains), so the plan leads with those.
- Expected result of the hybrid: about −2.0k production lines now (−1.6%), about 3.5 agent runs
  and about 25 minutes of Daenon's headset time. The full refactor reaches only about −2.5k,
  because splitting the hot files mostly moves code, but it makes them smaller, and costs more
  than twice the runs, about 2 hours of headset tests, and steady merge conflicts in the files
  with the most churn.

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
  unit names, install layout.
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
| Recorders: hands `Session` (`hands/rec/session.py`), gaze `Session` (`gaze/rec/session.py`), `RecorderBackend` (`recorder/window.py`), hands `Backend` (`hands/rec/ft_handrec.py`) | 1,582 / 896 / 856 / 797 | 45 + 36 | test_session and others | gaze `main.qml` is 61% copied from the hands one; Takes, Viewer and Export pages once per window |
| `input/input-relay.py` `Relay` | 1,197-line class, 48 methods; file 2,536 (this branch) | 67 | keys, hostin, control golden | typing, Full Keyboard link, host devices, pause, devices in one class |
| `gaze/gazecheck.py` `Checks` | 1,181, 57 methods | 30 | gaze/test | one class of checks; mostly independent methods |
| `float/ft_floatd.py` `Daemon` | 1,002; `on_event` 83 | low | float/test, request golden | fine as is (see "not worth it") |
| `layout/ft_layout.py` | 1,988 (this branch); `remote_command` 210, `main` 203 | 30 | layout/tests | remote displays and CLI in the same module |
| `display-settings/ft_display_settings.py` `Backend` | 819, 91 methods | low | 1 import test | thin, but only a smoke test |
| Native mains: `hands/track/main.cpp`, `hands/track/replay.cpp`, `hands/rec/panel/ft-handpanel.cpp`, `gaze/panel/ft-gazepanel.cpp` | 1,001 / 751 / 604 / 418-line `main()`s | low | replay gate on dody | main and replay set up the same pipeline twice |
| `frunner/ft_frunner.py`, `tour/ft_tour.py` `pages()` | 711 / 342 | low | yes | `pages()` is data written as code |

### Copies

- Glyph code: `STB_TRUETYPE_IMPLEMENTATION` in 5 files (`frunner/ft-spotpanel.cpp`,
  `hands/rec/panel/ft-handpanel.cpp`, `gaze/panel/ft-gazepanel.cpp`,
  `layout/countdown/ft-reset-timer.cpp`, `screens/label.cpp`).
- Pass-through wrappers over `ft_conf`/`ft_store`: `read_conf` x5, `write_conf_value` x3,
  `write_value` x1, `write_json` x3 (voice settings, hands takes, recorder common).
- `host()` in Input Settings next to the shared `fthost.host_command`.
- Fixed `.tmp` names, not the safe `mkstemp` ones: `gaze/rec/eyerec.py`, `gaze/rec/ft_eyerec.py`,
  `gaze/rec/sounds.py`, `recorder/age.py`.
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
| CI on tags only | **open**: `.github/workflows/release.yml` runs on `v*` tags only (Depot runners are paid) |
| HANDOFF.md and python-split-plan.md in the repo | **open**: moved to the `dev/maintainability` folder, 1,103 lines, 94 DEE refs, local paths |
| DEE refs in code | **open, small**: 10 outside that folder (`screens/vr.cpp` x2, levelpin, travel mode, display settings, 5 tests) |
| one-test `test_import.py` smoke files | **open**: 5 (display-settings, framedrop, remote-displays, remote, steam) |
| a file with zero tests passes; no test timeouts | **open** (not rechecked in the gate code) |
| `sys.path.insert` in non-test code | 38 files; left as is (see "not worth it") |

## 2. Target per area

Line counts are estimates; "−" is production code removed net.

| # | Area | Target | Deleted or merged | Before → after |
|---|---|---|---|---|
| A | Wrappers and data safety | Callers use `ft_conf`/`ft_store` directly; Input Settings and voice settings write the rules file with `update_json`; the four `.tmp` writers use `ft_store` | 12 wrappers; `host()` → `fthost.host_command` | about −120 |
| B | Recorders | One session core for what hands and gaze share (take numbering, manifest, timing, voice log); one window shell; shared Takes, Viewer and Export QML pages used by both windows; hands and gaze keep only what differs (cues, cameras, checks) | the copied 61% of gaze `main.qml`, the duplicate pages, duplicate session plumbing | 6,796 lines (4 classes plus 2 `main.qml`s) → about 5,600: about −1,200 |
| C | Glyph code | One font and label helper (a header next to `screens/label.cpp`) used by the 5 panels | 4 copies of the stb glyph setup | about −300 |
| D | Hand tracker mains | Shared pipeline setup (models, cameras or recording, tracker config) for `main.cpp` and `replay.cpp`; each `main` keeps its loop | duplicate setup | 1,752 → about 1,350: about −400 |
| E | Relay | `Relay` keeps devices and the event loop; typing plus the Full Keyboard link, host devices, and the pause become modules it owns; the first pass's `Form`/`Datagram` shrink to plain functions plus a dict | dispatch overhead from the first pass | 2,536 → about 2,450 in 4 files: about −90 |
| F | Input Settings | `Backend` stays the one object QML sees (no QML renames), but it only forwards to plain modules: rules (extending `input-settings/input_rules.py`), gaze, hands, voice (reusing `voice/voice_settings.py`), Bluetooth, travel mode | its own conf and host helpers | 2,202 → about 2,050; `Backend` 1,676 → under 600 |
| G | `screens/vr.cpp` | The file-scope state in one struct; `ft_vr_poll` and the overlay update in their own file; the command handlers in their own file; no new classes | dead state found while moving | 6,350 → about 6,150 in 3–4 files |
| H | Tests | Analysis tools out of `screens/tests/handinput_test.cpp` into a tool; the 5 import-only tests become one real test each or go; the gate fails a file with zero tests and has a per-file timeout | padding | tests about −1,500 lines, no lost coverage |
| I | Hygiene | The handoff and split-plan docs leave the repo (where to is Daenon's call); DEE refs in code become plain words | | docs −1,103 |
| J | ft-pointer `main()` | **DEE-322's**, not this plan's. After it lands, only follow-ups that touch nothing DEE-322 changed | | not counted |

Hybrid total (A, B, C, D, H, I now; E, F, G on touch): about −2.0k production now (A −120,
B −1,200, C −300, D −400), about −2.25k when E and F follow. Full (all of them, G too): about
−2.5k. Splitting E, F and G mostly moves code: their value is smaller files, not fewer lines.

### Where abstraction is not worth it

- **ft-floatd `on_event`**: 83 lines whose branch order carries meaning (the window record and
  launches first). A table there would hide that order. Leave it.
- **ft_layout's `remote_command` and `main`**: plain argument handling read top to bottom.
  Moving remote displays out of `ft_layout` is fine as a plain move when that code next changes;
  a command table is not.
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
| 0 Merge the first pass | `solid-refactor` (the first pass) into experimental by merge commit; the known conflict is 3 hunks in Input Settings (keep theirs, drop the two moved definitions) | full `just test` on the merge | cleanup 3 done; Daenon's go (decision 1) |
| 1 Hygiene and gate | I; H (zero-test fail, timeouts, import tests, handinput tools); A (wrappers, `update_json`, `.tmp` writers), one wrapper per commit | testgate for each touched area; a golden of each wrapper's file output first | 0 |
| 2 Recorders | B in steps: shared session core with golden manifests from both recorders; window shell; Takes, then Viewer, then Export pages, each with the old page kept behind one property until the headset check passes, then deleted | hands/rec, gaze/rec, recorder tests; new golden of a scripted fake session's output files (manifest, takes, voice log) | 1 |
| 3 Native copies | C (font helper; offscreen render of each panel's text compared pixel for pixel before and after); D (replay output on a fixed capture identical, run on dody through `frame-job`) | ctest; render golden; replay golden | 1; any time |
| 4 Relay (on touch, or now in the full option) | E: typing and the Full Keyboard link, host devices, pause; one module per commit | keys-test, hostin-test, control golden | 0 |
| 5 Input Settings (on touch) | F: one page's logic per commit behind the unchanged `Backend`; an inventory test that every name `main.qml` uses still exists on `Backend` | input-settings/test; rules-file golden per slot call | cleanup's travel mode merged |
| 6 vr.cpp (full option only, or when a feature needs it) | G: state struct, then the poll split, then the handlers file | screens tests, vrcmd, testbench recorded traces | DEE-208 and DEE-322 merged; no other screens branch open |

Coordination with cleanup 3 and DEE-322: this plan never touches `ft-pointer.cpp`, and touches
`screens/vr.cpp` only in phase 6, after both are merged. Before each phase, read experimental's
log and open worktrees, and leave any file another thread has changed in the last day.

## 4. Expected regressions and how to make up for them

| Phase | What could break | Caught off the headset? | Compensation |
|---|---|---|---|
| 0 | Input Settings defaults after the conflict | yes (tests) | keep theirs; testgate input-settings |
| 1 | a conf or rules write losing a key or changing format; a recorder file written under a different name | yes | golden output per wrapper, recorded first; `update_json` tests with two writers |
| 2 | a take saved with a different manifest; the Viewer or Export page missing a control; hands and gaze each needing something the other doesn't | mostly (golden files); UI flow no | goldens of both recorders' outputs; old pages kept until checked; HEADSET-TESTS: one hand take, one eye take, view and export each (about 15 min) |
| 3 | a panel's text shifted or clipped; replay output drifting | yes (render and replay goldens) | pixel-exact goldens; a quick look at each panel in the headset (about 5 min) |
| 4 | key timing (taps, held modifiers), typing through the Full Keyboard, host devices joining | partly (keys-test covers timing logic, not real devices) | control golden; keys-test; HEADSET-TESTS: Meta tap, Meta+J/K, typing in Full Keyboard, Ctrl+Alt+Shift+Esc (about 10 min); no host streams in agent tests |
| 5 | a QML binding to a missing name (silent in Qt), a page that stops saving | partly | the `main.qml` name-inventory test; rules-file goldens; HEADSET-TESTS: change one setting on each page (about 10 min) |
| 6 | overlay order, pose timing, frame pacing, pins | **no**: these need SteamVR and the headset | one commit per moved group for bisecting; testbench recorded traces of overlay transforms with the headset not worn (then `release`, `compositor auto`); HEADSET-TESTS: pins, spin, float, dock, recenter (about 30–45 min); perf measured passively in real use |

Rollback points: each phase is one merge commit on experimental, so `git revert -m 1` undoes a
phase. Inside phase 2 the old QML pages stay until the headset check. No feature flags in
shipped code otherwise: they would be new code to delete later.

## 5. Cost and benefit

A "run" is one agent session the size of the 10/31 night run (about 13 commits, one review, two
full test runs).

| | A. Full refactor | B. Targeted now | **C. Hybrid (recommended)** |
|---|---|---|---|
| Phases | 0–6 | 0–3 | 0–3 now; 4–6 when a feature touches the file |
| Agent runs | about 8–9 | about 3.5 | about 3.5 now, about +0.5 per touched hotspot later |
| Daenon's headset time | about 2 h | about 25 min | about 25 min now, about 10–45 min per hotspot later |
| Production lines | about −2.5k | about −2.0k | about −2.0k now, about −2.25k with E and F |
| Largest functions after | ft-pointer (DEE-322) only over 1,000 | hand mains and vr.cpp unchanged | as B, shrinking as touched |
| Risk | medium-high (phase 6 can only be checked in the headset) | low | low now |
| Merge-conflict cost | high: vr.cpp (120 commits in a month), the relay (67) and Input Settings (60) are where features land | low: recorders and panels change rarely | low; each hotspot is refactored on the branch that changes it anyway |
| Payoff | every hotspot smaller | the copied code gone; the hot files stay big | the copied code gone now; hot files get smaller as they're worked on, when the test is cheapest to write |

Verdict: the full refactor pays the most where it costs the most. The hot files are
where features land every day, so a long refactor there fights every other thread, and phase 6
can only be judged in the headset. The copied code is the opposite: low churn, testable off the
headset, and it is where the lines actually are. Take that now and refactor the rest on touch.

## 6. Open decisions for Daenon

1. Merge `solid-refactor` (the first pass: 11 code and test commits, +294 production lines) into experimental after cleanup 3,
   or keep its tests and drop its dispatch tables? Recommended: merge, then trim the tables in phase 4.
2. HANDOFF.md and python-split-plan.md (1,103 lines, 94 DEE refs, local paths): delete from the
   repo, or move to private notes outside it? Recommended: move out.
3. A free CI check on pushes (GitHub-hosted runner: lint plus the Python tests), next to the
   tags-only Depot release? It costs nothing on public repos; today nothing runs before a tag.
4. The 5 one-test `test_import.py` files: replace each with one real test, or delete them?
   Recommended: replace for display-settings (819-line Backend), delete the others.
5. `screens/tests/handinput_test.cpp` analysis tools: move them out of the gate into a tool?
   Recommended: yes.
6. Recorder data format: confirm it must not change, even file names inside a take.
   Recommended: no change, so old takes and uploads stay valid.
7. Phase 6 (vr.cpp split): schedule it after DEE-208 and DEE-322 with a screens freeze of about a
   day, or leave vr.cpp to on-touch refactoring? Recommended: on touch.
8. Headset test budget: about 25 minutes for the hybrid now; OK?
