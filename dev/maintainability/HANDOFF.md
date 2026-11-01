# Maintainability series: handoff

State on 2026-10-31, branch `experimental` in `~/frametop/.worktrees/experimental`. All of this
is local: nothing in the series has been pushed, installed or fast-installed. The rest of
DEE-303 became DEE-319 (step 19), which is done. No ticket of the DEE-292 series is left queued.

## Where the series is

| Step | Tickets | Commits |
| --- | --- | --- |
| 1 | DEE-299 | b1bd58c |
| 2 | DEE-300 (with the DEE-295 and DEE-296 fixes) | 1bb43cb, e2b5609, acc4d05, c56aab7, 16dc51b, 364dba1, 7c3abb1, 2bffb10 |
| 3 | DEE-302, plus DEE-300 follow-ups | 655fa8c, 4c16b1e, c48201a, 63089ef, dd8f7ba, 80954b5 |
| 3b | DEE-311 | 865b852, b69b905, 2971c8d, af02582 (this note) |
| 4 | DEE-306 | 3bb0ebf, 688de89, d464ac0, 35d9da4, 576b5d6, fb0077e |
| 5 | DEE-307 | 84d60b3, 5277de1, cc476bf, 509bdf2, 60c9c3a, 383f0d8, 0cf0d21 |
| 6 | DEE-308 | d298586, 0fb9407, 5d57eda, 099f7c2 |
| 7 | DEE-310 | 898e760, d24de60, fe0a217, 6c0f032, 3d5af59, 3ad697c, 25dd9f9 (note) |
| 8 | DEE-304 | cb6d442, 60cc62e, c55e509, ceced31, 6ac87b6, a8a6a7e, 77b59d2, 9da7cd3, 8c72d59, e1a6a8d (note) |
| 9 | DEE-305 | a286deb, 8e1b0f7, 6a4d65d, 97006a9, 729588d, fbc84e4, 48786c4, cd11d96 (note) |
| 10 | DEE-303, part 1 | 76ecd9b, d80ef80, a88d458, f4d5db7, 63b363b, 3a8ad3d, f00bd79 (note) |
| 10b | DEE-305 follow-up (review note #9) | 176eb76 |
| 11 | DEE-309 | 77d0c35, a33b578, b6809e1, a0216d2, 91e19e3, 25dbde4, 165ed70, c9a4197, a0e2dfe, 972ac35, then this update |
| 12 | DEE-312 | ed4efd1, c340d33, d2edd33 (note) |
| 13 | DEE-298 | 3005442, a3d58c8 (note) |
| 14 | DEE-313 | 5b9c10b, 237d493, a1f3d6c, 26c16e3, 278c53e, 4146400, 4a456cb, 2bff435, 2877e21 (note) |
| 15 | DEE-314 | 09667a7, cdf9291, 2129cc6, f73dad7, bd73091 (note), 2a9898a (review fixes) |
| 16 | DEE-315 | 0a450b5, 2ca1f2e, 9fb2aa4, f91de1b (note) |
| 17 | DEE-316 | 3c26536, 2adbbdf, 36f2f8b, 265d025, 8714a6c (review fixes), d99a3cb (note) |
| 18 | DEE-318 | 89fbbad, 089e5cb, 52e0622, 72f51f9, b1a9a40, e566c5b, 621559b, df415e1, bae1c59 (review fix), 848e556 (note) |
| 19 | DEE-319 | c102462, 71fae6d, 431fea1, eb1c53d, ca8b510, f84aaeb, 5bd08c9, then review fixes f1942dd, 41391e5, a5a7dd5, afca878, then this update |

Nothing is left queued in the series. The open items below are follow-ups, not tickets.

DEE-310, DEE-304, DEE-305, DEE-309, DEE-312, DEE-298, DEE-313, DEE-314, DEE-315, DEE-316, DEE-318 and DEE-319 are done (steps 7 to 19). DEE-303 part 1 is done (step
10), and its remainder was DEE-319 (step 19). DEE-301's CI and lint work stays local only: no workflow change is pushed.

## Conventions the series set

### Config and stored files

- `conf/ft_conf.py` is the one reader and writer of `frametop.conf`-style `KEY=value` files.
  `set_value(key, value, path)` keeps comments, changes the last line that sets the key, and
  writes atomically. Every settings app's conf writer wraps it (display-settings,
  input-settings, remote, handsmode, gazecheck). Its tests are `conf/test/conf-test.py`.
- `conf/ft_store.py` (stdlib only) is the one way to read and write Frametop's JSON files.
  - `load_json_strict(path, default, check)` returns the default for a missing, empty or
    whitespace-only file. A file that doesn't parse or fails `check` is kept as
    `NAME.bad-TIME` (`-1`, `-2` on a clash) and raises `StoreError`. Callers stop saving
    when they get it. They don't stop running.
  - `load_json` is the lenient read. It warns once and returns the default.
  - `write_text` and `write_json` write through mkstemp, fsync and `os.replace`. They keep the
    file's mode, follow symlinks and take the umask from /proc.
  - `locked(path, timeout=10)` takes an flock on a `.NAME.lock` sidecar. It isn't re-entrant.
  - `update_json`, `backup`, `refuse`, `is_object` and `dumps` cover the rest.
  - Its tests are `conf/test/store-test.py`.
- A file that doesn't read is never saved over. Each fix in DEE-300 is pinned by a test that
  writes a bad file and checks it's still there afterwards.

### Tests

- `docs/tests/test_doc_links.py` (in test-python) fails on a doc link to a missing file or
  heading, or a repo path a doc or comment names that git doesn't track. `frame-hands/notes/...`
  (the separate frame-hands project), build outputs and `dev/maintainability/` are skipped.
- `just test` runs, in order: test-python, test-c, test-keyboard, test-bash, test-testbench and
  test-fast-install. It stops at the first recipe that fails (see the open items).
- Run it on the Frame like this, and only one full run at a time:
  `frame-job --local -- podman exec -w $PWD dev taskset -c 0-3 just test`. If a run dies,
  check with `pgrep -af "just test"` that it really stopped before starting another.
- `scripts/testgate/testgate.py PATTERN...` is the Python gate.
  - A pattern that matches no file fails.
  - A file whose tests were all skipped is SKIPPED, with the reasons, never "ok".
  - A plain script skips itself with exit 77 (its last stdout line is the reason). A
    `skip  REASON` stdout line is a partial skip.
  - New Python test folders must be added to test-python's pattern list in the `justfile`.
- test-c compiles and runs each C and C++ test through its `run NAME CMD...` helper, so one
  failure doesn't hide the rest and the gate still fails.
- test-keyboard skips (and says so) without `libopenvr_api.so`. As root it runs the startup and
  settings tests as nobody.
- test-bash syntax-checks every shell script, found by shebang and by `*.sh`. A count guard
  (40 or more) catches a broken search.
- Tests live in one `tests/` folder per component. Some older components still use `test/`.
- A test that can't run offline in the dev container skips with a printed reason. It never
  passes quietly.
- In an agent shell, a background wait that says "matched" may not mean the run has finished.
  Wait for the `exit N` line the run appends to its log.

### Commits and safety

- Format: `type(area): what changed, in plain words (DEE-NNN)`, for example
  `test(layout): ...`, `fix(store): ...`, `refactor(store): ...`, `build(just): ...`. The body
  explains why in full sentences. No trailers (no Co-Authored-By).
- Local commits only. No push, tag, PR or remote branch change until Daenon says
  "let's push to remote".
- No `rm` or `git rm` in agent shells (it stalls T3 on an approval prompt). Leave a file that
  should go and list it below. DEE-310 had a one-off exception from Daenon: one batched `git rm`
  of the backup keyboard's tracked files.
- No install, fast-install, restarts, testbench or recorder runs, and no live config edits,
  during the series. Anything that needs those goes under device checks.
- Reviews: at most one read-only Opus reviewer, for a significant change, after commit, with no
  re-review loop.

## Open items

### From DEE-299 and DEE-300

- Hot readers that use the lenient `load_json` pay for a parse and a warning each time. That
  hasn't been measured.
- A held `locked()` can freeze a settings window for up to 10 s while another writer holds the
  lock.
- `~/.bashrc` mounted as a single-file bind mount has no fallback when `os.replace` can't
  replace it.
- frametop-hand-recorder's `package.sh` could copy `conf/` so it shares ft_store, instead of
  keeping its own writer.
- Done (DEE-320): fitcoach and game_pause wrote through fixed `.tmp` names.
- Done (DEE-320): in Input Settings and the voice settings, the lock covered the write, not the
  read before it.

### From DEE-302

