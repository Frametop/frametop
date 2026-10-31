# Frametop design

Why Frametop is built the way it is, and what we learned about SteamVR on the Steam Frame while building it. [reference.md](reference.md) describes the parts and how to run them. Read the SteamVR notes here before changing how the screens, the pointer, or the input relay talk to SteamVR.

## Goals

- Several desktop screens in SteamVR, each a real monitor with its own resolution and shape, placed anywhere around you.
- One physical mouse that drives a cursor through that 3D arrangement, and through the rest of SteamVR too: the dashboard, Steam, and other overlays.
- Controllers stay fully usable, and VR games aren't disturbed.
- Everything runs on the Frame itself, installed from a terminal on the headset.

## Screens

### Why our own compositor

SteamOS already shows a single-screen Plasma desktop in VR by running KWin nested inside gamescope. The first version of Frametop did the same with gamescope's `PerWindow` mode and `kwin --output-count N`, which gives each KWin output its own SteamVR overlay. It worked, but gamescope has two limits that ruled it out:

- It draws every window into one shared canvas of a single size, letterboxed, and never tells a window what size to be. So every screen had the same resolution and shape; portrait screens had to be rotated outputs on rolled panels.
- Its OpenVR backend allocates an upload buffer of exactly 1920 × 1080 × 4 bytes, so anything larger, such as 3440 × 1440, aborts it at start.

It also leaves the panels to SteamVR's dashboard, which places them and doesn't expose their position to other programs.

ft-screens replaces it. It's a small wlroots compositor that hosts the nested KWin. KWin's nested Wayland backend opens one window per output and resizes an output when the host configures its window, so ft-screens can give each screen any size, live. KWin needs `wl_compositor` v4 or later, `wl_shm`, `wl_seat`, `xdg_wm_base`, and `zwp_linux_dmabuf_v1` v4 with feedback from its host. KWin 6.6, which SteamOS build 20261009.6183636 ships, also refuses to start unless the host offers `wp_single_pixel_buffer_manager_v1`, `wp_viewporter`, `zwp_pointer_constraints_v1`, and `wp_presentation`. It draws each output's pixels on a synchronized subsurface and attaches a 1×1 single-pixel buffer to the toplevel, stretched to the output with viewporter. ft-screens presents the largest DMA-BUF in that tree, the toplevel's own included (KWin 6.2 draws there), and sends presentation feedback for the toplevel: KWin stops rendering after two frames without it. `xdg_toplevel_icon_manager_v1` and `zwp_keyboard_shortcuts_inhibit_manager_v1` stay optional. `scripts/update-check.py` fails when a newer KWin requires a host protocol this ft-screens binary does not name.

Frames go to SteamVR the same way gamescope sends them: OpenVR's `IVRIPCResourceManagerClient` imports each DMA-BUF (`ImportDmabuf`), and the overlay shows it as a `TextureType_SharedTextureHandle` texture. There's no copy and no size limit. The Frame's SteamVR supports this interface, but the OpenVR header bundled with SteamVR's samples predates it, so the build fetches a pinned header from Valve's openvr repository.

