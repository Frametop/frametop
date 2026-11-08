# Desktopless mode

Desktopless mode puts every app on a panel of its own and hides the desktops they came from. The Frame's screens hide, and so do the displays of your Linux PCs. Every app window on them comes out as a panel: the Frame's windows float ([floating-windows.md](floating-windows.md)), and each PC window streams on its own ([remote-displays.md](remote-displays.md), window panels). Turn it off and every window goes back where it was. The Frame's go back onto their screens, and the PCs' go back onto their desktops, at the place and size they had.

Turn it on or off with the switch at the top of Frametop Settings' Screens page, with Meta+Shift+D, or with "Desktopless mode on or off" in frunner. The command line is `ft-layout desktopless on|off|toggle|status`.

## What comes out

- Every window comes out, from every workspace (user choice, 2026-11-07). The exception is a PC window on a special workspace (a scratchpad), which the PC keeps.
- A window opened while it's on becomes a panel too. On the Frame, a window that would open on a hidden screen floats. On a PC, `ft-layout desktopless watch` checks every 2 s. It runs as the `frametop-desktopless` user unit while the mode is on.
- A PC that connects later gives its windows up as soon as its Frametop windows instance answers.
- A window you send back to its PC while the mode is on stays there. The watcher leaves windows it has opened before alone.
- Panels run out at 16 remote panels, counting the hidden display panels. When a PC has more windows than that, the ones on a workspace that shows come out first, then the most recently used (Hyprland's focus history; user choice, 2026-11-07). The rest come out as panels free up.

## Where the panels go

- An app that had a panel in desktopless mode before goes back to that place. Frame apps use the place ft-floatd keeps for each app. PC apps use the place the layout keeps (`desktopless.places`, by PC and desktop app, or by window class when the app isn't known). Both places are kept relative to the screens, so they stay put in the room.
- Panels without a place of their own are fanned out in front of you on a curve, each turned to face you (user choice, 2026-11-07). A row of the fan spans at most 150 degrees, then the next row goes above it.

## How it works

- `ft-layout desktopless on` keeps which screens and PC displays were hidden already (`desktopless.before`). It then hides every screen and each Linux PC's display panels. It asks ft-floatd to `float all` and opens each PC window with `ft-layout window`'s window panels. Last, it fans out the new panels and starts the watcher.
- A hidden display panel stays connected. Its stream holds the PC's keyboard and mouse for the Frame, since window streams don't take the input lock.
- `off` keeps where each PC window panel is, closes them all (each PC puts its windows back), shows what wasn't hidden before, and docks every floating window (`dock all`). Windows whose screen is still hidden dock onto the first screen that shows.
- The PC side is the Frametop windows instance (oma7's fork, `hyprland_windows.cpp`). Its window list says which workspace shows, the focus order, whether a window floats, where it sits, and its desktop app. A window that comes back without being dropped on a monitor goes to its old place. A floating one gets its old position and size. A tiled one splits in where its middle was, when its workspace shows.

## Limits

- Only Linux PCs with a Frametop windows instance (`windows=PORT`, [remote-displays.md](remote-displays.md)) give their windows up. A Windows PC's displays stay as they are (DEE-348).
- PC windows that close while the Frame is away come back new: their places are kept only at `off`.
- Each window panel is a video stream of its own, and a hidden display panel still decodes every frame its PC sends (ft-stream shows 10 a second of them). Many at once may be more than the Frame's decoder keeps up with.