- `session/test` (needs the host's AT-SPI, dbus-daemon and PyGObject, skipped in the container)
  and `session/tests` (pytest) are two folders for one component.
- Done (DEE-321): a test file with TestCase classes but no `unittest.main()` ran no tests and passed.
- Partly done (DEE-321): testgate files time out (900 s, `FT_TEST_TIMEOUT`). pytest, test-c, the
  keyboard tests and the bash recipes still have none.
- The settings tests write PNGs to fixed paths in /tmp.
- `conf/test/conf-test.py` has `if C:` blocks that skip silently when the C writer isn't built.
- `just test` stops at the first failing recipe, so later recipes don't report.
- test-bash doesn't recognise a shebang with a CRLF ending.
- Left on disk because nothing may be deleted in agent shells:
  - the empty folders `pointer/test`, `layout/test` and `screens/test`, left behind when their
    tests moved to `tests/`
  - root-owned output in `build/tests/keyboard`, including its `xdg-runtime`
  - `/tmp/ft-keyboard-test-0`

### From DEE-311

Covered by tests:
- the layout planner and named layouts (`layout/tests/test_plan.py`)
- `load_layout` on a bad file (DEE-300's `layout/tests/test_layout_file.py`)
- the conf writer (`conf/test/conf-test.py`, 15 `set_value` cases)
- camd's camera roles and buffer layouts (`hands/tests/xrcams_test.c`)
- import smoke tests for display-settings, remote-displays, steam, remote and framedrop
- a compile check for ft-powerd

Still open:
- (Done in DEE-307: `ft_vr_command`'s parser and the pose math are in `screens/vrcmd.h` and
  `screens/posemath.h`, with tests.)
- ft-hands' camera map (`cameras_from_xrservice_log`, `camera_for_pipe` in
  `hands/track/main.cpp`) is in the same position. That file links ncnn.
- The hand panel (`hands/rec/panel/ft-handpanel.cpp`) wasn't examined.
- Profiles beyond rename and delete (start profile, apply on start) have no direct test.
- Two processes calling `set_value` at once can lose an update, because it doesn't lock. That
  is a behaviour gap, not a flaky test.
- The ~1,300 lines of analysis tools in `screens/tests/handinput_test.cpp` (5,613 lines) weren't
  moved to a tools folder. They're `main`'s `--sweeps`, `--take`, `--grabthat`, `--carries` and
  `--replay` modes, and they use the same synthetic-hands and replay helpers as the tests. Moving
  them means deciding which helpers move too, so it isn't a mechanical split.
- Components with only a documented reason, no test:
  - decoration, host and setup are shell. They change KWin, macOS or Bluetooth state, so
    test-bash only syntax-checks them.
  - framedrop's shell scripts run on a devkit or the headset. Same syntax check.
  - ft-remote-settings and FrameDrop's progress window are GTK 4. Their smoke tests skip in the
    dev image (no PyGObject) and pass on the Frame.

### From DEE-306

`input/input-relay.py`'s `main()` is now a `Relay` class. main() makes the virtual devices and
the sockets, then calls `recover_from_restart()` and `run()`. The state lives in `PointerLease`,
`HeldKeys`, `Typing` and `FullKeyboardLink`; the pause's is `game_pause.GamePause`, as before.
`input/test/restart-test.py` makes a `Relay` with fakes and never runs the loop. Use it as the
model for new relay unit tests.

- keys-test and hostin-test rename the ft-screens socket by replacing its constructor line in the
  source text (`screens_sock = socket.socket(...)`, once, in main()). Keep that line as it is, or
  change those tests with it.
- Running the relay tests with a RELAY argument that points at a copy outside `input/` fails 4
  keys-test checks (Frametop's tools are found relative to the relay's path). That holds for an
  unchanged copy too.

Still open:
- L5 and L6 recovery stays in `GamePause.load`, not in `recover_from_restart`: it has to run
  before the rules configure the pause (with automatic pausing off, `configure` ends a pause's tie
  to its game). Moving it means changing GamePause's construction.
- `control_command` is still one if-chain of about 170 lines. Splitting it per command is next if
  the control socket grows.
- restart-test doesn't cover K7's stop when the helper isn't running, or K3 when ft-screens isn't.
- After a restart, a plain key the desktop had down stays in this relay's record until the next
  scan (about 1 s); ft-screens has already let go of it. Pinned in restart-test, not changed.

### From DEE-307

- The pose math is in `screens/posemath.h` (templates over any type with `float m[3][4]`),
  tested by `screens/tests/posemath-test.cpp`.
- `ft_vr_command`'s 46 conditions are in `vrcmd::Parse` (`screens/vrcmd.h`), in the same order
  with the same scans. ft_vr_command binds its old variable names as references into the
  parsed `Cmd`, at the same scopes, so the branch bodies didn't change. A new command needs a
  verb in vrcmd.h, a condition in `Parse` at the right place in the order, and a case in
  `screens/tests/vrcmd-test.cpp`.
- ft-pointer has one `sendPose(S, R, origin, basis)`. The caller passes the headset's standing
  and raw poses: placements use `sendPoseNow` (read at prediction 0), the loop passes this
  tick's `hmd` and `hmdRaw` (prediction 0.011).
- The ft_pointer driver no longer takes move, recenter, aim, gaze or distance. Nothing sent them:
  the helper handles the relay's move, recenter and gaze itself. It follows the head until the
  helper's first pose, then holds the helper's pose.

Still open:
- vr.cpp's other globals (about 80, over 1,000 uses). Only the SteamVR panel list is grouped
  (`SteamList g_steam`). The rest sit next to the types they use, so one struct for all of them
  means moving those types first. Group them one feature at a time, the way `g_spin` and
  `g_steam` are.
- The scans inside ft_vr_command's branch bodies (cutouts, spin, the screens list, hide/show
  arguments) stay in vr.cpp, untested.
- Done (DEE-322): ft-pointer's `sendPose` had no offline test. `pointer/common/vec3.h` is the
  OpenVR-free half of vrmath.h, and `pointer/helper/devpose.h` is tested.

### From DEE-308

`dev/maintainability/python-split-plan.md` names the cohesive pieces of the five large Python
classes (Input Settings' Backend, the gaze probe and service, the hand recorder's Session and the
voice daemon). Each time one of those files is touched, move at least one piece out, as plain
Python with unit tests, and keep a thin method in the class.

- Backend's file I/O is in `input-settings/input_rules.py` (`read_json`, `read_conf`,
  `write_conf_value`, `save_rules`). The Commands page's logic is in
  `input-settings/input_commands.py`. Both have no Qt and take paths and the host runner as
  arguments. ft_input_settings keeps `read_json`, `read_conf`, `write_conf_value`, `host`,
  `RULES_PATH` and `CONF_PATH` under their names and passes them at call time, so tests can
  still patch them there. Tests: `input-settings/test/test_input_rules.py`,
  `test_input_commands.py`.
- ft_input_settings puts its own folder on `sys.path`, because `pack/pyside-runtime.py` loads
  it by its path.

Still open:
- The rest of Backend (relay socket and polling, devices and maps, key combinations,
  controllers and pause, gaze, voice, hands and models, panels, Bluetooth) and the other four
  classes, per the plan.
- `from input_rules import read_json` makes a second name: a test that patches
  `input_rules.read_json` doesn't reach Backend, which calls `ft_input_settings.read_json`.
  Patch it on ft_input_settings.
- `input_rules.save_rules`' docstring says it "writes nothing" on a bad file, but
  `load_json_strict` writes the `.bad-` copy. Wording only; fix it with the next code change.

### From DEE-310

Daenon's decision (2026-10-31, "We can remove the backup keyboard"): Full Keyboard is the only
keyboard. It's recorded in `docs/design.md`, along with why its Steam keyboard replacement is on
by default in Frametop's mode.

- Removed (one `git rm`): `screens/keyboard.cpp`, `screens/keyboard.h`, `screens/rounded.h` and
  `screens/tests/rounded-test.cpp`. That test checked the old panel's renderer byte for byte, so
  test-c has 31 `: ok` lines now, not 32 (29 programs, `hand-asset.py --check` and the
  ft-powerd compile check). Removed in code: the FT_KEY and FT_KEYBOARD_CLOSED events,
  `ft_vr_keyboard_show` and `hide`, `panel_key`, `kb_screen` and `kb_auto`, the panel's laser,
  aim, drag and step-aside code, `keyboard=` in vr.cpp's debug line, and `kb_screen=` and
  `kb_auto=` in compositor.c's.
- The label font code (Noto Sans through stb_truetype) that the lazy susan, the reset question and
  the pill used is now `screens/label.cpp` (`label::Draw`, `label::Width`), unchanged.
- While Full Keyboard isn't running (no `fullkb` for 3 s), `vrkeyboard show` and `toggle` log
  `keyboard not opened (...): Full Keyboard isn't running` and reply `ok no keyboard`.
