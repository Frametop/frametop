# Spotwatch

Spotwatch is Frametop's search for apps and remote displays, like Spotlight on a Mac. Press Meta+Space and a glass search pill opens straight ahead of where your head points. Type part of an app's name, or a remote display's `{host} - {display}`, and pick from what it finds:

- **An app that's already running:** its most recent window comes to you. The lazy susan spins the panel it's on, a screen or its own floating window, to straight ahead, and typing goes to it.
- **An app that isn't running:** it starts as a floating window, where that app last floated or in front of you.
- **A remote display:** spotwatch connects it if needed, then spins it to the front.

Use Tab, Shift+Tab, or the arrow keys to move through the results, and Enter or a click to open one. Shift+Enter or a right click opens the item's menu:

- **For an app:** its own launch options, such as a browser's New Private Window, plus Open new window (even when one is open), Open docked on this screen, and each of its open windows.
- **For a remote display:** Go to (connecting it first if needed) and Disconnect.

Esc goes back from a menu and closes the search. Meta+Space again, or a click anywhere else, closes it too.

Spotwatch works while the Frametop desktop is running and Frametop isn't paused for a VR game. It's the `spotwatch` action in Frametop Input Settings, so a mouse button, a controller button, or another key combination can open it too. When it opens from a mouse or controller, Frametop's keyboard opens with it.

## Not built

- Searching open windows by title, Frametop's own actions ("Reset layout"), and profiles (decision 4).
- Moving the caret within the query. Typing and Backspace work at the end only.
- A setting for its distance and size (`--distance` and `--width` on ft-spotpanel; 0.75 m and 0.42 m, `spotwatch/reserve.h`).

## Decisions

These were settled with the user on 2026-10-10. The numbers may be cited in the code, so they stay as they are.

| # | Question | Decision |
|---|---|---|
| 1 | The look | A rounded glass pill with a results card under it. The glass is faux, because SteamVR's overlays have alpha but no backdrop blur: a translucent dark tint, a soft sheen from the top, a bright rim fading down the edges, and a shadow outside. Real blur would mean reading the panels back every frame on the Frame's GPU |
| 2 | Where it opens | Fixed in the room, centred where the head points (not the eye gaze), at 0.75 m, upright, tilted no more than 60° up or down. The card hangs below the pill, so the pill stays put as results change. That spot is reserved (2026-10-17): the lazy susan's front slot stays at least 0.15 m behind it, and the search draws over any panel nearer than it |
| 3 | An app with several windows | One row per app. Enter goes to its most recently active window, and Shift+Enter or a right click lists the others with the menu items |
| 4 | What it searches | Apps and remote displays only. Hidden remote displays are left out, and disconnected ones show with Connect |
| 5 | Disconnected remote displays | Enter connects the display (`ft-layout remote connect`), waits for its panel, then spins to it |
| 6 | When it works | Only while Frametop is running and active: the session starts it, and the input relay doesn't run the action while Frametop is paused. A pause while it's open closes it |
| 7 | Without a keyboard | `spotwatch` is an action like the others, so it can be bound to a mouse button or a controller button. Opened that way, Frametop's keyboard opens too and closes with it |
| 8 | The menu | The app's desktop actions, Open new window (the app's own new-window action when it has one, else a plain launch), Open docked on this screen (the screen nearest straight ahead), and its windows |
| 9 | An app that isn't running | It floats, where ft-floatd puts any launched app (where it last floated, or in front of you) |
| 10 | A window on a pinned panel | Pinned panels don't spin. Typing goes to it where it is (`front N`), and a short note says where it is ("on your left wrist") |
| 11 | Shortcut | Meta+Space by default. Plasma's KRunner has Alt+Space, and nothing in Frametop used Meta+Space |
| 12 | Branch | `spotwatch`, off `experimental`, merged into `experimental` |

## How it works

Spotwatch has three parts, and needs changes in ft-screens, ft-floatd and the input relay:

