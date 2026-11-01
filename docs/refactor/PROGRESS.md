# SOLID refactor progress

Plan: [SOLID-PLAN.md](SOLID-PLAN.md). Branch `solid-refactor` in `~/frametop/.worktrees/solid`, from experimental 3ca4e7e.

| Step | Commit | Tests run | Notes |
|------|--------|-----------|-------|
| 1 plan | 14d2958 | docs/tests/test_doc_links.py | |
| 2 shared input-actions tables | ab44c7c | testgate: input/test, input-settings/test, voice/test, tour/test, conf/test (22 files, 125 unittest tests, 0 skips); ruff clean | `conf/ft_input_actions.py` holds VR_BUTTONS, GAZE_ACTIONS, VOICE_ACTIONS, DEFAULT_KEY_BINDINGS, VOICE_KEY_BINDINGS, RENAMED_ACTIONS and renamed_actions(), once. The relay, Input Settings, input_rules and voice_settings import them under the same names. voice/test's source-text sync check is now an identity check. |
| 3 relay control protocol, characterized | (this commit) | input/test/control-test.py (104 cases); a mutation (calpanel's 3.0 s as 4.0) fails it | `input/test/control-test.py` runs every control socket command and its main branches (uid, bound or not, Full Keyboard's address, paused, no pointer) on a Relay with recorder parts, against `control-golden.json`, recorded from the code before step 4. |

## Shared files touched (for the merge with experimental)

- `input-settings/ft_input_settings.py` (step 2): three small hunks, the GAZE_ACTIONS and DEFAULT_KEY_BINDINGS definitions replaced by one import near the top. The cleanup 3 thread works on Input Settings (travel mode); on a conflict, keep theirs and drop the two definitions again.

## For Daenon

None yet.
