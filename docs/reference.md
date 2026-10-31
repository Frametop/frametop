# Frametop reference

How each part of Frametop works, where its settings live, and the commands for running parts of it by hand. For why it's built this way, see [design.md](design.md).

## The desktop

From the headset, open Launch a program → Desktop. The installer replaces that launcher entry with Frametop's (`~/.local/share/applications/deckard-nested-desktop.desktop`), and `desktops.sh uninstall` gives the stock single-screen desktop back.

The installer also adds Native Desktop to the same list: a copy of SteamOS's entry for its own desktop (`~/.local/share/applications/native-deckard-nested-desktop.desktop`), skipped when SteamOS has no such entry. The copy is made at install time, so `scripts/update-check.py` warns when SteamOS's entry changes, and `desktops.sh install` refreshes it. In Native Desktop, typing goes to Steam's side, so the input relay doesn't grab keyboards there, and a Meta tap runs Frametop's Meta action (by default, the Steam menu) as well as opening Plasma's launcher.

From a terminal, on the Frame or from a PC over SSH:

```
desktops.sh install        # the launcher's Desktop entry starts Frametop; Native Desktop is the stock one
desktops.sh uninstall      # back to the stock SteamOS desktop, as Desktop
desktops.sh start | stop | restart | status | log [lines]
```

To have the desktop start by itself, turn on Start with SteamVR on the Power tab of Frametop Display Settings, or run `desktops.sh autostart on`. Both set `AUTOSTART=1` in `~/.config/frametop.conf`. `frametop-autostart.service`, which `desktops.sh install` adds, runs `session/autostart.sh` each time SteamVR starts. The Frame starts SteamVR when it boots, so the setting covers boot too. The script waits for SteamVR's server and compositor, then runs `desktops.sh start`. The desktop quits when SteamVR does, so a SteamVR restart brings it back. If you close the desktop, it stays closed until SteamVR starts again. Its log: `journalctl --user -u frametop-autostart`.

`session/frametop-session.sh` runs the desktop. It starts ft-screens in the `dev` container (log: `~/.local/state/frametop/screens.log`, kept across starts and rotated at 8 MB to `screens.log.1` and `.2`), then KWin and Plasma on the host inside it. Only one Frametop desktop runs at a time. Its check doesn't look for Native Desktop, and running both at once is untested. `desktops.sh start` runs it in its own systemd unit, `frametop-desktop`. It keeps its Plasma config in `~/.config/frametop`, separate from the stock desktop's.