- Frametop's code in upstream's `run_vr` is now `FrametopVr` (`keyboard/src/frametop_vr.cpp`),
  with no OpenVR calls. vr.cpp calls its hooks and fills `FrametopVrAccess` through its
  runtime-watched wrappers. The diff against upstream's vr.cpp went from 259 changed lines to 102.
  Tests: `keyboard/tests/frametop_tests.cpp` (`hooks_tests`).
- FrametopSink resends desktop key releases that the relay didn't take (`owed_`, its socket
  full) before the next key and at every pump. Presses aren't resent. A relay that restarts has
  let go of everything and drops keys until the next `kbhello`, so a release owed then is lost
  there, harmlessly. Tests: `owed_release_tests` (its stand-in relay has no hello gate).
- Names waiver: `framekeyboard` and `framekeyboard-native-guard` stay as they are. See
  `keyboard/FRAMETOP.md#names` and the root AGENTS.md.
- `keyboard/AGENTS.md` now opens by saying the root rules win. FRAMETOP.md lists the upstream
  parts that Frametop's mode never reaches. `scripts/update-check.py` has hints for libei
  (gamescope's EIS server; Full Keyboard builds its own libei client in) and
  deckard-steamvr-session. A deckard-mesa hint was dropped as unverified.
- Review: one Opus reviewer after 6c0f032 found nothing Critical or Important. Its five Minor
  findings were fixed in 3d5af59 and 3ad697c (owed release order, the restart wording,
  reference.md's "crashes", gaze/README.md's pointer, the update-check hints).
- Tests: the last full `just test` ran on 3ad697c (`/tmp/dee306/a310b.log`, exit 0): the same
  counts as DEE-308's 5d57eda except test-c, 31 `: ok` lines (rounded-test removed).
- DEE-192 (non-US layouts) belongs to Full Keyboard. The old panel was US-only, so nothing it
  needs was removed.

Still open:
- `docs/charts/G3-input.mmd` and its SVG still say "Frametop's key panel is up (vrkeyboard show)".
  The chart needs regenerating, not just a text edit.
- `docs/hazards.md:21` mentions "Frametop's keyboard's" Shift as history. It was left as it is.
- Stale `screens/build/keyboard.o` sits in the worktree's build folder (untracked, not deleted).
- A key press the relay didn't take isn't resent, on purpose: a late press would type.
- No offline test runs ft-screens' `vrkeyboard` path (it needs a nested compositor, and
  `--no-vr` next to the live desktop has crashed it before).
- `pending_*` settings copies in `keyboard/src/app.cpp` follow upstream's pattern. Not changed.

### From DEE-304

Shared headers for code that had been copied between programs. No behaviour change except
where noted.

- `screens/handshape.h`: the 21 MediaPipe joints by name, `FingerExtension`, `Openness` (four
  fingers), `Curl3` and `PalmNormal`, for handinput.cpp, palmmenu.cpp and grabthat.cpp. Test:
  `screens/tests/handshape-test.cpp`.
- Openness: the two copies were different measures, not drift. handinput's averaged four
  fingers; the palm menu's averaged three (middle, ring, little), which is `Curl3`. The palm menu
  keeps its three-finger measure on purpose, since its thresholds were tuned on it, and
  handshape.h says so.
- `posemath::kPitchClampDeg` (85) replaces the literal in ft-pointer.cpp (7 clamps, 2
  `LimitPitch`) and gazehand.h. The driver's copy went with DEE-307.
- `hand-asset.py` now writes `handasset.h` (declarations) and `handasset.cpp` (the data, compiled
  once). `--check` covers both. The data is byte-identical.
- `hands/include/fh_shm.hpp` is the one reader for ft-hands' `hands` and `gestures` files, used by
  the pointer helper (`handsin.h`) and ft-screens (`handcut.cpp`). It looks the file up again at
  most once a second and maps it again when it's gone, replaced, or a new size. handcut's old
  3-second stale reopen isn't needed, because ft-hands rewrites the same file. The helper now
  also checks the file's owner. Test: `hands/tests/shm_test.cpp`.
- `conf/ftsock.h` (C and C++): every C/C++ socket name, `ft_sock_addr`, `ft_sock_send` (0 or an
  errno, always MSG_NOSIGNAL) and `ft_sock_send_logged`, which logs a destination when it starts
  failing (at most once every 10 s) and when it works again. Used by compositor.c, vr.cpp, the
  pointer helper, driver and probe, ft-gaze and its panel, the hand recorder's panel, frunner's
  panel, ft-handview, ft-powerd and ft-stream. New log lines: "to @NAME: ERROR" and "to @NAME:
  works again". ft-gaze's question to ft-screens returns at once when the send fails. Test:
  `conf/test/ftsock-test.c`, built as C and as C++.
- handinput.cpp's three least-squares velocity fits (throw and rewind, push speed, flick) call one
  `LineSlope`. handinput-test's replay output is byte-identical. The push's fit now accepts a time
  spread of exactly 1e-9 s^2, which it used to reject.
- `screens/rendernode.h`: ft-screens opens the render node once. overlaytex and the cutouts'
  Renderer share one refcounted GBM device on the VR thread. compositor.c takes the node's path
  from it.
- The remote-streams verify replay (`.claude/skills/verify-remote-streams/scripts/replay-src`)
  follows ft-stream's new socket names and includes ftsock.h. Only its harness was compiled here;
  the replay itself wasn't run.
- Review: one Opus reviewer after 9da7cd3. It found one Important issue (the replay's extractor
  no longer matched) and four Minor ones. Fixed in 8c72d59: the replay, ftsock.h's include in
  calpanel.h moved outside `extern "C"`, and the helper's send-log map capped at 64 names.
  Accepted as they are: the new log lines, and the push fit's boundary.
- Tests: the last full `just test` ran on 8c72d59 (`/tmp/dee306/a304b.log`, exit 0).
  test-c has 35 `: ok` lines, up from 31: handshape-test, shm_test, and ftsock-test built as C
  and as C++. Everything else matches DEE-310's 3ad697c run (python files 97 ok, 2 skipped).

Still open:
- `overlaypanel.h` and `gpuctx.h` were not written. ft-spotpanel, ft-gazepanel, ft-handpanel and
  ft-reset-timer still each set up their own overlay, DMA-BUF and EGL code, and handview.cpp's GPU
  setup still duplicates handrender.cpp's. This is the biggest part left. It needs device checks
  of every panel, so it's better as its own ticket.
- ft-handview (its own GPU setup) and ft-handtest still open the render node themselves. That
  covers ft-screens only.
- handinput.cpp still uses many joint literals (`p[8]`, `p[4]`) besides the ones handshape.h
  replaced. It could switch to the named joints.
- The keyboard fork's FrametopLink keeps its own socket-address code, to keep the fork isolated.
  The Python programs still name their sockets themselves.
- The remote-streams replay should be run once (`scripts/replay` in that skill) to confirm its
  baseline.

### From DEE-305

One Python client for the datagram sockets, and no blocking host calls on the settings apps' UI
threads. No behaviour change except where noted.

- `conf/ftipc.py`: `Client` (`ask`, `send`, `why` = `nobody`, `timeout` or the error), one-shot
  `ask` and `exchange`, and `Worker` for UIs. The servers don't echo request IDs, so there are
  none. Instead the client drains its socket before each question and moves to a new socket
  after a timeout, so a late answer goes to a closed socket. That protection doesn't hold for a
  client bound to a name (`bind=`): ft-gazectl, gaze-report and ft-gazeprobe keep their old
  bound names (`ft_gazectl.PID` ...), so they only get the drain, as before. ft_gazed only
  answers the sender's address, so ft-gazeprobe, the one long-lived one, could autobind to get
  the full protection. `MAX_REPLY` is
  1 MiB, replies decode as UTF-8 with bad bytes replaced, and the timeout is per call. Test:
  `conf/test/ftipc-test.py` (late answer, waiting datagram, 150 KB answer, worker order, a
  failing job, a failing deliver).
- Moved to it: ft_layout's `Socket` and `ask_float`, ft_floatd's screens socket, frunner,
  Display Settings, Remote Displays, ft-gazectl, gazecheck's `ask`, ft-gazeprobe, the bench's
  recorder socket, gaze-report, keys-report and report-watch. No-answer messages now say why.
- Display Settings and Remote Displays ask through one `ftipc.Worker` each. Answers come back
  through a queued Qt signal. The poll is one job, and a new poll isn't queued while one is
  pending. Input Settings runs the Bluetooth read, Apply fixes (pkexec) and the Add check on
  threads. Their busy flags clear in a `finally`.
- `conf/fthost.py`: one `host_command`. Recorder's version won, because it starts host commands
  in the home folder (`env -C ~`): Display Settings and Remote Displays now do too. Their
  callers pass absolute paths. `recorder/common.py` keeps its own copy, because the standalone
  Hand Recorder ships without `conf/`.
