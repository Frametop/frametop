# Splitting the large Python classes (DEE-308)

Five Python classes hold most of their component's logic and can only be tested end to end.
The plan is incremental: each time one of these files is changed for another reason, at least one
of the pieces below moves into its own module, with unit tests and no behaviour change. A
moved piece is plain Python where it can be (no Qt, GTK or sockets), and the class keeps a thin
method that calls it, so QML, D-Bus and socket callers don't change.

Sizes are from 2026-10-31 (lines in the class).

## Input Settings `Backend` (`input-settings/ft_input_settings.py`, 1,603 lines)

| Piece | Methods today | Module |
| --- | --- | --- |
| Conf I/O: the rules file and frametop.conf | `read_json`, `read_conf`, `write_conf_value`, `_save_rules` | `input_rules.py` (done in DEE-308) |
| Command management | `commands`, `commandNameProblem`, `addCommand`, `removeCommand`, `runCommand` | `input_commands.py` (done in DEE-308) |
| Relay socket and status polling | `_send`, `_refresh`, `_read`, `_check_driver`, `driver_block` | next: a reader that turns datagrams into status updates |
| Devices and button maps | `devices`, `roles`, `setRole`, `mappings`, `setMapping`, capture | |
| Key combinations | `comboName`, `keyShortcuts`, shortcut capture, `_save_shortcut` | (`tour/ft_tour.py` already shares `key_bindings`) |
| Controllers and the pause | `controller*`, `pause*`, `gesture*` | |
| Gaze | `gaze*`, `_gaze_status`, `_open_probe`, `_gaze_check` | |
| Voice | `voice*`, `dictation*`, `_fetch_model` | next to `voice/voice_settings.py` |
| Hands and models | `hands*`, `setHand*`, `_model_view`, `_model_job`, `useModel` | |
| SteamVR panels (ignore list) | `panelGroups`, `setPanelIgnored`, `_save_ignore` | |
| Bluetooth | `bluetooth`, `refreshBluetooth`, `applyBluetoothFixes` | |

## Gaze probe `Probe` (`gaze/probe/ft-gazeprobe`, 2,403 lines)

- Window and panel layout: `build_ui`, `build_menu`, `build_toolbar`, `build_panel`, `place`,
  fullscreen.
- Input and sample flow: `on_sample`, `corrected`, `correction`, `refit_live`, `filtered`.
- Calibration: `calib_points`, `start_calibration` to `finish_calibration`,
  `save_calibration`, `load_calibration`, `refine_calibration`. The fit and file format can
  be pure functions.
- Accuracy test and results: `test_points`, `start_test` to `finish_test`,
  `summarize_run`, `update_stats`.
- Snapping: `snap_*`, `steady_for`, `press_fixation`.
- Practice and capture modes: `practice_*`, `start_capture` to `finish_capture`.
- Drawing: `draw*`, `degree_grid`, `bullseye`, `round_rect`, `text`.

## Gaze service `Service` (`gaze/ft-gazed`, 871 lines)

- Settings, calibration and lessons files: `load_settings`, `load_fusion`,
  `load_calibration`, `load_lessons`, `save_lessons`, `forget_lessons` (onto `ft_store`).
- Sources: `wanted_sources`, `sync_sources`, the helper and eyes processes (`start_*`,
  `stop_*`, `read_*`, `judge_eyes`).
- Awake and idle rules: `waking`, `update_awake`, `use_reason`, `idle_reason`.
- Corrections: `refit`, `correction`, `look`, `lesson`, `taken`.
- Control socket: `on_control`, `status`, `send`.

## Hand recorder `Session` (`hands/rec/session.py`, 1,678 lines)

- Run state machine: `start`, `next_step`, `redo`, `pause`, `resume`, `skip`, `stop`,
  `state`, `_run`.
- Files written: `_make_dir`, `_write_calibration`, `_write_device`, `_save_session`.
- Units and devices: `_start_unit`, `_stop_units`, `_ensure_ring`, `_start_tracker`,
  `_start_panel`, `_quiet_hands`.
- Steps and script: `_steps`, `_step_list`, `_run_step`, `_sweep`, `_countdown`.
- Feedback and prompts: `_feedback`, `_controller_feedback`, `_hints`, `_prompt_event`,
  `_begin_prompt`, `_run_prompt`.
- Checks on the take: `_cameras_back`, `_no_hands_seen`, `_ready`, `_tip_distance`,
  `_palm_share`.

## Voice daemon `Voice` (`voice/ft-voiced`, 1,058 lines)

- Settings and named commands: `load_settings`, `apply_options`, `load_named_commands`.
- Audio front end: `on_audio`, `_make_vad`, `trim_to_speech`, `_window_audio`,
  `_quiet_cut`, `update_mic`, `mic_needed`.
- Wake words and command listening: `_make_kws`, `reset_kws`, `on_wake`,
  `listen_for_command`, `_command_audio`.
- Dictation and decoding: `start_dictation`, `cancel_dictation`, `_moonshine_*`,
  `_commit`, `_context`, `_uncommitted`, `live_moonshine`, `update_whisper`.
- Inputs: `push_to_talk`, `listen_relay`, `controller_loop`, `_controller_session`.
- Output and chimes: `_send_text`, `_post`, `chime*`, `broadcast`, `run_hook`.