The small textures ft-screens draws itself (the controls, the hand glows, "grab that"'s halo, the carousel) go the same way, through `screens/overlaytex.cpp`: one linear DMA-BUF per distinct image, imported once and shared. Not `SetOverlayRaw`: the Frame's SteamVR never gives back the memory block each call takes, so a process's 201st call fails ("Refusing to create memory block because 201 blocks are already outstanding" in the journal) and the overlay keeps its old texture at its new size. That was the glow bug of 2026-10-15, after about 40 minutes of hand use. Other programs call it only a few times, or as a fallback.

Before settling on this, we tried KWin's screencast virtual outputs. KWin 6.2.5 crashed (`std::out_of_range` in `textureForOutput`) streaming one while nested in gamescope, and its headless backend doesn't implement virtual outputs at all.

A few wlroots details worth knowing: `wlr_shm_create` wants DRM format codes, not `wl_shm` ones, and KWin asks for server-side decorations before its first commit, when setting the mode would assert, so ft-screens answers on the first commit.

### Placing and sizing

Because the overlays are ours, ft-screens places them exactly with `SetOverlayTransformAbsolute` and draws its own controls under each screen. OpenVR has no overlay-relative transforms, so the controls are repositioned whenever their screen moves.

The curved layout chains screens edge to edge, like monitors on a desk: the middle screen (or the seam between two) straight ahead, and each neighbour hinged at the previous screen's outer edge and turned to face you. An earlier version spaced screens by angle, which assumes every screen sits on the circle; a flat 3.6 m screen's edges are much farther away than its centre, so its neighbours landed in front of it. Solving for the hinge angle needs a scan and bisection, because a simple fixed-point iteration diverges for screens nearly as wide as twice their distance.

`SetOverlayCurvature` takes the fraction of a full cylinder that the overlay's width covers, so the radius is width / (2π × curvature). The stated width is the arc length, and the cylinder bends toward the viewer: at a 2 m radius, a 3.57 m wide screen's corners sit about 75 cm closer to you and 23 cm further in than a flat screen's would. Controls on a curved screen are placed on that cylinder, and the bar gets the same curvature.

A resize handle has to be able to shrink a screen from any direction, so the dragged corner follows the laser along the screen's diagonal rather than taking the larger of its horizontal and vertical reach. Pushing and pulling a carried screen moves it along the line from your head, because the 3D mouse's virtual controller sits just in front of the bar, below the screen's centre, so the line from the device points mostly upward.

Wherever ft-screens needs to know where a laser points (showing the controls, the resize tab, the roll knob), it uses the laser's own pose, the render model's `tip` component, rather than the controller's pose. On the Frame's controllers the tip points 40° below the pose's forward axis, so rays from the pose missed what the laser was actually on. The 3D mouse's virtual controller has no tip, and its laser runs along its pose. ft-screens reads the tip with `GetComponentState`: `GetComponentStateForDevicePath` without an input source handle fails for every component while a VR game runs, so in games the rays came from the pose, 40° too high.

`ComputeOverlayIntersection` ignores `SetOverlayIntersectionMask`, and a control can't be allowed to cover part of its screen, so the resize tab sits entirely outside the corner.

What SteamVR hits isn't the texture's shape but the mouse scale's: an overlay is as tall, for SteamVR's laser and `ComputeOverlayIntersection`, as its width times the mouse scale's height over its width, and the default scale is 1 × 1. With it, the grab bar (a 256 × 24 texture) took hits in a square as tall as the bar is wide, so it caught clicks meant for the bottom tenth or so of the screen above it. Measured with a 0.2 m wide 256 × 24 overlay: a hit area 199 mm tall at the default scale, 18 mm at 256 × 24 (the bar itself is 18.8 mm), and no change from an intersection mask. `MakeChrome` sets each control's mouse scale to its texture size.

### When SteamVR loses the room

Every screen sits in SteamVR's standing space, so it stays put in the room only while standing does. When the Frame loses tracking, vrserver switches to its dummy universe (424242) and, once tracking is back in a new tracking world without relocalizing, to a new universe with a guessed floor ("default floor height without bounds": standing at (0, 1.6, 0), yaw 0, in `~/.config/openvr/config/chaperone_info.vrchap`). When it relocalizes, it restores the saved room, and standing jumps: vrserver logged three such restores on 2026-10-27 and 2026-10-28, and with the room's floor about 1.07 m under raw tracking's origin on this Frame, a guessed 1.6 m floor puts standing about 0.5 m off. Screens placed while the temporary universe was in use (the desktop starting, a reset, a carry) jumped with it, mostly up. The SteamVR dashboard doesn't show it, because it's placed in front of you each time it opens.

So ft-screens remembers, for each screen, the universe it was placed in (`screens/universefix.h`). It reads the HMD's `Prop_CurrentUniverseId_Uint64` every tick (a cached read: about 0.1 µs), and when it changes, the screens placed in the universe it left move by standing's jump, if that one was temporary, so they stay where you saw them. The jump is the head's: standing changes in one tick while the head hardly moves, so it's the one head step around the switch, within three ticks either side, that the head's own motion (the median step there) doesn't explain, turned about the vertical only. Screens placed in the room stay put, since the restore puts the room back where they were in it. A recenter keeps the universe, so screens follow a recenter as before. The front slot's home and parked remote displays follow with their screens. A universe the file doesn't list counts as a room, which leaves things as they were before the fix.

`ft-layout capture` and `save` ask ft-screens' `universe` first and refuse in a temporary universe: a layout is saved relative to the head, and in a temporary universe the screens placed in the room aren't where they are from it.

ft-screens' tracking watch (`screens/trackwatch.h`) logs standing and chaperone events, raw->standing moves, universe changes, and head jumps in standing, with wall-clock times to line up with `vrserver.txt`, and the fix logs each switch it handles (`universe A (temporary) -> B (room): standing jumped ...; moved screen(s) ...`). The log is `~/.local/state/frametop/screens.log`, kept across desktop starts and reboots and rotated at 8 MB (`screens.log.1`, `.2`). It was `/tmp/frametop-screens.log`, which is in RAM and was emptied at every desktop start.

### Pinning

Pinning started as "bring the screen to your wrist", which doesn't work for big screens, because their centre is far from the edge you bring close. It became aiming: while a screen is carried, the line from the carrying device to its bar is tested against the other hand controllers. Crossing a controller's 6 cm ring arms the pin (leaving past 9 cm, so it doesn't flicker), and crossing it again disarms it. The pin happens on release, with the screen's pose at that moment, so you can arm it and then turn the screen. An earlier version pinned the moment the laser touched the wrist, which left the screen at whatever angle the carrying hand had while pointing there.

A pinned screen's alpha follows the angle between its front and the direction to your head, fully visible inside the wrist angle and fading over the last 10°.

A head pin is the same pin on the headset (device index 0): the screen's transform is relative to the headset, so SteamVR keeps it rigidly in your view with no lag from us. It skips the facing rule, since a screen on your head always faces you the way it did when pinned. There's no aiming gesture for it: the line from the carrying device can't sensibly pass through your own head, and a ring in front of your face would be in the way. So it's set from Frametop Display Settings or `ft-layout`, and it pins the screen where it is. Carrying a head-pinned screen re-pins it on release, like a wrist pin, so it can be adjusted in VR.

A level head pin (`SCREEN_PIN_LEVEL=1`, "Pin only follows horizontal movement" in Display Settings) follows the head turning and walking, but not looking up or down, tilting, or crouching. SteamVR can only pin an overlay rigidly to a device, so a level pin is an overlay in the room that ft-screens moves every tick: its frame is on the floor under the head, turned with the head's forward laid flat (tilting doesn't change it; looking up or down only shortens it, and near straight up or down a little of the right vector takes over smoothly: `screens/levelpin.h`), and the pin's offset holds the height. The head pose is predicted for when the next frame shows (vsync to photons), since SteamVR moves device-pinned overlays with its own prediction and an unpredicted one trails a turn. It moves only shown level pins, and only when the frame moved by 0.2 mm or about 0.003 degrees, so a still head costs nothing; a turning head costs one pose read and about ten transform calls per pinned screen per tick. Outside ft-screens it's still a head pin: `get` reports it relative to the headset and `pin N head REL` takes it so, so profiles keep working either way, and switching the setting changes existing head pins over where they are.

### Named layouts

Named layouts are now profiles, which also hold which screens are hidden and which apps to open, with where their windows go. See [profiles.md](profiles.md).

A profile's screen part is the custom arrangement under a name: each screen's pose relative to your head, width, curve, and pin, but not its resolution or scale, which need a desktop restart or belong to KWin. Using one copies it into the custom arrangement, so everything that applies the layout (desktop start, Meta+Shift+R, Arrange now) works unchanged, and `active` remembers which name it came from. Saving without a name (`ft-layout capture`) clears `active`, because the screens have been placed by hand since. Layouts are kept per screen number, so one saved with a different screen count still applies: missing screens keep their last saved place or the preset's.

### Visibility and VR games

`VROverlayFlags_MakeOverlaysInteractiveIfVisible` keeps SteamVR's laser mouse on while an overlay with that flag is visible. Without it, the laser is off whenever the dashboard is closed: the first click on a panel only turns it on, and the laser turns off again as soon as it leaves every panel. With it, controllers work the screens normally, but the laser also takes the controllers away from a VR game.

`IVRApplications::GetCurrentSceneProcessId()` is 0 when no game is running (the Frame's home environment isn't a scene app) and the game's process ID while one is. ft-screens checks it twice a second, turns the flag off while a game runs, and by default hides the screens unless the dashboard is open. Flatscreen games run inside Steam's gamescope overlay and aren't scene apps, which is why "only with the dashboard open" is offered as a controller setting.

In a game, Frametop's panels work like SteamVR's own floating windows: point a controller at one and its laser comes on, point away and the game has the controllers again. ft-screens turns the flag on for a panel while a hand controller's laser pose meets it, its controls, or a floating window's popups. It finds that from the poses it already reads to show the controls, so SteamVR's laser doesn't have to be on first. Leaving takes a margin two control-sizes wide and 0.3 s, a drag or a held button keeps the flag on, and the keyboard, a single overlay, uses SteamVR's `ComputeOverlayIntersection`. The 3D mouse doesn't need any of this: it has its own laser mode.

## Floating windows

[floating-windows.md](floating-windows.md) describes the feature and its parts. Drag and drop and the clipboard only work between windows of one compositor, so a floating window stays a KWin window and gets a KWin output of its own: one of the spare outputs KWin opens after the screens, shown by ft-screens as a panel cropped to the window. What follows is how KWin 6.2.5 behaves underneath that, from its source (`src/backends/wayland/`) and from trying it on the Frametop desktop.

### KWin's nested outputs

- Disabling a nested output keeps its host window. `Output::applyChanges` only flips `enabled`, KWin stops rendering it (no more commits), and Plasma drops its desktop view. So ft-screens keeps the same toplevel, and its screen numbers stay put. Enabling the output again resumes on the same toplevel.
- Each output's host window is titled `KDE Wayland Compositor WL-<n>`, with `- Output disabled` appended while it's disabled (`WaylandOutput::updateWindowTitle`, on every `enabledChanged`). ft-screens reads the title to tell screens (`WL-0` to `WL-<SCREENS-1>`) from spares, and to see a spare turn on and off.
- A spare resized while it's disabled comes up at the new size on its first frame, so floating a window needn't blink. Outputs with gaps between them are accepted, so ft-floatd places spares apart from the screens and from each other: a grid below them, 4000 logical pixels a cell. Xwayland's coordinates are the layout's times its largest output scale, and they wrap past 32767, so an X11 window on a spare past that took no clicks (Brave on the 4th spare at x 27000 and scale 1.25 sat at x -31506; KWin still focused it). The grid ends at 20000 for 16 spares, within the limit up to scale 1.6.
- KWin keeps a Wayland popup inside its parent's output (`XdgPopupWindow::updateRelativePlacement` uses the output's placement area), and X11 apps place their menus within the monitor. That's why a floating window's output has a margin around the window: menus and dropdowns open past the window's edges, into the margin.
- KWin makes a nested output the size it's configured to times its scale, rounded (at 1.5 it lays out 1067 × 667 on a 1600 × 1000 buffer), and gives the buffer a whole buffer scale (1.2 becomes 2). A buffer whose size isn't a multiple of that is a protocol error that disconnects KWin, so ft-floatd sizes spares in multiples of it. After a scale change, ft-floatd asks for the output's size again in the new scale's terms, or the next configure would make it the old size times the scale.
- **Virtual outputs don't work.** `createVirtualOutput` makes an output window but never adds it to the backend's `m_outputs`, so `findOutput()` returns null when the pointer enters it, and the next line dereferences it (`Q_ASSERT` is compiled out). A click on such a panel would crash KWin. This rules out virtual outputs (`stream_virtual_output`) for floating windows without a patched KWin.

