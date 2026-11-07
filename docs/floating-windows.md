# Floating windows

Any desktop app can float in VR in a panel of its own, like SteamVR's floating windows, while it stays part of the Frametop desktop. Drag and drop, the clipboard, and focus keep working between floating windows and the screens.

- **Float a window** with "Float in VR" in its window menu (Alt+F3), the float button left of Close in its title bar, or the float key (Meta+Shift+F by default). Start an app floating with "Launch as Standalone" in its right-click menu in the Application Launcher or the taskbar, with `ft-float launch` or `ft-float run`, or from a profile ([profiles.md](profiles.md)).
- **Tear it off** its screen: move it by its title bar and pull the wheel (scroll toward you). It floats and stays on the laser.
- **Put it back** with the dock button under its panel, the title bar button, the float key, or "Back to Desktop" in the window menu. It returns to the screen, position, and size it came from. Or carry it onto a screen until the screen lights up, and let go: it docks there. The `dock_all` action docks every floating window.
- **Move it** by its title bar or the bar under its panel (the wheel pushes and pulls it), **resize it** by its edges or the corner tab, **make it bigger or smaller** at the same pixels by scrolling on the corner tab, and **change its scale** with Meta+scroll over it. Each app's last floating place, size, density, and scale are remembered.
- Files, text, and images drag between any two floating windows, and between floating windows and the screens.