- One resolution list, `ft_layout.SCREEN_RESOLUTIONS` (Display Settings' list). Remote Displays
  gains "portrait 4K" (2160x3840), the same pixel count as 4K. It already offered portrait
  sizes.

Copies left alone, on purpose:

- `gaze/bench/gazebench.py`, `gaze/rec/session.py`, `hands/rec/session.py` and gazecheck's
  panel/service class: one socket gets both events and replies, and the code sorts them. ftipc's
  drain would drop events.
- `recorder/one_window.py`: SOCK_STREAM. `tour/ft_tour.py`, `input/rules-migrate.py`: send
  only. The input-relay and game-pause code: servers, or non-blocking.
- `voice/voice_settings.py`: reload and devices on one socket. It could move to `ftipc.Client`
  in a later pass.

Open:

- Fixed in 176eb76: fast-install now restarts the desktop for `conf/` changes (last, and kept
  back while the headset is worn), so a change to `conf/ftipc.py` alone reaches ft_floatd,
  frunner and the settings apps.
- Display Settings' curve: the layout edit now runs when ft-screens answers. If a screen were
  removed in those milliseconds, index `i` could point at another screen.
- No test drives the Display Settings or Remote Displays worker path (the poll guard, the
  answers' order).

### From DEE-303 (part 1)

The recorders' shared code, part 1. No behaviour change except the private folders and
tools/ring.py's new fields.

- One `sha256_file` (`recorder/files.py`, with a progress step) and one `now_iso`
  (`recorder/contributions.py`).
- `recorder/privacy.py`: `key_identifies`, `value_identifies`, `strip_calibration`, moved out of
  hands/rec/session.py. hands/rec/validate.py imports it instead of the session runner. Test:
  `recorder/tests/test_privacy.py`.
- Private recordings: the 9 recorder entry points (both windows, both session.py, takes.py and
  hub.py, and gaze eyerec.py) set umask 077 under `__main__`. Session and round folders, their
  `takes/` and eyerec's camera folders are made 0700 explicitly. Recordings made earlier keep
  their modes. The window has made the base folder 0700 since 2026-10-02.
- `recorder/fhformat.py`: ft-camd's ring and FHSET01. Readers: hands/rec session, takes,
  validate and sides; hands/camcheck.py; tools/ring.py, show_set.py and cut_sets.py. Test:
  `recorder/tests/test_fhformat.py` builds a C++ program against fhring.h and record.h and checks
  every size, offset and constant. It skips without a C++ compiler. tools/ring.py now reads
  `flags` (`cam.dark`, `cam.color`) and `dark_mean()`. Test: `hands/tests/test_ring.py`.
  test_camcheck builds its ring from fhformat, so it no longer checks camcheck against itself.
  fast-install's camwatch entry now lists `recorder`.
- The re-export shims are gone: hands/rec/session.py, both takes.py and ft_handrec.py import
  only what they use. The window, hubs and tests import `recorder.button`, `recorder.units`,
  `recorder.common` and `recorder.files` themselves.

Judgment calls:

- fhformat lives in `recorder/`, not `hands/`. The standalone Hand Recorder ships `recorder/`
  whole, and every hands/rec reader already imports it. camcheck, sides and the three tools
  append the repo root to `sys.path` if it's missing (append: it shadows nothing).
- The `sys.path` inserts stay. hands/rec and gaze/rec import their siblings by bare name
  (`session`, `takes`, `hub`, `validate`, the same names in both folders), and the programs run as
  scripts from the tree. Removing the inserts means making the two folders packages and changing
  every launcher, rehearse.sh, the tests and the standalone recorder's copy. That's a ticket of
  its own.
- voicelog.py keeps its one-line `mono_ns`: hands/tools/voice_flags.py imports it with only
  hands/rec on the path.

Open (the rest of DEE-303): done in DEE-319, except the Takes, Viewer and Export pages (see DEE-319 below):

- One hub CLI: gaze/rec/hub.py and hands/rec/hub.py's command-line part (hands 293-417 at
  c13d168).
- One control-loop base for pause, redo and stop (hands session.py ~1799-1875 and 2226-2311, gaze
  743-809 and 1007-1085 at c13d168).
- Shared QML pages in `recorder/qml/` for both windows, and test_qml_backend over both.
  `RecorderBackend` (recorder/window.py) is still about 860 lines.
- The `sys.path` inserts (above).
- No test of the hand session's 0700 folders: gaze has `test_round_folders_are_private`.
- The standalone Hand Recorder repo copies files from here: recorder/privacy.py and fhformat.py
  are new in `recorder/`, which it ships whole. Check its copy list at its next sync.

### From DEE-309

Every build download is pinned by a full commit and checked by sha256, with one list of packages
and one base image for both build containers.

- ncnn is in `uv.lock`'s tracker group (`ncnn==1.0.20260526`). A uv override with a never-true
  marker drops ncnn's `opencv-python`, which would clash with `opencv-python-headless`. The
  image's import check and release.yml's smoke test import ncnn. In-box,
  `gaze/tracker/build.sh` falls back to a hashed `pip install --no-deps --require-hashes` of the
  locked cp314 aarch64 wheel (for an older image without ncnn), and stops if ncnn still doesn't
  import.
- `scripts/stb.sh`: one stb fetcher at commit 2c980bb5, with a sha256 per header (`stb_fetch DIR
  HEADER...`). screens, gaze, hands/rec and layout/countdown use it, and fast-install rebuilds
  those four when it changes.
- hands/Makefile fetches ncnn's source as the release's full-source zip, checked against
  GitHub's listed digest, in place of a git clone.
- `scripts/openvr.sh` and the Containerfile fetch the OpenVR SDK by commit 09240643. Full
  Keyboard builds against `scripts/openvr.sh`'s header (`OPENVR_INCLUDE_DIR`, default
  `third_party/openvr`, byte-identical to the pin).
- install.sh fetches distrobox 1.8.2.5 by its commit, 40c3cd72.
- get.sh checks a downloaded Frametop.zip against the `SHA256SUMS` beside it, and drops a zip
  that doesn't match. Every release with a zip since v0.2.2 publishes one. Test:
  `pack/test/get-test.py` (fake curl).
- stream/build.sh builds ft-stream only. `--spikes` adds ft-streamtest and the other spike, and
  ft-streamtest now uses `host.cpp`'s `ftstream::Client` and `WebApi` instead of its own copy.
  The iris header moved to `stream/iris.h`.
- `setup/packages.txt` holds both containers' Fedora packages (`both:`, `dev:`, `image:`).
  setup/dev-container.sh takes its base image from the Containerfile's `FROM ...@sha256` line.
- testbench/requirements-mcp.txt lists every wheel's sha256, and testbench/install.sh installs
  with `--require-hashes`. It stays outside `uv.lock` because its certifi and pycparser differ
  from the lock's.
- Test: `pack/test/pins-test.py` scans the build files for a curl without a sha256, a git clone,
  a git fetch without a full commit, pip without `--require-hashes`, and CMake downloads. It
  also tests stb.sh with a fake curl, and checks the OpenVR pins and the package list.

Open:

- Done (DEE-324): `stream/spike/iris.h` was no longer used (`stream/iris.h` replaced it).
- Not hashed: the dev container's `requirements.txt` venvs (gaze/tracker, voice) and
  `gaze/tracker/.frame-job`'s SETUP on the 7i. Hashing them means every platform's wheels.
  pins-test doesn't scan `.frame-job` files.
- Pinned by git commit only, no sha256: whisper.cpp and moonlight. dnf packages float within the
  pinned base digest. The base tag has moved on (08f9a0...), and the registry still serves the
  pinned 034cb7...
- get.sh doesn't check a local `--zip` file. A `--zip` URL with a query string can't find its
  SHA256SUMS. When the `latest` Location lookup fails, the zip and the sums are each fetched
  through `latest/download`, so a release published between the two shows as a mismatch.
  get-test covers neither the stable channel's lookup nor the API channel (its fake curl ignores
  `-I`).
- The gaze/tracker in-box fallback pins only the cp314 wheel, so an image with another Python
  now stops instead of warning. In the dev container, `pip install -r requirements.txt` still
  pulls ncnn's dependencies, `opencv-python` included.
- The tracker group now installs ncnn's dependencies (requests, tqdm, portalocker) into the
  image. They're hash-locked, but before this ncnn went in with `--no-deps`.
- pins-test is a heuristic: a "sha256" anywhere in the next 8 lines counts, `curl "$url"` with
  no flag first isn't matched, and files outside its list (hands/models.sh, voice/models.sh,
  framedrop/devkit.sh, pack/install-release.sh) aren't scanned. Those are pinned today.
