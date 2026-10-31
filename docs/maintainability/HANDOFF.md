# Maintainability series: handoff

State on 2026-10-31, branch `experimental` in `~/frametop/.worktrees/experimental`. All of this
is local: nothing in the series has been pushed, installed or fast-installed. The next thread
starts at DEE-305 (DEE-310 was done out of order, after DEE-308).

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
| 8 | DEE-304 | cb6d442, 60cc62e, c55e509, ceced31, 6ac87b6, a8a6a7e, 77b59d2, 9da7cd3, 8c72d59, then this update |

What's left, in order:

1. DEE-305 and DEE-303
2. DEE-309, DEE-312, DEE-298, then DEE-313 to DEE-316

DEE-310 and DEE-304 are done (steps 7 and 8). DEE-301's CI and lint work stays local only: no workflow change is pushed.

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
- fitcoach and game_pause still write through fixed `.tmp` names.
- In Input Settings and the voice settings, the lock covers the write, not the read before it.
  Moving all 17 callers to `update_json` needs restructuring.

### From DEE-302

- `session/test` (needs the host's AT-SPI, dbus-daemon and PyGObject, skipped in the container)
  and `session/tests` (pytest) are two folders for one component.
- A test file with TestCase classes but no `unittest.main()` runs no tests and still passes.
- No test has a timeout. A hung test hangs the gate.
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
- ft-pointer's `sendPose` has no offline test: `vrmath.h` includes OpenVR. Testing it needs an
  OpenVR-free split of vrmath.h.

### From DEE-308

`docs/maintainability/python-split-plan.md` names the cohesive pieces of the five large Python
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

## Device checks still to do

Each needs an install and the headset:

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
- After DEE-308: in Input Settings, map a button, add a key combination, add a command, run it,
  and remove it (its bindings go too); change a pointer setting; open it with a bad
  `frametop-input.json` (nothing is saved over it).