### The pointer

- Pointer positions reach KWin only through motion events: the output's position in the layout plus the position on its window. When ft-screens stops sending motion, KWin's pointer stays put.
- KWin starts an interactive move on the press itself, before any motion. So ft-screens stops sending motion as soon as a press lands in a floating window's title bar (from the frame and client rectangles ft-floatd sends it), with no round trip, and carries the panel instead. KWin gets the release at the press point, and the window moves by nothing on its output.
- KWin's nested backend ignores the position in `wl_pointer.enter`, and wlroots drops a motion to the position it entered at, so the first click after crossing onto another panel landed where KWin's pointer had been. ft-screens enters one unit off.

### The KWin script

The KWin side is a script (`float/frametop-float.js`), not a C++ effect, because a script keeps working across KWin updates and an effect would have to match the host's exact KWin build. KWin scripts can call D-Bus but can't serve it, so ft-floatd's commands come back through a long poll: the script calls `NextCommand`, which answers when a command is ready, or empty after 20 seconds, under KWin's 25-second D-Bus timeout. If no reply comes within 30 seconds, a watchdog calls again, and waits twice as long each time until a reply comes (up to 5 minutes). The 30 seconds have to stay above KWin's timeout, or a late reply would start a second poll. A few things about KWin's script engine:

- `windowAdded` reports popups as windows of their own (`popupWindow` true, `transient` true) with their geometry.
- Setting `frameGeometry` applies asynchronously: the app has to answer the new size first.
- A script can't read a window's maximize mode, so the script counts a window as maximized when it fills its output's maximize area.
- `globalThis` isn't defined. `print` goes to the journal unless `QT_FORCE_STDERR_LOGGING=1`.
- `callDBus` never calls back when a call fails (an error reply, the name gone from the bus, or the 25-second timeout). KWin only logs `Received D-Bus message is error`, so a script that waits for the callback waits forever.