- `keyboard/CMakeLists.txt`'s comment names `pack/test/test_pins.py`. The file is
  `pack/test/pins-test.py`.
- A failed distrobox fetch leaves `~/dev/src/distrobox.part.PID` behind, and a failed stb
  download leaves `NAME.part`. Both are harmless.

### From DEE-312

Eye frames and voice clips are written 0600, and a link at their path isn't followed.

- ft-eyegrab's lab modes (`--rec`, `--seq`, `--dump`) open their files through
  `create_private`: `O_NOFOLLOW`, mode 0600 whatever the umask, and a file with a second name
  (a hard link) is refused. An old file at the path is emptied and made 0600. They run with
  sudo, so the file goes to `SUDO_UID:SUDO_GID` (root owned it before) and lab tools read a
  recording without root. `--share` already made its file 0600 with `O_NOFOLLOW`.
- The buffer list's `struct stat` is zeroed and a failed fstat is reported, so `ino` is never
  stack garbage.
- ft-voiced's `save_clip` opens with `O_NOFOLLOW` and 0600 and sets 0600 on an existing clip.
- Tests: `gaze/tracker/tests/eyegrab-files-test.c` in test-c (builds ft-eyegrab.c with
  `FT_EYEGRAB_NO_MAIN`), and `Clips` in `voice/test/test_daemon.py`. Both run with the umask
  at 0 and check that a link's target is left alone.

Open:

- The clips folder is still made with the umask's mode (usually 0755), so its file names (the
  dictation times) show. The eyegrab output folder is whatever the user made.
- A refused clip raises in the dictation path, as any failed clip write did before, so that
  dictation fails. Logging and going on would be kinder.
- Old clips saved before this fix stay 0644 until pruned (the newest `save_clips` are kept).
- Unchanged here: recorder folders' default mode (DEE-319) and the calibration backups'
  growth (M2).

### From DEE-298

`ft-layout remote add` checks every option before it pairs, and unpairs if the display then
isn't saved.

- `--fps` and `--bitrate` must be whole numbers and `--size` must be `WxH`. A bad one is a plain
  `remote add: ...` error (exit 1) before ft-stream or the host is reached.
- Saving goes through `remote_add_save`. If it fails (or Ctrl-C) after pairing, `ft-stream
  unpair` runs for the new client at the same `ADDRESS[:PORT]`. A failed unpair is said, and the
  first error is still raised.
- Tests: `layout/tests/test_remote_add.py`, with a fake `run_stream` and no ft-screens.

Open:

- `--fps 0` is still accepted, as `remote set fps=0` is. No range check was added.
- A failure in `start_remotes_now` after the save doesn't unpair: the display is saved and can
  be connected later or removed (which unpairs).
- A `display:` app still asks the host for its monitors before the options' other checks; that
  call doesn't pair.

### From DEE-313

Dead code out, docs matched to the code, and uninstall.sh's gaps closed.

- Removed: `ft`'s installed mode and `ft update` (`ref_file`, `published_file`, `published`,
  `reject_latest`, `default_published`, `update`, `repo_mode`, `need_repo`); `ft` now needs a
  checkout. `pauseGestureText` and `setGazeTracker` in `input-settings/ft_input_settings.py`.
- Kept, as the ticket allowed: ft-gazed's "source" path (the fallback before per-eye
  calibration, and `--source`); ft-eyegrab's `--rec`, `--seq` and `--dump` (tested by
  `gaze/tracker/tests/eyegrab-files-test.c`, used by `gaze/tracker/lab`); `stream/host.h:4`
  is right (`stream/spike/ft-streamtest.cpp` includes it).
- uninstall.sh: `desktop_entries DIR` takes `DIR/ft-*.desktop` and `DIR/frametop-*.desktop`;
  `~/.cache/frametop-framedrop` goes with the caches, `~/.local/state/frametop-trial` with the
  settings, `~/.local/share/frametop/gaze/bench` with the recordings. Sourced, it only defines
  its functions (`scripts/tests/test_uninstall.py`).
- Splits (headings left behind with a link): hands/README.md -> hands/tracking.md,
  hands/cutouts.md, hands/rendered-hands.md; docs/reference.md -> docs/reference-apps.md,
  docs/reference-features.md; docs/design.md -> docs/design-input.md, docs/design-session.md.
  gaze/README.md's lines over 1,500 characters wrapped.

Open:

- No doc is over 50 KB now, but hands/README.md (48 KB) and hands/cutouts.md (39 KB, one
  section) are close.
