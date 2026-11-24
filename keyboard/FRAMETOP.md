# Full Keyboard in Frametop

Frametop's keyboard is [Full Keyboard for Steam Frame](https://github.com/TaiKeid/steam-frame-full-keyboard) by TaiKeid and contributors, under the MIT License (Copyright (c) 2026 TaiKeid and contributors; the release carries its `LICENSE` and its third-party parts' licences). Frametop runs TaiKeid's own release package, unmodified, as a hosted keyboard: Frametop is its shell, through the keyboard's hosted protocol (protocol 1, [its integration guide](https://github.com/TaiKeid/steam-frame-full-keyboard/blob/main/docs/integration.md)). It shows on the desktop's screens, under the SteamVR dashboard, and in place of Steam's own keyboard in Steam's text fields.

Until 2026-11-20 this folder held a patched copy of the source (0.8.1, `git subtree`). Now nothing of theirs is in the repository: every build downloads the newest release.

## The release

`keyboard/build.sh` (`keyboard/fetch.py`, standard library only) puts the newest stable release in `keyboard/build/release`, as Daenon asked (2026-11-20): "on future builds, we actually just install the latest version of the keyboard rather than pinning a specific version". It's the one input the root AGENTS.md lets go unpinned. The fetch still:

- takes only TaiKeid's repository, over HTTPS, and only a published, stable release whose tag is a plain version no older than 0.19.1, the first release Frametop's host was tested with;
- checks the package against the release's `SHA256SUMS` and GitHub's own digest of it, and unpacks it with tarfile's `data` filter, every entry under the package's folder;
- runs its program where it can (on the Frame, with SteamVR's `libopenvr_api`) to check it says that version and speaks hosted protocol 1;
- writes what went in to `keyboard/build/release/RELEASE` (version, sha256, where from); ft-kbhost logs it at each start.

`FULLKB_VERSION=x.y.z keyboard/build.sh` takes that exact release instead, for going back to a known-good one while a new release has a problem. `ft dev build` passes it to the image as a build argument (`pack/Containerfile`). `GITHUB_TOKEN`, when set, goes to api.github.com only, for GitHub's rate limit of 60 unauthenticated requests an hour; `ft dev build` passes it as a build secret, which no image layer keeps. CI doesn't set it.

A protocol 1 change upstream (it's marked experimental) could break the host. The tag floor and the program check catch a release that drops protocol 1; a change within it would show in ft-kbhost's log, and `FULLKB_VERSION` goes back meanwhile.

## The host (`keyboard/host/`)

`frametop-keyboard.service` runs `keyboard/host/ft-kbhost` with SteamVR, on the host. It starts the keyboard as `framekeyboard --vr --hosted FD --input ei --start-enabled`, with its settings in `~/.config/frametop-keyboard`, and does what Frametop's patches did before:

- `hosted.py`: the socket pair, the `ready` / `hello` / `subscribe` handshake, the lease (a ping every 2/3 s), one JSON line at a time. A request the keyboard refuses is logged with its name. The keyboard asks a standalone copy of itself to quit when it starts; one too old to understand that (exit 11) is stopped by ft-kbhost as the guide says (its `framekeyboard-autostart.service`, or SIGTERM), and the service starts again.
- `host.py`: what shows, where, and where typing goes. Typing goes, newest reason first: into a Steam field that opened it (the text sink, then `steamhook.js`); to Frametop's desktop while the relay says typing goes there (the sink, then the relay as `kbtext` / `kbkey`, `keys.py`); otherwise to gamescope's focus through the keyboard's own libei output, with the sink off. The panel's "Stop shell typing" sends typing to gamescope until the route changes.
- `placement.py`: where it opens. Over the screens it opens 0.70 m ahead and 0.45 m under the eyes, facing them, tilted 26.6°, so a hand's laser reaches its bottom row without lowering the hand past where Frametop puts the laser away. In the dashboard it hangs 14 cm under the bottom edge of Frametop's tile of Steam's page (`frametop.steamdash`) or Steam's own main overlay. ft-screens says which (`screens/vrkbspot.cpp`).
- `touch.py`: fingertips, as forwarded pointers. ft-screens sends `kbpoke SIDE VERB [x y z]` (hover, press, move, up, end; the fingertip's point in the standing universe), the relay passes it on as `kb poke ...`, and this puts it on the panel as the keyboard last reported its pose. Forwarding turns the keyboard's own lasers off, so it's on only from a fingertip's first poke until 0.6 s after the last; a press heard nothing more of for 3 s is cancelled.
- Steam's PIN lock (`conf/ft_pinlock.py`): while it holds, or its state is uncertain, the keyboard hides and opens for nothing, and Steam's fields keep Steam's own keyboard, so its PIN screen stays usable.
- Quitting it from its panel ends the keyboard; ft-kbhost then exits with 3, and the service leaves it closed until SteamVR starts again or someone asks for it: the Open/close keyboard action (a mapped button, the palm's Quick Settings, the button on Settings' Keyboard page) starts the unit again and opens the keyboard once it says hello (`input/kb_revive.py`). A text field or one of Steam's fields doesn't bring a quit keyboard back. A release without hosted protocol 1 exits with 4 and also stays closed, with a log line naming `FULLKB_VERSION`. A keyboard ft-kbhost closed itself (it stopped reading, or sent a line over 1 MiB) exits with 1 and is restarted, at most 5 times in 2 minutes.

### Steam's text fields (`steamhook.js`, `steamhook.py`)

Hosted mode has no native Steam replacement, so Frametop has its own, much reduced from Full Keyboard's own bridge (`share/framekeyboard/bridge/steam_bridge.js` in the release). `steamhook.py` keeps one connection to Steam's SharedJSContext (`steam/steamui.py`) and installs `steamhook.js` there. Each of Steam's own windows (`valve.steam.gamepadui.main`, `.barpopup`, `.appoverlay.*`) shows Steam's keyboard through its `VirtualKeyboardManager.SetVirtualKeyboardShownInternal`; while the hook's lease is renewed and the relay lets Steam's fields take Full Keyboard, a show for an editable field (text, search, password, tel, url, a textarea or contentEditable) opens Full Keyboard instead. Its text goes in with `execCommand("insertText")`, and its editing keys (Backspace, Delete, arrows, Home, End, Enter, Tab, Escape, Ctrl+A/C/X/V) as typing would. The field's text is never read. Without renewal for 1.5 s the hook takes itself out, and Steam's own keyboard works as before.

The relay decides whether Steam's fields take it: Frametop Settings' Keyboard setting (`always`, or `no_keyboard` while no physical keyboard is connected; a host's keyboard that host input streams in counts as connected). Its "In Steam's text fields" setting can keep Steam's own keyboard there instead (`steam_fields_keyboard: steam`), which a Steam Controller or gamepad can type on, while the desktop keeps Full Keyboard (issue 102). Controller navigation of Full Keyboard itself is upstream's. A field asks through the relay (`kbsteam`), which answers `kb nosteam`, has ft-screens open it in place (`vrkeyboard show steam`), or, while ft-screens isn't running, answers `kb show` so it opens where it is. The relay reads the setting when it starts and on `reload`, which Frametop Settings sends after a change; an edit by hand needs `reload` too.

## Names

The release's programs keep upstream's names, `framekeyboard` and `framekeyboard-native-guard`, instead of Frametop's `ft-` names. This is a waiver from the root AGENTS.md: they're TaiKeid's, unmodified. `framekeyboard` fits in 15 characters for `pgrep -x`. Frametop's own program is `ft-kbhost`.

## How it works with the relay

Frametop's desktop runs in a nested KWin that gamescope never sees, so keys sent to gamescope can't reach it. The input relay (`input/input-relay.py`) owns typing for the desktop already: it knows where typing goes (the panel clicked last, from ft-screens), and it types Frametop Voice's text there. ft-kbhost talks to it over the relay's abstract datagram socket, `@frametop_relay` (`relaylink.py`):

- ft-kbhost says `kbhello OPEN` every second, and at once when the panel opens or closes. The relay answers each hello, and any change, with `kb DESKTOP SURFACE STEAM`: whether typing goes through the relay (to the Frametop desktop, or to Native Desktop while it has gamescope's focus); whether the screens show with Frametop not paused (the panel it opened over them hides when they go); whether Steam's fields may take it.
- While typing goes to the desktop, keys go to the relay as `kbkey CODE 0|1` and committed text as `kbtext TEXT`. The relay types them on the desktop's seat through ft-screens, in order with dictation's typing; the text goes through the desktop's keyboard layout and isn't logged. A release the relay doesn't take (its socket full) is sent again until it goes; a press that doesn't go isn't, since a late press would type.
- Native Desktop (SteamOS's own desktop, which runs in gamescope) reads keys with its own layout, not gamescope's, so text gamescope types for a US layout comes out wrong there: "a" as "q" on AZERTY, and an accent as the key gamescope borrows for it ("1"). While Native Desktop has gamescope's focus, the relay says DESKTOP 1 and types its text as Native Desktop's own keys (`~/.config/kxkbrc`), dead-key accents included, on its virtual keyboard.
- Shortcuts: the sink names a Ctrl, Alt or Super shortcut by its letter (the key that shows "a" comes as `KeyA` on every language, AZERTY's too; Cyrillic and IME languages send the Latin letter of the key's place). The relay presses the key that types that letter on the layout typing goes through, the Frametop desktop's or Native Desktop's (`input/kb_shortcuts.py`), so Ctrl+A selects all and Ctrl+Z undoes on an AZERTY or QWERTZ desktop whatever Full Keyboard's language is. A letter that layout has no plain key for (a Russian or Greek desktop) keeps its place, where the desktop's Latin fallback finds it.
- Opening it: the relay tells ft-screens it runs (`fullkb OPEN`), and ft-screens then passes on its `vrkeyboard` commands (a desktop text field gets focus, a mapped button, frunner, a layout reset) as `kbpanel show SPOT | hide`, which the relay sends as `kb show SPOT | hide`. SPOT is `head` or `dash` and a 3x4 pose. It's the only on-screen keyboard: while it isn't running, ft-screens opens nothing.
- A click on its panel (`org.framekeyboard.*` overlays) doesn't take typing away from the desktop.

Only same-user messages count on either side. ft-kbhost takes word only from the relay's own name, and sends desktop typing only while that word is under 3 seconds old, so a stopped relay never leaves typing addressed to a name another process could take. The relay takes keys and text only from the address that said hello.

## Known limits

- Keys other than text and a shortcut's letter go by their place on the keyboard, so Ctrl+1 is Ctrl+& on an AZERTY desktop. A key that shows punctuation where another layout has a letter goes as that letter with Ctrl, Alt or Super (Full Keyboard names it so): Ctrl+, on AZERTY is Ctrl+M, and on Dvorak Ctrl+, is Ctrl+W (which closes a tab), Ctrl+; is Ctrl+Z and Ctrl+' is Ctrl+Q. Text the desktop's layout has no key for (Chinese, Japanese, Korean, emoji) can't be typed on the desktop yet; it reaches Steam's fields and games.
- Touch typing with a controller's tip (the old patch's `touch.cpp`) is gone: the release has no such setting. Fingertips and lasers type.
- fast-install downloads a release only when `keyboard/fetch.py` or `keyboard/build.sh` change; a full build always takes the newest.

## Building and running

```
keyboard/build.sh                          # the newest release, in keyboard/build/release
FULLKB_VERSION=0.19.1 keyboard/build.sh    # an exact one
keyboard/run.sh install | uninstall        # frametop-keyboard.service, starts with SteamVR
keyboard/run.sh status | restart | log [lines]
keyboard/test/                             # the host's tests, offline (fakes for the keyboard, relay and Steam)
input/test/keys-test.py                    # the relay's side, offline
```