The title bar's float button is Frametop's own window decoration (`decoration/`), written in QML for KWin's Aurorae engine, which loads it without compiling. A C++ fork of Breeze would have to match SteamOS's exact KDecoration build. A decoration can only make the window requests KWin offers it, so the button toggles keep-below, which has no visible effect on a window alone on its own output, and the script treats keep-below as the floating flag.

### KWin's placement memory

KWin keeps each window's geometry, full screen, and maximized state for each layout of the outputs (its `PlacementTracker`, keyed by every enabled output's name and geometry). When the outputs come back to a layout it has seen, it puts every window back as it was in it. That's for plugging monitors in and out, and it does harm here. A spare output changes size after its window does, so what KWin keeps for a spare's size is the window's next size. Resizing a floating window back to an earlier size made the window and its output flip between two sizes for good (Dolphin went between 1187 and 1424 logical pixels wide, its output between 1687 and 1925). Full screen flipped the same way, and floating or docking one window could move others, even onto a spare or off one.

So the KWin script keeps where each window belongs: where ft-floatd put it, or where it went outside an output change. While KWin changes the outputs, it reports nothing to ft-floatd. Once KWin is done (`screensChanged` comes after its restore), it puts floating windows back, and the screens' windows too when only spares changed. KWin's resize request hasn't reached the app by then, so the app never sees it. A size asked for is held for a second, since an app can still answer an older request, and then the script takes the size the window has. If `screensChanged` doesn't come within 2 seconds, the script stops waiting for it.