When the VR launcher starts the desktop, it inherits the Steam client's environment. The session script drops the client's runtime from it (`LD_LIBRARY_PATH`, the `STEAM_*` settings, and the Steam overlay's Vulkan layer), so apps in the desktop use the system's libraries, including its video codecs, just as they would after a normal login.

The nested session also starts an AT-SPI accessibility registry through `session/ft-atspi` in Plasma's autostart. It discovers the bus from this session, ignores an inherited host accessibility address, and leaves an existing registry alone. Accessibility errors do not stop the desktop. This supplies the registry infrastructure for apps that expose AT-SPI trees; it does not enable gaze snapping or force Chromium/Electron accessibility. After an approved desktop restart, an AT-SPI-aware app should be visible on the nested bus. See [design.md](design-session.md#nested-accessibility) for native activation, fallback lifecycle, and the isolated test command.

KWin's blur and background contrast effects and its animations are off in this desktop, because KWin draws on the headset's GPU, which SteamVR needs. The session script turns them off once, the first time it starts (it leaves a setting you already have alone, and marks it done in `~/.config/frametop/frametoprc`), so turning them back on sticks. In the Frametop desktop, System Settings → Window Management → Desktop Effects has Blur and Background Contrast, and General Behavior has Animation speed. Or from a terminal, then restart the desktop:

```
kwriteconfig6 --file ~/.config/frametop/kwinrc --group Plugins --key blurEnabled true
kwriteconfig6 --file ~/.config/frametop/kwinrc --group Plugins --key contrastEnabled true
kwriteconfig6 --file ~/.config/frametop/kdeglobals --group KDE --key AnimationDurationFactor 1
```

The desktop's notifications can show outside it, in the Steam session. Plasma there runs on the desktop's own D-Bus, so its popups only appear on the desktop's screens. `session/ft-notifyfwd` (Plasma's autostart; log: `/tmp/frametop-notifyfwd.log`) hands each one to the notification server on the user bus, the one the Steam session's apps use, for example a VR notification server that draws them as panels. Its buttons work: a click there reaches the app as if made in Plasma, and dismissing it there closes it in Plasma. While it forwards, Plasma's own popups are off (do not disturb, held by "frametop"), and Plasma's notification history still has every one. `NOTIFY_FORWARD` in `~/.config/frametop.conf` (or Notifications on the Visibility & pins tab of Frametop Display Settings) sets it: `auto` (the default) forwards only when the user bus's server isn't SteamOS's own `steam_notif_daemon`, which hands notifications to a Steam client that can't show them on the Frame, so with stock SteamOS nothing changes; `on` forwards to any server, SteamOS's too, where they're lost (it's in the file only; the settings app shows it while it's set); `off` keeps them in the desktop. Restart the desktop after a change. Notifications Flatpak apps send through the notification portal go along too. KDE's inline reply shows as a plain Reply button there; see [design.md](design-session.md#notifications-outside-the-desktop).

Apps that draw their own title bar, like Firefox and Chromium, show the same buttons as KWin's title bars (minimize, maximize, and close unless changed in System Settings → Window Decorations). `session/ft-gtksettings` hands them KWin's button order through the Settings portal; D-Bus starts it in this desktop (log: `/tmp/frametop-gtksettings.log`). Frametop's float button has no GTK counterpart, so those apps use the key or the window menu to float (see [Floating windows](#floating-windows)), or KWin's title bar instead of their own: Use system title bar and borders in Chromium's tab strip menu. Firefox gets KWin's title bar in this desktop by default: the session sets `MOZ_GTK_TITLEBAR_DECORATION=none`, so Firefox's automatic title bar setting picks the system one here, without changing its profile (outside the desktop, it keeps its own). Firefox can't draw its own title bar in the desktop then, whatever Title Bar in its Customize Toolbar says. `FIREFOX_TITLEBAR=firefox` in `~/.config/frametop.conf` gives it back that choice, its own title bar by default (restart the desktop after a change). See [design.md](design-session.md#title-bar-buttons).

Three of the system's autostart programs don't start in this desktop: Discover's update notifier (`org.kde.discover.notifier`), which starts Discover to check for updates, IBus (`ibus`), which can't reach the desktop's apps because KWin's input method is `input/ft-textinput`, and Steam (`steam`), which is already running. The session script puts copies with `Hidden=true` in `~/.config/frametop/autostart` once (marked in `frametoprc`; Steam's was added later and is hidden once on desktops that already had the other two), and skips a name you already have a file for. Delete a copy to start that program again.

Settings are in two files, and Frametop Display Settings edits both. The screens (resolution, width in metres, scale, curve, which one has the taskbar) and their layout are in `~/.config/frametop-layout.json`. The backend, remote desktop, and pointer settings are in `~/.config/frametop.conf`; `session/frametop.conf.example` lists every key, and [config.md](config.md) says how the file is written (what's quoted, and what happens to a line outside that).

Restarting the desktop closes its windows. Before the unit stops, `session/keep-apps.sh` moves every program started in the desktop into a systemd scope of its own, so background work such as servers, tmux, and builds keeps running. An app that shuts down its own helper processes when its window closes will still lose them; run that kind of work outside the desktop, for example as a systemd user service.

## ft-screens, the compositor

`screens/compositor.c` is a small wlroots 0.20 compositor that hosts the nested KWin, and `screens/vr.cpp` is its SteamVR side. `screens/build.sh` builds it; the installer runs that for you.

Each KWin window is one screen. ft-screens sets its size with an `xdg_toplevel` configure and KWin resizes the output to match, live. Frames arrive as DMA-BUFs and go to SteamVR through OpenVR's `IVRIPCResourceManagerClient::ImportDmabuf`, with no copy and no size limit.

Every screen is an overlay named `frametop.screen.N` with five controls:

- `.bar` moves the screen. Drag it with any laser or with the 3D mouse, whose right-drag tilts. Scrolling while you drag pushes the screen away or pulls it closer, along the line from your head.
- `.curve` bends the screen into a cylinder around you, using your current distance as the radius, or makes it flat again.
- `.roll` rolls the screen when you drag it sideways, like a knob. It snaps level within 2.5°, and scrolling on it turns 5° per notch.
- `.resize`, the handle on a corner, sets the size from there, the opposite corner staying put (a screen keeps its aspect; a floating window's panel stretches while you drag, and the window gets that many pixels when you let go). It sits on the bottom right, and goes to whichever corner a laser, the gaze or a hand's pinch comes near, brightening as it comes; lit blue while resizing. Screens go down to 15 cm wide.
- `.reset`, left of the bar, is the quick reset, like Meta+Shift+R (`ft-layout reset`). It shows a head-following three-second countdown, then puts the screens and remote displays back in their layout around where you are now, without reopening the profile's apps or changing windows, fullscreen, hidden screens, sizes, or streams. A panel pinned to the head with its pin button goes back in the room (unless the profile saved it pinned); one on a wrist stays. Opening the profile (frunner, or Open profile in Display Settings) restores all of it.

The controls are sized from both the screen's width and its distance from you, follow the surface of a curved screen, and stay invisible until a laser or the 3D mouse's cursor lands on one or comes within about 1.5 times a button's size of it. While invisible they're still there, fully transparent, so SteamVR's laser can find them. They're translucent until a laser is on them, like SteamVR's own window controls.

To pin a screen to a wrist, carry it by its bar and sweep the laser across your other controller. A ring around that controller marks the target, and a dot shows where the laser passes. Crossing the ring arms the pin, and the ring and bar turn blue; crossing it again disarms it. When you let go while armed, the screen rides on that controller at the size, distance, and angle it had, so you can arm the pin first and then turn the screen the way you want. Grab a pinned screen's bar to adjust it; it goes back to the same wrist when you let go unless you disarm it. A pinned screen shows only while you're looking at its front, within the wrist angle, and fades out over the last 10°.

To pin a screen to your head, like a HUD, press the pin button left of its reset button (it turns blue; press it again to leave the screen in the room where it is then), or set it to On your head on the Visibility & pins tab of Frametop Display Settings (or `ft-layout pin N head`). Remote displays have the button too. A layout reset ends a pin the button made: the screens go back as the profile has them, pinned only if it saved them pinned. It rides on the headset where it is at that moment, so place it first, and it shows whenever the screens do. Grab its bar to move it; it goes back on your head where you let go. Sweeping across a wrist ring while you carry it moves it to that wrist, and sweeping across again leaves it in the room. The 3D mouse's dot stays in the room, so a head-pinned screen moves away from it when you turn your head, unless head follow is on. With Pin only follows horizontal movement on (Visibility & pins, `SCREEN_PIN_LEVEL=1`), a head-pinned screen follows you turning and walking but stays level and at its height: looking up or down, tilting your head, or crouching leaves it where it is. Switching it changes screens already on your head where they are.

The Visibility & pins tab of Frametop Display Settings decides when the screens show:

- Always. Meta+Shift+H (the input relay's `screens_toggle`, rebindable in Frametop Input Settings), the Hide/Show Screens menu entry, or a mapped mouse or controller button hides them, and shows them again.
- Only while the SteamVR dashboard is open.
- While you look at a chosen controller (the wrist gesture).
- Only after you show them with the hotkey.

In the last three modes the hotkey shows the screens anyway. A screen can also be hidden on its own (Screens shown on the same tab, or `ft-layout hide N`): it stays hidden whatever the mode or the hotkey says, until it's shown again there. Windows on it stay put, and a new window that would open on it floats instead (ft-floatd). Profiles use this to show only some screens. Two more settings on the same tab cover VR games, which ft-screens detects as SteamVR scene apps:

- During VR games, the Always mode hides the screens unless the dashboard is open (the default), or leaves them up.
- Controllers on the screens. Visible screens can keep SteamVR's laser mouse on, so controllers work them with the dashboard closed, but that also takes the controllers away from a game. By default this is off while a VR game runs, and the 3D mouse or the dashboard works the screens. Pointing a controller at a screen, a floating window, or the keyboard still turns its laser on, like SteamVR's own floating windows, and pointing away gives the game the controllers back. The other choices are always on, or only with the dashboard open, which also suits flatscreen games since they aren't scene apps.

Input from the lasers reaches KWin through ft-screens' own seat. Keys come from the input relay: from pass-through keyboards, a keyboard's media keys (its Consumer Control node), and any key a pointer device passes through. A mouse button passed through as a key in pointer mode (a side button for Back) goes to the screen the pointer is on instead, wherever typing goes, and only while the pointer is on a screen that shows and Frametop isn't paused. Its release always goes through, and ft-screens releases it itself when the pointer leaves the screens, its screen hides, or Frametop pauses (`scripts/test-relay-buttons.sh` checks this offline). A key's release always reaches KWin too: when the screen with keyboard focus closes, focus moves to one that's left and the keys held come up there, and a release with no screen focused waits for the next focus (`screens/held-keys.h`). `keys` on `@ft_screens` says what's held, and `keys release` (the relay's Ctrl+Alt+Shift+Esc) lets go of every key. Typing follows your last click: after a click on a screen it goes to the desktop, even with the SteamVR dashboard open, and after a mouse click on any other panel (the dashboard, Steam, an app like Spotify) it goes there instead. While it goes to the desktop, the relay grabs pass-through keyboards so gamescope, which reads every keyboard itself, doesn't type them into the Steam app too. A program that watches every keyboard for a hotkey loses a grabbed one; bind its key as a key combination with a command instead (Frametop Input Settings, Keyboard). Frametop Voice types its text through the relay the same way, so it lands where typing goes. Hidden screens don't take typing.

Frametop's keyboard opens by itself when a text field on the desktop gets focus, and stays open until its X, a layout reset, or a mapped button closes it (or, with Keep it open off in Frametop Input Settings, until the text field loses focus). KWin starts `input/ft-textinput` as the desktop's input method, and KWin activates it whenever the focused app turns on text input for a field. It tells the relay (`textfield 1` or `0`), the relay decides by the Keyboard setting in Frametop Input Settings, and ft-screens opens the keyboard (`vrkeyboard show`, `hide`, or `toggle` from a mapped button). Its keys reach the focused screen as key presses, so it works in every app, but only apps that use Wayland text input (Qt, GTK, Firefox) open it by themselves; Chromium, Electron and X11 apps need the button. The session drops the `QT_IM_MODULE=xim` and `GTK_IM_MODULE=xim` that the gamescope session sets, or Qt and GTK apps wouldn't use Wayland text input either.

Frametop's keyboard is Full Keyboard for Steam Frame, by TaiKeid (`keyboard/`, MIT; [keyboard/FRAMETOP.md](../keyboard/FRAMETOP.md)), when it runs: the user service `frametop-keyboard.service` starts it with SteamVR (`framekeyboard --frametop`), and restarts it if it crashes. It has full layouts, languages and input methods, themes, and replaces Steam's own keyboard. ft-screens passes its `vrkeyboard` commands to it through the input relay. It's the only on-screen keyboard: while it isn't running (no word from it for 3 seconds), a text field or the keyboard button opens nothing, and ft-screens' log says `keyboard not opened (...): Full Keyboard isn't running`. A keyboard you plug in, dictation, and Steam's own keyboard in the dashboard still work then. (ft-screens' own key panel, a US layout only, was the fallback until the 2026-10-31 cleanup removed it.) It shows over the screens with the dashboard closed while Frametop asked for it, and in the dashboard as always. Its typing goes where typing goes: on the desktop, the relay types it on ft-screens' seat (keys by their place, so pick the desktop's layout in it too; text the desktop's layout has no key for, like Chinese or Japanese, isn't typed there yet), and Steam and games get it through gamescope as before. A click on its panel leaves typing where it was. Touch typing is on by default: touch a key with the front tip of a controller to press it, or poke it with a fingertip as on Frametop's screens, with no laser (turn the controller part off in its Settings, General, Touch typing). Its settings are in `~/.config/frametop-keyboard`.

```
keyboard/run.sh status | restart | log [lines] | install | uninstall
FULL_KEYBOARD_TESTS=1 keyboard/build.sh     # build it with its tests
```

KWin's nested backend doesn't undo a screen's scale on pointer input, so ft-screens divides panel positions (in pixels) by it. `ft-layout` sends it each screen's scale as KWin reports it (`scale N s`) whenever it applies scales: at desktop start and from Frametop Display Settings. A scale changed only in Plasma's own display settings is put back to the Frametop layout's the next time `ft-layout` runs.

ft-screens listens for datagrams on the abstract socket `@ft_screens` and replies to the sender:

```
place N x y z yaw pitch roll     width N metres          curve N radius|on|off
pin N|all left|right|head [matrix]    unpin N|all        size N w h
get N    screens    head    universe    state    key code value    keys    keys release    scale N s    vrkeyboard show|hide|toggle|close    fullkb 0|1
visibility always|dashboard|gesture|toggle    wrist degrees    gesture left|right degrees
hide | show | toggle    controllers always|outside_games|dashboard    ingames hide|visible    pause on|off|state
steamvrmode gamepad(0|1) theater-app-id ours(0|1)    gamepadpill
conceal N|all    reveal N|all    concealed    cutouts on|off|state    cutouts predict on|off    cutouts lead ms
cutouts matte circle|mesh|capsules    cutouts margin deg [ms [deg]]    cutouts shift off|spec    cutouts roomview auto|on|off
hands L R allowed    handinput on|off|force|state    palmmenu on|off|state    grabthat on|off|state
float N mpp x y w h title    unfloat N    pose N matrix    sub N k x y w h | sub N k off    minimized N 0|1    carry N    hold N ms
moving ID 0|1    torn N gx gy
rates focused in_view hidden    rates?    watch seconds    phase ms
spin next|prev|DEGREES|to N    front N    frunner on|off    spindistance M
```

Each screen draws at a frame rate for how much of it you see. KWin draws a screen only after ft-screens gives it a frame callback, and its apps wait for theirs, so the rate of callbacks is the screen's frame rate, for KWin and the apps on it alike. A screen is focused while you look at it (within 12 degrees of where your head points), while a laser or the mouse is on it or was in the last 1.5 seconds, while it's carried, and while you type on it; it gets every display frame. The rest of what you can see (within 60 degrees) gets 15 frames a second, and a hidden screen, one behind you, and everything while paused get one a second. A level goes up at once and comes down after a moment (1.5 s from focused, 0.5 s from in view). A video, or anything moving over a large part of a screen (6% or more of it, redrawn on 8 commits in a row, 10 or more a second), keeps every frame while in view. A floating window is a screen of its own here. Nothing that stands still costs anything at any rate: KWin sends a frame only when something on the screen changed. `rates F V H` sets the three rates in Hz (0: every display frame; default `0 15 1`, also `ft-screens --rates 0,15,1`), and `rates?` shows them, the display's rate, and for each screen its level, its milliseconds between frames, and whether it counts as a video. `watch S` gives every screen full rate for S seconds: remote desktop renews it while a VNC client is connected, since a viewer sees what KWin draws. The ticks (SteamVR events and the callbacks) come once per display frame, 1 ms after the vsync (`phase ms` changes that, for tuning), in step with the display rather than on a timer that drifted through the frame.

`spin` is the lazy susan. By default it brings panels to a front slot at reading distance and leaves the rest where they are: `next` and `prev` bring the next one round on the right or left (the one in the slot goes home), and `to N` brings panel N. With `SCREEN_SPIN_BRING=0` in `~/.config/frametop.conf` it turns every panel in the room about your head instead: `next` and `prev` bring the next one on the right or left to straight ahead, and `to N` turns the short way to panel N. A brought screen comes `SCREEN_SPIN_DISTANCE` ahead (1 m; 0.9 to 3; Frametop Input Settings, Hands, Gestures, which sends `spindistance M`). Push or pull one in the slot (the wheel while carrying it, or a carry straight out or in) and it comes back that far and as big in view each time, never nearer than frunner's search allows, until the reset button; a hand's push still sends it home. Either way, a number of degrees turns the room that far. Typing goes to the panel that ends up in front. `front N` gives panel N the typing where it is, as a spin to it would. `frunner on`, renewed every second, gives frunner's search the typing, and `frunner off` gives it back ([Frunner](#frunner)).

`conceal` and `reveal` hide and show one screen on its own (`ft-layout hide` and `show` send them), and `concealed` lists those screens. `pause on` (from the input relay, when Frametop pauses for a VR game) hides every screen and floating window whatever else says, and slows the desktop down; `pause off` undoes it. `cutouts` turns the hand cutouts on and off (`ft-handsctl cutouts`), and sets their matte, its margin and the per-distance correction (hands/README.md, "Hand cutouts and near input"); they cut only with Room View on, detected or forced (`cutouts roomview`, hands/README.md "Rendered hands"). `hands` is the pointer helper's word on hand input (each side `hand`, `controller` or `off`, and whether it's allowed now); `handinput` turns near input on (following that word), off, or on without it, and `handinput state` reports what each hand is doing, its counts (presses, one-frame presses, pinch clicks, grabs by card and fist, poke attempts and those refused, crossings off the content, presses at the edge, flicks and throws, holds through tracking loss, second hands joining a held panel, pinches that waited to become fists, buttons pinched) and what held presses back; each press, release, refused poke and grab also goes to ft-screens' log as a `hands:` line. `palmmenu` turns the palm menu (a bare hand palm up, pinch: the Steam menu) on and off, and `palmmenu state` reports it. `grabthat` turns "grab that" (an open hand held out to a panel farther than arm's reach, then a fist pulled back: it comes to arm's reach) on and off, and `grabthat state` reports each hand's state, its counts and the panels that came. The last line is ft-floatd's, for floating windows: N is a floating window's panel, numbered on from the screens, one per spare output. `float` gives the window's rectangle in its output, metres per pixel, and the title bar's height, and shows the panel; `unfloat` hides it. `pose` places it (a 3x4 matrix, standing universe), `sub` shows popup or dialog k over it, `minimized` hides it while its window is minimized, `carry` moves it with the laser that pressed the window's own title bar, `moving` is KWin's word that a window on a screen is being moved by its title bar (a pull on the wheel then tears it off), `torn` carries a torn-off window on with the laser that tore it, held at gx, gy in its pixels, and `hold` keeps its picture and crop for up to ms (0 to 2000; 0 ends it) while ft-floatd changes the window's scale or size: new frames wait, a `float` meanwhile goes up with the first frame 60 ms after it, so the in-between frames don't show.

## Input relay

SteamVR opens input devices only when it starts. A Bluetooth mouse that sleeps and reconnects gets new device nodes, SteamVR keeps reading the dead ones, and the mouse stops working until SteamVR restarts. `input/input-relay.py` avoids this. It creates two virtual devices, `frametop virtual mouse` and `frametop virtual keyboard`, through `/dev/uinput` before SteamVR starts. It then grabs USB and Bluetooth mice and keyboards as they come and go and forwards their events, so SteamVR only ever sees the virtual devices, which never go away.

It runs as the user service `frametop-input-relay.service`, ordered before `steamvr.service`.

The relay also owns the volume keys, on every device that has them, the headset's buttons included. It changes the volume itself (`wpctl`, 5% a step, repeating while held), and nothing else sees a volume key, gamescope and SteamVR included: on devices with a keymap (the headset's `gpio-keys`, USB and Bluetooth keyboards) it remaps just the volume entries to unused codes (`KEY_MACRO29`, `KEY_MACRO30`), so the headset's click button and the other keys still work, and it grabs `pmic_resin`, which has only volume down. The keymaps go back when the relay stops. With `--no-grab` it leaves the volume keys alone.

```
desktops.sh relay install     # enable it (starts with the next reboot or SteamVR start)
desktops.sh relay status | log | uninstall
input/input-relay.py --no-grab   # try it without taking devices from SteamVR
input/test/keys-test.py          # key combinations, modifier taps, dictation's and Full Keyboard's typing, and what reaches the desktop, against fake devices (safe next to the live relay)
input/test/roles-test.py         # each node's role, Keyboard + 3D pointer included, offline
input/rules-migrate.py           # one-time changes to a key_bindings list of your own (install and update run it)
input/test/rules-migrate-test.py # those changes, on files in a temporary folder
steam/ft-steam menu              # what Open Steam menu does; ft-steam check: Steam's UI still has the calls
```

The first time, the relay has to start before SteamVR, so reboot or restart SteamVR after installing it. After that it's safe to restart on its own: systemd keeps the virtual devices open in its file descriptor store (`FileDescriptorStorePreserve=yes`), so SteamVR keeps the same devices.

## The 3D mouse

A mouse drives SteamVR the way a controller's laser does, but shows up as a small dot anchored in the room. It snaps onto panels and works on the dashboard, Steam, overlays, and the desktop. Three pieces make it work:

- The input relay, in pointer mode (`POINTER=1`), sends mouse motion, clicks, and scrolling to the helper. A deliberate movement or a click wakes the pointer, and 30 seconds without mouse input releases it.
- The helper, `pointer/helper/ft-pointer`, runs in the `dev` container as `frametop-pointer.service` and starts with SteamVR. It keeps the cursor, tests it against every visible overlay, draws the dot, and sends the driver an exact pose.
- The driver, `pointer/driver/` (`ft_pointer`), is loaded by SteamVR. It's an invisible virtual controller whose laser follows the cursor. It takes the treadmill role, no hand, so the real controllers keep theirs and one can be used beside it (`POINTER_ROLE` and the driver's `role` setting; see docs/design.md, "Treadmill role").
- The dashboard patch, `pointer/dashpatch/` (`frametop-dashpatch.service`, starts with SteamVR), lets that treadmill move SteamVR's own windows by their grab bars, which SteamVR allows only for the head or a hand. It goes into SteamVR's dashboard page through the dashboard's debugger, which the installer turns on at 127.0.0.1:8089 (`VRWebHelper/DebuggerEnabled`; the uninstaller turns it off). `pointer/dashpatch/run.sh status` shows whether it's in. It also reads two things only that page knows and tells ft-screens (`steamvrmode ...`): whether SteamVR's controllers are in Gamepad mode (Enter Gamepad Mode in the Steam menu), and which Steam game shows in a theater window. A theater game counts as a game for `controllers outside_games`, and in Gamepad mode no panel turns SteamVR's laser on, so pointing at a screen doesn't take the pad from the game; the grips bring the laser back. Gamepad mode with a Frametop screen or the Full Keyboard last clicked (on top of SteamVR's focus stack) counts as laser mode, since SteamVR switches to Gamepad mode by itself when the Steam menu closes and the pad can't drive a screen.

Gamepad mode works for Frametop panels too, through SteamVR's own. With the Steam menu open, ft-screens shows an "Enter Gamepad Mode" pill above the panel typing goes to (a desktop screen, a floating window, or a remote display). Clicking it puts SteamVR's controllers in Gamepad mode for that panel; the grips bring the laser back, as in a game. SteamVR gives its Gamepad mode only to a dashboard overlay's owner, so ft-screens owns a hidden one, `frametop.gamepad` (no tab, and overlay flag `1 << 4`, which hands its page ft-screens' pid for VR input). The pill's click goes to ft-dashpatch (`gamepad` on `@ft_dashpatch`, from the same user only), which pushes SteamVR's input focus onto that overlay's page and closes the Steam menu, as SteamVR's own pill does for a tab that isn't in front. ft-dashpatch then tells ft-screens the Gamepad mode is Frametop's (`steamvrmode 1 <app> 1`: SteamVR's VR input goes to ft-screens' pid, with no Steam app's input in the way), and ft-screens tells the input relay `gamepad desktop|remote` every second (`gamepad off` as it ends). SteamVR sends an overlay no input in Gamepad mode, so the relay's controller reader (`input/game_pause.py`) reads the buttons, sticks and triggers raw from vrserver's web socket. A desktop panel gets them as a virtual Xbox 360 pad on uinput (`frametop virtual gamepad`, kept in the relay's fd store), and a remote display as a Moonlight Xbox controller on the host (`pad ...` through ft-screens to its ft-stream). The pad lets go while the dashboard is open, ends when Frametop pauses or a VR game starts, and buttons held as it starts wait for their release (`input/vrpad.py`). Steam also sees the virtual pad, as a controller; it gets no input from SteamVR while the pad is Frametop's.

Whichever device you used last wins. Picking up a controller hands the laser back at once, and moving the mouse takes it again. With hand interaction (`POINTER_HANDS=1`), this goes per hand: a controller used recently wins its hand, and the other hand can stay bare. When the headset comes off, the pointer lets go, so the displays can sleep, and it stays off until you're wearing the headset again.

To move a floating panel, left-drag its grab bar. The scroll wheel pushes and pulls it while you drag. Hold the right button while dragging and move the mouse to tilt the panel around the grab point; the right press isn't sent as a click. The tilt stays for the rest of the drag, and releasing the left button drops the panel as it is. A mapped Toggle dashboard button wakes the pointer if needed and holds the virtual system button for 0.12 s, because SteamVR ignores a press and release in the same instant. Open Steam menu / close dashboard (`steam_menu`) needs no pointer: `steam/ft-steam menu` asks Steam's UI, over its debugging port (`steam/steamui.py`), to show its dashboard overlay and focus the Steam frame's menu, or to hide the dashboard if it's up.

```
pointer/driver/build.sh && pointer/driver/install.sh install   # then restart SteamVR
pointer/helper/build.sh && pointer/helper/run.sh install
pointer/helper/run.sh status | log | restart
pointer/driver/install.sh probe     # devices, hand roles, activity, charging, who owns the dashboard pointer
pointer/probe/build.sh              # probes: vrprobe, lasertest (who has the laser), ctlnoise (controller noise)
```

The pointer settings are in `~/.config/frametop.conf`: `POINTER_SENSITIVITY`, `POINTER_IDLE`, `POINTER_WAKE_COUNTS`, `POINTER_CONTROLLER_PICKUP`, `POINTER_DISTANCE`, `POINTER_CURSOR_DEG`, `POINTER_ORIGIN_FRACTION`, `POINTER_ORIGIN_MARGIN`, `POINTER_SCENE_RADIUS`, `POINTER_EDGE_REACH`, `POINTER_LASER_WIDTH`, `POINTER_IGNORE`, the head follow settings `POINTER_FOLLOW`, `POINTER_LEASH_DEG`, `POINTER_LEASH_DELAY`, `POINTER_LEASH_RETURN`, and `POINTER_FOLLOW_REACH`, and the gaze mode settings `POINTER_GAZE`, `POINTER_GAZE_RETAKE`, `POINTER_GAZE_NUDGE_MAX`, `POINTER_GAZE_HOLD`, `POINTER_GAZE_CONTROL_HOLD`, `POINTER_GAZE_DOT`, `POINTER_GAZE_SHOW`, `POINTER_GAZE_MOUSE`, `POINTER_GAZE_MOUSE_MOVE`, and the keyboard clicks' `POINTER_HEAD_DEADZONE` and `POINTER_KEY_TAP`, and the gaze service's `GAZE_TRACKER` (`auto`, the default: our own eye tracker when it's installed, else SteamVR's; or `own` or `steam`), `GAZE_EYE` (the eye bias), `GAZE_FIT_WATCH` (the put-on check and the slip note, off by default: [gaze/README.md](../gaze/README.md), "Headset fit"), and `GAZE_FUSION` (our tracker's gaze mixed with SteamVR's, off by default: [gaze/README.md](../gaze/README.md), "Gaze pointer"), and our eye tracker's `EYES_SIZE_GUARD` (`both`, the default, `long`, or `off`: [gaze/README.md](../gaze/README.md), "Our own eye tracker"). The example config explains each. Frametop Input Settings changes them live; after editing the file by hand, restart the relay or the helper (the gaze service reads its four again when the file changes; ft-eyes reads `EYES_SIZE_GUARD` when it starts).

## Frametop Input Settings

Moved to [reference-apps.md](reference-apps.md#frametop-input-settings).

## Frametop Display Settings and ft-layout

Moved to [reference-apps.md](reference-apps.md#frametop-display-settings-and-ft-layout).

## Frametop Remote Displays

Moved to [reference-apps.md](reference-apps.md#frametop-remote-displays).

## Floating windows

Moved to [reference-apps.md](reference-apps.md#floating-windows).

## Frunner

Moved to [reference-apps.md](reference-apps.md#frunner).

## Frametop Tour

Moved to [reference-apps.md](reference-apps.md#frametop-tour).

## Displays off and sleep

Moved to [reference-features.md](reference-features.md#displays-off-and-sleep).

## Pausing for VR games

Moved to [reference-features.md](reference-features.md#pausing-for-vr-games).

## Voice dictation

Moved to [reference-features.md](reference-features.md#voice-dictation).

## Gaze pointer (experimental)

Moved to [reference-features.md](reference-features.md#gaze-pointer-experimental).

## Hand tracking (experimental)

Moved to [reference-features.md](reference-features.md#hand-tracking-experimental).

## Remote desktop over VNC

Moved to [reference-features.md](reference-features.md#remote-desktop-over-vnc).

## Limits

- A controller button can't show hidden screens; a mapped mouse or keyboard button can.
- KWin's cursor isn't drawn on the screens, because KWin draws it as a host cursor, which ft-screens doesn't render. The 3D mouse's dot and SteamVR's laser dot show where you're pointing.
- The old gamescope backend (`BACKEND=gamescope`) still works, but it gives every screen the same resolution, at most 1920×1080 pixels' worth, and arranging screens borrows the pointer for a few seconds.