- The link check reads inline links and paths only, not reference-style `[x]: path` links
  (the docs don't use them yet).
- `ft` outside a checkout now stops with a message; `ft clean` outside one went with it.

### From DEE-314

Personal setup out of the product.

- Tests and ft_layout's example use 192.0.2.x (documentation range) for addresses; Remote
  Displays' Address hint keeps 192.168.1.20 (a generic example, not a real machine).
- `.gitignore` lists `.frame-job` (no effect on `gaze/tracker/.frame-job` until it's untracked);
  `gaze/tracker/.frame-job.example` keeps the settings; AGENTS.md
  and docs/charts/README.md say to run the command after `--` without frame-job.
- TLA+ models: `docs/charts/fake-worn-guard/` and `docs/charts/remote-streams/` (moved from the
  two skills' `model/`); the skills' `scripts/check` read them there. `test_verify_skill.py`
  skips without `.claude/`.
- DEE refs: gone from product code, docs and tests. Kept as the record: this folder,
  `docs/charts/` (the findings tables, and a few ids in the models and charts), and `.claude/`.

Open:

- Resolved 2026-10-31 (DEE-324): `gaze/tracker/.frame-job` untracked.
- The review ran with DEE-315 (one Opus reader, 12 findings, none in behaviour). Fixed in
  2a9898a: broken comment lines in input-relay.py, install.sh and keys-test.py; logfile.h's
  trackwatch wording; gaze/README.md naming the tracked `.frame-job` (the doc check would fail
  once it's untracked); AGENTS.md's "gitignored"; three small wordings. Left: long lines in
  input-relay.py's docstring and rules-migrate-test.py (no limit enforced); personal host
  names in testbench tests (`Deej-5090`, `oma7`; only addresses were in scope); the Address hint
  above.
- testbench/ recommendation: keep it in the repo for now. It's dev-only (not in the image,
  `.dockerignore`), its tests run in `just test`, and the verify skills and AGENTS.md use it in
  place. Splitting it means get.sh, which full-clones, stops carrying it: a separate repo (or a
  submodule the dev setup fetches), `test-testbench` fetching it or skipping, and the skills
  pointing at its new path. Daenon decides.

### From DEE-315

Stop the repo growing. Rewriting history is out of scope.

- The hand models' data (`screens/handasset.cpp`, about 1 MB; the ticket's `handasset.h` before
  DEE-304 split it) is made by the build: `screens/build.sh` and test-c run
  `screens/tools/hand-asset.py --cpp build/.../handasset.cpp` and compile that. The 1 KB header
  stays as source, checked by `--check`, which also checks the tracked `.cpp` while it's there.
  `.gitignore` lists the `.cpp` (no effect while tracked).
- `get.sh` clones with `--single-branch --filter=blob:none`, tries again without `--filter`
  when git refuses it (and never over a folder a failed clone left), and an update fetches the
  wanted branch by name, so switching versions still works. `scripts/tests/test_get.py` tests it
  with a stub git (7 tests, in test-python). The script gained uninstall.sh's source guard.

LFS or release assets, for the tour clips (`tour/clips/*.webp`, 5.8 MB, one commit) and the
charts' SVG and PNG (1.5 MB, 16 files, 7 commits). Recommendation: release assets, and only
when they next change. Not LFS.

- With the filtered clone, a new install downloads only the current files, so the old copies
  cost nothing to users now. What's left is the remote's own growth, which comes from re-cutting
  clips or re-rendering charts. Moving them is only worth it then.
- LFS needs git-lfs on every clone (SteamOS doesn't ship it, so get.sh, the dev container and
  CI's checkout (`lfs: true`, for the image's `COPY .`) would all need it), and GitHub's LFS
  storage and bandwidth quota on Frametop/frametop. Every install would spend that bandwidth.
- Release assets: a pinned file on a release (or the Hugging Face models repo), fetched with a
  sha256 check the way `hands/models.sh` fetches the hand student. Clips: `tour/install.sh` (or
  install.sh) fetches them into `tour/clips/`, the image build does the same, and the clips'
  README keeps the recipe. Charts: the docs link the release's SVGs, or keep only the `.mmd`
  (GitHub draws Mermaid in Markdown) and drop the PNG previews.
- What stays in the repo either way: the `.mmd`, `.tla` and `.cfg` sources, the clips' README,
  `screens/assets/hands/*.glb` (the build's input now), and anything the install reads that has
  no pinned download.
- Not done: the build can't re-render the charts. There's no renderer in the repo (they were
  drawn outside it), and mermaid-cli would add Node and a headless Chromium to the dev container.
- About as big, for later: `hands/models/ncnn/*` (5.9 MB, one commit) and
  `hands/rec/poses/contact-sheet.png` (two versions in the history, 1.9 MB).

Open:

- Daenon: `git rm --cached screens/handasset.cpp` (then delete the local copy, or `git rm` it
  outright) once this lands. Nothing reads it after 0a450b5.
- Daenon: the LFS or release-assets call above. Nothing on the remote changed.

### From DEE-316

One installer helper: `scripts/install-lib.sh`, sourced after `scripts/_env.sh`.

- `install_unit TEMPLATE [NAME]` and `install_menu_entry TEMPLATE [NAME]` write the filled
  template to `~/.config/systemd/user` or `~/.local/share/applications` on the Frame.
  `enable_units UNIT...` prints the reload-and-enable command for the installer's own host step.
  `FT_TEST_UNIT_DIR` and `FT_TEST_APPS_DIR` move the folders, for tests only.
- Migrated: the unit installers power, pointer/helper, keyboard, pointer/dashpatch, gaze, voice,
  hands and desktops.sh (autostart, relay); the menu entries gaze/bench, gaze/rec, gaze/probe,
  display-settings, tour, input-settings, remote-displays, hands/rec and remote.
- Kept as they were: each script's steps before and after the reload, voice's `2>/dev/null` on
  enable, autostart's `--quiet`, and hands reloading without enabling (HANDS_MODE decides). The
  bench and the recorders install under `frametop-*` names from `ft-*` templates.
- Left alone: `gaze/tracker/install.sh` and `setup/bluetooth/install.sh` (root system units
  through sudo, with their own edits), desktops.sh's session entry (`@SESSION@`, the Native
  Desktop copy), and `keyboard/scripts/install-local.sh` (the vendored keyboard's installer).
  None of the copies looked like a bug.
- `scripts/tests/test_install_lib.py` (in test-python) installs all 12 units and 12 entries into
  a temp HOME and a prefix, with stub systemctl, ssh and sudo. A capture of the old copies'
  output at f91de1b matched byte for byte. It doesn't run the installers themselves.
- test-bash runs `shellcheck -S warning scripts/install-lib.sh` when ShellCheck is there, and
  prints a skip line otherwise. `setup/packages.txt` has a `dev: ShellCheck` line. The release
  image doesn't get it, so CI's run skips it.
- Review (one Opus reader, 8 findings, none Critical or Important). Fixed in 8714a6c: the
  overrides renamed to `FT_TEST_*` (an exported `FT_` setting could have moved a real
  install's units); the recorders' entry named once for install and uninstall; the test's
  docstring and the AGENTS.md bullet say what it proves; the shellcheck step's comment; no
  trailing comment on the shellcheck directives. Left: voice passes `"$unit 2>/dev/null"` through
  enable_units, which keeps its text the same; two ssh round trips for input-settings and an
  `mkdir` per tour entry, which cost only time.

Open:

- Settled: `setup/dev-container.sh` ran once on 2026-10-31 at 7:45 PM MT. ShellCheck 0.11.0 is
  installed in the dev container, and test-bash's shellcheck step runs and passes.
- `setup/packages.txt` is in fast-install's image set, so a fast-install over 265d025 is refused
  unless it's given `--partial`. The dev line doesn't change the image.
- During this ticket, a commit briefly took in the untracked `hands/rec/demos/`. It was undone
  (`git reset --soft`) before anything else, and the GIFs are untracked again (their blobs sit
  unreferenced in `.git` until a gc).

### From DEE-318

Two shared headers in `screens/`, next to DEE-304's:

- `screens/overlaypanel.h`: the standalone panels' three-buffer DMA-BUF ring (`Ring`: Make,
  Drop, Fill, Bounds, Check; `Texture`), plus the pure `ToCopy`, `Advance` and
  `CopyPremultiplied`. ft-spotpanel, ft-gazepanel and ft-handpanel derive their `Buffers` from
  it and keep their own Present: gaze's `g_vr` around Make, Drop and the SteamVR calls; the hand
  panel's lock around only the two SteamVR calls and its demo-clip area update; texture bounds in
  both; none of that in ft-spotpanel. Each still opens its own device on the render node (separate
  processes). The stderr lines are the same.
- `screens/gpuctx.h`: the EGL and GLES extension lookup (`Procs`, `Load`, `Complete`) and the
  surfaceless GLES 2 context (`Open`, `Close`) for the hand cutouts' Renderer (handrender.cpp) and
  ft-handview's Gpu. The `need` bits keep each one's checks: ft-handview requires
  eglDestroyImageKHR, the Renderer glEGLImageTargetTexture2DOES. ft-handview and ft-handtest's
  test pattern now take their GBM device from `rendernode::Acquire`; build.sh already linked
  rendernode.o into both.
- Small differences, all on failure paths: a context that can't be made current is destroyed
  in Open (the Renderer used to leave it); ft-handview's failed GBM device gets rendernode's line;
  ft-handtest checks its device (it used to pass a null one to gbm_bo_create) and shares the
  Renderer's instead of making a second one.
- Left alone: ft-screens' `overlaytex.cpp` (images kept by content, a cache, not a ring),
  `layout/countdown/ft-reset-timer.cpp` (SetOverlayRaw only), `stream/iris.h` (the stream's own
  EGL), and each program's VR_Init and CreateOverlay (keys, app types and retries differ).
- Tests in test-c: `screens/tests/overlaypanel-test.cpp` (area clamping, the whole-picture
  counter over three buffers, the premultiplied copy and row padding) and
  `screens/tests/gpuctx-test.cpp` (which calls each program needs). All six programs compiled and
  linked in the dev container (OpenVR's symbols left unresolved: its library is SteamVR's).
- Review (one Opus reader, nothing Critical or Important, 3 Minor). Fixed in bae1c59: the
  whole-picture counter moved into the pure `Advance`, with a test. Left: see Open.

Open:

- Possible bug kept (it was ft-handpanel's): Fill marks a buffer as holding the current whole
  picture before mapping it, so after a failed map that buffer's next area update lands on old
  pixels. Fix: set `x.whole` only after a good map.
- Nothing pins which `need` bits each program passes; swapping them would let the Renderer start
  without glEGLImageTargetTexture2DOES.

### From DEE-319

Done (the rest of DEE-303):

- hands/rec and gaze/rec are packages (`hands.rec`, `gaze.rec`), with no `sys.path` inserts left
  in them. Their programs run as `python3 -P -m hands.rec.X` with the tree's top on `PYTHONPATH`:
  the launchers (`hands/rec/ft-handrec`, `gaze/rec/ft-eyerec`), `rehearse.sh`, and the window's
  hub children (`recorder.common.tree_env()`). testgate puts the tree's top on every test's
  `PYTHONPATH`. `-P` keeps the current folder, which may be another checkout, off the path.
- One hub command line: `recorder.hub.cli()`. The two hubs differed only in the description, the
  upload argument (session or round) and its help text, all three now parameters.
  `recorder/tests/test_hub_cli.py` pins it, with stubbed hub functions.
- One control loop: `recorder.controls.Controls` (start, next_step, pause, resume, redo, skip, stop,
  join, state, `_controls`, `_pause`, `_unpause`, and `_Skip`, `_Stop`, `_Redo`). Each session keeps
  its differences as hooks: the thread name, the resume hints, how the recording stops and starts,
  the hand session's feedback timers, the eye session's "Nothing to go back to from here", and the
  hand session's debug-only `redo()`. `button_gesture` stays per session, since the two really
  differ. `recorder/tests/test_controls.py` pins the loop.
- Shared QML in `recorder/qml/`: the Upload page, MarkdownText and DraftBanner. Each window sets
  its differences as properties (`itemNoun`, `recorderName`, the dataset functions, the rehearsal
  text, `reviewText`, `offerFree`, `loginTextWhileWaiting`) and its own `draftText`.
  `recorder/qmlcheck.py` compiles a window's QML offscreen with a temp HOME, and both
  test_qml_backend files check the shared pages against their backend and compile them.
- A test of the hand session's 0700 folders (`test_session_folders_are_private`).
- Review fixes: `one_window` recognises a recorder started with `-m` (the stuck-window takeover
  had stopped working); `-P` on every `-m` and `-c` run; the command lines' usage lines say
  `python3 -m`; `pack/pyside-runtime.py` restores `sys.path`.

Open:

- The Takes, Viewer and Export pages are still one copy per window (diffs of 75, 200 and 300
  lines with small differences). The Upload page shows the pattern for moving them.
- Daenon: the dataset review on dody (and anyone else) runs `validate.py` as
  `PYTHONPATH=<tree> python3 -m hands.rec.validate DIR` (or `gaze.rec.validate`) now. Run by
  path, it stops with "No module named 'recorder'".
- The standalone Hand Recorder (frametop-hand-recorder, not touched here): its copy list is its
  own `package.sh`, so this repo has none. At its next sync it needs `hands/rec/__init__.py` and
  all of `recorder/` (`qml/`, `controls.py`, `privacy.py`, `fhformat.py`, `qmlcheck.py`), a
  launcher that runs `python3 -P -m hands.rec.ft_handrec` with `PYTHONPATH`, and CI that runs the
  tests as modules. Listed in hands/rec/DESIGN.md.
- Kept as it was, maybe an oversight: the eye window shows the login text while the login code
  waits; the hand window hides it. The hand window's "Delete local copy" text says "This round".
- `--help` under `-m` now names the program `python3 -m hands.rec.hub` (Python 3.14's argparse)
  instead of `hub.py`. The window reads only the JSON lines.
- `PYTHONPATH` with the tree now reaches every child of the window and of rehearse.sh. Nothing at
  the tree's top shadows the standard library.
- `gaze/fitcoach.py` still imports a bare `eyes_share`, and `conf/ft_store.py` a bare `ft_conf`,
  so each exists as two module copies (`gaze.tracker.eyes_share` too). Both hold no state.
- Take folders inside `takes/` are made without a mode and rely on the programs' umask 077, as
  before.
- `ft_handrec.VALIDATE_PATH` is unused.
- 41391e5's message has a muddled last sentence: the failing case now asserts exit status 1
  exactly instead of any nonzero status.

## DEE-320 to DEE-324 (the re-audit of f7409a3)

Daenon's run of 2026-10-31, from the read-only re-audit. Local commits on `experimental`.

- DEE-320 file safety: 661f751, ec2c442, 7b49027, c4cc1ee, 8c03ccc. Rules edits in Input Settings
  and the voice settings read and write under one lock (`input_rules.edit_rules`,
  `voice_settings.edit_rules`); game_pause, frunner, fitcoach and fix-panels write through ft_store;
  gaze-report and update-check read frametop.conf with ft_conf.
- DEE-321 test gate: 6c03707, 272745e, 9cd69fa, d752bc8. Ruff (`[tool.ruff.lint]` in
  pyproject.toml) fails `just test`; testgate fails a file that defines tests and runs none, and
  times a file out after 900 s, ending its whole process group.
- DEE-323 recorder pages: 2e0fc2d, ac87c36. Takes, Viewer and Export are shared in `recorder/qml/`.
- DEE-322 splits: 1657c7d, 8678c49, 3474f4b. `ft_vr_command` dispatches to one handler per group
  (`vrcmd::GroupOf`); ft-pointer's tilt and pose lines are in devpose.h.
- DEE-324 cleanup: this folder moved out of `docs/`, `stream/spike/iris.h` and the G7 traces
  untracked, ticket numbers cut from comments.

Open:

- ft-pointer's `main()` is still about 3,270 lines. Splitting it means the Pointer struct design
  of the old `refactor/pointer-main` branch (left out of experimental): Daenon's call whether to
  redo it, port it, or stop at the tested pieces.
- `input/rules-migrate.py` still writes the rules file without the lock.
- A held rules lock can keep Input Settings' window waiting up to 10 s (the GUI thread).
- Releases now lint: `./ft dev test` runs `just test`, which runs ruff first.
- Resolved (Daenon approved): `gaze/tracker/.frame-job` and `screens/handasset.cpp` are untracked
  and ignored; both stay on disk, and the build and tests make handasset.cpp in build/.

## Device checks still to do

Each needs an install and the headset:

- After DEE-319 (no reinstall: the menu entries call the launchers, whose Exec lines didn't
  change): the Hand Recorder and the Eye Recorder open from their menu entries, a round runs end
  to end with pause, resume, redo and stop from the headset button, and both Upload pages show
  their own texts. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-318 (needs a build and install of ft-screens, ft-handtest, ft-handview,
  ft-spotpanel, ft-gazepanel and ft-handpanel): every overlay panel shows and updates without
  flicker (frunner, gaze calibration and bench, the Hand Recorder's panels and a demo clip), the
  hand cutouts and ft-handview draw as before. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-316: a real install, and a reinstall or upgrade between releases. The units and
  menu entries come out the same as before, under the same names, and the same units are
  enabled. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-315: a full build (`screens/build.sh`, and the image's) makes ft-handview with the
  generated `build/handasset.cpp`, and the hands look as before. `get.sh` for real: a new
  `--experimental --clone-only --dir` clone, then a switch to `--stable` in it. Agents never
  run get.sh.
- After DEE-313: `uninstall.sh --dry-run` on the headset lists only Frametop's menu entries
  and names the three added paths; Input Settings' Gaze page and pause gesture after a
  fast-install. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`. Agents never run uninstall.sh.

- Display Settings and Remote Displays show their error message when a file doesn't read.
- Start and apply with a bad layout file: arranging still works and the file is kept.
- Panel and probe calibration saves, then ft-gazed reloads them.
- ft-eyes prunes the real calibration folder down to its 10 newest.
- `ft-cmd add` updates the real `~/.bashrc`.
- A Hand Recorder recording and export.
- Input Settings with a bad `frametop-input.json`.
- Voice settings with a bad `frametop-voice.json`.
- `session/test` on the host.
- The GTK smoke tests (`remote/tests`, `framedrop/tests`) run, rather than skip, in an
  environment with PyGObject.
- The relay after DEE-306: start it paused and unpaused, restart it with a key and a click held,
  reload with a bad rules file. Nothing should differ from before.
- After DEE-307 (needs a build and install of ft-screens, ft-pointer and the ft_pointer driver):
  the pointer before its first move (it follows the head), pointing, show and hide, a hand role
  change, tilting a panel while dragging it, placements (calibration sweep, panel placement) and
  a touch on a SteamVR panel. Panel commands from ft-layout, ft-floatd and the helper (place,
  pin, float, pose, sub, hands, steamvr list), and SteamVR panels by hand.
- After DEE-310 (needs a build and install of ft-screens and Full Keyboard): typing with Full
  Keyboard on the screens with a laser, a controller tip and a fingertip; opening it from a text
  field, the keyboard button and frunner; closing it with its X and with a layout reset; the
  dashboard and Steam-field replacement; with Full Keyboard quit, a text field opens nothing and
  the log says why; a relay restart with Shift held leaves nothing stuck; a non-US layout
  (German or Polish) on the desktop and in Steam; the lazy susan's labels, the reset question
  and the Gamepad pill still have their text. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-304 (needs a build and install of ft-screens, ft-handview, the pointer helper and
  driver, ft-gaze and its panel, the recorder's and frunner's panels, ft-powerd and ft-stream):
  presses, grabs, throws, flicks, pushes, the palm menu and "grab that"; the pointer's pitch
  limit; cutouts and gestures after an ft-hands restart; ft-handview's models; every socket path
  (float and dock, scale, frunner, relay keys, the dashboard pill, gaze calibration, the panels,
  a stream with host input); the new "to @NAME" log lines; no "no render node" fallback. Listed
  in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-303 part 1 (Python only, a fast-install): a Hand Recorder round with the light
  check, the camera check, a take, Review, Export and validate; the new session folder 0700
  and its files 0600; an Eye Recorder round (round, takes and cam folders 0700); a debug export's
  calibration.json validates; ft-camwatch and `python3 hands/camcheck.py`; both upload pages.
  Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-305 (Python only, a fast-install): Display Settings' poll, pins, curve, hide and
  show, power status and Displays off, with the window staying smooth; Remote Displays' stream
  states and portrait 4K; Input Settings' Bluetooth refresh, Apply fixes with the pkexec prompt
  (the window keeps repainting) and Add command; ft-layout, floatd and frunner asks; ft-gazectl,
  gazecheck, gaze-report, ft-gazeprobe and Gaze Bench; restart desktop and the ft-layout runs
  from both apps. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-308: in Input Settings, map a button, add a key combination, add a command, run it,
  and remove it (its bindings go too); change a pointer setting; open it with a bad
  `frametop-input.json` (nothing is saved over it).
- After DEE-309 (needs an image build, which wasn't done): the image builds with
  `setup/packages.txt`, ncnn from `uv.lock`, ncnn's source zip and OpenVR by commit, and
  release.yml's smoke test passes; `setup/dev-container.sh` makes a container from the pinned
  digest; `get.sh --release` with a real release; a fast-install of gaze/tracker on an older
  image (the hashed ncnn fallback); Full Keyboard builds with the new include dir;
  `testbench/install.sh` with hashes; `stream/build.sh --spikes`; ft-streamtest against a host.
  Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-312 (needs a build of ft-eyegrab and a fast-install of voice/): `sudo ft-eyegrab
  --rec`, `--seq` and `--dump` write 0600 files owned by the sudo user, which the lab tools read
  without sudo, and refuse a symlinked path; list mode shows real inodes; with `save_clips` on,
  clips are 0600. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.
- After DEE-298 (Daenon only, against a host he chooses; agents never pair): `ft-layout remote
  add HOST ADDRESS Desktop --fps 60fps` fails at once and the host lists no new client; a
  normal add still pairs, saves and connects. Listed in `~/Desktop/Projects/HEADSET-TESTS.md`.

## Bundle A (branch bundle-a)

Branch `bundle-a` in `~/frametop/.worktrees/bundle-a`, from experimental bae1c59 (2026-10-31).
Local only: nothing pushed, installed or fast-installed. Tickets DEE-205, DEE-192, DEE-204.

Tests: baseline is the cleanup thread's full run at bae1c59 (python files 108 ok, 2 skipped;
751 unittest tests, 7 skips; test-c 38 ok; keyboard ctest 22/22; testbench 249, 20 skipped;
fast-install 28). The final full run at 89937af passed: python files 107 ok, 3 skipped; 752
unittest tests, 16 skips; test-c 34 ok; keyboard ctest 22/22; testbench 249, 20 skipped;
fast-install 28. Every difference comes from the fresh worktree, which has no ncnn/hands or
gaze build, ft-handpanel, gaze panel, or untracked demo clips. In the experimental checkout
those tests run: sides, pinch, flagbuf and mmap-layout in test-c, and hands/rec test_session and
gaze/rec test_panel in Python. The run also printed `shellcheck scripts/install-lib.sh: ok`,
so ShellCheck is in the dev container now.

### From DEE-205

Commits 230ba01, d79b851 (review fix).

- The Meta tap (`steam_menu`, a default) already opened and closed the Steam menu, and the
  Keyboard + 3D pointer role (`combo`) already existed. The gap: a keyboard that sends its
  keys and its mouse-key layer on one input node gets that node as a pointer node, and
  key combinations only ran on pass-through nodes. Now keys (below BTN_MISC) on a combo
  device's mouse node go through `key_binding` first, unless the device's button map has the
  key. A key held in a combination still releases through it. Plain 3D pointers (the Z3) are
  unchanged. Docs: the relay's header, `docs/reference-apps.md` Devices.
- Tests: `input/test/keys-test.py` (tap, Meta+J, pointer mode, a key mapped while held, a
  mapped key, a plain pointer).
- Review (one Opus reader, no Critical or Important). Fixed in d79b851: a key mapped while
  held in a combination stayed in `held.combos` and blocked later taps; more test cases.
  Left: unplugging during a combination leaves `held.combos` set until release_keys. This is
  older than this branch and pass-through keyboards have it too.

### From DEE-192

Commit cc9180d.

- Full Keyboard already loads profiles as data (`keyboard/languages/*.json`, bundled, or
  `~/.config/frametop-keyboard/languages/` without a rebuild). Added `pl-pl` (Polish
  programmer's: US places, letters on AltGr). `keyboard/docs/languages.md` and FRAMETOP.md's
  change list name it. `keyboard/tests/language_tests.cpp` checks all nine AltGr letters and
  their capitals.
- Open: on the desktop, Full Keyboard's text goes through the desktop's layout (kxkbrc), so ą
  types only if the Frametop desktop's layout has it (Polish). On a US desktop the relay drops
  it and logs that. Fixing this needs the relay to type characters the layout lacks (a
  per-character keymap through ft-screens, or a text path in the input method): a design call.
- Follow-up (Daenon chose the input-method path, 2026-10-31). The per-character keymap can't
  work: KWin 6.2.5 doesn't load keymaps from its host. Commits 0508f4b, 7298a42, 0c1e04e (review
  fixes).
  - Typing to the desktop now sends a character the desktop's layout has no key for to
    ft-textinput as "commit TEXT". ft-textinput commits it to the focused text field with
    `commit_string`. Characters the layout has still go as keys, so US English is unchanged.
    With no input-method text field focused, the character is dropped and logged as before.
    The log line now says so.
  - Trust: the relay sends only to the address that said "textfield 1" with the same uid.
    ft-textinput listens on an autobound name and takes commits only from `@frametop_relay`
    with the same uid. ft-textinput's docstring now says it types these characters.
  - Order: a character typed as keys and one committed take different routes to KWin. A 50 ms
    gap (`TYPE_SWITCH`) comes between a switch from one route to the other, and that includes
    Full Keyboard keys after a commit. ft-textinput handles KWin's events before the relay's,
    so a commit never uses a stale serial.
  - Works in apps that take input-method text (Qt, GTK, Firefox). It doesn't work in XWayland
    or Electron apps: those still drop the character.
  - Tests: keys-test (routing, the Steam and no-text-field fallbacks, the sender's name, the
    gap before a Full Keyboard key) and the new `input/test/textinput-test.py` (ft-textinput
    against a fake KWin: commit_string with its serial, other senders ignored, nothing with no
    context).
  - Review (one Opus reader, 1 Important, 5 Minor). Fixed in 0c1e04e: Full Keyboard keys now
    wait the gap after a commit, and a full ft-textinput queue no longer stops later commits.
  - Left open after review: commits go to the context activated last.
  - Renewal (Daenon, 2026-10-31; commits 59fff74, cade24f) closes the review's two other open
    items: a crashed ft-textinput's stale address, and a relay restart losing the address.
    - While a field stays focused, ft-textinput sends "textfield 1 renew" every 2 s.
    - The relay takes a renewal as an address refresh only: it doesn't open Full Keyboard again.
      A renewal from a new address (an ft-textinput KWin started again) replaces the old one.
    - The address lapses 7 s (`TEXT_INPUT_LAPSE`) after the last report, so nothing goes to a
      dead ft-textinput's address. A restarted relay finds the address at the next renewal,
      without a focus change.
    - Tests: keys-test (renewal finds the address after the relay forgot it, without opening
      the keyboard; a new address replaces the old one; other textfield words ignored) and
      textinput-test (renewals while focused, from the same address, none after). The 7 s lapse
      itself has no timed test.
    - Full run at cade24f passed, identical to 0c1e04e (python files 108 ok, 3 skipped; 752
      unittest tests, 16 skips; same test-c lines; keyboard ctest 22/22; testbench 249, 20
      skipped; fast-install 28).
  - Merge check (2026-10-31, read-only): `git merge-tree --write-tree experimental bundle-a`
    against experimental f1942dd merges cleanly. Only this file changed on both sides.
  - Full run at 0c1e04e passed. It matches 89937af except one more Python file ok, the new
    test (108 ok, 3 skipped). test-c printed the same ok lines.

### From DEE-204

Commits 0c4c414, 89937af (review fixes).

- Frametop's own Meta actions could already go on controller buttons. New: the opt-in action
  `keys:CODES` (evdev codes joined by `+`, as in `key_bindings`) for controller buttons, mouse
  buttons (pointer mode) and key combinations. On the press it taps the combination where
  typing goes: the desktop through ft-screens, or the virtual keyboard for Steam. Modifiers
  the desktop holds come up around the tap. It allows up to 6 keys and refuses volume, power,
  sleep, wake and suspend keys. It doesn't work while paused. It's set by hand in
  `frametop-input.json`, for example `"controller_buttons": {"right/b": "keys:125+32"}`
  (Meta+D). Input Settings keeps it and shows it as "Press Meta+D". Docs:
  `docs/reference-apps.md` Controllers.
- Tests: keys-test's `keys:` block (specs, controller button on the desktop, key combination to
  Steam and on the desktop with held modifiers, a refused key); `input-settings/test/
  test_rules_file.py` (save, label, limits, dropdown entry).
- Review (one Opus reader, 2 Important, 4 Minor). Fixed in 89937af: held desktop modifiers
  around the tap; Input Settings' check matches the relay's; dropdowns list mapped `keys:`
  actions (a row was blank); power keys refused; the mouse-button caveat is in the docs. Left: a
  tap can land while dictation or Full Keyboard holds a modifier for a character (rare, and
  nothing stays stuck). With typing on Steam, Steam still sees physically held modifiers.
- Open, for Daenon: a capture UI for `keys:` on the Controllers page, context-sensitive
  shortcuts, and a radial menu (DEE-172) weren't built.

### Device checks for Bundle A

Listed in `~/Desktop/Projects/HEADSET-TESTS.md` under "Bundle A". Each needs a fast-install
of input/, input-settings/ and keyboard/ (a Full Keyboard build).
