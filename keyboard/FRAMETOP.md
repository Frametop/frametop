# Full Keyboard in Frametop

This folder is [Full Keyboard for Steam Frame](https://github.com/TaiKeid/steam-frame-full-keyboard) by TaiKeid and contributors, under the MIT License ([LICENSE](LICENSE), Copyright (c) 2026 TaiKeid and contributors). Its own third-party parts and their licences are in [third_party/README.md](third_party/README.md). Frametop runs it as its keyboard: on the desktop's screens, in the SteamVR dashboard, and in place of Steam's own keyboard.

It was imported at 0.8.1, commit `ef915ef75169ac886b7de86d18df2e59150cde2f`, with `git subtree --squash`; the squash commit carries TaiKeid as its author. Everything here is theirs except the changes listed below.

## Frametop's changes

- `--frametop` (`src/main.cpp`): the VR keyboard, typing through gamescope (libei) as before, started enabled and hidden. Its settings live in `~/.config/frametop-keyboard`. It has no updater and no startup unit: Frametop's service starts it with SteamVR, and it updates with Frametop. The first run turns the Steam keyboard replacement on. Quitting it from its panel exits with status 3, so the service leaves it closed until SteamVR starts again.
- `src/frametop.cpp`, `include/framekeyboard/frametop.hpp`: the link to Frametop's input relay (below), and a key sink that sends typing to the desktop or to Steam.
- `src/vr.cpp`: the panel can show over Frametop's screens with the dashboard closed, while Frametop asked for it; lasers can click it there; it tells Frametop whether it's up.
- `src/app.cpp`: no Updates button and no Start with SteamOS setting in Frametop's mode.
- `src/japanese.cpp`, `tests/japanese_tests.cpp`: Anthy also loads as `libanthy-unicode.so.0`, Fedora's name for it.
- `CMakeLists.txt`: the above, `frametop-tests`, and `OPENVR_RUNTIME_PATH` can be set, so the build loads SteamVR's own `libopenvr_api` first like Frametop's other programs.
- Frametop's own files: `build.sh`, `run.sh`, `frametop-keyboard.service`, and this file.

## How it works with Frametop

Frametop's desktop runs in a nested KWin that gamescope never sees, so keys sent to gamescope can't reach it. The input relay (`input/input-relay.py`) owns typing for the desktop already: it knows where typing goes (the panel clicked last, from ft-screens), and it types Frametop Voice's text there. Full Keyboard talks to it over the relay's abstract datagram socket, `@frametop_relay`:

- Full Keyboard says `kbhello OPEN` every second, and at once when it opens or closes. The relay answers each hello, and any change, with `kb DESKTOP SURFACE`: whether typing goes to the desktop, and whether the screens show with Frametop not paused (then it may show over them with the dashboard closed).
- While typing goes to the desktop, its keys go to the relay as `kbkey CODE 0|1` and composed text as `kbtext TEXT`. The relay types them on the desktop's seat through ft-screens, in order with dictation's typing; the text goes through the desktop's keyboard layout and isn't logged. Otherwise they go to gamescope as before. A key comes up where it went down, whichever way typing goes by then, and when Full Keyboard stops, the relay lets go of the keys it held.
- Opening it: the relay tells ft-screens it runs (`fullkb OPEN`), and ft-screens then passes on its `vrkeyboard` commands (a desktop text field gets focus, a mapped button, spotwatch, a layout reset) as `kbpanel show|hide`, which the relay sends Full Keyboard as `kb show|hide`. Frametop's older panel (`screens/keyboard.cpp`) is used only while Full Keyboard isn't running.
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