KWin also ends an interactive move or resize whenever the outputs change. So during a resize by a floating window's edge, ft-floatd only crops the panel to the window, and resizes the output when the drag ends. The margin is the room to grow until then.

## The 3D mouse

Moved to [design-input.md](design-input.md#the-3d-mouse).

## Input relay

Moved to [design-input.md](design-input.md#input-relay).

## The desktop session

Moved to [design-session.md](design-session.md#the-desktop-session).

## Displays off on a stand

Moved to [design-session.md](design-session.md#displays-off-on-a-stand).

## Pausing for VR games

Moved to [design-session.md](design-session.md#pausing-for-vr-games).

## SteamOS updates

Moved to [design-session.md](design-session.md#steamos-updates).

## Approaches we dropped

- WayVR, an existing Wayland desktop for VR. It built and connected to SteamVR on the Frame, but nothing showed in the headset. It has no bindings for the Frame's controllers, and its KDE screen capture needs `xdg-desktop-portal-kde`, which SteamOS doesn't ship.
- gamescope in `PerWindow` mode, for the reasons above. Frametop still supports it as `BACKEND=gamescope`. In that mode SteamVR's dashboard owns the panels, so `ft-layout` floats each one with `vrcmd --dock-overlay` and the pointer helper carries it into place with the virtual controller, hovering first and then sliding at 0.5 m/s, because the dashboard exaggerates fast movements.
- A capture pipeline from a headless KWin through KWin's screencast protocol. It's workable, but ft-screens gets the frames directly with less code.

## Open questions

- A controller button that shows the screens during a game. Games own the controllers, so this needs SteamVR input actions for ft-screens.
- Drawing KWin's cursor on the screens.
- Frame pacing and GPU cost with several busy screens haven't been measured.
- Whether the screen-jump fix (`screens/universefix.h`) catches every screen jump after walking away. It was built from vrserver's logs, not a logged repro of a jump; the tracking watch's lines and the fix's `universe` lines in `~/.local/state/frametop/screens.log` show the next one. A recenter saved per tracking world can also differ from a universe's own standing (the Room Setup origin) when tracking comes back in a new tracking world; screens follow that like any recenter.
- Real standby on a stand, with rendering and tracking paused, not just the backlight off. SteamVR has no call for it, and its activity level follows the proximity sensor.
