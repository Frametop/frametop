# SOLID refactor plan

Branch `solid-refactor`, from experimental 3ca4e7e (2026-10-31). The goal is to move Frametop toward the SOLID principles with no behavior change. CLI flags, socket and IPC message formats, config file formats, unit names and the install layout stay as they are. No new dependencies.

Progress, test runs and items for Daenon are in [PROGRESS.md](PROGRESS.md).

## How the hotspots were picked

Line counts (`wc -l` over tracked sources), duplicated top-level names across Python files, and a light read of the largest classes. Ranked by value over risk: a step ranks high when it removes duplication or isolates pure logic behind a seam that tests can reach, and when tests already cover the area, or a characterization test can cover it first.

## Ranked hotspots

| # | Where | Size | Problem | Seam |
|---|-------|------|---------|------|
| 1 | Input rules tables: `input/input-relay.py`, `input-settings/ft_input_settings.py`, `input-settings/input_rules.py`, `voice/voice_settings.py` | 3-4 copies | `DEFAULT_KEY_BINDINGS`, `VOICE_KEY_BINDINGS`, `VR_BUTTONS`, `GAZE_ACTIONS`, `RENAMED_ACTIONS` and `renamed_actions()` are copied between the relay and its two settings apps. A test (`voice/test/test_commands.py`) compares the source text to keep them in step. | One shared module, `conf/ft_input_actions.py`: the rules file's vocabulary, one source of truth, imported by all four. |
| 2 | `Relay.control_command` in `input/input-relay.py` | 180 lines, about 30 commands | One if-chain for the whole control socket protocol. A new command means editing the chain (open/closed). No test covers the dispatch. | Characterization test of the protocol first, then a table of one small handler per command, in the same order. The peer-credentials parse in `handle_control` becomes a pure function. |
| 3 | `Relay` in `input/input-relay.py` | 1200 lines | Device scanning, key bindings, typing, Full Keyboard link, host devices, the control socket, the pause, all in one class. | After step 2, move the evdev/uinput ioctl layer (pure constants and ioctl builders) to a new `ft_evdev.py` in `input/`. Further splits (typing, Full Keyboard link) only where tests already reach them. |
| 4 | `layout/ft_layout.py` | 2171 lines | Config, pure geometry, the ft-screens socket, gamescope's vrcmd, remote displays, named layouts, profiles and the CLI in one module. | Move the pure geometry section (`direction`, `yaw_pitch`, vector math, `plan`, `relative_pose`, preview math) to a new `layout_geometry.py` in `layout/`, re-exported by `ft_layout` so importers and tests don't change. |
| 5 | SteamVR settings access in `gaze/eye-rate.py` and `pointer/dashpatch/ft-dashpatch.py` | 2 copies | Same `vrcmd`, `settings_file`, `steamvr_up` pattern copied. | A small shared helper with an injectable command runner, if both read the same; otherwise only noted. |
| 6 | `float/ft_floatd.py` `Daemon` | 1000 lines | KWin/kscreen I/O, D-Bus, placement math and slot bookkeeping in one class. | Pure placement and slot logic out of the class behind the existing tests (`float/test/`). Later step; scope set after 1-4. |
| 7 | `input-settings/ft_input_settings.py` `Backend` | 1600 lines | A Qt god object: rules editing, conf, host commands, gaze, hands, voice pages. | The QML-facing API stays; pure parts move out (as `input_rules.py` already did). Last, because the cleanup 3 thread works on Input Settings (travel mode). |

### Noted only (another thread is working there tonight)

- `screens/vr.cpp` (6339 lines): `ft_vr_command` was just split per group on experimental (DEE-322), and pin-to-head pose code is being changed (DEE-208). Seam for later: the overlay and pose math into headers with tests, as `posemath`, `levelpin` and `universefix` already are.
- `pointer/helper/ft-pointer.cpp` (4248 lines): `main()` and `sendPose` are being split (cleanup 3). Seam for later: the main loop's per-source handlers behind small interfaces, after that split lands.
- `recorder/` QML pages, `install.sh` and the pack install docs, the `.gitignore` and docs cleanup: left alone.
- Duplicated `in_container()` (`conf/fthost.py`, `layout/ft_layout.py`, `recorder/common.py`): ft_layout's copy doesn't check `/.dockerenv`, so merging it is a (tiny) behavior change; `recorder/` ships without `conf/` on purpose. Left as is, noted.
- `input-settings` `host()` and `conf/fthost.host_command()` build different argv (no `env -C ~`). Merging changes behavior; noted.

## Sequence

Each step is one local commit, with the touched area's tests passing first. A characterization test comes first where the logic isn't covered.

1. This plan and PROGRESS.md.
2. `conf/ft_input_actions.py`: the shared tables; relay, input settings, input_rules and voice_settings import them. Replace the source-text sync test with an identity check.
3. Characterization test for the relay's control socket (`input/test/control-test.py`).
4. `control_command` as a handler table; `peer_creds()` pure.
5. Full `just test` checkpoint.
6. a new `ft_evdev.py` in `input/`: the ioctl layer.
7. a new `layout_geometry.py` in `layout/`: ft_layout's pure geometry.
8. Full `just test` checkpoint, one Opus review of steps 2-7.
9. Steps 5-7 of the hotspot table, as time allows, each with its own checkpoint.
10. Final full `just test` on the last code commit.

## Rules for each step

- No change to a socket message, CLI flag, conf key, unit or installed path. Moved names stay importable from the old module (re-exports), because tests and other components reach them there.
- A module-level name that tests replace (`relay.SCREENS`, `relay.log`, `relay.read_rules`, ...) stays where it is: moving it would cut the test's patch from the code that reads it.
- Targeted tests before each commit, ruff clean (`just lint` rules).
