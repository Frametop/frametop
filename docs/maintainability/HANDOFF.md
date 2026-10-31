# Maintainability series: handoff

State on 2026-10-31, branch `experimental` in `~/frametop/.worktrees/experimental`. All of this
is local: nothing in the series has been pushed, installed or fast-installed. The next thread
starts at DEE-307.

## Where the series is

| Step | Tickets | Commits |
| --- | --- | --- |
| 1 | DEE-299 | b1bd58c |
| 2 | DEE-300 (with the DEE-295 and DEE-296 fixes) | 1bb43cb, e2b5609, acc4d05, c56aab7, 16dc51b, 364dba1, 7c3abb1, 2bffb10 |
| 3 | DEE-302, plus DEE-300 follow-ups | 655fa8c, 4c16b1e, c48201a, 63089ef, dd8f7ba, 80954b5 |
| 3b | DEE-311 | 865b852, b69b905, 2971c8d, af02582 (this note) |
| 4 | DEE-306 | 3bb0ebf, 688de89, d464ac0, 35d9da4, 576b5d6, then this update |

What's left, in order:

1. DEE-307 and DEE-308
2. DEE-304, DEE-305 and DEE-303
3. DEE-309, DEE-312, DEE-298, then DEE-313 to DEE-316

DEE-310 is skipped. DEE-301's CI and lint work stays local only: no workflow change is pushed.

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
  should go and list it below.
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
- `ft_vr_command` and the pose math (`PanelPose`, `Mul`, `Inverse`) in `screens/vr.cpp` sit in
  an anonymous namespace and use OpenVR types and globals. Testing them needs them moved into
  a header, the way `levelpin.h` and `universefix.h` were. That isn't a tests-only change.
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