- **`spotwatch/ft-spotwatch`** (`ft_spotwatch.py`) runs inside the desktop's Plasma session, started from its autostart like ft-floatd, so it's up exactly while the desktop is. It keeps the app index (Gio's, so it sees what the desktop's menus see), the remote displays from the layout, the usage counts, and the search state. It decides what each key and click does. Its log is `/tmp/frametop-spotwatch.log`.
- **`spotwatch/ft-spotpanel.cpp`** is the pill: a SteamVR overlay drawn on the CPU, like ft-gazepanel, built by `screens/build.sh` and run by ft-spotwatch in the container. It holds no state beyond the picture. ft-spotwatch sends it the whole state each time, and it sends back hovers, clicks, right clicks, scrolls, and clicks beside the card. Its clear area catches those clicks.
- **`spotwatch/spotsearch.py`** ranks the results with plain Python, so its test runs anywhere. Name matches come first, from the strongest kind of match to the weakest: the name's start, a word's start, initials, anywhere in the name, then letters in order. Generic names, keywords, and program names count for less and never match fuzzily. Every word of the query must match. Use counts and use in the last day raise an item, and so does an app that's running.

The search has to take the typing from wherever it went, Steam included. While it's open, ft-screens sends every key to `@ft_spotwatch` instead of KWin or a remote display's stream, from keyboards and Frametop's keyboard alike, and tells the input relay that typing goes to the desktop, so the relay grabs the keyboards. ft-spotwatch holds this with `spotwatch on`, renewed every second. ft-screens lets it lapse after 3 seconds, so a crashed ft-spotwatch can't keep the keys. A click on any other panel, or a pause, ends it, and ft-screens says `dismiss` so the pill closes.

ft-spotwatch turns key codes into text on the desktop's keyboard layout (Plasma's `kxkbrc`) through libxkbcommon, and falls back to a US layout without it. The input relay sends no key repeats, so ft-spotwatch repeats a held Backspace or arrow key itself.

Bringing a window to the front takes three commands:

1. ft-floatd's `activate ID` makes the window KWin's active window, unminimized.
2. ft-screens' `spin to N` brings panel N to the front slot at reading distance (or, with `SCREEN_SPIN_BRING=0`, turns the ring of panels the short way so it ends straight ahead). Screens are 1 to S, floating windows come after them, and remote displays start at 101.
3. When the spin settles, ft-screens gives that panel the typing, as any spin does. When the panel can't spin (pinned, or a floating window that's minimized), `front N` gives it the typing where it is.

ft-floatd's `spotlist` lists every app window with its desktop file name, title, panel, and when it was last active. The KWin script keeps that timestamp.

Launching uses ft-floatd's `launch APP.desktop`, which now takes `action=ACTION` (one of the desktop file's actions) and `screen=N` (open docked on screen N; the KWin script's `to-output`). Without ft-floatd (`FLOAT_SLOTS=0`), spotwatch launches through Gio itself, and nothing counts as running.

Apps' icons come from the desktop's icon theme (`kdeglobals`, then breeze and hicolor). GdkPixbuf rasterizes them to 64x64 RGBA files in `~/.cache/frametop/spotwatch` for the panel, one at a time in idle moments after the desktop starts. Use counts are kept in `~/.config/frametop-spotwatch.json`.

```
spotwatch/test/spotwatch-test.py    # the ranking, then ft-spotwatch against a fake ft-screens and ft-floatd (offline)
spotwatch/tools/render.py OUT.png    # what the panel draws, without a headset (ft-spotpanel --render)
```

## Risks

- **Stray key releases.** A key pressed before the search opened is released to wherever it went, so nothing stays held. A key pressed in the search and released after it closes (the Enter that opened something) reaches the window that now has the typing as a release without a press. Apps ignore those.
- **Laser input.** While the pill shows, it makes SteamVR's lasers active (`MakeOverlaysInteractiveIfVisible`), as Frametop's keyboard does. The search is short-lived, but during that time a game loses its controllers.
- **Matching windows to apps.** Matching uses KWin's desktop file name, then the window class. An app whose windows report neither, such as some X11 apps under Flatpak, doesn't show as running, and Enter starts it again.
