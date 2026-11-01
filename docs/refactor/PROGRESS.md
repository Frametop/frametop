# SOLID refactor progress

Plan: [SOLID-PLAN.md](SOLID-PLAN.md). Branch `solid-refactor` in `~/frametop/.worktrees/solid`, from experimental 3ca4e7e.

| Step | Commit | Tests run | Notes |
|------|--------|-----------|-------|
| 1 plan | 14d2958 | docs/tests/test_doc_links.py | |
| 2 shared input-actions tables | (this commit) | testgate: input/test, input-settings/test, voice/test, tour/test, conf/test (22 files, 125 unittest tests, 0 skips); ruff clean | `conf/ft_input_actions.py` holds VR_BUTTONS, GAZE_ACTIONS, VOICE_ACTIONS, DEFAULT_KEY_BINDINGS, VOICE_KEY_BINDINGS, RENAMED_ACTIONS and renamed_actions(), once. The relay, Input Settings, input_rules and voice_settings import them under the same names. voice/test's source-text sync check is now an identity check. |

## Shared files touched (for the merge with experimental)

- `input-settings/ft_input_settings.py` (step 2): three small hunks, the GAZE_ACTIONS and DEFAULT_KEY_BINDINGS definitions replaced by one import near the top. The cleanup 3 thread works on Input Settings (travel mode); on a conflict, keep theirs and drop the two definitions again.

## For Daenon

None yet.
