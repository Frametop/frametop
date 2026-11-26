# Full Keyboard in Frametop

This folder is [Full Keyboard for Steam Frame](https://github.com/TaiKeid/steam-frame-full-keyboard) by TaiKeid and contributors, under the MIT License ([LICENSE](LICENSE), Copyright (c) 2026 TaiKeid and contributors). Its own third-party parts and their licences are in [third_party/README.md](third_party/README.md). Frametop runs it as its keyboard: on the desktop's screens, in the SteamVR dashboard, and in place of Steam's own keyboard.

It was imported at 0.8.1, commit `ef915ef75169ac886b7de86d18df2e59150cde2f`, with `git subtree --squash`; the squash commit carries TaiKeid as its author. Everything here is theirs except the changes listed below.

## Frametop's changes

- `src/vr.cpp`: the opt-in Steam PIN access gate hides and cancels Full Keyboard while locked or state is uncertain, and leaves native replacement disabled so Steam's PIN UI remains usable. Standalone builds without Frametop's shared header retain upstream behavior.
- `--frametop` (`src/main.cpp`): the VR keyboard, typing through gamescope (libei) as before, started enabled and hidden. Its settings live in `~/.config/frametop-keyboard`. It has no updater and no startup unit: Frametop's service starts it with SteamVR, and it updates with Frametop. The first run turns the Steam keyboard replacement on. Quitting it from its panel exits with status 3, so the service leaves it closed until SteamVR starts again.
- `src/frametop.cpp`, `include/framekeyboard/frametop.hpp`: the link to Frametop's input relay (below), and a key sink that sends typing to the desktop or to Steam.
- `src/frametop_vr.cpp`, `include/framekeyboard/frametop_vr.hpp` (`FrametopVr`): Frametop's part of the VR loop. The panel can show over Frametop's screens with the dashboard closed, while Frametop asked for it; lasers can click it there; it tells Frametop whether it's up; touch typing's tips and fingertips. It's OpenVR-free and tested in `tests/frametop_tests.cpp`.
- `src/vr.cpp`: only hook calls into `FrametopVr` in `run_vr` (connect, placement, the visibility gate, the request at each frame's start, touch and fingertips, the between-frame tip reads), and `frametop_vr_access()`, which reads what the hooks need through vr.cpp's runtime-watched wrappers. Keep those blocks as they are when merging upstream; the rest of the file is theirs.
- `src/placement.cpp`, `include/framekeyboard/placement.hpp`: in Frametop's mode it opens where Frametop's old keyboard did, 0.70 m ahead and 0.35 m under the eyes, facing them, instead of 0.85 m ahead and 0.65 m under, so a hand's laser reaches its bottom row without lowering the hand past where Frametop puts the laser away. Under the dashboard it sits as before.
- `src/app.cpp`: no Updates button and no Start with SteamOS setting in Frametop's mode.
- `src/touch.cpp`, `include/framekeyboard/touch.hpp`, `src/frametop_vr.cpp`, `src/app.cpp`, `src/config.cpp`, `tests/touch_tests.cpp`: touch typing. A Frame controller's tip presses the key it touches, in both modes, on by default (General -> Touch typing, `touch_typing`; [configuration](docs/configuration.md)). A hand's fingertip presses keys too, with no laser: ft-screens' hand input decides its hovers and presses as on Frametop's screens, and sends them through the relay (below; `src/frametop.cpp`'s `HandPoke`, `tests/frametop_tests.cpp`).
- `languages/pl-pl.json`, `docs/languages.md`, `tests/language_tests.cpp`: a Polish profile (Polish programmer's layout: US key places, ą ć ę ł ń ó ś ź ż on AltGr). Another layout is one JSON file like it: in `languages/` here, or in `~/.config/frametop-keyboard/languages/` without a rebuild ([configuration](docs/configuration.md)).
- `src/japanese.cpp`, `tests/japanese_tests.cpp`: Anthy also loads as `libanthy-unicode.so.0`, Fedora's name for it.
- `CMakeLists.txt`: the above (`src/frametop.cpp`, `src/frametop_vr.cpp` and `src/touch.cpp` in `keyboard_core`), `frametop-tests`, and `OPENVR_RUNTIME_PATH` can be set, so the build loads SteamVR's own `libopenvr_api` first like Frametop's other programs.
- Frametop's own files: `build.sh`, `run.sh`, `frametop-keyboard.service`, and this file.

## Names

Its programs keep upstream's names, `framekeyboard` and `framekeyboard-native-guard`, instead of Frametop's `ft-` names (`ft-keyboard`, `ft-kbguard`). This is a waiver from the root AGENTS.md. The names are upstream's in `CMakeLists.txt`, in `bridge/native_bridge.py`, which finds the guard by name, in `src/startup.cpp`, in its packaging scripts, and in their tests, so renaming them would add Frametop changes to several upstream files that every pull would have to merge. Frametop's own files use them too: `frametop-keyboard.service` (`pkill -x framekeyboard`), `uninstall.sh`, `scripts/fast-install` and `pack/Containerfile`, and installed units would have to move along with them. `framekeyboard` fits in 15 characters for `pkill -x`. Nothing finds the guard by process name, so its longer name is truncated harmlessly. Rename them only together with upstream, or with an install step that moves the installed unit.

## Upstream parts Frametop doesn't use

These stay in the tree, unchanged, so subtree pulls stay clean. In `--frametop` mode they're never reached, or never shown:

- The updater: `src/updates.cpp`, `include/framekeyboard/updates.hpp`, `update/update_helper.py`, `docs/updates.md`, and the `updates` and `update-helper` tests. `src/app.cpp` hides its button, and `src/vr.cpp` still includes its header.
- Start with SteamOS: `src/startup.cpp` and the `startup-unit` test. Frametop's service starts it, and `src/app.cpp` hides the setting.
- Packaging and release: `scripts/package.sh`, `scripts/install-local.sh`, `tools/check_release_binary.py`, `cmake/steam-frame-aarch64.cmake` (Frametop builds natively in its container), `docs/releasing.md` and the `installer` test.
- Typing through uinput (`--input uinput`, `UInputSink`): Frametop types through libei and its relay.
- The desktop preview (`src/preview.cpp`, `scripts/preview.sh`): a development window, not the VR keyboard.
- `design/index.html`: upstream's approved look, for their own reviews.

Their tests still run in `just test-keyboard`, so a pull that breaks them shows up.

Settings follow upstream's pattern: each one has a `pending_*` copy in `src/app.cpp` that Settings edits, applied in several places. `touch_typing` does the same, so a new Frametop setting needs about five edits there too.

## How it works with Frametop

Frametop's desktop runs in a nested KWin that gamescope never sees, so keys sent to gamescope can't reach it. The input relay (`input/input-relay.py`) owns typing for the desktop already: it knows where typing goes (the panel clicked last, from ft-screens), and it types Frametop Voice's text there. Full Keyboard talks to it over the relay's abstract datagram socket, `@frametop_relay`:

- Full Keyboard says `kbhello OPEN` every second, and at once when it opens or closes. The relay answers each hello, and any change, with `kb DESKTOP SURFACE`: whether typing goes to the desktop, and whether the screens show with Frametop not paused (then it may show over them with the dashboard closed).
- While typing goes to the desktop, its keys go to the relay as `kbkey CODE 0|1` and composed text as `kbtext TEXT`. The relay types them on the desktop's seat through ft-screens, in order with dictation's typing; the text goes through the desktop's keyboard layout and isn't logged. Otherwise they go to gamescope as before. A key comes up where it went down, whichever way typing goes by then, and when Full Keyboard stops, the relay lets go of the keys it held. A release the relay doesn't take (its socket full) is sent again, before the next key and each frame, until it goes; a press that doesn't go isn't sent again, since a late press would type. A relay that restarts has let go of everything and takes keys only after the next `kbhello`, so a release owed then is dropped there, harmlessly. The test's stand-in relay has no hello gate.
- Opening it: the relay tells ft-screens it runs (`fullkb OPEN`), and ft-screens then passes on its `vrkeyboard` commands (a desktop text field gets focus, a mapped button, frunner, a layout reset) as `kbpanel show|hide`, which the relay sends Full Keyboard as `kb show|hide`. It's the only on-screen keyboard: while it isn't running, ft-screens opens nothing (its older panel, `screens/keyboard.cpp`, was removed in the 2026-10-31 cleanup; [docs/design.md](../docs/design.md)).
- A fingertip on its panel: ft-screens sends the relay `kbpoke SIDE VERB [x y z]` (hover, press, move, up, end; the fingertip's point in the standing universe), only taken from this user, and the relay sends Full Keyboard `kb poke ...`. Full Keyboard puts the point on its panel as it shows then and presses keys with it like a laser's cursor, but without a laser and without haptics. A press it hears nothing more of for 3 seconds comes up. ft-screens shows the hand's dot on it as on its own screens.
- A click on its panel (`org.framekeyboard.*` overlays) doesn't take typing away from the desktop.
- Its Steam keyboard replacement (`bridge/`) is theirs, unchanged.

Only same-user messages count on either side. Full Keyboard takes word only from the relay's own name, and sends desktop typing only while that word is under 3 seconds old, so a stopped relay never leaves typing addressed to a name another process could take. The relay takes keys and text only from the address that said hello.

Known limits: on the desktop, keys go by their place on the keyboard, so pick the same layout in Full Keyboard as the desktop's. Text the desktop's layout has no key for (Chinese, Japanese, Korean, emoji) can't be typed on the desktop yet; it reaches Steam's fields as before.

## Building and running

```
keyboard/build.sh                          # build/frametop/framekeyboard, in the dev container
FULL_KEYBOARD_TESTS=1 keyboard/build.sh    # with its tests (ctest)
keyboard/run.sh install | uninstall        # frametop-keyboard.service, starts with SteamVR
keyboard/run.sh status | restart | log [lines]
input/test/keys-test.py                    # the relay's side, offline
```

## Updating from upstream

```
git subtree pull --prefix keyboard https://github.com/TaiKeid/steam-frame-full-keyboard <tag> --squash
```

Then build with its tests, run `input/test/keys-test.py`, and update the version and commit above.