How KWin behaves underneath all this, and what the KWin script does about it, is in [design.md](design.md#floating-windows).

## Not built

These were decided (see the table) but aren't built:

- New windows of a floating app placed where that app's windows went last time, or to the parent's right (decision 15).
- +/- scale buttons on the floating panel's bar (decision 18). Meta+scroll changes the scale.
- The glow at the edge of your view toward a floating window activated out of sight, and the setting that brings it in front of you instead (decision 19).
- A Floating windows section in Frametop Display Settings (decision 1). `FLOAT_SLOTS` and `FLOAT_MARGIN` are set in `~/.config/frametop.conf`.
- A drag proxy on the catcher, so a drag's icon shows while the laser is between panels.
- Keeping floating windows out of Show Desktop (Meta+D) (decision 9). Nothing handles it yet.

## Decisions

The numbers are cited in the code, so they stay as they are. Struck-out text was replaced by a later decision.

| # | Question | Decision |
|---|---|---|
| 1 | How many windows can float at once | 16 spare outputs by default (also the most), configurable with `FLOAT_SLOTS`; a change needs a desktop restart. A Display Settings control isn't built |
| 2 | Menus and dropdowns | Each floating output has a margin around the window. The panel shows only the window, and each open popup gets a small overlay of its own, cut from the same buffer |
| 3 | Tearing off | ~~Drag the title bar past a screen's edge and let go in the air, with a small dead zone past the edge~~ Replaced (2026-10-30): while KWin moves the window by its title bar on a screen, pull the wheel. The window floats and the laser keeps carrying it; if floating it takes more than a second, it stays floating where it came up |
| 4 | Docking by dragging | Carry a floating window (any input: lasers, the 3D mouse, a hand) until the point you hold it by is on a shown screen within 10 cm of its surface (`FLOAT_DOCK_DISTANCE`). The screen lights up, and letting go docks the window there, the held point under the laser |
| 5 | Visibility | Floating windows follow the same rules as the screens: the hide hotkey, the visibility modes, and the games rule |
| 6 | Windows a floating app opens | They float too |
| 7 | Launching floating from the headset | ~~One "Frametop Apps" launcher entry with a picker~~ Replaced by 26 and profiles (27) |
| 8 | Build order | Not kept here: it only set the order of the work |
| 9 | Show Desktop (Meta+D) | Floating windows stay. Not built |
| 10 | Frametop Apps and visibility | ~~The entry starts the desktop with each screen hidden on its own, so only floating windows show~~ Replaced: a profile can hide screens (27) |
| 11 | Window frame | KWin's title bar and border stay. Frametop's bar, close, and "back to desktop" are extras |
| 12 | Resizing | The window's own edges and Frametop's corner tab both change the size in pixels at the same density; the output follows |
| 13 | Margin | 300 px on each side, configurable (`FLOAT_MARGIN`) |
| 14 | All spares in use | The window stays on the screens, with a notification |
| 15 | Where a floating app's new windows go | Not built: where that app's windows went last time, otherwise to the parent's right, curving around you. For now, a new window that opens on a floating window's output floats a little in front of it |
| 16 | Where the code is written | Not kept here: it was about the work, not about Frametop |
| 17 | Size when docked by dragging | The current floating size in pixels, shrunk to fit the screen, all of it on the screen |
| 18 | Bigger text | A scale for each window (KWin's output scale): Meta+scroll over the window. Remembered for each app. +/- buttons on its bar aren't built |
| 19 | Switching to a window you can't see | It's focused. Not built: a glow at the edge of your view that points to it, and a setting that moves it in front of you |
| 20 | Full screen | The window fills its own panel. The margin drops to zero while it's full screen, and the panel keeps its size and place |
| 21 | Named layouts | ~~They cover the screens only~~ Replaced by profiles (27). Outside a profile, floating windows use the placement remembered for each app |
| 22 | The float key | One toggle: it floats a window, or docks it if it already floats. The input relay owns it (`float_toggle`), Meta+Shift+F by default, rebindable in Frametop Input Settings and mappable to mouse and controller buttons. KWin has no shortcut of its own for it, so one press can't fire twice |
| 23 | Which window the key acts on | The window under the desktop's pointer; the active window if there's none there (the wallpaper, the taskbar) |
| 24 | Docking everything | A `dock_all` action, with no default binding |
| 25 | A button on every window | A float button left of Close in the title bar, from Frametop's own QML window decoration, made to look like Breeze. It shows a dock icon on floating windows. Apps that draw their own title bar (Chromium, Electron, GTK) use the key |
| 26 | Launching one app floating | "Launch as Standalone" in the right-click menu of every app in the Application Launcher and the taskbar, from copies of the apps' desktop files that only the Frametop desktop reads. It replaces the Frametop Apps entry (7, 10) |
| 27 | Profiles | Named layouts become profiles: the screens' places, which screens show, and the apps and their windows, floating or not. See `docs/profiles.md` |
| 28 | Panel size at the same pixels (2026-10-30) | Scroll on the corner tab: 5% a notch, from half to twice the screen's density. Remembered for each app; the float key and the title bar button float an app at the density it last had |

Also: floating windows get the wrist pin, the head pin, and pass-through (`pointer-ignore`) like screens. Every gesture works with the controllers as well as the 3D mouse. A window launched floating uses the primary screen's density. VNC shows only the primary screen.

## The approach: each floating window gets a KWin output of its own

Drag and drop and the clipboard only work between windows of the same compositor. A Wayland window can't move from one compositor to another. So a floating window has to stay a KWin window.

ft-screens already shows each KWin output as a panel. It sets the output's size with an `xdg_toplevel` configure, and KWin resizes the output to match. So a floating window gets an output of its own, sized to fit it, and ft-screens shows that output as a panel with its own controls. To KWin this is an ordinary desktop with more monitors. Dragging between two floating windows is the same as dragging between two monitors, which KWin already handles. ft-screens moves the pointer between panels in the middle of a drag: `handle_vr_event` moves pointer focus to another KWin window even while a button is held.

Alternatives considered:

- **Run floating apps directly on ft-screens.** It's a wlroots compositor, so apps could connect to it and get a panel per window. But they would get no drag and drop or clipboard with desktop apps without a bridge. Also, a window that's already on the desktop could never float, because a Wayland client can't change compositors. Rejected.
- **One large hidden "canvas" output.** Every floating window would sit on one big output, and each panel would show a crop of it (`SetOverlayTextureBounds`). That needs only one extra output, with no copies. But an 8K canvas uses about 128 MB per buffer, with two or three buffers in KWin's swapchain. It would also have to repack windows whenever one resized, full screen would fill the whole canvas, and every window would share one scale. It was the fallback in case per-window outputs didn't work.
- **Screencast single windows** (`zkde_screencast` `stream_window`, over PipeWire). This adds copies and latency, and the window still needs a real place in KWin's layout to receive input. Rejected.
- **SteamOS's own floating windows** (Launch a program from the dashboard). Those apps run in gamescope, outside KWin, so they can't drag and drop with the desktop.

### Where the extra outputs come from: spare outputs

KWin's nested backend opens its outputs at start (`--output-count`). The session starts KWin with the screen count plus `FLOAT_SLOTS` outputs (default and most 16). ft-floatd turns off the spares nothing floats on with `kscreen-doctor` once it starts. Floating a window enables a spare, and docking the window disables it again. `FLOAT_SLOTS` limits how many windows can float at once, and changing it means restarting the desktop. How KWin's nested backend treats disabled outputs, and why its virtual outputs can't be used instead, is in [design.md](design.md#floating-windows).

ft-screens creates a `screen` for each toplevel in the order they appear, and indexes its settings by that order. Spares come after the screens, so they get indices `SCREENS` and up. Their panels are hidden while their output is disabled.

## How the parts fit together

```
  KWin script "frametop-float"                ft-floatd (host, Python)                 ft-screens
  window events, moves, menus  ── D-Bus ──▶   window ↔ output ↔ panel table   ── @ft_screens ──▶   panels, controls,
  runs commands                ◀─ long poll ─  spare outputs (kscreen-doctor)  ◀─ @frametop_float ─  lasers, catcher
```

- **KWin script `frametop-float`** (`float/frametop-float.js`). ft-floatd loads it into the desktop's KWin over D-Bus (`org.kde.kwin.Scripting`). A script keeps working across KWin updates. A C++ effect would have to match the host's exact KWin build, and the build container is Fedora, not SteamOS. The script watches windows (`windowAdded`/`windowRemoved`, `frameGeometryChanged`, `outputChanged`, `interactiveMoveResizeStarted`/`Finished`, `fullScreenChanged`, `maximizedChanged`, `minimizedChanged`, `keepBelowChanged`, `windowActivated`) and the outputs (`screensChanged`). It runs commands: move a window to an output, set its geometry, put it on all virtual desktops, and restore it. It adds "Float in VR" ("Back to Desktop" on a floating window) to the window menu (`registerUserActionsMenu`). It registers no shortcut: the float key belongs to the input relay. KWin scripts can call D-Bus but can't serve it, so commands come back through a long poll. The script calls ft-floatd's `NextCommand`, which answers when a command is ready, and then the script calls it again, or after 30 seconds without an answer. It also keeps KWin's placement memory from moving windows (see design.md).
- **ft-floatd** (`float/ft-floatd`, Python). The host has dbus-python and PyGObject. It runs inside the desktop's Plasma session, started from its autostart. It owns `org.frametop.Float` on the session's private bus, and it keeps the table of which window is on which output and panel. It enables and disables spare outputs and sets their scale and position with `kscreen-doctor`, and their size through ft-screens. It tells ft-screens where each floating window goes and tells the script which window goes where. It launches apps floating, opens profiles' apps, and remembers each app's placement and scale, keyed by desktop file name. Commands come in on `@frametop_float`, from `ft-float`, the input relay, and ft-screens.
- **ft-screens.** A spare output's panel is a floating window's. Floating panels get the same bar, curve, roll, resize tab, and wrist and head pins as screens, plus dock and close buttons left of the bar. Other parts: the catcher, popup and dialog overlays, and carrying a panel during a KWin move. `MAX_SCREENS` (screens and spares together) is 24. Commands arrive on `@ft_screens`. Events go out to `@frametop_float` from an unbound socket, the same way ft-screens talks to the input relay.
- **Session script.** Adds `FLOAT_SLOTS` to KWin's output count, starts ft-floatd from the desktop's autostart, installs Frametop's window decoration and chooses it in the session's `kwinrc`, and writes the Launch as Standalone copies of the apps' desktop files.
- **ft-layout.** Arranges only the screens' outputs, and leaves the spares (`WL-<SCREENS>` and up) to ft-floatd, enabled or not.
- **ft-pointer.** The drag lock crosses onto other Frametop panels (see "Drag and drop between panels"), and a left release also goes to ft-screens as a backstop for the catcher.
- **Input relay.** Owns the float key: `float_toggle` and `dock_all` send `float pointer` and `dock all` to ft-floatd.

Program names stay within 15 characters (`ft-floatd`). Overlay keys are `frametop.float.N` and `frametop.float.N.bar`, and so on; a floating window's popups and dialogs are `frametop.float.N.sub.K`.

## A floating window

- **Output and margin.** Its output is the window's frame plus a margin on each side (`FLOAT_MARGIN`, default 300 px). KWin keeps a Wayland popup inside its parent's output, so the margin gives menus and dropdowns room past the window's edges. X11 apps place their own menus within the monitor, so the same applies. Enabled spares sit apart from the screens and from each other in KWin's layout, so nothing spills from one to the next. Memory: a 1600 × 1000 window with a 300 px margin is about 14 MB per buffer, 42 MB for three.
- **What the panel shows.** Only the window's frame: ft-screens crops the output's buffer with `SetOverlayTextureBounds` and maps mouse positions through the crop. Each open popup or dialog gets a small overlay of its own, cut from the same buffer and placed a few millimetres in front of the window, so the main panel never changes size. KWin tells scripts about popups as windows of their own (`windowAdded` with `popupWindow`), so the script reports their rectangles.
- **Where it appears.** Floated from a screen, the panel starts where the window was on that screen, 30 cm in front of it. It faces you as the screen does at its centre, so a window floated from near a big screen's edge isn't turned away from you (ft-floatd's `face_eye`). Launched floating, it goes where that app last floated, or in front of you, 0.8 to 2 m away.
- **Size and scale.** The panel's width is the window's pixel width times the source screen's metres per pixel, so text stays the same size in VR. A window launched floating uses the primary screen's density. Each window also has a scale (KWin's output scale), changed with Meta+scroll over the window in steps of 10% and remembered for each app. A bigger scale makes the content bigger at the same panel size. Notches that come while a change is under way add up into the next one, and the panel keeps its picture while KWin changes the scale (ft-screens' `hold`), so it goes from one scale to the next without the frames in between. Scrolling on the corner tab changes the density instead: the panel gets 5% bigger or smaller a notch with the same pixels (ft-floatd's `density`), from half to twice its screen's density, the tab staying under the laser. That's remembered for each app too.
- **Window state.** An ordinary window, not maximized, placed inside its output with the margin around it, and set to show on all virtual desktops. It keeps its title bar and border. Apps that draw their own title bar (GTK, Chromium) keep theirs.
- **Moving.** Press the title bar. KWin starts an interactive move on the press itself, before any motion, so ft-screens stops forwarding pointer motion to KWin as soon as a press lands in a floating window's title bar (from the frame and client rectangles ft-floatd sends it). KWin's pointer stays at the press point and the window moves by nothing. For apps that draw their own title bar, the script reports the move and ft-screens stops then (`carry`); ft-floatd puts back any few pixels the window slipped before that. Meanwhile ft-screens carries the panel with the pressing device, the same way the bar does: it follows rigidly, scroll pushes and pulls, and the 3D mouse's right-drag tilts. When the button comes up, KWin gets the release at the press point. The bar under the panel works too.
- **Resizing.** The window's own edges (inside the margin, so KWin's resize works as on the desktop) and Frametop's corner tab both change the window's size in pixels at the same density, so the app lays itself out again. KWin ends a resize by the window's edge whenever an output changes, so during one the panel follows the window and the output follows only when the drag ends: the margin is the room to grow until then. The corner tab (and a hand's pinch at a corner, or two fists) only stretches the panel while it goes on, every frame, the opposite corner staying put; when it ends, the window gets that many pixels in one resize, ft-floatd resizes the output to keep the margin, and the panel keeps its stretched picture (`hold`) until the window's new one comes (user, 2026-10-27: "it doesn't actually update the pixels ratio and whatnot until I release so we can have the animation be smooth"). Before, each step resized the window and its output 20 times a second, and every output change made KWin reconfigure. The stretch grows the panel in its own plane, which takes it off the sphere round you; when the corner tab or the pinch lets go, a panel in the room turns about the corner that stayed put, over a quarter of a second, to meet your head as it did when grabbed, so it isn't left turned away from you (not straight above or below your head). It doesn't turn while you drag: the laser's point on the panel sets the size, and a panel turning under it would feed back into it. A resize by the window's own edges isn't turned. A screen's tab only scales the panel. The minimum is 320 × 200, like screens.
- **KWin's placement memory.** KWin puts windows back where they were for each layout of the outputs it has seen, which fights spare outputs that follow their windows' sizes. The KWin script undoes it ([design.md](design.md#kwins-placement-memory)).
- **Full screen.** The window fills its own panel: the margin drops to zero while it's full screen, and the output is the panel's size in pixels. The panel keeps its size and place. On leaving full screen, the margin comes back.
- **Buttons.** The close button closes the window. The dock button docks it where it came from.
- **Minimize.** Minimizing, from the title bar or the taskbar, hides the panel, and restoring it shows the panel again. Floating windows stay in the desktop's taskbar and in Alt+Tab.
- **New windows.** Popups and dialogs of a floating window (`transientFor`) show as small overlays over it. Another window of a floating app that opens on its output floats too, a little in front of it. Any other window that opens on a floating window's output goes to the first screen that shows. When every spare is in use, the window stays on the screens and a notification says so.
- **On a hidden screen.** A new window that opens on a screen hidden on its own floats instead, where that app last floated or in front of you.

## Tearing a window off a screen

Decision 3, built 2026-10-30:

1. Press a desktop window's title bar on a screen and move it. KWin starts an interactive move, the script reports it (`screen-move`), and ft-floatd passes it on to ft-screens (`moving <window> 1`, and `0` when the move ends).
2. Pull the wheel (scroll toward you) during that move. ft-screens releases the button in KWin where the pointer is, which ends the move, and asks ft-floatd to float the window (`tear <window> <x> <y>`, the pointer's place on the screen in its pixels). The button stays held in ft-screens, and its release doesn't go to KWin.
3. ft-floatd has the script float the window as the float key would, its panel in front of where the window was, and answers `torn <panel> <gx> <gy>`: where in the window the laser held it.
4. ft-screens moves the panel so that point is on the laser and carries it with the laser that tore it, as by its title bar: the wheel pushes and pulls it, and carried onto a screen it docks there (see "Putting it back"). Letting go leaves it floating.
5. If the answer takes more than a second, or the button is already up, the window stays floating where it came up.

A hand's pinch doesn't tear: hands have no wheel. Only a real KWin move counts, so a pull while selecting text or dragging a file scrolls as usual.

## Putting it back

- **Button.** The dock button returns the window to the screen, position, and size it had before it floated. If that screen is hidden now, it goes onto the first screen that shows.
- **Dragging.** Carry the floating window, by its title bar or its bar, until the point you hold it by is on a shown screen within 10 cm of its surface (`FLOAT_DOCK_DISTANCE`, 0 turns it off): push it there with the wheel, or move the controller or hand forward. The screen lights up, and letting go docks the window there (ft-floatd's `dockat`), the point you held under the laser, at its current size in pixels, shrunk to fit if the screen is smaller and kept on the screen. A carried panel keeps its distance, so moving a floating window in front of a screen never docks it by accident. With a hand, the line runs from your head through the panel.
- Docking disables the output and hides the panel.

## Getting at it: the float key and the title bar button

Decisions 22 to 25.

- **The float key.** The input relay owns it: the action `float_toggle`, bound to Meta+Shift+F unless the rules file says otherwise (a rules file with no `key_bindings` gets that default; one with its own list, even an empty one, doesn't). It can be rebound or removed in Frametop Input Settings, and mapped to a mouse button or a Frame controller button like any other action. The relay takes the combination before it reaches the desktop and sends `float pointer` to ft-floatd, which asks the script for the window under KWin's pointer (`workspace.cursorPos`, top of `workspace.stackingOrder`, popups and dialogs counting as their parent). With none there, the wallpaper or the taskbar, it's the active window. KWin's pointer is where the 3D mouse or a laser last was on a Frametop panel. A window that floats docks; any other floats. The KWin script has no shortcut of its own (ft-floatd removes one that an older script registered), so one press can't float a window and dock it again.
- **Docking everything.** `dock_all` (no default binding) sends `dock all`, which docks every floating window where it came from.
- **The title bar button.** Breeze can't take a button of its own, and a C++ fork of it would have to match SteamOS's exact KDecoration build (Plasma 6.3 replaces KDecoration2 with KDecoration3). So the Frametop desktop gets its own window decoration, written in QML for KWin's Aurorae engine, which loads it without compiling (`decoration/`, installed to `~/.local/share/kwin/decorations/kwin4_decoration_qml_frametop`, chosen in the session's `kwinrc` only, so Desktop Mode keeps Breeze; `decoration/apply.sh` switches the running desktop to it or back to Breeze). It's drawn to look like Breeze, with a float button left of Close. The button calls `requestToggleKeepBelow()`, the one window request a decoration can make that has no visible effect here, and the KWin script reads the change: keep-below set on a window on the screens floats it, cleared on a floating window docks it. The script keeps keep-below set on every floating window, however it was floated, so the button shows its dock icon there. A window alone on its own output loses nothing by being kept below (only the wallpaper is under it). If the window can't float (every spare is in use), the script clears the flag again. Keep Below Others in a window's menu does the same as the button.
- **Apps that draw their own title bar** (Chromium and Electron apps, GTK apps) never show KWin's decoration, so they don't get the button. They use the key, or the window menu (Alt+F3).

## Launching an app floating

- **In the desktop.** Use "Float in VR" in any window's menu, its title bar button, or the float key.
- **From the menu.** Right-click an app in the Application Launcher, or in the taskbar (where it starts another window of that app), and pick "Launch as Standalone" (decision 26). The launcher has no way to add an entry to every app's menu, but its menu shows each app's own desktop actions. So the Frametop desktop reads copies of the apps' desktop files with one more action added (`float/ft_apps.py`). They're written to `~/.local/share/frametop/apps/applications` from every desktop file in `XDG_DATA_DIRS`: by the session script before Plasma starts, and by ft-floatd whenever an app is installed, changed, or removed. The session puts `~/.local/share/frametop/apps` first in `XDG_DATA_DIRS`. Plasma's app cache is keyed by those directories, so Desktop Mode never sees the copies. Desktop files in `~/.local/share/applications` come before every data dir, so an app you've customized there keeps your copy and has no Launch as Standalone. The action runs `ft-float launch <desktop file name>`.
- **From a command.** `ft-float run <command>` and `ft-float launch <app.desktop>` start an app and float its first window. ft-floatd records the process it started, and new windows are matched by PID, including child processes. Some single-instance apps (Firefox, D-Bus-activated apps) open the window from a process that was already running. Those are matched by desktop file name. Either way, the window has to show up within 30 seconds.
- **From the headset with the desktop off.** A profile's launcher entry starts the desktop in that profile, and a profile can hide every screen and hold only floating apps (`docs/profiles.md`). There's no separate Frametop Apps entry or picker.
- **Remembered placement.** Each app's last floating pose (relative to the primary screen's panel, so it moves with the screens' layout), size in pixels, and scale, keyed by desktop file name, in `~/.config/frametop-float.json`. It's kept whenever one of the app's windows stops floating. With nothing remembered, the window opens in front of you, at the primary screen's density, 0.8 to 2 m away. A profile's own placement wins when the profile opens the app.

## Drag and drop between panels

KWin handles the protocols: Wayland, X11 through Xwayland, and the portal's file transfer. Frametop has to get the pointer right between panels.

- **Crossing panels.** When the laser moves onto another panel mid-drag, ft-screens gives that panel's KWin window pointer focus. KWin puts its cursor at that output's position, and the drop target gets enter and motion events. Floating windows add nothing new here, but they make gaps between panels the normal case.
- **Gaps (the catcher).** While the laser is between panels, none of Frametop's overlays get its events, and ft-screens clears pointer focus on `FT_LEAVE` even with a button held. A release in empty space would never reach KWin, and the drag or move would stay stuck until the next click. So while a button is held on a Frametop panel and the laser leaves all of them, ft-screens puts an invisible catcher overlay on the laser. A release on the catcher releases in KWin wherever the pointer last was. Dropping in a gap cancels, just as dropping outside any window does. The pointer helper also tells ft-screens when the mouse's left button comes up ("up"), in case the catcher misses it. This also covers window moves and drags on the screens that end off a panel.
- **The 3D mouse's drag lock.** While the button is held, the drag lock keeps the cursor at its distance and stops hit tests, so a drag onto a nearer panel would pass behind it. So while the button is held, the helper keeps testing the other Frametop panels (not the one pressed on, and not while carrying one) and moves onto a panel the ray meets. Off the edge of the pressed panel, it keeps that panel's plane, so moves and resizes past the edge still work.
- **Drag icon.** KWin 6 draws the drag icon as part of its scene, on the output its pointer is on. In a gap it stays at the source panel's edge.
- **Flatpak apps.** Dropping files into a sandboxed app goes through the document portal, the same path that the session script's file-picker fix covers ([design.md](design.md#the-desktop-session)).

## Things that must keep working

- **Typing follows the last click.** A click on a floating panel counts as a click on the desktop, since the window is a KWin window.
- **Visibility.** Floating windows follow the screens' rules: the hide hotkey, the visibility modes, and hiding during a VR game unless the dashboard is open. Controllers' lasers are off in games.
- **Headset standby.** Nothing new may poll SteamVR with new clients, so no new `vrcmd` loops.
- **The pointer helper's overlay list.** The helper learns about overlays by running `vrcmd --overlays` in the background, so a new floating panel appears in its next listing.
- **Plasma.** An enabled floating output gets a desktop view (wallpaper) under its window, hidden by the crop. Plasma doesn't add panels to new outputs by default. A floating output must never become primary. With spare outputs, the output count stays the same, which avoids the lost-taskbar problem in design.md's open questions.
- **Restarting the desktop** closes every window, floating ones included. Each app's placement is remembered, so an app launched floating again comes back where it was.

## Risks

- A SteamOS update can change KWin's script API or its nested backend. The script and the output handling are the parts to recheck after one, and KWin's placement memory with them (the script undoes it, see [design.md](design.md#kwins-placement-memory)).
- GPU memory: each floating output has its own swapchain of two or three buffers, including the margin. The Frame has 16 GB shared, with about 4 GB free in normal use (2026-09-29).
- Frame pacing with many panels hasn't been measured (already an open question in design.md). Each output is a separate render pass in KWin.
