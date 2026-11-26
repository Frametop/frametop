# Desktopless mode

Desktopless mode puts every app on a panel of its own and hides the desktops they came from. The Frame's screens hide, and so do the displays of the PCs you put in the mode. Every app window on them comes out as a panel: the Frame's windows float ([floating-windows.md](floating-windows.md)), and each PC window streams on its own ([remote-displays.md](remote-displays.md), window panels). Turn it off and every window goes back where it was. The Frame's go back onto their screens, and the PCs' go back onto their desktops, at the place and size they had.

Turn it on or off on Frametop Settings' Desktopless mode page (also a switch at the top of the Screens page), with Meta+Shift+D, or with "Desktopless mode on or off" in frunner. The command line is `ft-layout desktopless on|off|toggle|status`.

## Which sources are in it

Each source has its own switch, on the Desktopless mode page under "What's in it" (user choice, 2026-11-20):

- **This Frame** is in by default. Out, its screens and their windows stay as they are while the mode is on. The switch is `desktopless.frame` (absent means in), and it lasts across on and off. `ft-layout desktopless frame on|off`.
- **Each PC** is out by default: its display panels stay, and its windows stay on it. A PC with many windows open would otherwise make a panel of each, more than the Frame's decoder and SteamVR's overlays keep up with. In, its displays hide and every window comes out. Only a PC with a Frametop windows instance can be in (the "Show its apps and windows in the launcher" switch on its Remote Displays card). The switch is `"desktopless": true` on its host entry. `ft-layout desktopless host NAME on|off`.
- While the mode is on, a switch applies right away. A PC put out has its window panels closed (their places kept) and its displays shown again. The Frame put out shows its screens and docks its windows; put back in, its screens hide and its windows float again.
- `ft-layout desktopless sources` prints the mode and every switch as JSON.

## What comes out

- Every window comes out, from every workspace (user choice, 2026-11-07). The exception is a PC window on a special workspace (a scratchpad), which the PC keeps. A Windows PC lists the windows of every virtual desktop (Task View's) too (2026-11-15). Windows draws only the desktop it shows, so a window from another one comes to that desktop when its panel opens, and goes back to its own when it's put back. Built without a Windows PC to try it on: see the fork paragraph in [remote-displays.md](remote-displays.md).
- A window opened while it's on becomes a panel too. On the Frame, a window that would open on a hidden screen floats. On a PC, `ft-layout desktopless watch` checks every 2 s. It runs as the `frametop-desktopless` user unit while the mode is on.
- A PC that connects later gives its windows up as soon as its Frametop windows instance answers.
- A window you send back to its PC while the mode is on stays there. The watcher leaves windows it has opened before alone.
- Panels run out at 16 remote panels, counting the hidden display panels. When a PC has more windows than that, the ones on a workspace that shows (on Windows, the virtual desktop shown) come out first, then the most recently used (Hyprland's focus history; user choice, 2026-11-07). The rest come out as panels free up.

## Where the panels go

- An app that had a panel in desktopless mode before goes back to that place. Frame apps use the place ft-floatd keeps for each app. PC apps use the place the layout keeps (`desktopless.places`, by PC and desktop app, or by window class when the app isn't known). Both places are kept relative to the screens, so they stay put in the room.
- Panels without a place of their own are fanned out in front of you on a curve, each turned to face you (user choice, 2026-11-07). A row of the fan spans at most 150 degrees, then the next row goes above it.

## How it works

- `ft-layout desktopless on` keeps which screens and PC displays were hidden already (`desktopless.before`). It then hides every screen (with the Frame in) and the display panels of each PC in the mode. It asks ft-floatd to `float all` (with the Frame in) and opens each PC window with `ft-layout window`'s window panels. Last, it fans out the new panels and starts the watcher.
- A hidden display panel stays connected. Its stream holds the PC's keyboard and mouse for the Frame, since window streams don't take the input lock.
- `off` keeps where each PC window panel is, closes them all (each PC puts its windows back), shows what wasn't hidden before, and docks the floating windows (`dock desktopless`). Windows that floated before the mode came on stay floating (ft-floatd's `float keep` remembers them). Windows whose screen is still hidden dock onto the first screen that shows.
- The PC side is the Frametop windows instance (linuxpc's fork, `hyprland_windows.cpp`). Its window list says which workspace shows, the focus order, whether a window floats, where it sits, and its desktop app. A window that comes back without being dropped on a monitor goes to its old place. A floating one gets its old position and size. A tiled one splits in where its middle was, when its workspace shows.

## Profiles and desktop start

- A profile keeps whether desktopless mode was on when it was saved. With it on, the profile also keeps where each PC app's window panels were (`profiles[NAME].desktopless.places`, by PC and app). The Frame's apps are kept as a profile always keeps them ([profiles.md](profiles.md)).
- Opening a desktopless profile hides the desktops before it arranges anything, so the screens don't show first. After its Frame apps open, every window comes out. Then each PC app it had that isn't open on its PC starts there (`/open`), as a window panel at its place. Only apps with a desktop file can start that way. A window the profile knows by its class alone comes out only if it's open.
- Opening a profile saved with the mode off, while it's on, closes the PC window panels (the windows go back), shows the profile's screens, and docks the windows the mode floated. With the mode off already, the profile's floating windows stay as they are.
- A profile saved while the mode is on keeps the screens and displays that were hidden before the mode, not the ones the mode hides.
- The desktop starts in the mode it stopped in. With the mode on, its window panels connect again, every Frame window floats once ft-floatd is up, and the watcher starts.
- A PC that disconnects (it went to sleep, or the Frame left) puts its windows back by itself a minute later, each at its old place. When it connects again, the panels take the windows that are still there.

## Limits

- SteamVR allows 128 overlays for every app together. Each panel takes one, and the panels share four sets of controls (36 overlays; "SteamVR's overlay limit" in [remote-displays.md](remote-displays.md)). Before that, each panel took eight with its controls, and a run on 2026-11-15 stopped at 6 window panels next to 3 display panels. When SteamVR makes no panel for a window, that window stays on its PC and the count reached is the cap. The rest come out as panels close. 16 remote panels in all is the other cap (`REMOTE_MAX`), counting the hidden display panels.
- Each window goes back to its own workspace and monitor. A tiled one on a workspace that isn't showing at off splits in next to the window last used there, so the order on that workspace and its split sizes can differ from before (Hyprland's dwindle tree isn't kept).
- One change at a time: on, off, a profile and each pass of the watcher take turns (a lock in `$XDG_RUNTIME_DIR`). Pressing Meta+Shift+D twice turns the mode on, then off.
- Frame windows float only while ft-floatd has spare outputs. Past that, or with ft-floatd not answering, the rest stay on their hidden screens until the mode goes off (the log says so).
- ft-floatd keeps which windows floated before the mode only while it runs. After a desktop restart in the mode, off docks every floating window.

- Only PCs with a Frametop windows instance (`windows=PORT`, [remote-displays.md](remote-displays.md)) can be in the mode: a Linux PC's second instance, or a Windows PC with the Windows build (`windows=47989`, DEE-348). Another PC's displays stay as they are.
- The switches are the same for every profile: a profile keeps whether the mode is on, not which sources are in it.
- A PC app's place is kept at `off` and when a profile is saved. A panel moved and then closed while the mode is on goes back to the place it had before next time.
- Each window panel is a video stream of its own, and a hidden display panel still decodes every frame its PC sends (ft-stream shows 10 a second of them). Many at once may be more than the Frame's decoder keeps up with.
