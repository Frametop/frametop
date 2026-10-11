// ft-pointer: the universal 3D mouse's brain (OpenVR overlay client, runs in the dev container).
//
// input-relay.py (pointer mode) sends mouse commands here; this program keeps the
// cursor, does collision against SteamVR's overlays, draws the free-space dot, and
// sends the ft_pointer driver the exact pose of its virtual controller.
//
//   relay -> @ft_pointer_helper -> ft-pointer -> @ft_pointer -> ft_pointer driver (inside vrserver)
//
// Cursor model:
//   - anchor: head position at the last recenter; yaw/pitch: direction from it (mouse-driven).
//   - Every frame a ray from the anchor is tested against every visible overlay
//     (ComputeOverlayIntersection). On a hit the cursor sits on that surface; otherwise
//     it floats `distance` metres out and a small dot overlay is shown there, which the
//     laser can hit, so SteamVR never draws a free-flying laser.
//   - Then the line of sight from the eye (not the anchor) to that point is tested too:
//     after the head moves, something nearer can cover the point, and the cursor goes on
//     whatever you see under it (panels close together in view, at different depths).
//   - Looks: the compositor ignores live changes to dashboard.laserRayWidthScale (only
//     the dashboard's own Settings screen reloads it), so the beam can't be switched
//     off per device. Instead the laser starts POINTER_ORIGIN_FRACTION (0.95) of the way
//     from the eye to the cursor, along the line of sight: what's left of the beam is a
//     few centimetres long and effectively invisible, and SteamVR's hit dot (sized by
//     distance from the origin) becomes tiny. Our own white dot is the visible cursor
//     everywhere: a non-interactive dot on panels (the laser passes through it), and an
//     interactive one in free space (the laser lands on it instead of flying off).
//   - The controller ray starts at the eye and aims at the cursor point. Everything here
//     is computed in the standing universe; the pose sent to the driver is converted to
//     SteamVR's raw tracking space (drivers report raw poses; on the Frame the standing
//     origin is ~1.6 m above the raw one, so sending standing coordinates put the laser
//     origin 1.6 m above the head). While our device
//     owns the dashboard pointer, dashboard.laserRayWidthScale is 0 so only the dot shows.
//     It's restored when a controller takes the pointer back.
//
// Headset off: when SteamVR says nobody is wearing the headset, the pointer is released and
// stays off until it's worn again, so the displays can sleep (see the main loop). While the
// pointer is off the helper also stops listing overlays with vrcmd, whose connection every
// second kept SteamVR from going to standby.
//
// Idle: while the pointer is off (and hand gestures are off), the main loop waits for a command
// on its socket for up to 250 ms instead of running every 8 ms, or 20 ms while it reads mapped
// Frame controller buttons (vrbuttons.h), which have no event to wait for. What has to go on
// meanwhile still does: the headset's activity level, the game check and the relay's "vrgame"
// and "gazeawake" repeats, and a mouse command wakes it at once. The overlay lookups (the 50 ms
// visibility poll, the once-a-second handles) wait for the pointer to wake, and run right then.
//
// Unchanged frames: while the pointer is awake, a frame where nothing moved reuses the last one's
// result. The collision passes (ComputeOverlayIntersection on every visible overlay, twice) run
// again when the mouse or the anchor moved, the eye moved 5 mm, an overlay showed or hid, or
// 100 ms have passed (overlays also move by themselves), and always while dragging. The dots'
// setters go to SteamVR only when their value changes, and a dot's placement only when it moved
// 0.2 mm or the eye 5 mm (its facing and size then change by well under 1%). The pose goes to the
// driver, which keeps the last one, when the laser's origin moved 0.2 mm or its direction 0.04
// deg (0.1 mm where it lands, 15 cm on), and at least every 100 ms.
//
// Last used wins: when a real controller moves (picked up), the pointer is released
// (driver "hide", which also drops its hand role hint), so the controller gets
// its role and laser back. The next mouse input reconnects and claims the laser again.
// Moving means faster than 0.35 m/s or 2 rad/s, both times POINTER_CONTROLLER_PICKUP,
// for 100 ms in a row with the controller tracked normally: a single sample over the
// limit was enough before, and controllers resting on a desk released the pointer on a
// knock or a tracking jump.
// SteamVR gives a contested hand role to the most recently used device, and a held
// Frame controller counts as used (touch sensors). If our device hasn't got the hand
// role within a second of waking, the pointer is released (no orphan white dot) and
// mouse input can't wake it again for 2 s.
//
// Laser mode: with the dashboard closed, SteamVR keeps its laser mouse off until a
// click (the first click on a panel only turned it on, the second one clicked), and a
// laser that leaves every panel turns it off again. While the pointer is awake, the
// helper shows frametop.pointer.lasermode: a transparent 1 mm overlay 50 m below the
// head with VROverlayFlags_MakeOverlaysInteractiveIfVisible, which keeps SteamVR's
// laser mouse mode on as long as it's visible. It's hidden whenever the pointer is
// released, so controllers and VR games get the normal behaviour back.
//
// Tilt: while the left button is held (dragging a panel by its grab bar, which SteamVR
// moves rigidly with the controller), pressing the right button enters tilt mode. The
// right press is not forwarded; mouse motion then rotates the virtual controller around
// the grab point (horizontal: about the vertical axis, vertical: about the view's
// horizontal axis), so the panel turns around that pivot. The tilt accumulates for the
// whole drag: after the right button is released, the rotation stays applied (about the
// moving cursor point) so the grabbed panel keeps its new orientation, and pressing right
// again continues from it. Releasing the left button drops the panel; the tilted pose (and
// the drag lock) are held 0.5 s longer, because SteamVR's dashboard finishes a floating
// move up to 150 ms after the release (UndockedOverlay.endFloatingWindowMove measures the
// push distance first) and reads the controller pose again then.
//
// Scene-graph overlays: the dashboard's dock (valve.steam.gamepadui.bar) and the controls
// under floating windows (valve.steam.gamepadui.floatingfooter, undock and friends) have
// no texture (0x0) and a placeholder width, so ComputeOverlayIntersection never hits
// them. For those the ray is tested against the overlay's plane, within
// POINTER_SCENE_RADIUS (0.5 m) of its origin; the laser-catching dot sits 5 cm behind the
// plane, so the laser reaches the buttons and still lands on the dot between them.
// Only absolutely placed 0x0 overlays count as scene-graph: gamescope's app panels (the
// desktops) also report 0x0, but they're placed as dashboard tabs, stay up when the
// dashboard closes, and ComputeOverlayIntersection hits them normally.
//
// SteamVR Settings (a workaround for that page only): Steam's pages (Library and the rest)
// are drawn in valve.steam.gamepadui.main, a dashboard overlay ComputeOverlayIntersection hits
// exactly. SteamVR's Settings page isn't: the main overlay is hidden, and the page is drawn by
// the scene-graph panel (valve.steam.gamepadui.frame.menu.N), whose shape OpenVR doesn't give
// out. Its transform's plane isn't the page's surface, which is nearer, so the laser, starting
// a few cm in front of where we thought the page was, started behind it: most of the page
// took no clicks, which went through to a desktop screen behind, and the page covered our
// dot. So while the cursor is on that page (OnSettingsPage), the laser starts near the eye
// (SETTINGS_ORIGIN) and SteamVR's own hit test finds the page; our dot is drawn close in front
// (SETTINGS_DOT), and the laser-catching dot sits far behind everything (SETTINGS_CATCHER),
// invisible and with SteamVR's hit dot hidden, so it can't cover the page. The beam and
// SteamVR's hit dot on the page then look like a controller's. Everywhere else nothing
// changes.
//
// Panel edges: off a panel, the cursor stays on that panel's plane while it's within
// POINTER_EDGE_REACH (0.3 m) of the last point it touched, instead of jumping to
// POINTER_DISTANCE. A floating panel's resize margins and the window controls under it
// sit just outside the panel, and the laser has to start in front of that plane to reach
// them (a controller's laser always does: it starts at the hand). Like on scene-graph
// planes, the laser-catching dot sits 5 cm behind the plane.
//
// Drag lock: while the left button is held, the cursor keeps the distance it had at the
// press and collision is frozen, so dragging past a panel's edge (resizing, moving)
// doesn't jump the cursor to free space or swap in the laser-catching dot, which made
// SteamVR's resize snap back.
// A left release also goes to ft-screens ("up"), which releases a button held on its
// screens in KWin if SteamVR gave the release to some other overlay.
// Across Frametop's panels (a drag and drop, or a window moved from one screen or floating
// window to another), the lock gives way: while the left button is held on one of ft-screens'
// panels, the ray is still tested against ft-screens' panels, and the cursor goes onto
// whichever it meets first. Not while that panel is being carried (its title bar or its bar):
// then the ray would find what's behind it.
//
// Head follow (experimental, off by default; POINTER_FOLLOW=1, or the relay's "follow toggle"): the cursor
// is carried by a reference direction, where the head faced when it last settled, and turns
// with it, keeping its offset (mouse movement changes the offset, up to POINTER_FOLLOW_REACH,
// 70 deg, so the cursor can sit in a corner of the view). While the head stays within
// POINTER_LEASH_DEG (10) of the reference, nothing moves on its own: the cursor stays put in
// the room. Once the head has been past the leash for POINTER_LEASH_DELAY (0.2 s; a glance
// out and back doesn't count), the reference follows: it eases toward the head's facing with a
// time constant of POINTER_LEASH_RETURN (0.2 s), never falling further behind than the leash
// (or than it already was), until it lands on the facing, and the cursor is back where it was
// in the view. Then it waits for the leash again. Earlier tries: dragging the reference only at
// the leash's end left it up to the leash off after turning back (getting it centred took an
// overshoot), and easing it all the time moved the cursor on every small head movement. At 0
// the reference is the head's facing, so the cursor is head-locked. Head roll is ignored (the
// frames have no roll), so tilting the head doesn't swing the cursor. The ray origin (the
// anchor) moves to the eye with the reference, so leaning inside the leash doesn't move the
// cursor either. While the left
// button is held (and the drop hold after it), the leash still moves the reference but the
// cursor stays put in the room, so a click or a drag can't be nudged by the head; the offset
// is taken up from where the cursor is when the hold ends, so it doesn't jump.
//
// Gaze mode (experimental, off by default; POINTER_GAZE=1, "gaze on|off|toggle", or the
// relay's gaze_toggle): the pointer goes where you look, and the mouse does the last bit
// (MAGIC pointing: Zhai, Morimoto and Ihde, CHI 1999). The gaze service (gaze/ft-gazed)
// sends the corrected gaze 90 times a second, "gz <yaw> <pitch> <raw yaw> <raw pitch>"
// (head-relative degrees), and while the gaze has the pointer, the cursor ray is simply
// that gaze from the eye: nothing is steered, so nothing can pile up. Moving the mouse takes
// the pointer from the gaze, and it moves from where the gaze left it, as usual. Looking
// well away from it (more than POINTER_GAZE_RETAKE, 5 deg, for 120 ms, with the mouse still
// for 300 ms) gives it back to the gaze; small eye movements around the pointer don't.
//   A left press while the gaze has the pointer isn't sent yet: the pointer stops where the
// gaze put it, and if the gaze is off, you drag it onto what you meant with the mouse
// (still holding the button; panels only see it hover). The release clicks there, a press
// and a release 40 ms apart. Held still for POINTER_GAZE_HOLD (0.5 s) instead, it becomes
// a real press where the pointer is, so drags work: hold, then move. After a click that
// didn't need correcting, and after a drag, the gaze has the pointer again.
//   Outside games (no scene application), gaze mode keeps the pointer: the relay doesn't
// release it when the mouse is idle ("gazeawake 1|0" tells it). A controller that moves
// still releases it, as without gaze (last used wins), and in games the mouse wakes it and
// idling releases it, as without gaze. Gaze mode is a mouse feature: the controllers aren't
// part of it. Steam reads the Frame controllers itself, outside SteamVR's bindings, so
// controller clicks at the gaze can't be done cleanly (docs/gaze-controllers.md).
//   The dot shows all the time in gaze mode (POINTER_GAZE_DOT=always, the default). With
// POINTER_GAZE_DOT=moving it only shows while the mouse moves it (within POINTER_GAZE_SHOW,
// 1 s), while a press is held, and briefly for each click (a pulse); otherwise it's
// transparent (still there for the laser to land on), since you know where you're looking.
//   When the mouse took the pointer and you then click, the nudge was probably onto what
// you were looking at: from the raw gaze when the mouse took over to where you clicked is
// the tracker's error there. The helper sends it to ft-gazed as a lesson ("lesson <raw yaw>
// <raw pitch> <true yaw> <true pitch>", the true direction relative to the head as it was
// when the mouse took over) if the mouse moved at least 0.2 deg, the click came within 10 s,
// and the correction (raw gaze to click) is within POINTER_GAZE_NUDGE_MAX (55 deg, half what
// the headset shows across: the Frame's eyes see 109 deg each). Past that, the tracker is far
// off (or you went somewhere else with the mouse): it isn't learned, and ft-gazed is asked
// for its quick check instead ("recheck <deg>"; it waits out its cooldown). Our tracker's
// clicks since its calibration put a one-dot check within 15 deg for the next 2 minutes and
// 25 for the next 10, so the check gets back under it (2026-10-01). A held press dragged onto
// the target is the same: from the raw gaze at the press to the release. With no
// fresh gaze (a blink, the service stopped, the headset off), the pointer stays put.
//
// Gaze precision (the relay's gaze_precision and gaze_drag actions, bound to a mouse button or
// a key combination; "precision|gazedrag mouse|keyboard 1|0" here): gaze mode aims, the button
// makes it exact. Pressing gaze precision stops the pointer where you look, as gaze mode's
// mouse press does; while it's held, the mouse steers the pointer by its moves. Releasing
// clicks where the pointer is. A correction is a lesson for the gaze tracker, as with the
// mouse. Gaze drag is the same with a real press at once, dragging until the release, for
// title bars, grab bars, and selections. Without gaze mode both still work from wherever the
// pointer is.
//   In gaze mode the mouse's left button is a gaze precision button (POINTER_GAZE_MOUSE =
// precision, the default), or clicks at once where the pointer is (direct). With precision the
// mouse's buttons work like the keyboard clicks: the right button's press is held back the same
// way, and the right click comes on the release, where the pointer is by then (gaze_right).
// Pressing the right button while the left one's press is held back presses the left button
// where the pointer is now, and the mouse drags (gaze_left then gaze_right): the drag lasts
// while either button is held. So once you've moved the pointer, the left button alone only
// clicks; to drag from there, press the right one. Pressing the right one again (a double right
// click, with the left still held) tilts, as a right press does during any drag (see Tilt).
// Held still for POINTER_GAZE_HOLD, either press is a real one.
//   POINTER_GAZE_MOUSE_MOVE: held (the default) or free. Held: while the gaze has the pointer,
// moving the mouse does nothing; it moves the pointer only during a press (as a correction,
// like a keyboard click's head). So a bumped or drifting mouse can't pull the pointer off what
// you're looking at, and every mouse move is a correction worth learning. With the gaze stale
// for a second (the tracker stopped, eyes closed), in a game, or the headset off, the mouse
// moves the pointer as usual. Free: the mouse takes the pointer whenever it moves.
//   Keyboard clicks (the relay's gaze_left and gaze_right, Meta+J and Meta+K by default;
// "gazekey left|right 1|0" here) work like gaze mode's mouse press, steered by the head: the
// press stops the pointer where you look, and while the keys are held the pointer stays put in
// your view, so turning your head carries it onto what you meant (past
// POINTER_HEAD_DEADZONE, 0.5 deg, so a still head doesn't wobble it). The release clicks there
// (left, or right for gaze_right), and a correction is a lesson for the gaze tracker, as with
// the mouse. Held still for POINTER_GAZE_HOLD instead, it's a real press, and the head drags.
// A quick tap (let go within POINTER_KEY_TAP, 0.25 s) clicks where the dot was at the press,
// whatever the head did meanwhile, and tells the gaze tracker it was right there (a lesson
// with no correction). Pressing gaze_right while gaze_left aims (Meta+K with Meta+J held)
// presses the left button where the dot is now, so you can correct first and then drag; the
// drag lasts while either key is held. gaze_right pressed during a gaze_left drag (held still
// into one, or again after starting one with it: a double Meta+K) tilts while it's held (see
// Tilt): turning the head turns the panel, and the mouse can too; let go of it and the head
// drags again from there. Without gaze mode they work from wherever the pointer is.
//   The gaze calibration panel (gaze/panel/ft-gazepanel, run by the gaze service): while
// ft-gazed says it's up ("calpanel 1", renewed every second; it lapses 3 s after the last),
// the dot hides and a press answers the panel instead of clicking: a left click, gaze_left, or
// a gaze_precision or gaze_drag press sends "calaccept" to @ft_gazed (take this dot now), a right
// click or gaze_right "calquit" (calpanel.h).
//   POINTER_ROLE (treadmill, the default, or right, left, stylus): the role our device takes while
// connected. Treadmill is no hand, so the real controllers keep both hand roles and a controller
// can be used beside the pointer (or a bare hand's ray). The driver must hint it from the start
// (its default.vrsettings role 4, or driver_ft_pointer/role; a SteamVR restart to change), so this
// setting can't make a device added as a hand into a treadmill. With a hand role, a Frame controller
// in your hand counts as used through its touch sensors and takes its hand's role back, and then
// no click lands (see "no hand role" in the main loop): with a controller held in the right hand,
// the pointer needs the left hand, or the stylus role. docs/design.md, "Treadmill role".
//
// Hands (POINTER_HANDS, off by default; needs hand tracking, hands/): ft-hands publishes
// pinches and grips (hands/include/fh_gestures.h), read here every frame.
//   In gaze mode a pinch clicks where you looked, and held and moved, its hand nudges the pointer
// first (see "Gaze pinch" below). A pinch ended by losing the hand (or by a grip taking over)
// doesn't click.
//   Without gaze mode, a pinch is a real press, like the mouse's button: pressed when it closes
// and released when it opens, and held, its hand drags the pointer: past POINTER_PINCH_DEADZONE
// (1.5 deg of hand movement, seen from the eye; a tap's jitter and the pinch point shifting as the
// fingers close stay inside it), at POINTER_PINCH_GAIN (0.5: half the hand's angle, for
// precision). A tap is still a click where the pointer is. That's what the
// gaze probe's Click practice wants: it has its own gaze dot, frozen by the press and dragged
// by the pointer's movement until the release.
//   A grip (closing the hand) is a press and drag: the press where the pointer is, then the
// hand moves the pointer at POINTER_GRIP_GAIN (1: as far as it moves, seen from the eye), and
// opening the hand releases. So it drags whatever the pointer is on: a title bar moves the
// window, a panel's grab bar carries the panel, text is selected.
//   A grip begins only with the hand held up, at most POINTER_GRIP_BELOW (0.35 m) below the eyes:
// hands on a desk curl like a loose fist. (The user's own pinches sat 0.35-0.45 m below the eyes,
// elbow resting, so pinches have no such limit.)
//   The hand's movement is taken in the room, from where the eye was when the gesture began,
// with the head pose at each frame's capture time, so turning your head doesn't move it.
// The first gesture while the pointer is off only wakes it. Gestures are ignored in a VR game
// (unless the dashboard is up) and with the headset off, and while the mouse's button is held.
// Hand use keeps the pointer from the relay's idle release for two minutes, as gaze mode does.
//   ft-screens' direct touch (poking and grabbing its panels, HANDS_NEAR) is a hand input of its own:
// it needs the handoff's "hands" word below, which goes out with POINTER_HANDS=1, or with
// HANDS_NEAR=on without it (then the helper runs only the handoff and takes no gestures). HANDS_NEAR=1,
// the default, keeps today's meaning: direct touch with POINTER_HANDS.
//
// Handoff (hand interaction, with POINTER_HANDS; frame-hands/notes/hand-interaction.md section 1):
// per side, a controller used recently wins, otherwise the bare hand does (handoff.h has the
// rules). The input relay sends who touches or presses a controller ("ctl <side> touch 0|1",
// "ctl <side> press", "ctl <side> gone", "ctl ws 0|1", read from vrserver's web socket), the helper
// adds each controller's motion, charging and tracking, and the hands from ft-hands' hands file
// (handsin.h). Transitions go to the log ("handoff right: controller -> hand (no touch 1.5 s,
// still 3.0 s)"). ft-screens gets "hands <L> <R> <allowed>" on @ft_screens, on a change and every
// second: L and R are hand, controller or off (no hand tracking and no controller in use), and
// allowed is 1 when hands may work at all: the headset worn, and outside VR games, or in one with
// the dashboard open or Frametop toggled back on (the relay's "paused 0"; it sends "paused 1|0"
// on a change and at the helper's start). The relay also hears "vrdash 1|0" (the dashboard open, on a
// change and every 5 s in a game): it parks hand tracking while Frametop is paused for a game and
// unparks it while the dashboard is open (input/game_pause.py).
// ft-handview (screens/handview.cpp) gets the same "hands" on @ft_handview: it doesn't draw a hand
// holding a controller.
//
// Hand ray (hand interaction, with POINTER_HANDS and POINTER_HAND_RAY=1, the default;
// frame-hands/notes/hand-interaction.md sections 2 and 5): a bare hand raised and pointing shows a
// ray from it, Quest-style, and the ray drives our device, so it works on the dashboard, Steam's
// pages, overlays and Frametop's panels like a controller's laser. The model (body, shoulders, the
// pointing tests, filters) is hands/include/fh_ray.h, shared with ft-screens. Per side, from the
// hands file at each new frame; a side whose controller is in use (Handoff), hands not allowed (a
// game), or the mouse used within a second, has no ray. Each side's ray follows one tracked hand
// through a brief flip of ft-hands' left/right call. Which ray drives the pointer is rayowner.h's:
// the first ray to show; its owner keeps it through POINTER_RAY_GRACE (0.5 s) of its ray being down
// (the pointer holds that ray's last pose; closing the fingers can drop a ray for a moment), and
// the other hand takes it by pinching, by raising its ray while the owner's is down, or once the
// grace runs out. Changes go to the log ("hand ray: right -> left (left pinched)"), and so do the
// owner's ray going down and coming back.
//   A ray appearing wakes the pointer (role, show) and claims the laser, unless a controller that
// pressed a button within 2 s has it. A controller's trigger then takes the laser natively: our dot
// hides (rayInactive) until a ray pinch claims it back. The pose is the ray's own origin and
// direction (the beam shows, at POINTER_LASER_WIDTH), not the eye-to-cursor trick; our dot shows
// only where the ray meets a panel, and nothing catches the laser in free space.
//   Gestures by a hand whose ray is up, or may come up for it (it dropped within the grace, or is on
// its way up: fh_ray.h's ShowForPinch; it then shows), are ray gestures, decided once at the
// gesture's begin. A pinch by the other hand takes the ray and presses along its own ray. A pinch
// is the left button, pressed along the ray as it was just before the fingers began to close (the
// aim lock, rayowner.h), once a pose along it has gone out and 30 ms after a claim it had to make
// (the claim lands in 11-15 ms), held as long as the pinch, so a tap clicks and a hold drags; a
// tap that ends before its press could go out clicks then (released 40 ms on). The direction holds
// still at the lock until the ray has turned POINTER_RAY_DEADZONE (2.5 deg) from where it was at
// the begin (the evaluation saw ray shifts of 1.3-1.6 deg median, 2.4-3.5 at the 90th percentile,
// 150 ms into a pinch), then follows at gain 1. Each pinch logs one line: what it did ("pinch
// right: clicked on frametop.screen.0 (right ray, aim 100 ms before, held 0.15 s)") or why not. A middle
// pinch (thumb to middle finger, fh_gestures v3) is a right click on a tap; held and moved, it
// scrolls: the virtual stick deflects with the hand's displacement from the begin (0.5 cm dead zone,
// full at 4 cm; up and down, and across the ray), times POINTER_RAY_SCROLL (1; -1 turns it round).
// A grip with the ray up does nothing. With the ray down, pinches and grips are gaze gestures, as
// below ("Hands"). A ray stays up while its pinch is held, and 250 ms after.
//   Smoothing (fh_ray.h, fh_smooth.h): the ray is read at this loop's time (At), not at the tracker's
// frames: its One Euro filters run in continuous time, so the pose sent to the driver moves every
// loop (about 100 Hz) instead of jumping at 15-30 Hz, which the dashboard's dragger turned into jerks.
// POINTER_RAY_SMOOTH_MINCUTOFF (0.8 Hz still), POINTER_RAY_SMOOTH_BETA (0.06 Hz per deg/s),
// POINTER_RAY_SMOOTH_HOLD (0.5 Hz still while a pinch is held). On the probe-moves replay (rayeval)
// the pose's second difference per 90 Hz tick went from 0.90-0.96 deg rms (p90 1.4) to 0.24-0.26
// (p90 0.24), at the same lag (110 ms behind the capture against 105). POINTER_RAY_STEADY (1): during
// a ray pinch the ray pivots on where the shoulder was at the press (fh_ray.h's steady shoulder), so
// looking around doesn't move what's dragged; push and pull's reach is filtered too.
// POINTER_RAY_TORSO_WRISTS (1): the shoulders face between the head and the hands seen (StereoKit's
// body model), so looking aside while pointing swings them less. POINTER_RAY_HAND_WEIGHT (0, 0..1):
// how far the ray turns toward the hand's own direction (StereoKit's aim blend; POINTER_RAY_HAND_PITCH
// -30 and POINTER_RAY_HAND_YAW 8 deg from the palm's axes), and POINTER_RAY_ANCHOR (0): the knuckle
// point pinned to the palm during a pinch (StereoKit's pinch anchor). Both measured worse on
// replays, so they're off (fh_ray.h has the numbers). A pinch's first POINTER_RAY_TAP (0.3 s) needs
// POINTER_RAY_TAP_DEADZONE (6 deg) of turn to drag, so a quick tap whose hand swung is still a click;
// then POINTER_RAY_DEADZONE. A held ray pinch ends on the pinch's end, the pointer going, or the
// gestures file unreadable for 0.5 s (one failed read under its sequence lock ended drags), and the
// log says which.
//   Push and pull (POINTER_RAY_PUSH=1): while a ray pinch carries something (it pressed on no
// panel's content: a grab bar, a scene-graph control, a panel's edge, one of ft-screens' controls;
// or the panel it pressed on starts to move, as a floating window by its title bar), the origin
// moves along the ray: the carried panel's distance d = d0 (h / h0)^3, where h is the reach
// (shoulder to knuckles) and h0, d0 those at the press, past 1.5 cm of reach, within 0.3-10 m.
// Both ft-screens and the dashboard follow our device rigidly, so the panel goes with it. The
// offset holds through the drop hold after the release.
//   ft-screens' near claims: "near <capture_ns> <L 0|1|2> <R 0|1|2>" says, for each hands frame it
// processed, which hands are near its panels: 1 within its touch zone (poke, pinch, fist: the
// gestures are its), 2 only in the zone's margin (the ray stays hidden so it doesn't flicker at the
// edge, but the gestures stay ours; older ft-screens send only 0 and 1). While those reports come,
// a gesture waits up to 60 ms for the report on its begin's frame and is left to ft-screens if its
// hand was claimed (1); a near hand (1 or 2) has no ray. The fingertip at any overlay (a cast from
// 20 cm behind the index tip along the finger, meeting one within 25 cm) counts as near too.
//   A ray that woke the pointer lets it go POINTER_RAY_RELEASE (2 s) after it went, with no mouse
// use and no gaze mode meanwhile. A ray that goes because its side's controller is in use or hands
// aren't allowed lets it go at once, unless the mouse is in use. In gaze mode the gaze takes the
// pointer back when the ray goes.
//
// Typing (with POINTER_HANDS, or HANDS_NEAR=on for ft-screens' side alone; typing.h has the rules,
// docs/hands-typing.md what they came from): a hand on a real keyboard touches thumb to index like
// a pinch, curls like a grip, and sits where the hand ray's raised test begins. The relay says
// "typing" on a key. For POINTER_PINCH_TYPING (2 s) after one, every hand is held back except one
// that has left the keyboard, and a hand that was on the keys stays held back, through any pause,
// until it rises POINTER_TYPING_RAISE (0.08 m) above where it typed or turns its palm up. Both
// hands down on the keys together (the keyboard posture, POINTER_TYPING_POSTURE, 0.3 s) count as
// typing before the first key, within POINTER_TYPING_RECENT (600 s) of one. A hand held back makes
// no pinch, middle pinch or grip ("pinch right: ignored (typing: the hand is on the keyboard)"),
// and has no ray: one that drove the pointer lets it go to the mouse or the gaze ("hand ray: right
// -> none (typing)"), and it can't claim it again until the hand is lifted. ft-screens gets "typing
// <L 0|1> <R 0|1>" on @ft_screens, on a change and every second: no pokes, near grabs or "grab
// that" by those hands. Changes go to the log ("typing: right hand on the keyboard", "typing: right
// hand off the keyboard (raised 0.09 m)").
//
// Gaze pinch (gaze mode with POINTER_HANDS; gazehand.h has the rules and the timings): the eyes aim
// and the hands click, Vision Pro style. In gaze mode the hand ray stays off unless
// POINTER_GAZE_HAND_RAY=1 (then a raised, pointing hand's pinches are still ray pinches, as above),
// so pinches and middle pinches are gaze pinches. ft-screens' near poke and grab, and its palm-up
// Steam menu, are the same as without gaze (its near claims come first).
//   A pinch holds its press back at the gaze from just before the fingers began to close (the gaze
// lock: the newest gaze that came 75 ms or more before the begin's capture), and the pointer goes
// there, off the gaze. A tap clicks there when it opens. Held and moved, the hand nudges the
// pointer, with low sensitivity: nothing moves until the pinch point has moved
// POINTER_GAZE_PINCH_DEADZONE (4 deg, seen from the eye; about 3 cm at arm's length), then the
// pointer follows at POINTER_GAZE_PINCH_GAIN (0.3 times the hand's angle) from where it is, so it
// doesn't jump as the nudge engages. The release clicks where the pointer is, and the correction
// (the locked gaze to the click) is a lesson for the gaze tracker, or past POINTER_GAZE_NUDGE_MAX a
// quick check, as with the mouse.
//   Held still (inside the dead zone) for POINTER_GAZE_HOLD (0.5 s), the pinch becomes a real press
// at the locked point, and the hand drags (at POINTER_GRIP_GAIN, 1, past 0.5 deg) until it opens.
// The hand drives the drag, not the gaze, as the mouse and the head do in gaze mode's other holds:
// the eyes jump about and look ahead to where a thing should go, so a dragged window would jump
// with them, and a drop would land wherever you last glanced. A pinch that has nudged only clicks.
//   A middle pinch is the right button the same way: a right click at the locked gaze, nudged
// first if moved; it never becomes a press. The rules for all gestures hold (hands allowed, typing,
// a controller on that side, the near claims).
//   Chord: while one hand's pinch is held (held back, nudged, or pressed), an index pinch by the other
// hand presses the right button and holds it until that pinch ends, so both buttons are down, as when
// both of a mouse's are held (here they reach the panel, while the mouse's right press during a left
// one tilts: see Tilt). A first pinch still held back becomes a real left press at once, where the
// pointer is (the lock, or where its nudge took it: the nudge is a lesson, as for a click), without
// waiting out POINTER_GAZE_HOLD; the right goes down 30 ms later, so the left lands first. Each pinch's
// end releases its own button. The first hand drags while it's held; if it lets go first, the second
// hand drags on from there. A quick second pinch is a right click during the left's press. A chord's
// hand lost (or the pointer going, hand input off, a controller on its side, the calibration panel)
// releases its button. A middle pinch by the other hand, or a third pinch, is ignored, and so is one
// by a hand typing holds back (see "Typing": it's checked before a pinch can be the chord's). gazehand.h
// (Chordable, Pinch::Chord, ChordButton) has the rules; its log line: "pinch left: the right button with
// the other hand's press, on ..., let go (held 0.80 s)".
//   POINTER_GAZE_PINCH=0 turns gaze pinches (and grips in gaze mode) off: in gaze mode a hand then
// clicks only by direct touch, or by a ray with POINTER_GAZE_HAND_RAY=1. Without gaze mode it does
// nothing.
// Each pinch logs one line: "pinch right: clicked at the gaze on frametop.screen.0 (gaze 80 ms
// before, held 0.18 s)", "nudged 2.4 deg, then clicked on ... (a lesson; ...)", "became a press on
// ... (held still 0.50 s; ...), dragged 6.0 deg, let go", or why not.
//
// Placement (for layout): SteamVR keeps a floating panel's position inside the
// dashboard, where nothing outside can set it, so the helper carries panels like a user
// would. It measures the panel (md::ScanPanel), aims the device at its grab bar
// (LAYOUT_GRAB_OFFSET, 7.5 cm below the bottom edge; the bands at 2-4 and 14-26 cm are
// other controls), presses, moves, and releases. While grabbed, the panel follows the
// device rigidly, except that the dashboard accelerates fast translations (0.1 m in 0.3 s
// moved it 0.19 m and turned it 8.5 deg, in jerky 25 ms steps right after the press). So
// the device hovers first, and the move is split into a rotation about the device origin
// (the eye) at 60 deg/s and a smooth 60 Hz slide at LAYOUT_SLIDE_SPEED (0.5 m/s; tested
// exact from 0.07 to 1 m/s). Scroll pushes along the panel normal, but only in whole notches of
// about 7 cm, so it isn't used. The result is measured again, and the move repeated up to
// twice while it's more than 1.5 cm or 1 deg off.
//
// Ignored panels: overlays matching POINTER_IGNORE are left out of the collision, so the cursor
// passes through them to what's behind. For display-only panels in the way, such as a
// head-locked performance overlay, which ComputeOverlayIntersection hits like any other. The
// laser starts just before the cursor point (see Looks), so a panel nearer to you doesn't
// catch it either.
//
// Commands (datagrams on @ft_pointer_helper): show, hide, recenter, move <dyaw> <dpitch>,
// follow on|off|toggle (head follow, until the next restart or a change to POINTER_FOLLOW),
// gaze on|off|toggle|? (gaze mode, likewise with POINTER_GAZE; ? only asks, and "? headset" adds
// worn|away to the reply), gz ... (the gaze, from ft-gazed),
// reload (re-read the settings below), debug (toggle a twice-a-second state log),
// overlays (replies with the overlay list as JSON, see OverlayList),
// vrbind/vrglobal/vrstatus (Frame controller buttons, see vrbuttons.h),
// ctl ... and paused 0|1 (the relay, see Handoff), near <capture_ns> <L 0|1|2> <R 0|1|2> (ft-screens:
// which hands are near its panels, for the hand ray and the gestures),
// and btn/scroll lines, which are forwarded to the driver unchanged. For layouts, with a
// reply datagram to the sender's (abstract) address:
//   place <overlay> <x> <y> <z> <yaw> <pitch> [roll [grab]]: centre in the standing
//     universe; the front faces back along the direction (yaw, pitch), turned by roll
//     (counterclockwise as seen, degrees) -> "ok ..." | "error ..."
//   measure <overlay> -> "ok cx cy cz width height xx xy xz yx yy yz zx zy zz" (centre,
//     size, and the panel's right, up, and front vectors)
//   head -> "ok x y z yaw pitch"
//   grabprobe <overlay>: log where below the panel SteamVR's laser hits something (to
//     find the grab bar again if a SteamVR update moves it)
//
// Settings (~/.config/frametop.conf): POINTER_DISTANCE (m, 1.5), POINTER_CURSOR_DEG
// (angular size of the dot, 0.4), POINTER_LASER_WIDTH (controller beam width to restore, 0.8),
// POINTER_ORIGIN_FRACTION (0.95): the laser starts this far along the eye-to-cursor line,
// but never closer than POINTER_ORIGIN_MARGIN (0.15 m) to the cursor point: SteamVR's
// small controls (undock, frame buttons) float a few centimetres in front of their
// panel, and a laser that starts behind them can't hit them. POINTER_FOLLOW (0) and
// POINTER_LEASH_DEG (10), POINTER_LEASH_DELAY (0.2 s), POINTER_LEASH_RETURN (0.2 s),
// POINTER_FOLLOW_REACH (70 deg): head follow, above. POINTER_GAZE (0), POINTER_GAZE_RETAKE
// (5 deg), POINTER_GAZE_NUDGE_MAX (55 deg), POINTER_GAZE_HOLD (0.5 s), POINTER_GAZE_DOT
// (always), POINTER_GAZE_SHOW (1 s): gaze mode, above. POINTER_CONTROLLER_PICKUP (1, 0.5 to 5): how hard a controller must
// move to take the laser back, above. POINTER_IGNORE (empty): ignored panels, above.
// POINTER_HANDS (0), POINTER_PINCH_GAIN (0.5), POINTER_PINCH_DEADZONE (1.5 deg),
// POINTER_GRIP_GAIN (1), POINTER_GRIP_BELOW (0.35 m): hands, above. POINTER_PINCH_TYPING (2 s; 0 off),
// POINTER_TYPING_RAISE (0.08 m), POINTER_TYPING_BELOW (0.2 m: the keyboard zone's palms at least this
// far under the eyes), POINTER_TYPING_POSTURE (0.3 s; 0 keys only), POINTER_TYPING_RECENT (600 s; 0
// always): typing, above.
// POINTER_HAND_RAY (1), POINTER_RAY_DEADZONE (2.5 deg), POINTER_RAY_PUSH (1), POINTER_RAY_SCROLL (1),
// POINTER_RAY_RELEASE (2 s), POINTER_RAY_GRACE (0.5 s), POINTER_RAY_SMOOTH_MINCUTOFF (0.8 Hz),
// POINTER_RAY_SMOOTH_BETA (0.06), POINTER_RAY_SMOOTH_HOLD (0.5 Hz), POINTER_RAY_STEADY (1),
// POINTER_RAY_TAP (0.3 s), POINTER_RAY_TAP_DEADZONE (6 deg), POINTER_RAY_TORSO_WRISTS (1),
// POINTER_RAY_HAND_WEIGHT (0), POINTER_RAY_HAND_PITCH (-30 deg), POINTER_RAY_HAND_YAW (8 deg),
// POINTER_RAY_ANCHOR (0): the hand ray, above.
// POINTER_GAZE_PINCH (1), POINTER_GAZE_HAND_RAY (0), POINTER_GAZE_PINCH_DEADZONE (4 deg),
// POINTER_GAZE_PINCH_GAIN (0.3): gaze pinches, above. HANDS_NEAR (1; on: without POINTER_HANDS): the
// handoff for ft-screens' direct touch, above.
// POINTER_GAZE_MOUSE (precision), POINTER_HEAD_DEADZONE (0.5 deg), POINTER_KEY_TAP (0.25 s),
// POINTER_ROLE (right): gaze precision and keyboard clicks, above.
#include <openvr.h>

#include "../../hands/include/fh_ray.h"
#include "calpanel.h"
#include "gazehand.h"
#include "handoff.h"
#include "handsin.h"
#include "rayowner.h"
#include "typing.h"
#include "vrbuttons.h"
#include "vrmath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <climits>

#include <fcntl.h>
#include <fnmatch.h>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace md;

std::map<std::string, std::string> ReadConfig() {
    std::map<std::string, std::string> conf;
    const char *home = std::getenv("HOME");
    std::ifstream in(std::string(home ? home : "") + "/.config/frametop.conf");
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            return s;
        };
        conf[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return conf;
}

double ConfDouble(const std::map<std::string, std::string> &c, const char *key, double fallback) {
    auto it = c.find(key);
    return it == c.end() ? fallback : std::atof(it->second.c_str());
}

int AbstractSocket(const char *name, bool bindIt) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bindIt) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path + 1, name, std::strlen(name));
        const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
            std::perror("bind @ft_pointer_helper (already running?)");
            std::exit(1);
        }
    }
    return fd;
}

std::string ExeDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.rfind('/'));
}

// One of ft-screens' panels showing a desktop: a screen (frametop.screen.N), another
// machine's display (frametop.remote.N), a floating window (frametop.float.N), or a floating
// window's popup (frametop.float.N.sub.K), not a control of theirs.
bool FramePanel(const std::string &key) {
    for (const char *prefix : {"frametop.screen.", "frametop.remote.", "frametop.float."}) {
        if (key.rfind(prefix, 0) != 0) continue;
        const std::string rest = key.substr(std::strlen(prefix));
        const size_t dot = rest.find('.');
        return dot == std::string::npos || rest.compare(dot, 5, ".sub.") == 0;
    }
    return false;
}

// One of ft-screens' drag controls on such a panel: its grab bar or its resize handle
// (frametop.screen.N.bar, frametop.remote.N.resize, ...). A gaze pinch on one presses at once
// (gazehand.h, POINTER_GAZE_CONTROL_HOLD): there's nothing to click on them.
bool FrameControl(const std::string &key) {
    for (const char *prefix : {"frametop.screen.", "frametop.remote.", "frametop.float."}) {
        if (key.rfind(prefix, 0) != 0) continue;
        const std::string rest = key.substr(std::strlen(prefix));
        const size_t dot = rest.find('.');
        if (dot == std::string::npos) return false;
        const std::string part = rest.substr(dot + 1);
        return part == "bar" || part == "resize";
    }
    return false;
}

void SendTo(int fd, const char *name, const std::string &msg) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name, std::strlen(name));
    const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
    sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr *>(&addr), len);
}

// JSON string literal (names come from other apps).
std::string JsonQuote(const std::string &s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
        } else out += char(c);
    }
    return out + "\"";
}

// Overlay keys, refreshed in the background from `vrcmd --overlays` (OpenVR has no
// public call to enumerate other apps' overlays). Hidden ones are listed too: the
// window controls under a floating panel only appear while something hovers the
// panel, and the cursor has to find them the moment they do, not a second later.
// Each vrcmd run is a shell and a new SteamVR client, about 30 ms of CPU, so the list is read
// every 20 seconds (kEvery) while the pointer is awake, and at once when something says it may
// have changed (Kick: the pointer waking, the dashboard opening or closing, a click that hit
// nothing), at most once a second. Showing and hiding the overlays it knows doesn't need a new
// list: the main loop polls their visibility (IsOverlayVisible) every 50 ms.
// Paused while the pointer is off: each vrcmd run connects to SteamVR as a new app, and a new
// app every second kept SteamVR (and the headset's displays) from going to standby.
// "overlays" requests (Frametop Input Settings' Ignored panels page) refresh the list even
// while paused, and are answered from this thread once it's fresh.
class OverlayList {
public:
    static constexpr auto kEvery = std::chrono::seconds(20), kGap = std::chrono::seconds(1);
    void Start() {
        out_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(lock_);
            while (running_) {
                // Sleeps while paused (until a request or the wake); otherwise until it's time, or
                // a kick once kGap has passed since the last read.
                const auto due = std::max(last_ + kGap, kicked_ ? last_ : last_ + kEvery);
                if (!waiting_.empty()) {
                } else if (paused_) {
                    cv_.wait(lk);
                    continue;
                } else if (std::chrono::steady_clock::now() < due) {
                    cv_.wait_until(lk, due);
                    continue;
                }
                kicked_ = false;
                lk.unlock();
                Refresh();
                lk.lock();
                last_ = std::chrono::steady_clock::now();
            }
        });
    }
    // Unpausing (the pointer woke) reads the list again at once.
    void SetPaused(bool paused) {
        std::lock_guard<std::mutex> guard(lock_);
        if (paused == paused_) return;
        if (!paused) kicked_ = true, last_ = {};
        paused_ = paused;
        cv_.notify_one();
    }
    // The list may have changed: read it again soon (not while paused).
    void Kick() {
        std::lock_guard<std::mutex> guard(lock_);
        kicked_ = true;
        cv_.notify_one();
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> guard(lock_);
            running_ = false;
            cv_.notify_one();
        }
        if (thread_.joinable()) thread_.join();
    }
    std::vector<std::string> Keys() {
        std::lock_guard<std::mutex> guard(lock_);
        std::vector<std::string> keys;
        for (const auto &e : entries_) keys.push_back(e.key);
        return keys;
    }
    // A panel just made by another Frametop program (ft-screens: a floating window's menu, the
    // keyboard): known at once, without waiting for the next read ("overlay <key>").
    void Add(const std::string &key) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            for (const auto &e : entries_)
                if (e.key == key) return;
            entries_.push_back({key, key, true});
        }
        ++generation_;
    }
    // Goes up by one each time the list is read, so the main loop knows to look the keys up again.
    unsigned Generation() const { return generation_; }
    // Answer `to` with {"t":"overlays","list":[{"key","name","visible"}...]} after the next refresh.
    void Request(const sockaddr_un &to, socklen_t len) {
        if (len <= offsetof(sockaddr_un, sun_path)) return;
        std::lock_guard<std::mutex> guard(lock_);
        if (waiting_.size() < 8) waiting_.push_back({to, len});
        cv_.notify_one();
    }

private:
    struct Entry {
        std::string key, name;
        bool visible;
    };
    void Refresh() {
        FILE *p = popen("LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null", "r");
        if (!p) return;
        std::vector<Entry> entries;
        char line[1024];
        while (std::fgets(line, sizeof line, p)) {
            // 'key' -- 'name', WxH visible VROverlayType_...
            if (line[0] != '\'') continue;
            const char *end = std::strchr(line + 1, '\'');
            if (!end) continue;
            const std::string key(line + 1, size_t(end - (line + 1)));
            const std::string rest(end);
            if (rest.find("Thumbnail") != std::string::npos || rest.find("Subview") != std::string::npos) continue;
            if (key.rfind("system.pointer", 0) == 0 || key.rfind("system.cursor", 0) == 0 ||
                key.rfind("frametop.pointer", 0) == 0 || key.rfind("frametop.guide", 0) == 0 ||
                key.rfind("frametop.handview", 0) == 0 ||  // the drawn hands (ft-handview): around the hand itself
                key == "frametop.gazepanel" ||  // the gaze calibration panel, fixed to the headset
                key == "frametop.catcher" ||  // ft-screens' release catcher: only on a laser mid-drag
                key == "system.HeadsetView" || key == "system.toast")
                continue;
            // The name can hold quotes; it ends at the last "', " (the size and state follow).
            const auto nameAt = rest.find("-- '"), nameEnd = rest.rfind("', ");
            const std::string name =
                nameAt != std::string::npos && nameEnd > nameAt + 3 ? rest.substr(nameAt + 4, nameEnd - nameAt - 4) : key;
            entries.push_back({key, name, rest.find(" not_visible ") == std::string::npos});
        }
        pclose(p);
        std::vector<std::pair<sockaddr_un, socklen_t>> waiting;
        {
            std::lock_guard<std::mutex> guard(lock_);
            entries_ = std::move(entries);
            waiting.swap(waiting_);
        }
        ++generation_;
        if (waiting.empty()) return;
        std::string msg = "{\"t\":\"overlays\",\"list\":[";
        for (size_t i = 0; i < entries_.size(); ++i)
            msg += std::string(i ? "," : "") + "{\"key\":" + JsonQuote(entries_[i].key) + ",\"name\":" +
                   JsonQuote(entries_[i].name) + ",\"visible\":" + (entries_[i].visible ? "true" : "false") + "}";
        msg += "]}";
        for (const auto &[to, len] : waiting)
            sendto(out_, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&to), len);
    }

    std::thread thread_;
    int out_ = -1;
    std::mutex lock_;  // guards everything below
    std::condition_variable cv_;
    bool paused_ = false, running_ = true, kicked_ = false;
    std::chrono::steady_clock::time_point last_{};  // the last read
    std::atomic<unsigned> generation_{0};
    std::vector<Entry> entries_;  // written only by the thread; the lock guards readers
    std::vector<std::pair<sockaddr_un, socklen_t>> waiting_;
};

// POINTER_IGNORE: overlay keys the pointer passes through, as if they weren't there
// (display-only panels such as a performance overlay). Comma-separated shell patterns
// (fnmatch, no escapes), so "vendor.app*" covers an app's overlays.
std::vector<std::string> ParseIgnore(const std::string &list) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        std::string item = list.substr(at, comma - at);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.push_back(item);
        at = comma + 1;
    }
    return out;
}

bool Ignored(const std::vector<std::string> &patterns, const std::string &key) {
    for (const auto &p : patterns)
        if (fnmatch(p.c_str(), key.c_str(), FNM_NOESCAPE) == 0) return true;
    return false;
}

vr::HmdMatrix34_t Billboard(Vec3 at, Vec3 eye);

// One of our dots, with what was last set on it: each setter goes to SteamVR only when its value
// changes (see "Unchanged frames" at the top).
struct DotOverlay {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    int shown = -1;  // unknown at first
    float alpha = -1, width = -1;
    bool placed = false;
    Vec3 at, eye;
    void Show(bool on) {
        if (shown == int(on)) return;
        shown = on;
        if (on) vr::VROverlay()->ShowOverlay(h);
        else vr::VROverlay()->HideOverlay(h);
    }
    void Alpha(float a) {
        if (a != alpha) vr::VROverlay()->SetOverlayAlpha(h, alpha = a);
    }
    // Width w, at `to`, facing `from`: left as it is for a change under 0.5% in width, and while
    // it moved less than 0.2 mm and the eye less than 5 mm.
    void Place(Vec3 to, Vec3 from, float w) {
        if (std::fabs(w - width) > 0.005f * width) vr::VROverlay()->SetOverlayWidthInMeters(h, width = w);
        if (placed && Length(to - at) < 0.0002 && Length(from - eye) < 0.005) return;
        placed = true, at = to, eye = from;
        const auto m = Billboard(to, from);
        vr::VROverlay()->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
    }
};

vr::HmdMatrix34_t Billboard(Vec3 at, Vec3 eye) {
    // Overlay faces +Z; point +Z at the eye, keep +Y roughly up.
    const Vec3 z = Normalize(eye - at);
    const Vec3 x = Normalize(Cross({0, 1, 0}, z));
    const Vec3 y = Cross(z, x);
    vr::HmdMatrix34_t m{};
    const Vec3 cols[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        m.m[0][c] = float(cols[c].x);
        m.m[1][c] = float(cols[c].y);
        m.m[2][c] = float(cols[c].z);
    }
    m.m[0][3] = float(at.x);
    m.m[1][3] = float(at.y);
    m.m[2][3] = float(at.z);
    return m;
}

std::vector<uint8_t> DotTexture(int size) {
    // White dot with a dark rim, soft edge, transparent outside.
    std::vector<uint8_t> px(size * size * 4, 0);
    const double c = (size - 1) / 2.0, r = size * 0.42, rim = size * 0.10;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double d = std::hypot(x - c, y - c);
            const double a = std::clamp(r - d + 0.5, 0.0, 1.0);
            const bool inner = d < r - rim;
            uint8_t *p = &px[(y * size + x) * 4];
            const uint8_t v = inner ? 255 : 40;
            p[0] = p[1] = p[2] = v;
            p[3] = uint8_t(a * 235);
        }
    return px;
}

// Unit vector v, turned toward unit vector `center` until it's at most maxRad from it.
Vec3 PullWithin(Vec3 v, Vec3 center, double maxRad) {
    if (std::acos(std::clamp(Dot(v, center), -1.0, 1.0)) <= maxRad) return v;
    Vec3 axis = Cross(center, v);
    if (Length(axis) < 1e-9) axis = Cross(center, {0, 1, 0});  // opposite: any perpendicular
    return RotateAbout(center, Normalize(axis), maxRad);
}

// Unit direction with its pitch limited to +-maxDeg (AimBasis needs it off vertical).
Vec3 LimitPitch(Vec3 d, double maxDeg) {
    const double pitch = std::clamp(std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI, -maxDeg, maxDeg);
    return Direction(std::atan2(-d.x, -d.z) * 180 / M_PI, pitch);
}

// SteamVR Settings page (see the top): the laser starts this far from the eye, the dot is
// drawn this far out, and the laser-catching dot sits this far out (metres).
constexpr double SETTINGS_ORIGIN = 0.25, SETTINGS_DOT = 0.6, SETTINGS_CATCHER = 8.0;

// Whether the line of sight from `eye` along `d` crosses the SteamVR Settings page, drawn by
// the dashboard's scene-graph panel (valve.steam.gamepadui.frame.menu.N, transform t); see
// "SteamVR Settings" at the top. The page has no size in OpenVR, so this is its area as
// measured on the Frame, generously: in metres from the panel's origin, which is near the
// page's left edge, it ran from about -0.35 (the sidebar) to 1.15 across and +-0.4 up and
// down, with the transform scaled 0.369. Kept in the transform's units so it scales with it.
bool OnSettingsPage(const vr::HmdMatrix34_t &t, Vec3 eye, Vec3 d) {
    const Vec3 c = Position(t), x{t.m[0][0], t.m[1][0], t.m[2][0]}, y{t.m[0][1], t.m[1][1], t.m[2][1]},
               z{t.m[0][2], t.m[1][2], t.m[2][2]};
    const double sx = Dot(x, x), sy = Dot(y, y), denom = Dot(d, z);
    if (sx < 1e-9 || sy < 1e-9 || std::fabs(denom) < 1e-6) return false;
    const double along = Dot(c - eye, z) / denom;
    if (along <= 0) return false;
    const Vec3 off = eye + d * along - c;
    const double u = Dot(off, x) / sx, v = Dot(off, y) / sy;  // in the transform's units
    return u >= -1.35 && u <= 3.4 && std::fabs(v) <= 1.25;
}

}  // namespace

// Placement speeds (see "Placement" at the top).
constexpr double kPlaceDegPerSec = 60;      // tested: 40 deg/s is applied exactly

int main() {
    double freeDistance = 1.5, cursorDeg = 0.4, originFraction = 0.95, originMargin = 0.15, sceneRadius = 0.5,
           edgeReach = 0.3, grabOffset = 0.075,
           slideSpeed = 0.5, leashDeg = 10, leashReturn = 0.2, leashDelay = 0.2, followReach = 70;
    // Head follow (see the top). followConf is POINTER_FOLLOW as last read: a reload only
    // overrides a "follow" command when the setting itself changed.
    bool follow = false, followConf = false, followReset = true;
    // Gaze mode (see the top); gazeConf is POINTER_GAZE as last read, like followConf.
    bool gazeOn = false, gazeConf = false;
    double gazeRetake = 5, gazeNudgeMax = 55, gazeHold = 0.5, gazeShow = 1, gazeControlHold = 0.12;
    bool gazeDotAlways = true;  // POINTER_GAZE_DOT (see the top)
    double headDeadzone = 0.5, keyTap = 0.25;  // POINTER_HEAD_DEADZONE, POINTER_KEY_TAP (keyboard clicks, see the top)
    // Hands (see the top): POINTER_HANDS, POINTER_PINCH_GAIN, POINTER_PINCH_DEADZONE, POINTER_GRIP_GAIN.
    bool handsOn = false;
    // POINTER_GAZE_PINCH: in gaze mode, a hand with no ray up clicks at the gaze (see "Gaze pinch").
    // HANDS_NEAR=on: ft-screens' direct touch without POINTER_HANDS, so the handoff runs and its
    // "hands" word goes out for it (handsTrack) while the pointer takes no gestures (see "Hands").
    bool gazePinchOn = true, nearOn = false, handsTrack = false;
    double pinchGain = 0.5, pinchDeadzone = 1.5, gripGain = 1.0, handBelow = 0.35;
    // Typing (see the top): POINTER_PINCH_TYPING, POINTER_TYPING_RAISE, POINTER_TYPING_BELOW,
    // POINTER_TYPING_POSTURE, POINTER_TYPING_RECENT.
    typing::Params typingParams;
    // The hand ray (see "Hand ray" at the top): POINTER_HAND_RAY, POINTER_RAY_DEADZONE, POINTER_RAY_PUSH,
    // POINTER_RAY_SCROLL, POINTER_RAY_RELEASE, POINTER_RAY_GRACE.
    bool rayOn = true, rayPush = true;
    double rayDeadzone = 2.5, rayScroll = 1.0, rayRelease = 2.0, rayGrace = 0.5;
    // ...its smoothing and a tap's dead zone (see "Hand ray" at the top): POINTER_RAY_SMOOTH_MINCUTOFF,
    // POINTER_RAY_SMOOTH_BETA, POINTER_RAY_SMOOTH_HOLD, POINTER_RAY_STEADY, POINTER_RAY_TAP,
    // POINTER_RAY_TAP_DEADZONE; the body and hand models' POINTER_RAY_TORSO_WRISTS, POINTER_RAY_HAND_WEIGHT,
    // POINTER_RAY_HAND_PITCH, POINTER_RAY_HAND_YAW, POINTER_RAY_ANCHOR. rayParamsNew: read again, for the rays.
    fhray::Params rayParams;
    bool rayParamsNew = true;
    double rayTap = 0.3, rayTapDeadzone = 6.0;
    // Gaze pinches (see "Gaze pinch" at the top): POINTER_GAZE_HAND_RAY, POINTER_GAZE_PINCH_DEADZONE,
    // POINTER_GAZE_PINCH_GAIN.
    bool gazeHandRay = false;
    double gazePinchDeadzone = 4.0, gazePinchGain = 0.3;
    // Gaze precision (see the top): POINTER_GAZE_MOUSE (precision: the mouse's left button
    // holds back its press in gaze mode; direct: it clicks at once), POINTER_ROLE.
    bool gazeMousePrecision = true;
    bool gazeMouseHeld = true;  // POINTER_GAZE_MOUSE_MOVE (see the top)
    std::string role = "treadmill";
    double pickupScale = 1;  // POINTER_CONTROLLER_PICKUP: scales the controller-moved limits
    std::vector<std::string> ignore;  // POINTER_IGNORE (see ParseIgnore)
    auto loadConfig = [&] {
        const auto conf = ReadConfig();
        freeDistance = std::clamp(ConfDouble(conf, "POINTER_DISTANCE", 1.5), 0.3, 10.0);
        cursorDeg = std::clamp(ConfDouble(conf, "POINTER_CURSOR_DEG", 0.4), 0.05, 5.0);
        originFraction = std::clamp(ConfDouble(conf, "POINTER_ORIGIN_FRACTION", 0.95), 0.0, 0.98);
        originMargin = std::clamp(ConfDouble(conf, "POINTER_ORIGIN_MARGIN", 0.15), 0.0, 1.0);
        sceneRadius = std::clamp(ConfDouble(conf, "POINTER_SCENE_RADIUS", 0.5), 0.05, 2.0);
        edgeReach = std::clamp(ConfDouble(conf, "POINTER_EDGE_REACH", 0.3), 0.0, 2.0);
        grabOffset = std::clamp(ConfDouble(conf, "LAYOUT_GRAB_OFFSET", 0.075), 0.0, 1.0);
        slideSpeed = std::clamp(ConfDouble(conf, "LAYOUT_SLIDE_SPEED", 0.5), 0.02, 2.0);
        leashDeg = std::clamp(ConfDouble(conf, "POINTER_LEASH_DEG", 10), 0.0, 90.0);
        leashReturn = std::clamp(ConfDouble(conf, "POINTER_LEASH_RETURN", 0.2), 0.0, 5.0);
        leashDelay = std::clamp(ConfDouble(conf, "POINTER_LEASH_DELAY", 0.2), 0.0, 5.0);
        followReach = std::clamp(ConfDouble(conf, "POINTER_FOLLOW_REACH", 70), 10.0, 89.0);
        const bool wantFollow = ConfDouble(conf, "POINTER_FOLLOW", 0) != 0;
        if (wantFollow != followConf) follow = followConf = wantFollow, followReset = true;
        gazeRetake = std::clamp(ConfDouble(conf, "POINTER_GAZE_RETAKE", 5), 1.0, 45.0);
        gazeNudgeMax = std::clamp(ConfDouble(conf, "POINTER_GAZE_NUDGE_MAX", 55), 1.0, 110.0);
        gazeHold = std::clamp(ConfDouble(conf, "POINTER_GAZE_HOLD", 0.5), 0.1, 5.0);
        gazeControlHold = std::clamp(ConfDouble(conf, "POINTER_GAZE_CONTROL_HOLD", 0.12), 0.05, 5.0);
        gazeShow = std::clamp(ConfDouble(conf, "POINTER_GAZE_SHOW", 1), 0.0, 30.0);
        headDeadzone = std::clamp(ConfDouble(conf, "POINTER_HEAD_DEADZONE", 0.5), 0.0, 5.0);
        keyTap = std::clamp(ConfDouble(conf, "POINTER_KEY_TAP", 0.25), 0.0, 1.0);
        const auto gd = conf.find("POINTER_GAZE_DOT");
        gazeDotAlways = gd == conf.end() || gd->second != "moving";
        const bool wantGaze = ConfDouble(conf, "POINTER_GAZE", 0) != 0;
        if (wantGaze != gazeConf) gazeOn = gazeConf = wantGaze;
        handsOn = ConfDouble(conf, "POINTER_HANDS", 0) != 0;
        gazePinchOn = ConfDouble(conf, "POINTER_GAZE_PINCH", 1) != 0;
        const auto hn = conf.find("HANDS_NEAR");
        nearOn = hn != conf.end() && hn->second == "on";
        handsTrack = handsOn || nearOn;
        pinchGain = std::clamp(ConfDouble(conf, "POINTER_PINCH_GAIN", 0.5), 0.05, 3.0);
        pinchDeadzone = std::clamp(ConfDouble(conf, "POINTER_PINCH_DEADZONE", 1.5), 0.0, 10.0);
        gripGain = std::clamp(ConfDouble(conf, "POINTER_GRIP_GAIN", 1.0), 0.05, 3.0);
        handBelow = std::clamp(ConfDouble(conf, "POINTER_GRIP_BELOW", 0.35), 0.05, 1.0);
        typingParams.hold = std::clamp(ConfDouble(conf, "POINTER_PINCH_TYPING", 2.0), 0.0, 10.0);
        typingParams.raise = std::clamp(ConfDouble(conf, "POINTER_TYPING_RAISE", 0.08), 0.02, 0.5);
        typingParams.below = std::clamp(ConfDouble(conf, "POINTER_TYPING_BELOW", 0.2), 0.0, 1.0);
        typingParams.posture = std::clamp(ConfDouble(conf, "POINTER_TYPING_POSTURE", 0.3), 0.0, 5.0);
        typingParams.recent = std::clamp(ConfDouble(conf, "POINTER_TYPING_RECENT", 600), 0.0, 86400.0);
        rayOn = ConfDouble(conf, "POINTER_HAND_RAY", 1) != 0;
        rayDeadzone = std::clamp(ConfDouble(conf, "POINTER_RAY_DEADZONE", 2.5), 0.0, 10.0);
        rayPush = ConfDouble(conf, "POINTER_RAY_PUSH", 1) != 0;
        rayScroll = std::clamp(ConfDouble(conf, "POINTER_RAY_SCROLL", 1.0), -5.0, 5.0);
        rayRelease = std::clamp(ConfDouble(conf, "POINTER_RAY_RELEASE", 2.0), 0.0, 60.0);
        rayGrace = std::clamp(ConfDouble(conf, "POINTER_RAY_GRACE", 0.5), 0.0, 3.0);
        rayParams.minCutoff = std::clamp(ConfDouble(conf, "POINTER_RAY_SMOOTH_MINCUTOFF", 0.8), 0.05, 30.0);
        rayParams.beta = std::clamp(ConfDouble(conf, "POINTER_RAY_SMOOTH_BETA", 0.06), 0.0, 5.0);
        rayParams.holdMinCutoff = std::clamp(ConfDouble(conf, "POINTER_RAY_SMOOTH_HOLD", 0.5), 0.05, 30.0);
        rayParams.holdShoulder = ConfDouble(conf, "POINTER_RAY_STEADY", 1) != 0;
        rayParams.torsoWrists = ConfDouble(conf, "POINTER_RAY_TORSO_WRISTS", 1) != 0;
        rayParams.handWeight = std::clamp(ConfDouble(conf, "POINTER_RAY_HAND_WEIGHT", 0), 0.0, 1.0);
        rayParams.handPitch = std::clamp(ConfDouble(conf, "POINTER_RAY_HAND_PITCH", -30), -90.0, 90.0);
        rayParams.handYaw = std::clamp(ConfDouble(conf, "POINTER_RAY_HAND_YAW", 8), -90.0, 90.0);
        rayParams.pinchAnchor = ConfDouble(conf, "POINTER_RAY_ANCHOR", 0) != 0;
        rayParamsNew = true;
        rayTap = std::clamp(ConfDouble(conf, "POINTER_RAY_TAP", 0.3), 0.0, 2.0);
        rayTapDeadzone = std::clamp(ConfDouble(conf, "POINTER_RAY_TAP_DEADZONE", 6.0), 0.0, 30.0);
        gazeHandRay = ConfDouble(conf, "POINTER_GAZE_HAND_RAY", 0) != 0;
        gazePinchDeadzone = std::clamp(ConfDouble(conf, "POINTER_GAZE_PINCH_DEADZONE", 4.0), 0.0, 15.0);
        gazePinchGain = std::clamp(ConfDouble(conf, "POINTER_GAZE_PINCH_GAIN", 0.3), 0.05, 2.0);
        const auto gm = conf.find("POINTER_GAZE_MOUSE");
        gazeMousePrecision = gm == conf.end() || gm->second != "direct";
        const auto mm = conf.find("POINTER_GAZE_MOUSE_MOVE");
        gazeMouseHeld = mm == conf.end() || mm->second != "free";
        const auto ro = conf.find("POINTER_ROLE");
        role = ro != conf.end() && (ro->second == "left" || ro->second == "right" || ro->second == "stylus")
                   ? ro->second
                   : "treadmill";
        pickupScale = std::clamp(ConfDouble(conf, "POINTER_CONTROLLER_PICKUP", 1), 0.5, 5.0);
        const auto ig = conf.find("POINTER_IGNORE");
        ignore = ParseIgnore(ig == conf.end() ? "" : ig->second);
    };
    loadConfig();
    const float laserWidth = float(ConfDouble(ReadConfig(), "POINTER_LASER_WIDTH", 0.8));

    vr::EVRInitError err = vr::VRInitError_None;
    while (true) {
        vr::VR_Init(&err, vr::VRApplication_Background);
        if (err == vr::VRInitError_None) {
            vr::VR_Shutdown();
            vr::VR_Init(&err, vr::VRApplication_Overlay);
        }
        if (err == vr::VRInitError_None) break;
        std::fprintf(stderr, "waiting for SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    auto *sys = vr::VRSystem();
    auto *overlay = vr::VROverlay();

    vr::VROverlayHandle_t cursor = vr::k_ulOverlayHandleInvalid;
    overlay->CreateOverlay("frametop.pointer.cursor", "Frametop pointer", &cursor);
    const int texSize = 64;
    auto tex = DotTexture(texSize);
    overlay->SetOverlayRaw(cursor, tex.data(), texSize, texSize, 4);
    overlay->SetOverlayInputMethod(cursor, vr::VROverlayInputMethod_Mouse);  // the laser can land on it
    overlay->SetOverlaySortOrder(cursor, 200);
    // Same dot, not interactive, drawn on panels at the hit point; the laser passes through.
    vr::VROverlayHandle_t marker = vr::k_ulOverlayHandleInvalid;
    overlay->CreateOverlay("frametop.pointer.marker", "Frametop pointer marker", &marker);
    overlay->SetOverlayRaw(marker, tex.data(), texSize, texSize, 4);
    overlay->SetOverlayInputMethod(marker, vr::VROverlayInputMethod_None);
    overlay->SetOverlaySortOrder(marker, 201);
    // Laser mode (see the top of the file).
    vr::VROverlayHandle_t laserMode = vr::k_ulOverlayHandleInvalid;
    overlay->CreateOverlay("frametop.pointer.lasermode", "Frametop pointer laser mode", &laserMode);
    std::vector<uint8_t> clear(4 * 4 * 4, 0);
    overlay->SetOverlayRaw(laserMode, clear.data(), 4, 4, 4);
    overlay->SetOverlayWidthInMeters(laserMode, 0.001f);
    overlay->SetOverlayInputMethod(laserMode, vr::VROverlayInputMethod_Mouse);  // the flag needs an input method
    overlay->SetOverlayFlag(laserMode, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    vr::HmdMatrix34_t below{};
    below.m[0][0] = below.m[1][1] = below.m[2][2] = 1;
    below.m[1][3] = -50;
    overlay->SetOverlayTransformTrackedDeviceRelative(laserMode, vr::k_unTrackedDeviceIndex_Hmd, &below);
    bool laserModeShown = false;
    DotOverlay cursorDot, markerDot;  // the setters for `cursor` and `marker` (see "Unchanged frames" at the top)
    cursorDot.h = cursor, markerDot.h = marker;
    // Controller beams keep the user's width; nothing here changes it any more.
    vr::VRSettings()->SetFloat("dashboard", "laserRayWidthScale", laserWidth);

    const int in = AbstractSocket("ft_pointer_helper", true);
    const int out = AbstractSocket(nullptr, false);
    // Frame controller buttons (vrbuttons.h). The build puts the binary in pointer/helper/build.
    ControllerButtons controllerButtons;
    {
        const std::string manifest = ExeDir() + "/../actions/ft_pointer_actions.json";
        char real[PATH_MAX];
        controllerButtons.Init(realpath(manifest.c_str(), real) ? real : manifest);
    }
    SendTo(out, "frametop_relay", "vrhello");  // the relay answers with the mapped buttons
    OverlayList overlays;
    overlays.Start();
    std::map<std::string, vr::VROverlayHandle_t> handles;
    std::map<std::string, bool> sceneGraph;  // no texture: plane test instead of ComputeOverlayIntersection
    std::map<std::string, bool> visible;     // refreshed every 50 ms
    auto lastVisible = std::chrono::steady_clock::now();
    auto lastSlow = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    unsigned listGeneration = 0;  // the overlay list the handles were looked up from
    // The plane of the last panel the cursor was on, and the last point on it (panel edges).
    Vec3 edgePoint, edgeNormal, edgeLast;
    std::string edgeKey;

    bool active = false, recenter = false, anchored = false;
    using Clock = std::chrono::steady_clock;
    // Unchanged frames (see the top): visibleGen goes up when an overlay shows or hides or the
    // handles change; pass is the last collision pass, posed the last plain pose sent.
    unsigned visibleGen = 0;
    struct Pass {
        bool valid = false;
        Clock::time_point at{};
        unsigned gen = 0;
        double yaw = 0, pitch = 0;
        Vec3 anchor, eye;
        double best = 1e9, distance = 0;
        std::string key;
        bool scene = false, onEdge = false, occluded = false;
        Vec3 point;
    } pass;
    struct Posed {
        bool valid = false;
        Clock::time_point at{};
        Vec3 origin;
        double yaw = 0, pitch = 0;
    } posed;
    Clock::time_point lastMouse{}, claimAt{}, claimRelease{}, wokeAt{}, noWakeUntil{};
    bool claimPending = false, claimHeld = false;
    // Last used wins: since when each controller has been moving (zero: it isn't).
    Clock::time_point movingSince[vr::k_unMaxTrackedDeviceCount] = {};
    // Tilt mode (see top of file).
    bool leftHeld = false, tilting = false, tiltStart = false, swallowedRight = false;
    // The mouse's buttons in gaze mode (see the top): leftDown, rightDown as the relay last said;
    // aimRight, the held-back press is the right button's; chordDrag, a drag the right button
    // began during the left's held-back press (the first release drops it, the other's is nothing).
    bool leftDown = false, rightDown = false, aimRight = false, chordDrag = false;
    bool ignoreLeftUp = false;
    // The keyboard clicks' keys as the relay last said, and a tilt gaze_right holds during a
    // keyboard drag (see the top): the head turns the panel, from where it was (keyTiltYaw, keyTiltPitch).
    bool keyLeftDown = false, keyRightDown = false, keyTilting = false;
    double keyTiltYaw = 0, keyTiltPitch = 0;
    double tiltYaw = 0, tiltPitch = 0;
    double dragDistance = 0, lastDistance = 1.5;  // drag lock: distance from the anchor at the press
    bool onVrSettings = false;       // the cursor is on the SteamVR Settings page (kept while dragging)
    bool catcherHidesHit = false;    // the laser-catching dot hides SteamVR's hit dot (Settings page)
    Clock::time_point dropHoldUntil{};  // after a left release: keep the drag pose this long
    bool debug = false;
    std::string lastHit;
    // The ft-screens panel the left button was pressed on, and where it was then (see the top).
    std::string pressKey;
    vr::HmdMatrix34_t pressPose{};
    auto lastDebug = Clock::now();
    vr::VROverlayHandle_t systemPointer = vr::k_ulOverlayHandleInvalid;
    overlay->FindOverlay("system.pointer", &systemPointer);
    Vec3 pivot, tiltOrigin, lastPoint, lastOrigin, lastAim{0, 0, -1};
    Basis tiltBasis{};
    bool headsetOff = false;  // nobody is wearing the headset (see the main loop)
    auto wake = [&](Clock::time_point t) {
        if (t < noWakeUntil || headsetOff) return;
        wokeAt = t;
        active = true;
        recenter = true;
        // Overlay handles and visibility weren't looked at while it was off (see "Idle" at the top).
        lastSlow = lastVisible = t - std::chrono::seconds(10);
        pass.valid = posed.valid = false;
        SendTo(out, "ft_pointer", "role " + role);  // POINTER_ROLE, before it takes it
        SendTo(out, "ft_pointer", "show");
        claimPending = true;  // take the laser without clicking, once SteamVR has bound the device
        claimAt = t + std::chrono::milliseconds(300);
    };
    Vec3 anchor;
    double yaw = 0, pitch = 0;
    Vec3 followRef{0, 0, -1};  // head follow's reference direction (see the top)
    auto followAt = std::chrono::steady_clock::now();  // its last update, for the easing
    bool following = false;                           // past the leash: easing toward the head
    double followLag = 0;                             // radians the reference trails the head
    std::chrono::steady_clock::time_point leashOutSince{};  // head past the leash since (delay)
    // Gaze mode (see the top).
    struct Gaze {
        double hy = 0, hp = 0, rhy = 0, rhp = 0;  // corrected, and raw
        Clock::time_point at{};
    } gz;
    bool gazeOwns = true;  // the pointer follows the gaze; false: the mouse has it
    bool nudging = false;  // the mouse took it from the gaze: the next click may be a lesson
    double nudgeRawHy = 0, nudgeRawHp = 0, nudgeMoved = 0;
    vr::HmdMatrix34_t nudgeHead{}, lastHead{};
    bool haveHead = false, havePoint = false;
    Clock::time_point nudgeAt{}, retakeSince{};
    // A held-back press (see the top): aimHeld while the button is down; then the click
    // (clickPress at the end of the next frame, clickRelease 40 ms later). gazeBack: give
    // the gaze the pointer again when the press or click is over.
    vr::TrackedDeviceIndex_t ours = vr::k_unTrackedDeviceIndexInvalid;
    bool aimHeld = false, clickPress = false, clickRelease = false, gazeBack = false;
    bool aimHand = false;  // the held-back press is a hold's (below): it never turns into a real press
    Clock::time_point aimSince{}, clickReleaseAt{};
    // Holds (see "Gaze precision" and "Hands" at the top): a pinch or grip, or a gaze
    // precision or gaze drag button, held now. What steers the pointer meanwhile (a hand, or
    // the mouse through its own moves), from where it pointed when the hold began (for a
    // hand: seen from the eye then, in the room), and the pointer then.
    hin::HandGestures handFile;
    hin::PoseHistory poses;
    enum class Src { None, Hand, Mouse, Head, Ray };
    struct Hold {
        Src src = Src::None;
        int side = -1;          // a hand's side (0 left, 1 right)
        bool grip = false;      // pressed at once and dragging (a grip, gaze drag), not a click on release
        bool engaged = false;   // past the dead zone
        bool pressed = false;   // a real press went out (a grip or gaze drag, or a pinch without gaze mode)
        bool promote = false;   // held still for POINTER_GAZE_HOLD, it becomes a real press (keyboard clicks)
        bool right = false;     // the right button (gaze_right)
        bool keyDrag = false;   // gaze_right pressed while gaze_left aimed: a left press, either key ends it
        double pressYaw = 0, pressPitch = 0;  // the pointer at the press (a quick tap clicks there)
        // A ray hold (see "Hand ray" at the top): kind 0 a pinch (the left button), 2 a middle pinch
        // (right click or scroll); its press waits for the laser claim (pressAt) and for a pose along
        // the locked aim to have gone out (aimSent); scrolling, and the pinch point in the room at the
        // begin; push and pull (pushOn, from reach h0 and distance d0). raySlot: the ray's side (the
        // ray that follows the pinching hand; side is the gesture's). released: the pinch ended before
        // its press went out, which then clicks (released at releaseAt). For the log: took (it took
        // the ray from the other hand), lockAge (the aim's age at the begin, s; < 0 none), the hit.
        int rayKind = 0, raySlot = -1;
        bool pendingPress = false, scrolling = false, pushOn = false, aimSent = false, released = false, took = false;
        Clock::time_point pressAt{}, releaseAt{}, beganAt{};
        double lockAge = -1;
        std::string pressHit;
        Vec3 beginPoint;
        double h0 = 0, d0 = 0, scrollX = 0, scrollY = 0;
        Vec3 origin;
        double refYaw = 0, refPitch = 0, startYaw = 0, startPitch = 0, lastYaw = 0, lastPitch = 0;
        gazehand::Pinch gaze;  // a gaze pinch (see "Gaze pinch" at the top), when gaze.On()
    } hold;
    // Gaze pinches (see "Gaze pinch" at the top): the gaze as it came, for the lock; a click one is
    // due (its log line goes out with the click, gazeClickSide: its side); why the held one ended;
    // what pressLeft sent the gaze service ("a lesson", "the quick check", or "").
    gazehand::GazeHistory gazeHist;
    gazehand::Pinch gazeClick;
    // The chord (see "Gaze pinch" at the top): the other hand's index pinch during a gaze pinch holds
    // the right button. chordSide: its hand; chordPinch: its pinch, followed from chordOrigin (the eye
    // at its begin); chordBtn: the right button's timing; chordLeft: the left's held-back press is due
    // first (this frame, after its pose); chordSteers: the left let go, so this hand drags.
    gazehand::Pinch chordPinch;
    gazehand::ChordButton chordBtn;
    int chordSide = -1;
    Vec3 chordOrigin;
    bool chordLeft = false, chordSteers = false;
    int gazeClickSide = 0;
    std::string gazeEndWhy, lessonSaid;
    // "precision|gazedrag <source> 1|0" from the relay, done in the frame (see Holds).
    struct DevicePress {
        std::string source;
        bool drag, down;
    };
    std::vector<DevicePress> devicePresses;
    // "gazekey left|right 1|0" from the relay (keyboard clicks), done in the frame.
    struct KeyPress {
        bool right, down;
    };
    std::vector<KeyPress> keyPresses;
    uint32_t seenBegins[3][2] = {}, seenEnds[3][2] = {};  // [pinch, grip, middle pinch][side], as last read
    bool handBaseline = false;
    int handOpens = 0;
    uint64_t handSeq = 0, handPublished = 0;
    Clock::time_point handUsed{};  // a gesture began then (keeps the pointer, like gaze mode)
    typing::Typing typist;           // the relay's "typing" (a key) and the hands: which hands typing holds back
    std::string typingSent;          // "typing L R" as last sent to ft-screens
    Clock::time_point typingSentAt{};
    // Hand interaction (see "Handoff" at the top): the hands in the room, the arbiter, and what
    // it last told ft-screens.
    int raySide = -1;  // the bare hand whose ray drives the pointer (see "Hand ray"), -1: none
    hin::HandsInRoom handsRoom;
    handoff::Handoff arbiter;
    bool framePaused = false;           // the relay's "paused 1|0": Frametop paused (for a VR game)
    bool dashboardUp = false;           // IsDashboardVisible, read at most every 100 ms
    bool dashSent = false;              // "vrdash" as last sent to the relay
    Clock::time_point dashSentAt{};
    Clock::time_point dashboardAt{};
    std::string handsSent;              // "hands L R allowed" as last sent to ft-screens
    Clock::time_point handsSentAt{};
    bool handsAllowed = false;          // hand interaction allowed now (see "Handoff" at the top)
    // Controllers, looked at once a second: type (ours is left out), side from the role hint, charging.
    struct DeviceInfo {
        bool controller = false, ours = false, charging = false;
        int hint = 0;
    };
    DeviceInfo devices[vr::k_unMaxTrackedDeviceCount];
    Clock::time_point devicesAt{};
    // ft-screens' near reports ("near <capture_ns> <L> <R>"): the last one, and when it came.
    uint64_t nearNs = 0;
    bool nearSide[2] = {false, false};    // near (1 or 2): the hand's ray hides
    bool nearClaim[2] = {false, false};   // ...and (1) its pinches and grips go to ft-screens
    Clock::time_point nearAt{};
    auto secsOf = [](Clock::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); };
    // The hand ray (see "Hand ray" at the top): the body model, each side's ray, which one drives the
    // pointer, and what happened to the laser.
    fhray::Body body;
    fhray::HandRay rays[2] = {fhray::HandRay(0), fhray::HandRay(1)};
    // Which ray drives the pointer, which hand each ray follows, and each ray's recent aims for a
    // pinch's aim lock (rayowner.h); rayHid: why each ray last hid, for the log.
    rayown::Owner rayOwner;
    rayown::Slots raySlots;
    rayown::AimHistory rayAims[2];
    std::string rayHid[2];
    bool rayInactive = false;           // a controller took the laser: our dot hides until a pinch claims
    bool rayWoke = false;               // the ray woke the pointer (it goes again when the ray has gone a while)
    Clock::time_point rayGoneAt{};      // when the last ray went
    double pushOffset = 0;              // push and pull: the origin moved this far along the ray (m)
    Vec3 rayOrigin, rayDir{0, 0, -1};   // the driving ray this frame (standing universe)
    // Gestures that began while ft-screens may still claim them (see "near" at the top).
    struct PendingBegin {
        int side, kind;
        fh_pinch_t g;
        bool ray;  // the ray was up at the begin
        Clock::time_point deadline;
        bool ended = false, lost = false;
    };
    std::vector<PendingBegin> pendingBegins;
    bool gripDown[2] = {false, false};          // a grip is down on that side (the gestures file, last read)
    bool rayHoldCancel = false;                 // the driving ray went mid-hold: the hold ends, as lost
    Clock::time_point handOkAt{};               // the gestures file last read whole
    // Let go of the virtual controller (its role and laser), as the other releases in the main loop do.
    auto releasePointer = [&](const char *why) {
        active = false;
        claimPending = claimHeld = false;
        cursorDot.Show(false);
        markerDot.Show(false);
        SendTo(out, "ft_pointer", "btn a 0");
        SendTo(out, "ft_pointer", "hide");
        rayWoke = false;
        std::printf("%s: pointer released\n", why);
        std::fflush(stdout);
    };
    // The laser's owner is a real controller whose side pressed a button within 2 s: a ray that just
    // appeared leaves it alone (see "Hand ray" at the top).
    auto controllerBusy = [&](Clock::time_point now) {
        const vr::TrackedDeviceIndex_t p = overlay->GetPrimaryDashboardDevice();
        if (p == vr::k_unTrackedDeviceIndexInvalid || p == ours || p >= vr::k_unMaxTrackedDeviceCount) return false;
        if (!devices[p].controller || devices[p].ours) return false;
        for (int s = 0; s < 2; ++s)
            if (secsOf(now) - arbiter.LastPress(s) < 2.0) return true;
        return false;
    };
    auto laserOurs = [&] { return ours != vr::k_unTrackedDeviceIndexInvalid && overlay->GetPrimaryDashboardDevice() == ours; };
    auto rayEnter = [&](int s) {
        const auto t = Clock::now();
        raySide = s;
        rayInactive = false;
        handUsed = t;
        if (gazeOn) gazeOwns = false, nudging = false;
        if (!active) {
            wake(t);  // role, show, and the claim 300 ms on
            rayWoke = active;
        } else if (!laserOurs() && !controllerBusy(t) && !claimPending && !claimHeld) {
            claimPending = true, claimAt = t;
        }
    };
    auto rayLeave = [&](bool controller) {
        if (raySide < 0) return;
        raySide = -1;
        rayGoneAt = Clock::now();
        rayHoldCancel = true;
        if (gazeOn) gazeOwns = true, nudging = false;
        // that side's controller is in use, or hands aren't allowed now (a VR game): the laser goes back,
        // as when a controller moves (last used wins), unless the mouse is in use
        if (controller && active && Clock::now() - lastMouse > std::chrono::seconds(1)) releasePointer("hand ray off (a controller, or a game)");
    };
    // The ray owner's changes (rayowner.h), to the log, one line each; apply: the pointer follows
    // (rayEnter, rayLeave).
    auto ownerChanges = [&](bool apply) {
        for (const std::string &l : rayOwner.log) std::printf("%s\n", l.c_str());
        for (const rayown::Change &c : rayOwner.changes) {
            // which one, for an "off" (a drag that ends this way says why in the log)
            std::string why = c.why;
            if (c.off && c.from >= 0)
                why = arbiter.State(c.from) != handoff::Owner::Hand ? "off: its controller is in use"
                    : !handsAllowed ? (headsetOff ? "off: the headset is off" : "off: a VR game")
                    : Clock::now() - lastMouse < std::chrono::seconds(1) ? "off: the mouse moved"
                    : c.why;
            std::printf("hand ray: %s -> %s (%s)\n", rayown::SideName(c.from), rayown::SideName(c.to), why.c_str());
            if (!apply) continue;
            if (c.to >= 0) rayEnter(c.to);
            else rayLeave(c.off);
        }
        if (!rayOwner.log.empty() || !rayOwner.changes.empty()) std::fflush(stdout);
        rayOwner.log.clear(), rayOwner.changes.clear();
    };
    // Gaze mode outside games, and its dot (see the top): lastMove/lastHeld/pulseAt.
    bool inGame = false, gazeAwake = false, gameSent = false;
    Clock::time_point inGameAt{}, gazeAwakeAt{}, gameSentAt{};
    Clock::time_point lastMove{}, lastHeld{}, pulseAt{};
    // The left button, as sent to the driver; pressRight: the next press is the right button
    // instead (gaze_right), heldButton: the one pressed.
    bool pressRight = false;
    std::string heldButton = "trigger";
    bool confirmLesson = false;  // a keyboard click's quick tap: a lesson with no correction (see the top)
    // The gaze calibration panel is up until then ("calpanel 1"; see the top); calOpened: it just
    // came up, so a press in progress ends without a click.
    Clock::time_point calPanelUntil{};
    bool calOpened = false;
    auto pressLeft = [&] {
        // ft-screens sends the keyboard to the panel clicked last; it sees clicks on
        // its own screens, but only we know when one lands on another panel.
        SendTo(out, "ft_screens", "click " + (lastHit.empty() ? std::string("-") : lastHit));
        lessonSaid.clear();
        // A click after nudging the gaze-placed pointer: the nudge is a lesson, or past
        // POINTER_GAZE_NUDGE_MAX, a quick check (see the top).
        if (gazeOn && nudging && !gazeOwns && havePoint && Clock::now() - nudgeAt < std::chrono::seconds(10) &&
            (confirmLesson || nudgeMoved >= 0.2)) {
            const Vec3 d = RotateInverse(nudgeHead, Normalize(lastPoint - Position(nudgeHead)));
            const double ty = std::atan2(-d.x, -d.z) * 180 / M_PI, tp = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI;
            const double off = std::hypot(std::remainder(ty - nudgeRawHy, 360.0), tp - nudgeRawHp);
            char msg[160];
            if (off <= gazeNudgeMax)
                std::snprintf(msg, sizeof msg, "lesson %.3f %.3f %.3f %.3f", nudgeRawHy, nudgeRawHp, ty, tp);
            else
                std::snprintf(msg, sizeof msg, "recheck %.0f", off);
            SendTo(out, "ft_gazed", msg);
            lessonSaid = off <= gazeNudgeMax ? "a lesson" : "the quick check";
            if (debug) std::printf("gaze %s (nudged %.2f deg, off %.2f)\n", msg, nudgeMoved, off);
            if (debug) std::fflush(stdout);
        }
        nudging = confirmLesson = false;
        // A click on nothing: maybe on an overlay that came up since the list was read.
        if (lastHit.empty()) overlays.Kick();
        leftHeld = true;
        dragDistance = lastDistance;
        pressKey.clear();
        if (FramePanel(lastHit)) {
            vr::ETrackingUniverseOrigin uo;
            auto it = handles.find(lastHit);
            if (it != handles.end() &&
                overlay->GetOverlayTransformAbsolute(it->second, &uo, &pressPose) == vr::VROverlayError_None)
                pressKey = lastHit;
        }
        tiltYaw = tiltPitch = 0;  // a new drag starts untilted
        dropHoldUntil = {};
        pulseAt = Clock::now();
        heldButton = pressRight ? "b" : "trigger";
        pressRight = false;
        SendTo(out, "ft_pointer", "btn " + heldButton + " 1");
    };
    auto releaseLeft = [&] {
        leftHeld = false;
        tilting = false;
        // Hold the drag pose (tilt, frozen distance) while SteamVR finishes the drop.
        dropHoldUntil = Clock::now() + std::chrono::milliseconds(500);
        SendTo(out, "ft_pointer", "btn " + heldButton + " 0");
        // ft-screens releases a button held on its screens in KWin even when SteamVR hands
        // the release to some other overlay (its catcher usually gets it; this is the backstop).
        SendTo(out, "ft_screens", "up");
        if (gazeBack) gazeOwns = true, gazeBack = false;
    };
    // The left button, from the relay's "btn trigger", with gaze mode's held-back press (see
    // the top).
    auto canAim = [&] {
        return gazeOn && gazeMousePrecision && gazeOwns && !aimHeld && !clickPress && !clickRelease &&
               hold.src == Src::None;
    };
    // Hold the press back: the pointer stops where the gaze put it.
    auto aimStart = [&](bool right) {
        gazeOwns = false;
        nudging = haveHead && Clock::now() - gz.at < std::chrono::milliseconds(200);
        nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
        nudgeAt = aimSince = Clock::now(), nudgeMoved = 0;
        aimHeld = true, aimRight = right;
    };
    // A drag the right button began (chordDrag): it lasts while either button is held.
    auto chordDrop = [&] {
        chordDrag = false;
        if (leftHeld) releaseLeft();
        if (debug) std::printf("mouse drag dropped\n");
        if (debug) std::fflush(stdout);
    };
    auto leftButton = [&](bool down) {
        leftDown = down;
        if (down) {
            if (aimHeld && aimRight && !aimHand) {
                ignoreLeftUp = true;  // the right's press is held back: the left does nothing
                return;
            }
            if (canAim()) {
                aimStart(false);
                return;
            }
            pressLeft();
            return;
        }
        if (ignoreLeftUp) {
            ignoreLeftUp = false;
            return;
        }
        if (chordDrag) {
            if (!rightDown) chordDrop();  // otherwise the right holds it (tilting, maybe)
            return;
        }
        if (aimHeld && !aimHand && !aimRight) {
            aimHeld = false;
            clickPress = true;  // after this frame's pose, so it lands where the pointer was moved to
            gazeBack = nudgeMoved < 0.2;
            return;
        }
        if (leftHeld) releaseLeft();
    };
    // The right button (see the top); false: not taken here (a tilt, or passed on as it is).
    auto rightButton = [&](bool down) {
        rightDown = down;
        if (down) {
            if (aimHeld && !aimHand && !aimRight) {
                // During the left's held-back press: press the left where the pointer is now (the
                // correction is a lesson), and the mouse drags.
                aimHeld = false;
                chordDrag = gazeBack = true;
                pressLeft();
                if (debug) std::printf("mouse drag began (right during left)\n");
                if (debug) std::fflush(stdout);
                return true;
            }
            if (canAim() && !leftHeld) {
                aimStart(true);
                return true;
            }
            return false;
        }
        if (chordDrag) {
            if (leftDown) return !swallowedRight;  // the left holds it; a tilt's release ends the tilt
            tilting = swallowedRight = false;
            chordDrop();
            return true;
        }
        if (aimHeld && !aimHand && aimRight) {
            aimHeld = aimRight = false;
            pressRight = clickPress = true;  // the right click, where the pointer was moved to
            gazeBack = nudgeMoved < 0.2;
            return true;
        }
        if (leftHeld && heldButton == "b") {  // its press, held still into a real one
            releaseLeft();
            return true;
        }
        return false;
    };
    // POINTER_GAZE_MOUSE_MOVE=held (see the top): the gaze is fresh and nothing is pressed, so a
    // mouse move doesn't move the pointer.
    auto mouseMoveHeld = [&] {
        return gazeOn && gazeMouseHeld && !inGame && !headsetOff && Clock::now() - gz.at < std::chrono::seconds(1) &&
               !aimHeld && !leftHeld && !tilting && hold.src == Src::None && !clickPress && !clickRelease;
    };

    // --- Panel placement (see "Placement" at the top of the file) ---
    // Device pose, given in the standing universe, sent to the driver in raw space.
    auto sendPose = [&](Vec3 originStanding, const Basis &b) {
        vr::TrackedDevicePose_t s, r;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &s, 1);
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0, &r, 1);
        const auto &S = s.mDeviceToAbsoluteTracking, &R = r.mDeviceToAbsoluteTracking;
        auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };
        const Vec3 o = Position(R) + toRaw(originStanding - Position(S));
        double q[4];
        BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
        char msg[200];
        std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", o.x, o.y, o.z, q[0], q[1], q[2], q[3]);
        SendTo(out, "ft_pointer", msg);
        posed.valid = false;
    };
    auto sleepMs = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    auto headPos = [&] {
        vr::TrackedDevicePose_t s;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &s, 1);
        return std::make_pair(s.bPoseIsValid, Position(s.mDeviceToAbsoluteTracking));
    };
    // Borrow the device and the laser for a placement: connect, claim, laser mode on.
    auto borrow = [&] {
        SendTo(out, "ft_pointer", "show");
        overlay->ShowOverlay(laserMode);
        cursorDot.Show(false);
        markerDot.Show(false);
        sleepMs(active ? 50 : 400);  // a fresh connect needs SteamVR to bind the device
        SendTo(out, "ft_pointer", "btn a 1");
        sleepMs(60);
        SendTo(out, "ft_pointer", "btn a 0");
    };
    auto giveBack = [&] {
        if (!active) {
            SendTo(out, "ft_pointer", "hide");
            overlay->HideOverlay(laserMode);
            laserModeShown = false;
        }
    };
    auto findPanel = [&](const char *key, Panel &p, Vec3 &eye) -> std::string {
        vr::VROverlayHandle_t h;
        if (overlay->FindOverlay(key, &h) != vr::VROverlayError_None) return std::string("no overlay ") + key;
        bool valid;
        std::tie(valid, eye) = headPos();
        if (!valid) return "no head pose (headset off?)";
        p = ScanPanel(h, eye, 0.5);
        if (!p.found) return std::string("panel not visible: ") + key;
        return "";
    };

    // grabprobe (maintenance): aim down from the panel's bottom edge, 1 cm a step, and log where
    // SteamVR's laser hits something (the hit dot shows) and whether the window controls are up.
    auto grabProbe = [&](const char *key) {
        Panel p;
        Vec3 eye;
        const std::string err = findPanel(key, p, eye);
        if (!err.empty()) {
            std::printf("grabprobe: %s\n", err.c_str());
            std::fflush(stdout);
            return;
        }
        borrow();
        vr::VROverlayHandle_t footer = vr::k_ulOverlayHandleInvalid;
        overlay->FindOverlay("valve.steam.gamepadui.floatingfooter", &footer);
        const Vec3 bottom = p.center - p.basis.y * (p.height / 2);
        std::printf("grabprobe %s: center (%.3f %.3f %.3f) %.3f x %.3f m\n", key, p.center.x, p.center.y, p.center.z,
                    p.width, p.height);
        for (int cm = -5; cm <= 45; ++cm) {
            const Vec3 target = bottom - p.basis.y * (cm / 100.0);
            sendPose(eye, AimBasis(target - eye));
            sleepMs(90);
            const bool dot = systemPointer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(systemPointer);
            const bool foot = footer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(footer);
            std::printf("  %+3d cm below the bottom edge: steamvr_dot=%d footer=%d", cm, dot, foot);
            if (foot) {
                vr::ETrackingUniverseOrigin uo;
                vr::HmdMatrix34_t t{};
                if (overlay->GetOverlayTransformAbsolute(footer, &uo, &t) == vr::VROverlayError_None) {
                    const Vec3 f = Position(t) - p.center;
                    std::printf(" footer at panel (%.3f %.3f %.3f)", Dot(f, p.basis.x), Dot(f, p.basis.y),
                                Dot(f, p.basis.z));
                }
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        giveBack();
    };

    // Carry a floating panel so its centre lands on `target` with frame `bt` (see
    // "Placement" at the top). Returns "ok ..." or "error ...".
    auto place = [&](const char *key, Vec3 target, const Basis &bt, double grabBelow) -> std::string {
        Panel p;
        Vec3 eye;
        std::string err = findPanel(key, p, eye);
        if (!err.empty()) return "error " + err;
        auto offBy = [&](const Panel &q, double &cm, double &deg) {
            cm = Length(q.center - target) * 100;
            const double c = (Dot(q.basis.x, bt.x) + Dot(q.basis.y, bt.y) + Dot(q.basis.z, bt.z) - 1) / 2;
            deg = std::acos(std::clamp(c, -1.0, 1.0)) * 180 / M_PI;
        };
        double cm, deg;
        int moves = 0;
        borrow();
        for (int attempt = 0; attempt < 3; ++attempt) {
            offBy(p, cm, deg);
            if (cm < 1.5 && deg < 1.0) break;
            // The rigid motion that takes the panel to the target: rotate by R, then move.
            auto turn = [&](Vec3 v) { return FromBasis(bt, ToBasis(p.basis, v)); };
            double q[4];
            BasisQuat({turn({1, 0, 0}), turn({0, 1, 0}), turn({0, 0, 1})}, q);
            const double angle = 2 * std::acos(std::clamp(q[0], -1.0, 1.0));
            const Vec3 axis = std::sin(angle / 2) > 1e-6 ? Normalize({q[1], q[2], q[3]}) : Vec3{0, 1, 0};
            const Vec3 grab = p.center - p.basis.y * (p.height / 2 + grabBelow);
            const Basis d0 = AimBasis(grab - eye);
            const Vec3 o1 = target + turn(eye - p.center);  // device origin at the end
            ++moves;
            sendPose(eye, d0);
            sleepMs(150);  // hover: the window controls come up
            SendTo(out, "ft_pointer", "btn trigger 1");
            sleepMs(150);
            // 1. Rotate about the device origin (the eye): the dashboard applies it exactly.
            const int rsteps = std::max(4, int(angle * 180 / M_PI / kPlaceDegPerSec * 60));
            for (int i = 1; i <= rsteps; ++i) {
                const double a = angle * i / rsteps;
                auto r = [&](Vec3 v) { return RotateAbout(v, axis, a); };
                sendPose(eye, {r(d0.x), r(d0.y), r(d0.z)});
                sleepMs(16);
            }
            // 2. Slide the device slowly: fast moves are accelerated by the dashboard.
            const Basis d1{turn(d0.x), turn(d0.y), turn(d0.z)};
            const int tsteps = std::max(4, int(Length(o1 - eye) / slideSpeed * 60));
            for (int i = 1; i <= tsteps; ++i) {
                sendPose(eye + (o1 - eye) * (double(i) / tsteps), d1);
                sleepMs(16);
            }
            sleepMs(100);
            SendTo(out, "ft_pointer", "btn trigger 0");
            sleepMs(600);  // the dashboard re-reads the pose up to 150 ms after the release
            err = findPanel(key, p, eye);
            if (!err.empty()) break;
        }
        giveBack();
        if (!err.empty()) return "error after the move: " + err;
        offBy(p, cm, deg);
        char msg[160];
        std::snprintf(msg, sizeof msg, "ok %s off by %.1f cm, %.1f deg after %d move%s", key, cm, deg, moves,
                      moves == 1 ? "" : "s");
        std::printf("place: %s\n", msg);
        std::fflush(stdout);
        return msg;
    };

    // "move <dyaw> <dpitch>" from the relay.
    auto mouseMove = [&](double a, double b) {
        // A move held back (POINTER_GAZE_MOUSE_MOVE=held) only wakes the pointer: it isn't using
        // the mouse, so a drifting mouse doesn't keep the gaze from taking the pointer back.
        const bool heldBack = mouseMoveHeld();
        if (!heldBack) lastMouse = Clock::now();
        // Any mouse input wakes the pointer (after a controller took over, or a helper restart).
        if (!active) wake(Clock::now());
        if (heldBack) return;
        // The mouse takes the pointer from a hand's ray (last used wins); the ray comes back once the mouse
        // has been idle a second and the hand points again (see "Hand ray" at the top).
        if (raySide >= 0 || rays[0].Shown() || rays[1].Shown()) {
            rays[0].Reset(), rays[1].Reset();
            rays[0].event.clear(), rays[1].event.clear();
            rayOwner.Drop("the mouse");
            ownerChanges(false);
            if (raySide >= 0) raySide = -1, rayGoneAt = Clock::now(), rayHoldCancel = true;
        }
        rayWoke = false;
        if (tilting) {
            tiltYaw += a;
            tiltPitch = std::clamp(tiltPitch + b, -80.0, 80.0);
            return;
        }
        lastMove = Clock::now();  // the dot shows while the mouse moves it (gaze mode)
        if (gazeOn && gazeOwns) {
            // The mouse takes the pointer from the gaze, from where the gaze left it.
            gazeOwns = false;
            nudging = haveHead && Clock::now() - gz.at < std::chrono::milliseconds(200);
            nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
            nudgeAt = Clock::now(), nudgeMoved = 0;
        }
        if (nudging || aimHeld) nudgeMoved += std::hypot(a, b);
        if (!anchored) recenter = true;
        yaw += a;
        while (yaw > 180) yaw -= 360;
        while (yaw < -180) yaw += 360;
        pitch = std::clamp(pitch + b, -85.0, 85.0);
    };

    std::printf("ft-pointer running: free distance %.2f m, dot %.2f deg\n", freeDistance, cursorDeg);
    std::fflush(stdout);

    while (true) {
        // The headset off: SteamVR drops the HMD's activity to idle as soon as it comes off.
        // An awake pointer (a connected controller, SteamVR's laser mode forced on) kept the
        // displays from sleeping, so it's released at once, and the mouse can't wake it until
        // the headset is back on (then the first mouse input does).
        const auto level = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
        headsetOff = level == vr::k_EDeviceActivityLevel_Idle || level == vr::k_EDeviceActivityLevel_Standby ||
                     level == vr::k_EDeviceActivityLevel_Idle_Timeout;
        if (headsetOff && active) {
            active = false;
            claimPending = claimHeld = false;
            cursorDot.Show(false);
            markerDot.Show(false);
            SendTo(out, "ft_pointer", "btn a 0");
            SendTo(out, "ft_pointer", "hide");
            std::printf("headset off: pointer released\n");
            std::fflush(stdout);
        }

        // Outside games, gaze mode keeps the pointer (see the top); the relay needs to know.
        {
            const auto t = Clock::now();
            if (t - inGameAt > std::chrono::milliseconds(500)) {
                inGameAt = t;
                inGame = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
            }
            // Hand gestures in the last two minutes keep it the same way.
            const bool handsRecent = handsOn && handUsed != Clock::time_point{} && t - handUsed < std::chrono::minutes(2);
            const bool awake = (gazeOn || handsRecent) && !inGame && !headsetOff;
            if (awake != gazeAwake || t - gazeAwakeAt > std::chrono::seconds(5)) {
                if (awake != gazeAwake)
                    std::printf("%s keeps the pointer: %s\n", gazeOn ? "gaze" : "hand use",
                                awake ? "yes" : "no (off, in a game, or headset off)");
                if (awake != gazeAwake) std::fflush(stdout);
                gazeAwake = awake;
                gazeAwakeAt = t;
                SendTo(out, "frametop_relay", awake ? "gazeawake 1" : "gazeawake 0");
            }
            // The relay pauses Frametop for VR games (input/game_pause.py). Repeated, so a relay
            // that restarts learns it, and silence (SteamVR gone) ends the game there.
            if (inGame != gameSent || t - gameSentAt > std::chrono::seconds(5)) {
                gameSent = inGame;
                gameSentAt = t;
                SendTo(out, "frametop_relay", inGame ? "vrgame 1" : "vrgame 0");
            }
        }

        // Commands from the relay.
        char buf[256];
        ssize_t n;
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        while ((n = recvfrom(in, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            // Reply to the sender (the layout tool binds an abstract address to get answers).
            const sockaddr_un sender = from;
            const socklen_t senderLen = fromLen;
            fromLen = sizeof from;
            auto reply = [&](const std::string &msg) {
                if (senderLen > offsetof(sockaddr_un, sun_path))
                    sendto(out, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr *>(&sender), senderLen);
            };
            // Mouse moves first, the most frequent command.
            double mx, my;
            if (std::strncmp(buf, "move ", 5) == 0 && std::sscanf(buf + 5, "%lf %lf", &mx, &my) == 2) {
                mouseMove(mx, my);
                continue;
            }
            // The gaze, from ft-gazed: not mouse input, it never wakes the pointer.
            double g[4];
            if (std::sscanf(buf, "gz %lf %lf %lf %lf", &g[0], &g[1], &g[2], &g[3]) == 4) {
                gz = {g[0], g[1], g[2], g[3], Clock::now()};
                gazeHist.Add({secsOf(gz.at), g[0], g[1], g[2], g[3]});  // for a gaze pinch's lock
                continue;
            }
            // The gaze calibration panel (see the top): presses answer it instead of clicking.
            {
                int on;
                if (std::sscanf(buf, "calpanel %d", &on) == 1) {
                    const bool was = Clock::now() < calPanelUntil;
                    calPanelUntil = on ? Clock::now() + std::chrono::seconds(3) : Clock::time_point{};
                    if (on && !was) calOpened = true;
                    continue;
                }
            }
            if (Clock::now() < calPanelUntil) {
                const CalPanelAnswer answer = calPanelAnswer(buf);
                if (answer == CalPanelAnswer::Accept || answer == CalPanelAnswer::Quit) {
                    SendTo(out, "ft_gazed", answer == CalPanelAnswer::Accept ? "calaccept" : "calquit");
                    continue;
                }
                if (answer == CalPanelAnswer::Ignore)
                    continue;  // their releases, and the other buttons: nothing to click now
            }
            {
                char kind[16], source[16];
                int v;
                if (std::sscanf(buf, "%15s %15s %d", kind, source, &v) == 3 &&
                    (!std::strcmp(kind, "precision") || !std::strcmp(kind, "gazedrag"))) {
                    lastMouse = Clock::now();
                    if (!active) wake(Clock::now());
                    devicePresses.push_back({source, !std::strcmp(kind, "gazedrag"), v != 0});
                    continue;
                }
                if (std::sscanf(buf, "gazekey %15s %d", source, &v) == 2 &&
                    (!std::strcmp(source, "left") || !std::strcmp(source, "right"))) {
                    lastMouse = Clock::now();
                    if (!active) wake(Clock::now());
                    keyPresses.push_back({!std::strcmp(source, "right"), v != 0});
                    continue;
                }
            }
            // Hand interaction (see "Handoff" at the top): who holds a controller, from the relay.
            {
                char side[16], what[16];
                int v = 0;
                const int got = std::sscanf(buf, "ctl %15s %15s %d", side, what, &v);
                if (got >= 2) {
                    const double t = secsOf(Clock::now());
                    const int k = !std::strcmp(side, "left") ? 0 : !std::strcmp(side, "right") ? 1 : -1;
                    if (!std::strcmp(side, "ws")) arbiter.Socket(std::atoi(what) != 0, t);
                    else if (!std::strcmp(what, "touch") && got == 3) arbiter.Touch(k, v != 0, t);
                    else if (!std::strcmp(what, "press")) arbiter.Press(k, t);
                    else if (!std::strcmp(what, "gone")) arbiter.Gone(k, t);
                    continue;
                }
                if (std::sscanf(buf, "paused %d", &v) == 1) {
                    framePaused = v != 0;
                    continue;
                }
                unsigned long long ns;
                int l, r;
                if (std::sscanf(buf, "near %llu %d %d", &ns, &l, &r) == 3) {
                    nearNs = ns, nearSide[0] = l != 0, nearSide[1] = r != 0, nearAt = Clock::now();
                    nearClaim[0] = l == 1, nearClaim[1] = r == 1;   // 2: the ray hides, the gestures stay ours
                    continue;
                }
            }
            if (std::strcmp(buf, "typing") == 0) {  // not mouse input: it never wakes the pointer
                typist.Key(secsOf(Clock::now()));
                continue;
            }
            if (std::strncmp(buf, "vrbind", 6) == 0) {
                std::printf("controller buttons: %s\n", controllerButtons.Bind(buf + 6).c_str());
                std::fflush(stdout);
                continue;
            }
            if (std::strncmp(buf, "vrglobal", 8) == 0) {
                const char *arg = buf + 8;
                while (*arg == ' ') ++arg;
                ControllerButtons::SetGlobal(std::strncmp(arg, "off", 3) != 0);
                reply(controllerButtons.Status());
                continue;
            }
            if (std::strncmp(buf, "vrstatus", 8) == 0) {
                reply(controllerButtons.Status());
                continue;
            }
            if (std::strncmp(buf, "overlay ", 8) == 0 && std::strncmp(buf + 8, "frametop.", 9) == 0) {
                overlays.Add(buf + 8);
                continue;
            }
            if (std::strncmp(buf, "overlays", 8) == 0) {
                overlays.Request(sender, senderLen);  // answered from the list's thread
                continue;
            }
            if (std::strncmp(buf, "gaze", 4) == 0) {
                const char *arg = buf + 4;
                while (*arg == ' ') ++arg;
                if (*arg != '?') {  // "gaze ?" only asks
                    gazeOn = std::strncmp(arg, "on", 2) == 0    ? true
                             : std::strncmp(arg, "off", 3) == 0 ? false
                                                                : !gazeOn;
                    gazeOwns = true, nudging = false;
                    std::printf("gaze mode %s\n", gazeOn ? "on" : "off");
                    std::fflush(stdout);
                }
                // "gaze ? headset" (the gaze service, which idles while nobody wears the headset)
                // also says whether someone does.
                if (std::strstr(arg, "headset"))
                    reply(gazeOn ? (headsetOff ? "ok on away" : "ok on worn") : (headsetOff ? "ok off away" : "ok off worn"));
                else
                    reply(gazeOn ? "ok on" : "ok off");
                continue;
            }
            // (Moves were taken first, above.)
            const bool mouseInput = std::strncmp(buf, "btn", 3) == 0 || std::strncmp(buf, "scroll", 6) == 0;
            if (mouseInput) lastMouse = Clock::now();
            // Mouse input wakes the pointer (after a controller took over, or a helper restart),
            // but a release doesn't (a button up, a scroll back to 0 0): the relay sends those with
            // the pointer off when Frametop pauses for a game (input-relay.py stand_down), and
            // waking would connect the virtual controller during the game. The release is still
            // handled below (it ends its press, or goes to the driver), so no button stays down.
            {
                char name[16];
                int v = 1;
                double sx = 1, sy = 1;
                const bool release = (std::sscanf(buf, "btn %15s %d", name, &v) == 2 && v == 0) ||
                                     (std::sscanf(buf, "scroll %lf %lf", &sx, &sy) == 2 && sx == 0 && sy == 0);
                if (!active && mouseInput && !release) wake(Clock::now());
            }
            char key[128];
            double px, py, pz, pyaw, ppitch, proll = 0, pgrab = -1;
            if (std::sscanf(buf, "grabprobe %127s", key) == 1) {
                grabProbe(key);
                continue;
            }
            if (std::sscanf(buf, "place %127s %lf %lf %lf %lf %lf %lf %lf", key, &px, &py, &pz, &pyaw, &ppitch, &proll,
                            &pgrab) >= 6) {
                reply(place(key, {px, py, pz}, PanelBasis(pyaw, ppitch, proll), pgrab >= 0 ? pgrab : grabOffset));
                continue;
            }
            if (std::sscanf(buf, "measure %127s", key) == 1) {
                Panel p;
                Vec3 eye;
                const std::string err = findPanel(key, p, eye);
                char msg[400] = "";
                if (err.empty())
                    std::snprintf(msg, sizeof msg, "ok %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                                  p.center.x, p.center.y, p.center.z, p.width, p.height, p.basis.x.x, p.basis.x.y,
                                  p.basis.x.z, p.basis.y.x, p.basis.y.y, p.basis.y.z, p.basis.z.x, p.basis.z.y,
                                  p.basis.z.z);
                reply(err.empty() ? msg : "error " + err);
                continue;
            }
            if (std::strncmp(buf, "head", 4) == 0) {
                vr::TrackedDevicePose_t h;
                sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &h, 1);
                const Vec3 e = Position(h.mDeviceToAbsoluteTracking);
                const Vec3 f = Rotate(h.mDeviceToAbsoluteTracking, {0, 0, -1});
                char msg[200];
                std::snprintf(msg, sizeof msg, "ok %.4f %.4f %.4f %.2f %.2f", e.x, e.y, e.z,
                              std::atan2(-f.x, -f.z) * 180 / M_PI, std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI);
                reply(h.bPoseIsValid ? msg : "error no head pose (headset off?)");
                continue;
            }
            if (std::strncmp(buf, "debug", 5) == 0) {
                debug = !debug;
                std::printf("debug %s\n", debug ? "on" : "off");
                std::fflush(stdout);
                continue;
            }
            if (std::strncmp(buf, "btn trigger 1", 13) == 0) {
                leftButton(true);
                continue;
            } else if (std::strncmp(buf, "btn trigger 0", 13) == 0) {
                leftButton(false);
                continue;
            } else if (std::strncmp(buf, "btn b 1", 7) == 0 && rightButton(true)) {
                continue;
            } else if (std::strncmp(buf, "btn b 0", 7) == 0 && rightButton(false)) {
                continue;
            } else if (std::strncmp(buf, "btn b 1", 7) == 0 && leftHeld) {
                tilting = tiltStart = swallowedRight = true;  // right press while dragging: tilt, no right-click
                continue;
            } else if (std::strncmp(buf, "btn b 0", 7) == 0 && swallowedRight) {
                tilting = swallowedRight = false;
                continue;
            }
            if (std::strncmp(buf, "recenter", 8) == 0) {
                recenter = true;
            } else if (std::strncmp(buf, "reload", 6) == 0) {
                loadConfig();
                lastSlow = Clock::now() - std::chrono::seconds(10);  // apply POINTER_IGNORE now
                pass.valid = false;
                std::printf("reloaded: free distance %.2f m, dot %.2f deg, origin %.2f, head follow %s, leash %.0f deg, "
                            "controller pickup %.1fx, %zu ignored\n",
                            freeDistance, cursorDeg, originFraction, follow ? "on" : "off", leashDeg, pickupScale,
                            ignore.size());
                std::fflush(stdout);
            } else if (std::strncmp(buf, "follow", 6) == 0) {
                const char *arg = buf + 6;
                while (*arg == ' ') ++arg;
                const bool was = follow;
                follow = std::strncmp(arg, "on", 2) == 0 ? true : std::strncmp(arg, "off", 3) == 0 ? false : !follow;
                if (follow && !was) followReset = true;
                std::printf("head follow %s (leash %.0f deg)\n", follow ? "on" : "off", leashDeg);
                std::fflush(stdout);
            } else if (std::strncmp(buf, "show", 4) == 0) {
                if (!active) wake(Clock::now());
            } else if (std::strncmp(buf, "hide", 4) == 0) {
                active = false;
                cursorDot.Show(false);
                markerDot.Show(false);
                SendTo(out, "ft_pointer", "hide");
            } else {
                SendTo(out, "ft_pointer", buf);  // btn, scroll
            }
        }

        vr::TrackedDevicePose_t all[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.011f, all, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hmd = all[0];
        vr::TrackedDevicePose_t hmdRaw;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.011f, &hmdRaw, 1);
        const auto tnow = Clock::now();

        // Claim pulse (switchlaserhand on the driver's "a" button, no click).
        if (claimPending && tnow >= claimAt) {
            SendTo(out, "ft_pointer", "btn a 1");
            claimPending = false;
            claimHeld = true;
            claimRelease = tnow + std::chrono::milliseconds(60);
        } else if (claimHeld && tnow >= claimRelease) {
            SendTo(out, "ft_pointer", "btn a 0");
            claimHeld = false;
        }

        // The overlay list is only needed while the pointer is awake (see OverlayList).
        overlays.SetPaused(!active || headsetOff);

        // Laser mode on while the pointer is awake.
        if (active != laserModeShown) {
            laserModeShown = active;
            if (active) overlay->ShowOverlay(laserMode);
            else overlay->HideOverlay(laserMode);
            if (debug) std::printf("laser mode %s\n", active ? "forced on" : "released");
            if (debug) std::fflush(stdout);
        }

        // Didn't get the hand role (a held controller keeps it): release, back off. Not as a treadmill,
        // which has no controller role at all (see POINTER_ROLE at the top): its hint says so.
        if (active && tnow - wokeAt > std::chrono::seconds(1) && ours != vr::k_unTrackedDeviceIndexInvalid &&
            sys->GetControllerRoleForTrackedDeviceIndex(ours) == vr::TrackedControllerRole_Invalid &&
            sys->GetInt32TrackedDeviceProperty(ours, vr::Prop_ControllerRoleHint_Int32) !=
                vr::TrackedControllerRole_Treadmill) {
            active = false;
            claimPending = claimHeld = false;
            cursorDot.Show(false);
            markerDot.Show(false);
            SendTo(out, "ft_pointer", "btn a 0");
            SendTo(out, "ft_pointer", "hide");
            noWakeUntil = tnow + std::chrono::seconds(2);
            std::printf("no hand role (a controller is in use): pointer released\n");
            std::fflush(stdout);
        }

        // Last used wins: a real controller being moved releases the pointer (see the top),
        // in gaze mode too. Not while a bare hand's ray drives it: that ray goes only when its own
        // side turns controller (see "Handoff" at the top), so a controller in the other hand
        // doesn't take it.
        if (active && hold.src == Src::None && raySide < 0 && tnow - lastMouse > std::chrono::milliseconds(500)) {
            for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                if (i == ours || !all[i].bPoseIsValid || all[i].eTrackingResult != vr::TrackingResult_Running_OK ||
                    sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) {
                    movingSince[i] = {};
                    continue;
                }
                const auto &v = all[i].vVelocity.v, &w = all[i].vAngularVelocity.v;
                const double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const double spin = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                if (speed <= 0.35 * pickupScale && spin <= 2.0 * pickupScale) {
                    movingSince[i] = {};
                    continue;
                }
                if (movingSince[i] == Clock::time_point{}) movingSince[i] = tnow;
                if (tnow - movingSince[i] >= std::chrono::milliseconds(100)) {
                    active = false;
                    claimPending = claimHeld = false;
                    cursorDot.Show(false);
                    markerDot.Show(false);
                    SendTo(out, "ft_pointer", "btn a 0");
                    SendTo(out, "ft_pointer", "hide");
                    std::printf("controller %u moved (%.2f m/s, %.1f rad/s): pointer released\n", i, speed, spin);
                    std::fflush(stdout);
                    break;
                }
            }
        } else {
            std::fill(std::begin(movingSince), std::end(movingSince), Clock::time_point{});
        }
        const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
        const Vec3 eye{hm[0][3], hm[1][3], hm[2][3]};

        if (recenter && hmd.bPoseIsValid) {
            anchor = eye;
            const Vec3 f{-hm[0][2], -hm[1][2], -hm[2][2]};
            yaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
            pitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            anchored = true;
            recenter = false;
            followReset = true;
        }

        // Gaze mode (see the top): the gaze has the pointer, or takes it back when you look
        // well away from it. Not while a press holds the pointer, and only on fresh gaze.
        if (hmd.bPoseIsValid) lastHead = hmd.mDeviceToAbsoluteTracking, haveHead = true;
        if (gazeOn && active && hmd.bPoseIsValid && !tilting && !leftHeld && !aimHeld && !clickPress && !clickRelease &&
            tnow >= dropHoldUntil && raySide < 0 && !chordBtn.On() &&
            tnow - gz.at < std::chrono::milliseconds(150)) {
            const Vec3 g = Rotate(hmd.mDeviceToAbsoluteTracking, Direction(gz.hy, gz.hp));
            if (!gazeOwns && havePoint) {
                const double off = std::acos(std::clamp(Dot(g, Normalize(lastPoint - eye)), -1.0, 1.0)) * 180 / M_PI;
                if (off > gazeRetake && tnow - lastMouse > std::chrono::milliseconds(300)) {
                    if (retakeSince == Clock::time_point{}) retakeSince = tnow;
                    if (tnow - retakeSince >= std::chrono::milliseconds(120)) gazeOwns = true, nudging = false;
                } else {
                    retakeSince = {};
                }
            }
            if (gazeOwns) {
                retakeSince = {};
                anchor = eye;
                anchored = true;
                yaw = std::atan2(-g.x, -g.z) * 180 / M_PI;
                pitch = std::clamp(std::asin(std::clamp(g.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
            }
        }

        // Hands (see the top): pinches and grips from ft-hands, and the handoff for ft-screens' direct touch.
        if (handsTrack) {
            vr::TrackedDevicePose_t h0;
            sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &h0, 1);
            if (h0.bPoseIsValid) poses.Add(tnow, h0.mDeviceToAbsoluteTracking);
        }
        // Handoff (see the top): per side, a controller in use or the bare hand.
        if (handsTrack) {
            const bool newHands = handsRoom.Update(poses);
            if (tnow - devicesAt > std::chrono::seconds(1)) {
                devicesAt = tnow;
                for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                    DeviceInfo &d = devices[i];
                    d.controller = sys->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller;
                    if (!d.controller) continue;
                    char type[64] = "";
                    sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
                    d.ours = std::strcmp(type, "ft_pointer") == 0;
                    d.hint = sys->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerRoleHint_Int32);
                    d.charging = sys->GetBoolTrackedDeviceProperty(i, vr::Prop_DeviceIsCharging_Bool);
                }
            }
            std::vector<handoff::Controller> ctls;
            for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                const DeviceInfo &d = devices[i];
                if (!d.controller || d.ours || i == ours) continue;
                const auto role = sys->GetControllerRoleForTrackedDeviceIndex(i);
                // its hand role; one our device holds (POINTER_ROLE=left|right) leaves it with its hint
                const int r = role == vr::TrackedControllerRole_LeftHand || role == vr::TrackedControllerRole_RightHand
                                  ? int(role) : d.hint;
                handoff::Controller c;
                c.id = i;
                c.side = r == vr::TrackedControllerRole_LeftHand ? 0 : r == vr::TrackedControllerRole_RightHand ? 1 : -1;
                c.connected = all[i].bDeviceIsConnected;
                c.charging = d.charging;
                c.tracked = all[i].bPoseIsValid && all[i].eTrackingResult == vr::TrackingResult_Running_OK;
                c.lost = !all[i].bPoseIsValid || all[i].eTrackingResult == vr::TrackingResult_Running_OutOfRange;
                const Vec3 p = Position(all[i].mDeviceToAbsoluteTracking);
                c.pos = {p.x, p.y, p.z};
                c.vel = {all[i].vVelocity.v[0], all[i].vVelocity.v[1], all[i].vVelocity.v[2]};
                const auto &w = all[i].vAngularVelocity.v;
                c.spin = std::sqrt(double(w[0]) * w[0] + double(w[1]) * w[1] + double(w[2]) * w[2]);
                ctls.push_back(c);
            }
            const uint64_t ns =
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow.time_since_epoch()).count());
            const bool live = handsRoom.Live(ns);
            std::vector<handoff::Hand> seen;
            if (live)
                for (const hin::RoomHand &h : handsRoom.hands) {
                    handoff::Hand b;
                    b.side = h.side, b.pos = {h.palm.x, h.palm.y, h.palm.z};
                    b.vel = {h.vel.x, h.vel.y, h.vel.z}, b.velValid = h.velValid;
                    seen.push_back(b);
                }
            arbiter.Update(secsOf(tnow), ctls, seen, live);
            for (const std::string &line : arbiter.log) std::printf("%s\n", line.c_str());
            if (!arbiter.log.empty()) std::fflush(stdout);
            arbiter.log.clear();
            // In a VR game, only with the dashboard open or Frametop toggled back on (not paused).
            if (tnow - dashboardAt > std::chrono::milliseconds(100)) {
                dashboardAt = tnow;
                dashboardUp = overlay->IsDashboardVisible();
            }
            // The relay parks hand tracking while Frametop is paused for a game, and unparks it while
            // the dashboard is open: "vrdash 1|0", on a change and every 5 s in a game.
            if (dashboardUp != dashSent || (inGame && tnow - dashSentAt > std::chrono::seconds(5))) {
                dashSent = dashboardUp, dashSentAt = tnow;
                SendTo(out, "frametop_relay", dashboardUp ? "vrdash 1" : "vrdash 0");
            }
            handsAllowed = !headsetOff && (!inGame || dashboardUp || !framePaused);
            const std::string msg = std::string("hands ") + handoff::Name(arbiter.State(0)) + " " +
                                    handoff::Name(arbiter.State(1)) + (handsAllowed ? " 1" : " 0");
            if (msg != handsSent || tnow - handsSentAt > std::chrono::seconds(1)) {
                if (msg != handsSent && debug) std::printf("%s\n", msg.c_str());
                handsSent = msg, handsSentAt = tnow;
                SendTo(out, "ft_screens", msg);
                SendTo(out, "ft_handview", msg);   // draws the bare hands with Room View off
            }

            // Typing (see the top): each new hands frame (ft-hands' side call), then which hands it
            // holds back now, for the ray below, the gestures, and ft-screens.
            typist.SetParams(typingParams);
            vr::HmdMatrix34_t typingHead;
            if (newHands && live && poses.At(handsRoom.capture_ns, typingHead)) {
                typing::Hand th[2];
                for (int s = 0; s < 2; ++s)
                    for (const hin::RoomHand &c : handsRoom.hands) {
                        if (c.side != s) continue;
                        th[s].seen = true;
                        th[s].palm = {c.palm.x, c.palm.y, c.palm.z};
                        th[s].down = -c.palmNormal.y;
                        // its ray up, or a gesture held (the chord's pinch too): in use (pointing, then Enter
                        // with the other hand)
                        th[s].busy = rays[s].Shown() || (hold.side == s && (hold.src == Src::Hand || hold.src == Src::Ray)) ||
                                     (chordBtn.On() && chordSide == s);
                        break;
                    }
                const Vec3 hp = Position(typingHead), hf = Rotate(typingHead, Vec3{0, 0, -1});
                typing::Head th0;
                th0.pos = {hp.x, hp.y, hp.z}, th0.fwd = {hf.x, hf.y, hf.z};
                typist.Frame(double(handsRoom.capture_ns) / 1e9, th0, th);
                for (const std::string &l : typist.log) std::printf("%s\n", l.c_str());
                if (!typist.log.empty()) std::fflush(stdout);
                typist.log.clear();
            }
            const bool typingSide[2] = {typist.Blocked(0, secsOf(tnow)), typist.Blocked(1, secsOf(tnow))};
            const std::string typingMsg = std::string("typing ") + (typingSide[0] ? "1" : "0") + (typingSide[1] ? " 1" : " 0");
            if (typingMsg != typingSent || tnow - typingSentAt > std::chrono::seconds(1)) {
                if (typingMsg != typingSent && debug) std::printf("%s\n", typingMsg.c_str());
                typingSent = typingMsg, typingSentAt = tnow;
                SendTo(out, "ft_screens", typingMsg);   // no pokes, near grabs or "grab that" by those hands
            }

            // The hand ray (see "Hand ray" at the top): each side's hand, when a new frame came.
            const bool mouseIdle = tnow - lastMouse >= std::chrono::seconds(1);
            auto mayPoint = [&](int s) {
                return handsOn && gazehand::RayAllowed(rayOn, gazeOn, gazeHandRay) && handsAllowed && mouseIdle &&
                       arbiter.State(s) == handoff::Owner::Hand && !typingSide[s];
            };
            if (rayParamsNew) {
                rayParamsNew = false;
                for (fhray::HandRay &r : rays) r.SetParams(rayParams);
            }
            vr::HmdMatrix34_t capHead;
            if (newHands && live && rayOn && handsOn && poses.At(handsRoom.capture_ns, capHead)) {
                const double tc = double(handsRoom.capture_ns) / 1e9;
                const Vec3 hp = Position(capHead), hf = Rotate(capHead, Vec3{0, 0, -1});
                fhray::V3 wrists[FH_HANDS_MAX_HANDS];  // the torso faces between the head and the hands
                int nw = 0;
                for (const hin::RoomHand &c : handsRoom.hands)
                    if (nw < FH_HANDS_MAX_HANDS) wrists[nw++] = {c.pts[0].x, c.pts[0].y, c.pts[0].z};
                body.Update(tc, {hp.x, hp.y, hp.z}, {hf.x, hf.y, hf.z}, wrists, nw);
                // the hand each side's ray follows: ft-hands' side call, held through a brief flip (rayowner.h)
                std::vector<rayown::SeenHand> seenHands;
                for (const hin::RoomHand &c : handsRoom.hands) seenHands.push_back({c.id, c.side});
                int pick[2];
                raySlots.Update(tc, seenHands, pick);
                for (int s = 0; s < 2; ++s) {
                    if (pick[s] < 0) continue;
                    const hin::RoomHand *h = &handsRoom.hands[size_t(pick[s])];
                    auto fv = [](Vec3 v) { return fhray::V3{v.x, v.y, v.z}; };
                    fhray::HandFrame f;
                    f.t = tc, f.stereo = h->stereo;
                    f.knuckle = fv(h->knuckle), f.wrist = fv(h->pts[0]), f.palmNormal = fv(h->palmNormal);
                    f.middle = fv(h->pts[9]), f.ring = fv(h->pts[13]), f.little = fv(h->pts[17]), f.palm = true;
                    f.elbow = fv(h->elbow), f.elbowConf = h->elbowConf;
                    f.grip = gripDown[s];
                    // near: ft-screens says so (a fresh report), or the fingertip is at an overlay: a cast
                    // from 20 cm behind the index tip along the finger meets one within 25 cm (the tip
                    // within 5 cm of it). Only while the ray could show, so it costs nothing otherwise.
                    f.near = tnow - nearAt < std::chrono::milliseconds(200) && nearSide[s];
                    if (!f.near && active && mayPoint(s) && (rays[s].Shown() || rays[s].LastTests().show)) {
                        const Vec3 tip = h->pts[8], fd = Normalize(h->pts[8] - h->pts[7]), from = tip - fd * 0.2;
                        for (const auto &[key, handle] : handles) {
                            if (!visible[key] || sceneGraph[key]) continue;
                            vr::VROverlayIntersectionParams_t params{};
                            params.vSource = {float(from.x), float(from.y), float(from.z)};
                            params.vDirection = {float(fd.x), float(fd.y), float(fd.z)};
                            params.eOrigin = vr::TrackingUniverseStanding;
                            vr::VROverlayIntersectionResults_t r{};
                            if (overlay->ComputeOverlayIntersection(handle, &params, &r) && r.fDistance < 0.25f) {
                                f.near = true;
                                break;
                            }
                        }
                    }
                    rays[s].Frame(body, f, mayPoint(s), secsOf(tnow));
                    rayAims[s].Add(tc, rays[s].Current(), rays[s].Shown(), h->pinchD[0]);
                }
            }
            for (int s = 0; s < 2; ++s) {
                rays[s].Tick(secsOf(tnow), mayPoint(s));
                if (rays[s].event.empty()) continue;
                if (rays[s].event.rfind("hide ", 0) == 0) rayHid[s] = rays[s].event.substr(5);
                if (debug) {
                    const fhray::Tests &ts = rays[s].LastTests();
                    std::printf("ray %s %s (below %.2f m, ahead %.2f m, palm %.2f, pitch %.0f, yaw off %.0f)\n",
                                s ? "right" : "left", rays[s].event.c_str(), ts.below, ts.ahead, ts.palm, ts.pitch, ts.yawOff);
                    std::fflush(stdout);
                }
                rays[s].event.clear();
            }
            // Which ray drives the pointer: the owner keeps it through a grace after its ray goes down
            // (rayowner.h); one whose side may not point (a controller in use, a VR game) leaves at once.
            rayown::Params op;
            op.grace = rayGrace;
            rayOwner.SetParams(op);
            // typing: the owner lets the pointer go to the mouse or the gaze, not to the controllers (an "off")
            if (rayOwner.Side() >= 0 && typingSide[rayOwner.Side()]) rayOwner.Drop("typing");
            const bool shown[2] = {rays[0].Shown(), rays[1].Shown()}, may[2] = {mayPoint(0), mayPoint(1)};
            rayOwner.Update(secsOf(tnow), shown, may, rayHid);
            ownerChanges(true);
            if (raySide >= 0 && !active && !headsetOff) {  // a wake that was held off (just released): again
                wake(tnow);
                rayWoke = active;
            }
        } else if (raySide >= 0 || rayOwner.Side() >= 0) {
            rayOwner.Drop("hand input off");
            ownerChanges(false);
            rayLeave(false);
        }
        // The ray woke the pointer and has been gone a while (no mouse, no gaze meanwhile): release it.
        if (active && rayWoke && raySide < 0 && hold.src == Src::None && !leftHeld && !gazeOn &&
            tnow - rayGoneAt > std::chrono::duration<double>(rayRelease) &&
            tnow - lastMouse > std::chrono::duration<double>(rayRelease)) {
            releasePointer("hand ray gone");
        }
        // A hand at a panel (ft-screens' near claim: it pokes and taps there): no laser at all (user,
        // 2026-10-11: the laser from the head to where the fingertip put the cursor got in the way of
        // tapping, and held the button so pokes were refused). The mouse's next move claims it back.
        if (active && hold.src == Src::None && !leftHeld && tnow - nearAt < std::chrono::milliseconds(200) &&
            (nearClaim[0] || nearClaim[1]) && tnow - lastMouse > std::chrono::seconds(1)) {
            releasePointer("a hand touches a panel");
        }
        // Where a gesture's point is, seen from the hold's origin: yaw and pitch, degrees.
        auto handAngles = [&](const float p[3], uint64_t t_ns, const Vec3 &from, double &hy, double &hp) {
            vr::HmdMatrix34_t head;
            if (!poses.At(t_ns, head)) return false;
            const Vec3 d = Normalize(Position(head) + Rotate(head, Vec3{p[0], p[1], p[2]}) - from);
            hy = std::atan2(-d.x, -d.z) * 180 / M_PI;
            hp = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI;
            return true;
        };
        // A gaze pinch ended (see "Gaze pinch" at the top): its log line now, or a click's once it goes out.
        auto gazePinchEnds = [&](bool lost) {
            if (!lost && !hold.pressed && aimHeld) {
                gazeClick = hold.gaze, gazeClickSide = hold.side;
            } else {
                const std::string why = gazeEndWhy.empty() ? "the gaze calibration panel came up" : gazeEndWhy;
                std::printf("%s %s: %s\n", hold.gaze.Right() ? "middle pinch" : "pinch", hold.side ? "right" : "left",
                            hold.gaze.Says(secsOf(tnow), lost, why, "", hold.gaze.ByChord() ? lessonSaid : "").c_str());
                std::fflush(stdout);
            }
            gazeEndWhy.clear();
        };
        auto endHold = [&](bool lost) {
            if (hold.gaze.On()) gazePinchEnds(lost);
            if (hold.pressed) {
                if (leftHeld) releaseLeft();
            } else if (aimHeld) {
                aimHeld = aimHand = false;
                if (lost) {
                    if (gazeOn) gazeOwns = true, nudging = false;  // no click: the pointer goes back to the gaze
                } else {
                    clickPress = true;  // the click is on the release, where the pointer is now
                    pressRight = hold.right;
                    gazeBack = nudgeMoved < 0.2;
                }
            }
            if (debug)
                std::printf("hold %s %s%s\n", hold.src == Src::Hand ? (hold.grip ? "grip" : "pinch")
                                              : hold.src == Src::Head ? (hold.right ? "key right" : "key left")
                                              : hold.grip ? "gaze drag" : "gaze precision",
                            lost ? "lost" : "released", hold.pressed ? "" : lost ? " (no click)" : " (click)");
            if (debug) std::fflush(stdout);
            hold = {};
        };
        // The press, once a hold's source is set: a real press (dragging until the release), or
        // held back like gaze mode's mouse press (see the top): the click comes on the release.
        auto startHold = [&](bool press) {
            hold.startYaw = hold.lastYaw = yaw, hold.startPitch = hold.lastPitch = pitch;
            hold.pressed = press;
            if (press) {
                gazeBack = gazeOn && gazeOwns;
                pressLeft();
            } else {
                gazeOwns = false;
                nudging = gazeOn && haveHead && tnow - gz.at < std::chrono::milliseconds(200);
                nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
                nudgeAt = aimSince = tnow, nudgeMoved = 0;
                aimHeld = aimHand = true;
                pulseAt = tnow;
            }
        };
        // The hold's source moved to (hy, hp): past the dead zone, the pointer follows at the gain,
        // from where it was when it got past.
        auto steer = [&](double hy, double hp, double deadzone, double gain) {
            const double dy = std::remainder(hy - hold.refYaw, 360.0), dp = hp - hold.refPitch;
            if (!hold.engaged) {
                if (std::hypot(dy, dp) <= deadzone) return;
                hold.engaged = true;
                hold.refYaw = hy, hold.refPitch = hp;
                hold.startYaw = hold.lastYaw = yaw, hold.startPitch = hold.lastPitch = pitch;
                return;
            }
            yaw = std::remainder(hold.startYaw + gain * dy, 360.0);
            pitch = std::clamp(hold.startPitch + gain * dp, -85.0, 85.0);
            // How far the nudge went: for a keyboard click, from where the dot was at the press
            // (a head wobbles on the way); otherwise the path, as with the mouse.
            if (!hold.pressed && hold.src == Src::Head)
                nudgeMoved = std::hypot(std::remainder(yaw - hold.pressYaw, 360.0), pitch - hold.pressPitch);
            else if (!hold.pressed)
                nudgeMoved += std::hypot(std::remainder(yaw - hold.lastYaw, 360.0), pitch - hold.lastPitch);
            hold.lastYaw = yaw, hold.lastPitch = pitch;
            lastMove = tnow;  // the dot shows while it moves (gaze mode)
        };
        // One line per pinch (index or middle) in the log: what it did (clicked where), or why not.
        auto pinchLog = [&](int side, int kind, const std::string &what) {
            std::printf("%s %s: %s\n", kind == 2 ? "middle pinch" : "pinch", side ? "right" : "left", what.c_str());
            std::fflush(stdout);
        };
        // The chord's right button (see "Gaze pinch" at the top): to the driver as it changes, and the
        // pinch's log line once it's over.
        auto chordSend = [&](int change) {
            if (change > 0) SendTo(out, "ft_pointer", "btn b 1"), pulseAt = tnow;
            else if (change < 0) SendTo(out, "ft_pointer", "btn b 0");
        };
        auto chordOver = [&](bool lost, const std::string &why) {
            pinchLog(chordSide, 0, chordBtn.Pressed() ? chordPinch.Says(secsOf(tnow), lost, why, "", "")
                                                     : "no right button: " + why);
            chordBtn.Reset();
            chordPinch = {};
            chordSide = -1, chordLeft = chordSteers = false;
        };
        // Its pinch ended, or the chord is cut short (lost): released now, or a tap's click goes on.
        auto chordEnds = [&](bool lost, const char *why) {
            if (!chordBtn.On()) return;
            chordSend(chordBtn.End(lost));
            if (chordBtn.Finished()) chordOver(lost, why);
        };
        auto chordHolding = [&](int s, int kind) { return chordBtn.On() && chordSide == s && kind == 0; };
        // A pinch held with no ray up (see "Hands" at the top); its log line says so.
        auto beginHold = [&](int side, bool grip, const fh_pinch_t &g) {
            handUsed = tnow;
            if (hold.src != Src::None) {
                // A grip takes over a pinch on the way to it (closing the hand passes through
                // one); otherwise one hold at a time.
                if (hold.src != Src::Hand || !grip || hold.grip || hold.pressed) {
                    if (!grip) pinchLog(side, 0, "ignored (no ray up; a press is already held)");
                    return;
                }
                if (hold.gaze.On()) gazeEndWhy = "a grip took over", gazePinchEnds(true);  // its log line
                aimHeld = aimHand = false;
                hold = {};
            }
            if (leftHeld || aimHeld || clickPress || clickRelease || chordBtn.On()) {  // the mouse's button is busy
                if (!grip) pinchLog(side, 0, "ignored (no ray up; the button is held)");
                return;
            }
            if (!active) {
                wake(tnow);  // the first gesture only wakes the pointer, like the first mouse move
                if (!grip) pinchLog(side, 0, "no ray up: it woke the pointer, no click");
                return;
            }
            vr::HmdMatrix34_t head;
            if (!poses.At(g.begin_ns, head)) return;
            // A grip only with the hand held up (hands on a desk curl like a loose fist; looking down
            // at them puts them straight ahead in the head's frame, where ft-hands can't tell). Typing
            // was checked before (see "Typing" at the top).
            const Vec3 at = Position(head) + Rotate(head, Vec3{g.begin_point[0], g.begin_point[1], g.begin_point[2]});
            if (grip && Position(head).y - at.y > handBelow) {
                if (debug) std::printf("hand grip ignored: %.2f m below the eyes\n", Position(head).y - at.y);
                return;
            }
            Hold h;
            h.src = Src::Hand, h.side = side, h.grip = grip, h.origin = Position(head);
            if (!handAngles(g.begin_point, g.begin_ns, h.origin, h.refYaw, h.refPitch)) return;
            hold = h;
            // A grip, or a pinch without gaze mode: a real press where the pointer is (where
            // you look, in gaze mode), dragging with the hand until it opens.
            startHold(grip || !gazeOn);
            if (!grip)
                pinchLog(side, 0, gazeOn ? "no ray up: held back at the gaze, clicks where the pointer is at the release"
                                         : "no ray up: a press where the pointer is, on " + (lastHit.empty() ? std::string("nothing") : lastHit));
            if (debug) std::printf("hand %s %s began\n", side ? "right" : "left", grip ? "grip" : "pinch");
            if (debug) std::fflush(stdout);
        };
        // Gaze precision and gaze drag buttons (see the top): the mouse steers.
        for (const DevicePress &p : devicePresses) {
            if (p.source == "left" || p.source == "right") continue;  // no controllers in gaze mode
            if (!p.down) {
                if (hold.src == Src::Mouse) endHold(false);
                continue;
            }
            if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
            Hold h;
            h.src = Src::Mouse, h.grip = p.drag;
            hold = h;
            startHold(p.drag);
            if (debug) std::printf("hold gaze %s began (%s)\n", p.drag ? "drag" : "precision", p.source.c_str());
            if (debug) std::fflush(stdout);
        }
        devicePresses.clear();
        // Keyboard clicks (see the top): held back at the gaze, steered by the head.
        auto headAngles = [&](double &hy, double &hp) {
            if (!hmd.bPoseIsValid) return false;
            const Vec3 f = Normalize({-hm[0][2], -hm[1][2], -hm[2][2]});
            hy = std::atan2(-f.x, -f.z) * 180 / M_PI;
            hp = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            return true;
        };
        for (const KeyPress &k : keyPresses) {
            (k.right ? keyRightDown : keyLeftDown) = k.down;
            if (!k.down) {
                if (keyTilting && k.right) {  // the tilt ends; the head drags again from here
                    keyTilting = tilting = false;
                    hold.engaged = false;
                    if (debug) std::printf("hold key left: tilt ended\n");
                    if (debug) std::fflush(stdout);
                }
                // The keys holding it: a gaze_left then gaze_right drag, either; otherwise its own.
                const bool held = hold.keyDrag ? keyLeftDown || keyRightDown : hold.right ? keyRightDown : keyLeftDown;
                if (hold.src == Src::Head && !held) {
                    if (!hold.pressed && aimHeld && tnow - aimSince < std::chrono::duration<double>(keyTap)) {
                        // A quick tap: a click where the dot was at the press, and the gaze was right.
                        yaw = hold.pressYaw, pitch = hold.pressPitch;
                        nudgeMoved = 0;
                        confirmLesson = true;
                    }
                    endHold(false);
                    keyTilting = false;
                }
                continue;
            }
            if (k.right && hold.src == Src::Head && !hold.right && hold.pressed && leftHeld) {
                // gaze_right during a gaze_left drag: tilt while it's held (see the top).
                tilting = tiltStart = keyTilting = true;
                headAngles(keyTiltYaw, keyTiltPitch);
                if (debug) std::printf("hold key left: tilt began\n");
                if (debug) std::fflush(stdout);
                continue;
            }
            if (k.right && hold.src == Src::Head && !hold.right && !hold.pressed && aimHeld) {
                // gaze_right while gaze_left aims: press the left button where the dot is now
                // (the correction is a lesson), then the head drags.
                aimHeld = aimHand = false;
                hold.pressed = hold.grip = hold.keyDrag = true, hold.promote = false, hold.engaged = false;
                headAngles(hold.refYaw, hold.refPitch);
                gazeBack = gazeOn;
                pressRight = false;
                pressLeft();
                if (debug) std::printf("hold key left: pressed (right key)\n");
                if (debug) std::fflush(stdout);
                continue;
            }
            if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
            Hold h;
            h.src = Src::Head, h.promote = true, h.right = k.right, h.pressYaw = yaw, h.pressPitch = pitch;
            if (!headAngles(h.refYaw, h.refPitch)) continue;
            hold = h;
            startHold(false);
            if (debug) std::printf("hold key %s began\n", k.right ? "right" : "left");
            if (debug) std::fflush(stdout);
        }
        keyPresses.clear();
        if (calOpened) {  // the calibration panel came up: a press in progress ends, no click
            calOpened = false;
            if (hold.src == Src::Ray) {
                pinchLog(hold.side, hold.rayKind, "no click (the gaze calibration panel came up)");
                rays[hold.raySlot].Hold(false, secsOf(tnow)), SendTo(out, "ft_pointer", "scroll 0 0");
            }
            if (hold.src != Src::None) endHold(true);
            if (leftHeld) releaseLeft();
            chordEnds(true, "the gaze calibration panel came up");
            aimHeld = aimHand = clickPress = false;
        }
        if (hold.src == Src::Head) {
            double hy, hp;
            if (headAngles(hy, hp) && keyTilting) {
                // The head turns the panel, as the mouse does in a tilt; the pointer stays put.
                tiltYaw += std::remainder(hy - keyTiltYaw, 360.0);
                tiltPitch = std::clamp(tiltPitch + hp - keyTiltPitch, -80.0, 80.0);
                keyTiltYaw = hy, keyTiltPitch = hp;
            } else if (headAngles(hy, hp)) {
                steer(hy, hp, hold.pressed ? 0.3 : headDeadzone, 1.0);
            }
        }
        fh_gestures_t hg;
        const bool handOk = handsOn && handFile.Read(hg);
        if (handOk) handOkAt = tnow;
        const char *const kLostPinch = "ft-hands lost the hand, or a grip took the pinch";
        // A ray hold (see "Hand ray" at the top): the pinch's press goes out once the laser claim
        // has landed; a middle pinch is a right click (a tap) or scrolling (held and moved).
        auto rayRoom = [&](const float p[3], uint64_t t_ns, Vec3 &out) {
            vr::HmdMatrix34_t head;
            if (!poses.At(t_ns, head)) return false;
            out = hin::ToRoom(head, p);
            return true;
        };
        // A ray pinch (see "Hand ray" at the top). side: the gesture's; rs: the ray's (the one that follows
        // the pinching hand). The aim is locked where the ray pointed just before the fingers began to
        // close (rayowner.h), and the dead zone counts from the ray now.
        auto beginRayHold = [&](int side, int rs, int kind, const fh_pinch_t &g) {
            handUsed = tnow;
            if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease || chordBtn.On()) {
                pinchLog(side, kind, "ignored (a press is already held)");
                return;
            }
            const bool took = rayOwner.Pinched(rs, true) == rayown::Owner::Pinch::Take;
            rays[rs].ShowForPinch();  // a ray that dropped as the fingers closed comes back
            rays[rs].event.clear();
            ownerChanges(true);       // a pinch by the other hand takes the ray (rayEnter)
            Hold h;
            h.src = Src::Ray, h.side = side, h.raySlot = rs, h.rayKind = kind, h.pressed = false;
            h.took = took, h.beganAt = tnow;
            auto angles = [](const fhray::V3 &d, double &y, double &p) {
                const Vec3 n = Normalize({d.x, d.y, d.z});
                y = std::atan2(-n.x, -n.z) * 180 / M_PI;
                p = std::clamp(std::asin(std::clamp(n.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
            };
            angles(rays[rs].Current().dir, h.refYaw, h.refPitch);
            fhray::Ray lock;
            if (rayAims[rs].Lock(double(g.begin_ns) / 1e9, handOk && hg.end_m > 0 ? hg.end_m : 0.035, lock, h.lockAge))
                angles(lock.dir, yaw, pitch);
            else
                yaw = h.refYaw, pitch = h.refPitch, h.lockAge = -1;
            h.startYaw = h.lastYaw = yaw, h.startPitch = h.lastPitch = pitch;
            if (!rayRoom(g.begin_point, g.begin_ns, h.beginPoint)) h.beginPoint = rayOrigin;
            // the laser: a pinch always claims it (if it isn't ours), and the press waits 30 ms for it
            rayInactive = false;
            const bool ours_ = laserOurs();
            if (!ours_ && !claimPending && !claimHeld) claimPending = true, claimAt = tnow;
            h.pendingPress = kind == 0;
            h.pressAt = tnow + std::chrono::milliseconds(ours_ ? 0 : 30 + (claimHeld ? 0 : 8));
            hold = h;
            rays[rs].Hold(true, secsOf(tnow));
            if (debug) std::printf("ray %s %s began\n", rs ? "right" : "left", kind ? "middle pinch" : "pinch");
            if (debug) std::fflush(stdout);
        };
        // What a ray pinch did, for its log line.
        auto rayPinchSays = [&](const char *what) {
            char b[256];
            const double held = std::chrono::duration<double>(tnow - hold.beganAt).count();
            char aim[48] = "aim now";
            if (hold.lockAge >= 0) std::snprintf(aim, sizeof aim, "aim %.0f ms before", hold.lockAge * 1000);
            std::snprintf(b, sizeof b, "%s (%s ray%s, %s, held %.2f s)", what, hold.raySlot ? "right" : "left",
                          hold.took ? ", took it" : "", aim, held);
            return std::string(b);
        };
        auto hitName = [&](const std::string &k) { return k.empty() ? std::string("nothing") : k; };
        auto finishRayHold = [&] {
            const int rs = hold.raySlot;
            hold = {};
            if (rs >= 0) rays[rs].Hold(false, secsOf(tnow));
        };
        // A ray pinch ended (lost: by losing the hand, a grip, or the ray; why says which).
        auto endRayHold = [&](bool lost, const char *why) {
            const int side = hold.side, kind = hold.rayKind;
            if (!lost && !hold.pressed && (kind == 0 || !hold.scrolling)) {
                // a tap before its press went out (or a middle tap): it clicks once the press can go
                // (the press block below), along the locked aim
                hold.released = hold.pendingPress = true;
                return;
            }
            if (kind == 0 && hold.pressed && leftHeld) releaseLeft();
            if (kind == 2 && hold.scrolling) SendTo(out, "ft_pointer", "scroll 0 0");
            std::string what;
            if (kind == 2 && hold.scrolling) what = "scrolled";
            else if (!hold.pressed) what = std::string("no click: ") + why;
            else if (lost) what = "pressed on " + hitName(hold.pressHit) + ", let go: " + why;
            else what = (hold.engaged ? "dragged from " : "clicked on ") + hitName(hold.pressHit);
            pinchLog(side, kind, rayPinchSays(what.c_str()));
            finishRayHold();
        };
        // A gesture that began: by the ray, held back like the gaze (beginHold), or nothing.
        // rayFor: the ray that follows the gesture's hand (rayowner.h's Slots), else its side's.
        auto rayFor = [&](int s, const fh_pinch_t &g) {
            const int r = raySlots.SideOf(g.hand_id);
            return r >= 0 ? r : s;
        };
        auto canRay = [&](int rs) { return rays[rs].CanShowForPinch(secsOf(tnow), rayGrace); };
        // The chord (see "Gaze pinch" at the top): an index pinch by the other hand during a gaze pinch.
        auto chordable = [&](int side, int kind) {
            return kind != 1 && hold.src == Src::Hand &&
                   gazehand::Chordable(hold.gaze, hold.side, side, kind == 2, chordBtn.On());
        };
        // The chord's pinch: the right button where the pointer is, until the pinch ends. A left still
        // held back becomes a real press (at the end of this frame, after its pose), and the right goes
        // down 30 ms after it.
        auto beginChord = [&](int side, const fh_pinch_t &g, const Vec3 &origin) {
            double hy, hp;
            if (!handAngles(g.begin_point, g.begin_ns, origin, hy, hp)) return;
            gazehand::Params gp;
            gp.dragGain = gripGain;
            chordSide = side, chordOrigin = origin, chordSteers = false;
            chordPinch.BeginChord(gp, secsOf(tnow), yaw, pitch, hy, hp);
            chordLeft = hold.gaze.Chord(secsOf(tnow));
            chordBtn.Begin(secsOf(tnow), chordLeft);
            if (debug) std::printf("hand %s pinch began: the chord's right button\n", side ? "right" : "left");
            if (debug) std::fflush(stdout);
        };
        // A gaze pinch (see "Gaze pinch" at the top): kind 0 a pinch, 2 a middle pinch (the right button).
        // The press is held back at the gaze locked from just before the fingers began to close; the
        // other hand's index pinch during one is the chord's (beginChord).
        auto beginGazePinch = [&](int side, int kind, const fh_pinch_t &g) {
            handUsed = tnow;
            const bool chord = chordable(side, kind);
            gazehand::Situation sit;
            sit.busy = !chord && (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease || chordBtn.On());
            sit.active = active;  // (typing was checked before, for the chord's pinch too: typing.h, not gazehand's key guard)
            if (const char *why = gazehand::Refused(sit)) {
                if (!sit.busy && !active) wake(tnow);  // the first gesture only wakes the pointer
                pinchLog(side, kind, why);
                return;
            }
            vr::HmdMatrix34_t head;
            if (!poses.At(g.begin_ns, head)) return;
            if (chord) {
                beginChord(side, g, Position(head));
                return;
            }
            Hold h;
            h.src = Src::Hand, h.side = side, h.right = kind == 2, h.origin = Position(head);
            if (!handAngles(g.begin_point, g.begin_ns, h.origin, h.refYaw, h.refPitch)) return;
            gazehand::Params gp;
            gp.deadzone = gazePinchDeadzone, gp.gain = gazePinchGain, gp.hold = gazeHold, gp.dragGain = gripGain;
            gp.controlHold = gazeControlHold;
            gazehand::GazeSample gs;
            double lockAge = -1;
            vr::HmdMatrix34_t gazeHead;
            const bool locked = gazeHist.Lock(double(g.begin_ns) / 1e9, gp, gs, lockAge) &&
                                poses.At(uint64_t(gs.t * 1e9), gazeHead);
            if (locked) {
                const Vec3 d = Rotate(gazeHead, Direction(gs.hy, gs.hp));
                yaw = std::atan2(-d.x, -d.z) * 180 / M_PI;
                pitch = std::clamp(std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
                anchor = eye, anchored = true;
            }
            hold = h;
            startHold(false);
            // the lesson's look is the locked one (ft-gazed finds it in what it sent)
            if (locked) nudging = true, nudgeRawHy = gs.rhy, nudgeRawHp = gs.rhp, nudgeHead = gazeHead;
            hold.gaze.Begin(gp, secsOf(tnow), yaw, pitch, h.refYaw, h.refPitch, kind == 2, locked ? lockAge : -1);
            if (debug) std::printf("hand %s %s began at the gaze\n", side ? "right" : "left", kind ? "middle pinch" : "pinch");
            if (debug) std::fflush(stdout);
        };
        auto startGesture = [&](int s, int kind, const fh_pinch_t &g, bool ray) {
            const int rs = rayFor(s, g);
            // (and still: a ray that went for good while ft-screens was asked isn't; a grip needs it up)
            ray = ray && (kind == 1 ? rays[rs].Shown() : canRay(rs));
            if (chordable(s, kind)) {
                beginGazePinch(s, kind, g);  // the chord, ray or not (see "Gaze pinch" at the top)
            } else if (ray && kind != 1) {
                beginRayHold(s, rs, kind, g);
            } else if (gazeOn && !gazePinchOn) {
                // POINTER_GAZE_PINCH=0: in gaze mode a hand with no ray up doesn't click (ft-screens' near
                // poke and grab are their own)
                if (kind != 1) pinchLog(s, kind, "ignored (no ray up; hand clicks at the gaze are off)");
            } else if (gazeOn && kind != 1) {
                beginGazePinch(s, kind, g);
            } else if (ray || kind == 2) {
                // a fist with the ray up, or a middle pinch without it: nothing
                if (kind == 2) pinchLog(s, kind, "ignored (no ray up)");
                else if (debug) std::printf("hand %s grip ignored (the ray is up)\n", s ? "right" : "left");
            } else {
                beginHold(s, kind == 1, g);
            }
        };
        auto holdingKind = [&](int s, int kind) {
            if (hold.side != s) return false;
            if (hold.src == Src::Ray) return hold.rayKind == kind;
            return hold.src == Src::Hand && hold.grip == (kind == 1) && hold.right == (kind == 2);
        };
        auto endGesture = [&](bool lost, const char *why) {
            if (hold.src == Src::Ray) endRayHold(lost, why);
            else {
                if (hold.gaze.On()) gazeEndWhy = why;
                endHold(lost);
            }
        };
        if (rayHoldCancel) {  // the driving ray went (a controller, the mouse, hand input off): its hold ends, as lost
            rayHoldCancel = false;
            if (hold.src == Src::Ray) endRayHold(true, "the ray went (a controller, the mouse, or hand input off)");
        }
        if (handOk && (hg.seq != handSeq || handFile.opens != handOpens)) {
            handSeq = hg.seq;
            handPublished = hg.publish_ns;
            // [pinch, grip, middle pinch][side]
            const fh_pinch_t *slots[3] = {hg.pinch, hg.grip, hg.pinch2};
            for (int s = 0; s < 2; ++s) gripDown[s] = hg.grip[s].flags & FH_PINCH_DOWN;
            // A new file, or a tracker whose counters went back: start counting from here.
            bool rebase = !handBaseline || handFile.opens != handOpens;
            for (int k = 0; k < 3; ++k)
                for (int s = 0; s < 2; ++s)
                    rebase = rebase || slots[k][s].begins < seenBegins[k][s] || slots[k][s].ends < seenEnds[k][s];
            if (rebase) {
                if (hold.src == Src::Hand || hold.src == Src::Ray) endGesture(true, "ft-hands started over");
                chordEnds(true, "ft-hands started over");
                pendingBegins.clear();
                for (int k = 0; k < 3; ++k)
                    for (int s = 0; s < 2; ++s) seenBegins[k][s] = slots[k][s].begins, seenEnds[k][s] = slots[k][s].ends;
                handBaseline = true, handOpens = handFile.opens;
            }
            // Not over a VR game (unless the dashboard is up), and not with the headset off.
            const bool allowed = handsAllowed;
            // ft-screens tells which hands are near its panels; while it does (a report within 2 s), a
            // gesture waits for its report on the begin's frame (60 ms at most), and one near is left to it.
            const bool screensNear = tnow - nearAt < std::chrono::seconds(2);
            for (int k : {1, 0, 2})  // grips first: one takes over a pinch
                for (int s = 0; s < 2; ++s) {
                    const fh_pinch_t &g = slots[k][s];
                    const bool began = g.begins != seenBegins[k][s], ended = g.ends != seenEnds[k][s];
                    const bool lost = g.flags & FH_PINCH_LOST;
                    seenBegins[k][s] = g.begins, seenEnds[k][s] = g.ends;
                    if (holdingKind(s, k) && ended) endGesture(lost, kLostPinch);  // the one held ended (another may have begun)
                    if (chordHolding(s, k) && ended) chordEnds(lost, kLostPinch);
                    for (PendingBegin &p : pendingBegins)
                        if (p.side == s && p.kind == k && ended) p.ended = true, p.lost = lost;
                    if (!began) continue;
                    if (!allowed) {
                        if (k != 1) pinchLog(s, k, "ignored (hands not allowed: a VR game, or the headset off)");
                        continue;
                    }
                    // Not by a hand typing holds back (see "Typing" at the top).
                    if (const char *why = typist.Why(s, secsOf(tnow))) {
                        if (k != 1) pinchLog(s, k, std::string("ignored (") + why + ")");
                        else if (debug) std::printf("hand %s grip ignored: %s\n", s ? "right" : "left", why);
                        continue;
                    }
                    // Not on a side that has a controller in use, and not by a hand at a controller
                    // (reaching for it or holding it: by position, whatever ft-hands calls its side).
                    bool atController = arbiter.IsController(s);
                    Vec3 at;
                    if (!atController && rayRoom(g.begin_point, g.begin_ns, at))
                        atController = arbiter.NearController({at.x, at.y, at.z});
                    if (atController) {
                        if (k != 1) pinchLog(s, k, "ignored (at a controller)");
                        else if (debug) std::printf("hand %s grip ignored: at a controller\n", s ? "right" : "left");
                        continue;
                    }
                    // Ray or gaze is decided once, here, from the state at the begin: a ray pinch when the
                    // pinching hand's ray is up, or may come up for it (fh_ray.h's ShowForPinch).
                    const bool ray = canRay(rayFor(s, g));
                    if (screensNear && nearNs < g.begin_ns) {
                        PendingBegin p{s, k, g, ray, tnow + std::chrono::milliseconds(60)};
                        p.ended = !(g.flags & FH_PINCH_DOWN), p.lost = lost;
                        pendingBegins.push_back(p);
                        continue;
                    }
                    if (screensNear && nearClaim[s]) {  // ft-screens' (a grab, a poke, a click)
                        if (k != 1) pinchLog(s, k, "left to ft-screens (the hand is near its panel)");
                        continue;
                    }
                    // A hand just in front of a panel (its ray hidden there) without the gaze: nothing to aim
                    // with, so no press at the mouse's pointer and no wake (user, 2026-10-11: the laser came
                    // back at odd moments, a pinch by a panel woke the pointer).
                    if (screensNear && nearSide[s] && tnow - nearAt < std::chrono::milliseconds(200) && !ray && !gazeOn) {
                        if (k != 1) pinchLog(s, k, "ignored (the hand is at a panel: no laser there)");
                        continue;
                    }
                    startGesture(s, k, g, ray);
                    // begun and ended since the last read (a quick tap): its release too
                    if (!(g.flags & FH_PINCH_DOWN) && holdingKind(s, k)) endGesture(lost, kLostPinch);
                    else if (!(g.flags & FH_PINCH_DOWN) && chordHolding(s, k)) chordEnds(lost, kLostPinch);
                }
            // A side that turns controller mid-gesture: the gesture ends, as lost.
            if ((hold.src == Src::Hand || hold.src == Src::Ray) && arbiter.IsController(hold.side))
                endGesture(true, "its side has a controller in use");
            if (chordBtn.On() && arbiter.IsController(chordSide)) chordEnds(true, "its side has a controller in use");
            // The held gesture's hand moves the pointer: past the dead zone (a tap's jitter,
            // the pinch point shifting as the fingers close), then at the gain, from there.
            if (hold.src == Src::Hand) {
                const fh_pinch_t &g = hold.grip ? hg.grip[hold.side] : hold.right ? hg.pinch2[hold.side] : hg.pinch[hold.side];
                double hy, hp;
                if ((g.flags & FH_PINCH_TRACKED) && handAngles(g.point, hg.capture_ns, hold.origin, hy, hp)) {
                    if (!hold.gaze.On()) {
                        steer(hy, hp, hold.grip ? 0.5 : pinchDeadzone, hold.grip ? gripGain : pinchGain);
                    } else if (hold.gaze.Hand(hy, hp)) {  // a gaze pinch: nudged, or dragged once pressed
                        yaw = hold.gaze.Yaw(), pitch = hold.gaze.Pitch();
                        if (!hold.pressed) nudgeMoved = hold.gaze.Moved();
                        lastMove = tnow;
                    }
                }
            }
            // The chord's hand (see "Gaze pinch" at the top): followed while the left's pinch drags; once
            // that has let go, it drags from where the pointer is.
            if (chordBtn.On()) {
                const fh_pinch_t &g = hg.pinch[chordSide];
                double hy, hp;
                if ((g.flags & FH_PINCH_TRACKED) && handAngles(g.point, hg.capture_ns, chordOrigin, hy, hp)) {
                    if (hold.src != Src::None) {
                        chordPinch.Track(hy, hp), chordSteers = false;
                    } else {
                        if (!chordSteers) chordPinch.Take(yaw, pitch), chordSteers = true;
                        if (chordPinch.Hand(hy, hp)) yaw = chordPinch.Yaw(), pitch = chordPinch.Pitch(), lastMove = tnow;
                    }
                }
            }
            // A middle pinch held and moved: scrolling, the stick deflected by the hand's displacement
            // from the begin (0.5 cm dead zone, full at 4 cm): up and down, and across the ray.
            if (hold.src == Src::Ray && hold.rayKind == 2) {
                const fh_pinch_t &g = hg.pinch2[hold.side];
                Vec3 now;
                if ((g.flags & FH_PINCH_TRACKED) && rayRoom(g.point, hg.capture_ns, now)) {
                    const Vec3 d = now - hold.beginPoint;
                    const Vec3 right = Normalize(Cross(rayDir, {0, 1, 0}));
                    const double dx = Dot(d, right), dy = d.y;
                    if (!hold.scrolling && std::hypot(dx, dy) > 0.005) hold.scrolling = true;
                    if (hold.scrolling) {
                        auto axis = [&](double v) {
                            const double a = std::clamp((std::fabs(v) - 0.005) / 0.035, 0.0, 1.0) * rayScroll;
                            return std::round((v < 0 ? -a : a) * 20) / 20;  // steps of 0.05: fewer datagrams
                        };
                        const double sx = std::clamp(axis(dx), -1.0, 1.0), sy = std::clamp(axis(dy), -1.0, 1.0);
                        if (sx != hold.scrollX || sy != hold.scrollY) {
                            hold.scrollX = sx, hold.scrollY = sy;
                            char msg[64];
                            std::snprintf(msg, sizeof msg, "scroll %.2f %.2f", sx, sy);
                            SendTo(out, "ft_pointer", msg);
                        }
                    }
                }
            }
        }
        // Gestures waiting for ft-screens' near report: decided once it's in (or 60 ms on).
        for (size_t i = 0; i < pendingBegins.size();) {
            PendingBegin &p = pendingBegins[i];
            const bool reported = nearNs >= p.g.begin_ns;
            if (!reported && tnow < p.deadline) {
                ++i;
                continue;
            }
            const PendingBegin q = p;
            pendingBegins.erase(pendingBegins.begin() + long(i));
            if (reported && nearClaim[q.side]) {
                if (q.kind != 1) pinchLog(q.side, q.kind, "left to ft-screens (the hand is near its panel)");
                else if (debug) std::printf("hand %s grip left to ft-screens (near)\n", q.side ? "right" : "left");
                continue;
            }
            if (reported && nearSide[q.side] && !q.ray && !gazeOn) {   // (as above)
                if (q.kind != 1) pinchLog(q.side, q.kind, "ignored (the hand is at a panel: no laser there)");
                continue;
            }
            startGesture(q.side, q.kind, q.g, q.ray);
            if (q.ended && holdingKind(q.side, q.kind)) endGesture(q.lost, kLostPinch);
            else if (q.ended && chordHolding(q.side, q.kind)) chordEnds(q.lost, kLostPinch);
        }
        // A ray pinch's press, once the claim has had its 30 ms and a pose along the locked aim has gone
        // out (so the press lands there, and lastHit is what it hits).
        // (and once a pointer the ray just woke has been bound: the wake's claim comes 300 ms on)
        // A pinch that already ended (a tap, or a middle tap: the right button) clicks: released 40 ms on.
        if (hold.src == Src::Ray && hold.pendingPress && hold.aimSent && tnow >= hold.pressAt && active &&
            tnow - wokeAt > std::chrono::milliseconds(350)) {
            hold.pendingPress = false;
            hold.pressed = true;
            gazeBack = false;
            pressRight = hold.rayKind == 2;
            pressLeft();
            hold.pressHit = lastHit;
            if (hold.released) {
                hold.releaseAt = tnow + std::chrono::milliseconds(40);
            } else {
                // push and pull: on a carry (a press on no panel's content: a grab bar, a scene-graph
                // control, an edge, one of ft-screens' controls); a panel that starts to move turns it on below
                hold.h0 = rays[hold.raySlot].Reach(), hold.d0 = lastDistance;
                hold.pushOn = rayPush && (lastHit.empty() || pass.onEdge || pass.scene || (lastHit.rfind("frametop.", 0) == 0 && !FramePanel(lastHit)));
            }
        }
        if (hold.src == Src::Ray && hold.released && hold.pressed && tnow >= hold.releaseAt) {
            if (leftHeld) releaseLeft();
            pinchLog(hold.side, hold.rayKind,
                     rayPinchSays(((hold.rayKind == 2 ? "right click on " : "clicked on ") + hitName(hold.pressHit)).c_str()));
            finishRayHold();
        }
        // ft-hands stopped (or hung) with a gesture held: it's over, as lost. Not on one read that failed:
        // the file is rewritten under a sequence lock, and a writer caught mid-write (ft-hands preempted
        // under a CPU cap) fails a read; that ended drags mid-way (2026-10-09 headset test).
        const uint64_t nowNs =
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow.time_since_epoch()).count());
        if (hold.src == Src::Hand || hold.src == Src::Ray || chordBtn.On()) {
            const char *why = !active ? "the pointer went"
                            : handsOn && tnow - handOkAt > std::chrono::milliseconds(500) ? "the gestures file can't be read"
                            : !handsOn ? "hand input turned off"
                            : nowNs - handPublished > 1'500'000'000ull ? "hand tracking stopped (nothing new for 1.5 s)"
                            : nullptr;
            if (why && (hold.src == Src::Hand || hold.src == Src::Ray)) endGesture(true, why);
            if (why) chordEnds(true, why);
        }
        if (hold.src != Src::None && hold.src != Src::Hand && hold.src != Src::Ray && !active) endHold(true);

        // The driving ray (see "Hand ray" at the top): the pointer's origin and direction. During a ray
        // pinch the direction holds still until the ray has turned rayDeadzone, then follows (steer);
        // a middle pinch holds it. Push and pull move the origin along the ray while a carry is held.
        // rayMode: a hand's ray has the pointer, driving it or (its owner's grace, rayowner.h) holding
        // its last pose.
        const bool rayDrives = active && raySide >= 0 && rays[raySide].Shown();
        const bool rayMode = active && raySide >= 0;
        if (rayDrives) {
            const fhray::Ray r = rays[raySide].At(secsOf(tnow));  // between tracker frames too (fh_smooth.h)
            rayOrigin = {r.origin.x, r.origin.y, r.origin.z};
            rayDir = Normalize({r.dir.x, r.dir.y, r.dir.z});
            const double ry = std::atan2(-rayDir.x, -rayDir.z) * 180 / M_PI;
            const double rp = std::asin(std::clamp(rayDir.y, -1.0, 1.0)) * 180 / M_PI;
            if (hold.src == Src::Ray && hold.raySlot == raySide) {
                // a tap's dead zone first: a quick pinch whose hand turned a little still clicks
                const bool tap = tnow - hold.beganAt < std::chrono::duration<double>(rayTap);
                if (hold.rayKind == 0) steer(ry, rp, tap ? std::max(rayDeadzone, rayTapDeadzone) : rayDeadzone, 1.0);
            } else {
                yaw = ry, pitch = std::clamp(rp, -85.0, 85.0);
            }
            if (hold.src == Src::Ray && hold.rayKind == 0 && leftHeld) {
                if (!hold.pushOn && rayPush && pressKey.empty() && FramePanel(lastHit) && hold.d0 > 0) {
                    // a panel being carried (a floating window by its title bar): push and pull from here
                    hold.pushOn = true, hold.h0 = rays[raySide].Reach(), hold.d0 = dragDistance;
                }
                if (hold.pushOn && hold.h0 > 0.05) {
                    // the panel's distance goes with the cube of the reach (design 2.7), past 1.5 cm
                    double dh = rays[raySide].Reach() - hold.h0;
                    dh = std::fabs(dh) <= 0.015 ? 0.0 : dh - std::copysign(0.015, dh);
                    const double dist = std::clamp(hold.d0 * std::pow((hold.h0 + dh) / hold.h0, 3), 0.3, 10.0);
                    pushOffset = dist - hold.d0;
                }
            } else if (tnow >= dropHoldUntil) {
                pushOffset = 0;  // held through the drop (SteamVR reads the pose again after the release)
            }
            anchor = rayOrigin + Direction(yaw, pitch) * pushOffset;
            anchored = true, recenter = false;
            handUsed = tnow;
            // a controller's trigger took the laser: our dot hides until a pinch claims it back
            if (!rayInactive && !claimPending && !claimHeld && tnow - wokeAt > std::chrono::milliseconds(500)) {
                const vr::TrackedDeviceIndex_t p = overlay->GetPrimaryDashboardDevice();
                if (p != ours && p < vr::k_unMaxTrackedDeviceCount && devices[p].controller && !devices[p].ours)
                    rayInactive = true;
            }
        }
        if (aimHeld || leftHeld || clickPress || clickRelease || chordBtn.On()) lastHeld = tnow;

        // Head follow (see the top): past the leash (for the delay), ease the reference to the
        // head's facing, and turn the cursor with it. Not in gaze mode: the gaze places it.
        if (follow && !gazeOn && active && anchored && hmd.bPoseIsValid && !rayMode) {
            const Vec3 head = LimitPitch(Vec3{-hm[0][2], -hm[1][2], -hm[2][2]}, 85);
            if (followReset) {
                followRef = head, followLag = 0, leashOutSince = {};
                followReset = following = false;
            }
            const double dt = std::min(0.1, std::chrono::duration<double>(tnow - followAt).count());
            const double leash = leashDeg * M_PI / 180;
            const double lag = std::acos(std::clamp(Dot(followRef, head), -1.0, 1.0));
            double keep = lag;  // how far the reference stays behind the head after this frame
            if (leash <= 0) {
                keep = 0;  // head-locked
            } else if (following) {
                keep = lag * (leashReturn > 0 ? std::exp(-dt / leashReturn) : 0.0);
                // A fast turn drags it at the leash's end; past it already (after the delay), it
                // can't fall further behind, and it closes in from there without a jump.
                keep = std::min(keep, std::max(leash, followLag));
                if (keep < 0.05 * M_PI / 180) keep = 0, following = false;  // landed on the facing
            } else if (lag > leash) {
                if (leashOutSince == decltype(leashOutSince){}) leashOutSince = tnow;
                if (std::chrono::duration<double>(tnow - leashOutSince).count() >= leashDelay)
                    following = true, leashOutSince = {};
            } else {
                leashOutSince = {};  // back inside before the delay: a glance
            }
            followLag = keep;
            const Vec3 ref = LimitPitch(PullWithin(followRef, head, keep), 85);
            if (!leftHeld && tnow >= dropHoldUntil) {
                // The cursor keeps its offset from the reference (frames without roll).
                Vec3 d = FromBasis(AimBasis(ref), ToBasis(AimBasis(followRef), Direction(yaw, pitch)));
                d = PullWithin(Normalize(d), ref, followReach * M_PI / 180);
                yaw = std::atan2(-d.x, -d.z) * 180 / M_PI;
                pitch = std::clamp(std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
                // The ray origin closes in on the eye as the reference does on the facing.
                anchor = eye + (anchor - eye) * (leash <= 0 ? 0.0 : lag > 1e-6 ? keep / lag : following ? 0.0 : 1.0);
            }
            followRef = ref;
        }
        followAt = tnow;

        // Slow work, once a second and when the overlay list is new: overlay handles, our device
        // index. Only while the pointer is awake (see "Idle" at the top).
        const auto now = std::chrono::steady_clock::now();
        if (active && (now - lastSlow > std::chrono::seconds(1) || overlays.Generation() != listGeneration)) {
            lastSlow = now;
            listGeneration = overlays.Generation();
            const auto before = handles;
            handles.clear();
            for (const auto &key : overlays.Keys()) {
                if (Ignored(ignore, key)) continue;
                vr::VROverlayHandle_t h;
                if (overlay->FindOverlay(key.c_str(), &h) != vr::VROverlayError_None) continue;
                handles[key] = h;
                // Scene-graph overlays are placed absolutely. Other overlays can report no
                // texture too (gamescope's app panels, placed as dashboard tabs, share theirs
                // from another process), and ComputeOverlayIntersection handles those.
                uint32_t tw = 0, th = 0;
                overlay->GetOverlayTextureSize(h, &tw, &th);
                vr::VROverlayTransformType tt = vr::VROverlayTransform_Invalid;
                overlay->GetOverlayTransformType(h, &tt);
                // ft-screens' panels (frametop.screen.N, floating windows with their popups,
                // frametop.float.N..., the keyboard) are 0x0 and absolute too (a shared
                // texture), but they're real panels of any size.
                sceneGraph[key] = (tw == 0 || th == 0) && tt == vr::VROverlayTransform_Absolute &&
                                  key.rfind("frametop.", 0) != 0;
            }
            if (handles != before) ++visibleGen;
            // Our device keeps its index; it's looked for again when a device is activated or
            // deactivated (the events below).
            for (vr::TrackedDeviceIndex_t i = 0; ours == vr::k_unTrackedDeviceIndexInvalid && i < vr::k_unMaxTrackedDeviceCount; ++i) {
                char type[64] = "";
                sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
                if (std::strcmp(type, "ft_pointer") == 0) ours = i;
            }
        }

        if (active && (now - lastVisible > std::chrono::milliseconds(50) || visible.size() != handles.size())) {
            lastVisible = now;
            std::map<std::string, bool> seen;
            for (const auto &[key, h] : handles) seen[key] = overlay->IsOverlayVisible(h);
            if (seen != visible) visible.swap(seen), ++visibleGen;
        }

        if (active && anchored && hmd.bPoseIsValid && tilting) {
            // Rotate the device around the grab point; the grabbed panel turns with it.
            if (tiltStart) {
                pivot = lastPoint;
                tiltOrigin = lastOrigin;
                tiltBasis = AimBasis(lastAim);
                tiltStart = false;  // angles carry on from any earlier tilt in this drag
                cursorDot.Show(false);
                markerDot.Show(false);
            }
            const double yr = tiltYaw * M_PI / 180, pr = tiltPitch * M_PI / 180;
            const Vec3 up{0, 1, 0}, side = tiltBasis.x;
            auto turn = [&](Vec3 v) { return RotateAbout(RotateAbout(v, side, pr), up, yr); };
            const Vec3 originStanding = pivot + turn(tiltOrigin - pivot);
            const Basis b{turn(tiltBasis.x), turn(tiltBasis.y), turn(tiltBasis.z)};
            const auto &S = hmd.mDeviceToAbsoluteTracking, &R = hmdRaw.mDeviceToAbsoluteTracking;
            auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };  // directions: raw <- standing
            const Vec3 originRaw = Position(R) + toRaw(originStanding - eye);
            double q[4];
            BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
            char msg[200];
            std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", originRaw.x, originRaw.y,
                          originRaw.z, q[0], q[1], q[2], q[3]);
            SendTo(out, "ft_pointer", msg);
            posed.valid = false;
        } else if (active && anchored && hmd.bPoseIsValid) {
            const bool dragging = leftHeld || tnow < dropHoldUntil;
            if (!dragging) tiltYaw = tiltPitch = 0;  // drop finished: back to plain pointing
            const Vec3 dir = Direction(yaw, pitch);
            // Nearest visible overlay along a ray (frozen while dragging).
            struct Hit {
                double along = 1e9;
                std::string key;
                bool scene = false;
                Vec3 point, normal;
            };
            auto nearest = [&](Vec3 from, Vec3 d) {
                Hit h;
                for (const auto &[key, handle] : handles) {
                    if (!visible[key]) continue;
                    if (sceneGraph[key]) {
                        // Plane test: overlay origin and its +Z normal, within sceneRadius of the origin.
                        vr::ETrackingUniverseOrigin uo;
                        vr::HmdMatrix34_t t{};
                        if (overlay->GetOverlayTransformAbsolute(handle, &uo, &t) != vr::VROverlayError_None) continue;
                        const Vec3 center = Position(t), normal{t.m[0][2], t.m[1][2], t.m[2][2]};
                        const double denom = Dot(d, normal);
                        if (std::fabs(denom) < 1e-4) continue;
                        const double along = Dot(center - from, normal) / denom;
                        const Vec3 at = from + d * along;
                        if (along > 0.05 && along < h.along && std::sqrt(Dot(at - center, at - center)) <= sceneRadius)
                            h.along = along, h.key = key, h.scene = true, h.point = at, h.normal = normal;
                        continue;
                    }
                    vr::VROverlayIntersectionParams_t params{};
                    params.vSource = {float(from.x), float(from.y), float(from.z)};
                    params.vDirection = {float(d.x), float(d.y), float(d.z)};
                    params.eOrigin = vr::TrackingUniverseStanding;
                    vr::VROverlayIntersectionResults_t r{};
                    if (overlay->ComputeOverlayIntersection(handle, &params, &r) && r.fDistance > 0.05f &&
                        r.fDistance < h.along) {
                        h.along = r.fDistance, h.key = key, h.scene = false;
                        h.point = {r.vPoint.v[0], r.vPoint.v[1], r.vPoint.v[2]};
                        h.normal = {r.vNormal.v[0], r.vNormal.v[1], r.vNormal.v[2]};
                    }
                }
                return h;
            };
            // Unchanged frames (see the top): the last pass still holds.
            const bool reuse = !dragging && pass.valid && tnow - pass.at < std::chrono::milliseconds(100) &&
                               pass.gen == visibleGen && std::fabs(yaw - pass.yaw) < 1e-6 &&
                               std::fabs(pitch - pass.pitch) < 1e-6 && Length(anchor - pass.anchor) < 0.001 &&
                               Length(eye - pass.eye) < 0.005;
            double best = pass.best, distance = pass.distance;
            std::string bestKey = pass.key;
            bool bestScene = pass.scene, onEdge = pass.onEdge, occluded = pass.occluded;
            Vec3 point = pass.point;
            if (!reuse) {
                Hit first;
                if (!dragging) first = nearest(anchor, dir);
                best = first.along;
                bestKey = first.key;
                bestScene = first.scene;
                Vec3 bestPoint = first.point, bestNormal = first.normal;
                onEdge = false;
                if (!dragging && best < 1e8 && !bestScene) {
                    edgeKey = bestKey, edgePoint = bestPoint, edgeNormal = Normalize(bestNormal), edgeLast = bestPoint;
                } else if (!dragging && best >= 1e8 && !edgeKey.empty() && visible.count(edgeKey) &&
                           visible.at(edgeKey)) {
                    // Just off a panel: stay on its plane (see "Panel edges" at the top).
                    const double denom = Dot(dir, edgeNormal);
                    if (std::fabs(denom) > 1e-4) {
                        const double along = Dot(edgePoint - anchor, edgeNormal) / denom;
                        const Vec3 at = anchor + dir * along;
                        if (along > 0.05 && std::sqrt(Dot(at - edgeLast, at - edgeLast)) <= edgeReach) {
                            best = along;
                            bestKey = edgeKey;
                            onEdge = true;
                        }
                    }
                }
                // Held on one of ft-screens' panels that stays where it is: onto whichever of them
                // the ray meets (see the top).
                if (leftHeld && !pressKey.empty()) {
                    vr::ETrackingUniverseOrigin uo;
                    vr::HmdMatrix34_t now{};
                    auto it = handles.find(pressKey);
                    bool still = it != handles.end() &&
                                 overlay->GetOverlayTransformAbsolute(it->second, &uo, &now) == vr::VROverlayError_None;
                    for (int i = 0; still && i < 3; ++i)
                        for (int j = 0; j < 4; ++j)
                            if (std::fabs(now.m[i][j] - pressPose.m[i][j]) > 0.001f) still = false;
                    if (still) {
                        Hit h;
                        for (const auto &[key, handle] : handles) {
                            if (!visible[key] || !FramePanel(key)) continue;
                            vr::VROverlayIntersectionParams_t params{};
                            params.vSource = {float(anchor.x), float(anchor.y), float(anchor.z)};
                            params.vDirection = {float(dir.x), float(dir.y), float(dir.z)};
                            params.eOrigin = vr::TrackingUniverseStanding;
                            vr::VROverlayIntersectionResults_t r{};
                            if (overlay->ComputeOverlayIntersection(handle, &params, &r) && r.fDistance > 0.05f &&
                                r.fDistance < h.along)
                                h.along = r.fDistance, h.key = key;
                        }
                        if (h.along < 1e8) dragDistance = h.along, lastHit = h.key;
                    } else {
                        pressKey.clear();  // carried: the lock holds for the rest of this press
                    }
                }
                // While dragging: keep the press-time distance and show the non-interactive marker.
                // On a scene-graph plane or a panel's edge: the laser-catching dot goes 5 cm behind it.
                distance = dragging ? dragDistance : (best < 1e8 ? best : freeDistance);
                point = anchor + dir * distance;
                occluded = false;
                if (!dragging) {
                    // The cursor lands on what you see under it: the ray above starts at the anchor,
                    // not the eye, so after leaning it can pick a panel that something nearer
                    // covers from where you are now (panels close together in view, at different
                    // depths). Anything in front of the point on the eye's line of sight wins.
                    // Not for a hand's ray: like a controller's, it lands where it points.
                    const double toPoint = std::sqrt(Dot(point - eye, point - eye));
                    const Hit front = rayMode ? Hit{} : nearest(eye, Normalize(point - eye));
                    if (front.along < toPoint - 0.02) {
                        occluded = true;
                        bestKey = front.key, bestScene = front.scene;
                        point = front.point;
                        if (!front.scene)
                            edgeKey = front.key, edgePoint = front.point, edgeNormal = Normalize(front.normal),
                            edgeLast = front.point;
                        distance = std::sqrt(Dot(point - anchor, point - anchor));
                        best = distance;
                        onEdge = false;
                    }
                    lastDistance = distance, lastHit = bestKey;
                }
            }
            const bool onScene = !dragging && ((bestScene && best < 1e8) || onEdge);
            const bool onPanel = dragging || (best < 1e8 && !onScene);
            const Vec3 sight = Normalize(point - eye);
            if (rayMode) onVrSettings = false;  // the beam starts at the hand: SteamVR finds the page itself
            if (!dragging && !reuse && !rayMode) {
                // SteamVR Settings (see the top): the dashboard's main panel is hidden and its
                // scene-graph panel shows the page.
                onVrSettings = false;
                const auto mainIt = visible.find("valve.steam.gamepadui.main");
                if (overlay->IsDashboardVisible() && !(mainIt != visible.end() && mainIt->second)) {
                    for (const auto &[key, handle] : handles) {
                        if (!visible[key] || key.rfind("valve.steam.gamepadui.frame.menu.", 0) != 0) continue;
                        vr::ETrackingUniverseOrigin uo;
                        vr::HmdMatrix34_t t{};
                        if (overlay->GetOverlayTransformAbsolute(handle, &uo, &t) == vr::VROverlayError_None &&
                            OnSettingsPage(t, eye, sight))
                            onVrSettings = true;
                    }
                }
            }
            if (!reuse) {
                pass = {!dragging, tnow, visibleGen, yaw, pitch, anchor, eye,
                        best, distance, bestKey, bestScene, onEdge, occluded, point};
            }
            if (onVrSettings != catcherHidesHit) {
                overlay->SetOverlayFlag(cursor, vr::VROverlayFlags_HideLaserIntersection, onVrSettings);
                catcherHidesHit = onVrSettings;
            }

            if (rayMode) {
                // A hand's ray (see "Hand ray" at the top): the beam shows, as a controller's does, and
                // our dot only where it meets a panel. No laser-catching dot in free space: the beam just
                // ends. None while a controller has taken the laser (rayInactive).
                if (rayInactive || (!onPanel && !onScene) || tnow < calPanelUntil) {
                    markerDot.Show(false);
                } else {
                    const Vec3 at = onPanel ? point + Normalize(eye - point) * 0.005 : point;
                    const double dist = std::sqrt(Dot(at - eye, at - eye));
                    markerDot.Alpha(1);
                    markerDot.Place(at, eye, float(2 * dist * std::tan(cursorDeg * M_PI / 360)));
                    markerDot.Show(true);
                }
                cursorDot.Show(false);
            } else if (onVrSettings) {
                // The dot close in front of the page, which is nearer than any guess of ours;
                // the laser-catching dot far behind everything, invisible, so it never covers
                // the page, with SteamVR's hit dot hidden on it.
                const Vec3 near = eye + sight * SETTINGS_DOT, far = eye + sight * SETTINGS_CATCHER;
                double alpha = 1;
                if (gazeOn && !gazeDotAlways) {
                    auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(tnow - t).count(); };
                    alpha = std::clamp(1 - std::min(secs(lastMove) - gazeShow, secs(lastHeld)) / 0.25, 0.0, 1.0);
                }
                if (tnow < calPanelUntil) alpha = 0;
                markerDot.Alpha(float(alpha));
                markerDot.Place(near, eye, float(2 * SETTINGS_DOT * std::tan(cursorDeg * M_PI / 360)));
                cursorDot.Alpha(0);
                cursorDot.Place(far, eye, float(2 * SETTINGS_CATCHER * std::tan(cursorDeg * M_PI / 360)));
                markerDot.Show(true);
                cursorDot.Show(true);
            } else {
                // On a panel: the non-interactive marker, pulled 5 mm toward the eye so it
                // draws on top. In free space: the interactive dot the laser lands on.
                DotOverlay &show = onPanel ? markerDot : cursorDot, &hide = onPanel ? cursorDot : markerDot;
                const Vec3 at = onPanel ? point + Normalize(eye - point) * 0.005 : onScene ? point + dir * 0.05 : point;
                const double dist = std::sqrt(Dot(at - eye, at - eye));
                // Gaze mode: a pulse for each click, and with POINTER_GAZE_DOT=moving, shown only
                // while something moves it or a press holds it (see the top); transparent
                // otherwise, the laser still lands on it.
                double scale = 1, alpha = 1;
                if (gazeOn) {
                    auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(tnow - t).count(); };
                    alpha = gazeDotAlways ? 1.0 : std::clamp(1 - std::min(secs(lastMove) - gazeShow, secs(lastHeld)) / 0.25, 0.0, 1.0);
                    const double pulse = secs(pulseAt);
                    if (pulse < 0.6) {
                        scale = 1 + 1.5 * std::max(0.0, 1 - pulse / 0.3);
                        alpha = std::max(alpha, std::clamp((0.6 - pulse) / 0.3, 0.0, 1.0));
                    }
                }
                if (tnow < calPanelUntil) alpha = 0;  // the calibration panel is up (see the top)
                show.Alpha(float(alpha));
                show.Place(at, eye, float(2 * dist * std::tan(scale * cursorDeg * M_PI / 360)));
                show.Show(true);
                hide.Show(false);
            }

            // Controller ray: from the eye, aimed at the cursor point, converted from the
            // standing universe to raw tracking space via the HMD's pose in both.
            // A hand's ray: from its origin along its direction (push and pull already in the anchor).
            const Vec3 aimStanding = rayMode ? dir : Normalize(point - eye);
            const auto &S = hmd.mDeviceToAbsoluteTracking, &R = hmdRaw.mDeviceToAbsoluteTracking;
            const Vec3 aim = Rotate(R, RotateInverse(S, aimStanding));  // raw <- head <- standing
            // Origin partway along the line of sight to the cursor (smaller hit dot).
            const double toPoint = std::sqrt(Dot(point - eye, point - eye));
            double originDist = std::max(0.0, std::min(toPoint * originFraction, toPoint - originMargin));
            if (onVrSettings) originDist = std::min(originDist, SETTINGS_ORIGIN);  // SteamVR finds the page
            const Vec3 originStanding = rayMode ? anchor : eye + Normalize(point - eye) * originDist;
            const Vec3 eyeRaw = Position(R) + Rotate(R, RotateInverse(S, originStanding - eye));
            lastPoint = point, lastOrigin = originStanding, lastAim = aimStanding;  // tilt starts from here
            havePoint = true;
            if (debug && tnow - lastDebug > std::chrono::milliseconds(500)) {
                lastDebug = tnow;
                if (systemPointer == vr::k_ulOverlayHandleInvalid) overlay->FindOverlay("system.pointer", &systemPointer);
                std::printf("dbg %s hit=%s dist=%.2f eye->point=%.2f origin=%.2f vrsettings=%d yaw=%.1f pitch=%.1f gaze=%s steamvr_dot=%d primary=%u\n",
                            dragging ? "DRAG" : occluded ? "INFRONT" : onEdge ? "EDGE" : onScene ? "SCENE" : (best < 1e8 ? "PANEL" : "FREE"), lastHit.empty() ? "-" : lastHit.c_str(),
                            distance, toPoint, originDist, onVrSettings, yaw, pitch,
                            !gazeOn ? "off" : tnow - gz.at > std::chrono::milliseconds(150) ? "stale" : gazeOwns ? "owns" : "mouse",
                            systemPointer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(systemPointer),
                            overlay->GetPrimaryDashboardDevice());
                std::fflush(stdout);
            }
            const double ayaw = std::atan2(-aim.x, -aim.z) * 180 / M_PI;
            const double apitch = std::asin(std::clamp(aim.y, -1.0, 1.0)) * 180 / M_PI;
            if (dragging && (tiltYaw != 0 || tiltPitch != 0)) {
                // Keep this drag's tilt applied, about the current cursor point.
                const double yr = tiltYaw * M_PI / 180, pr = tiltPitch * M_PI / 180;
                const Basis base = AimBasis(aimStanding);
                const Vec3 up{0, 1, 0}, side = base.x;
                auto turn = [&](Vec3 v) { return RotateAbout(RotateAbout(v, side, pr), up, yr); };
                const Vec3 o = point + turn(originStanding - point);
                const Basis b{turn(base.x), turn(base.y), turn(base.z)};
                auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };
                const Vec3 oRaw = Position(R) + toRaw(o - eye);
                double q[4];
                BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
                char msg[200];
                std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", oRaw.x, oRaw.y, oRaw.z, q[0],
                              q[1], q[2], q[3]);
                SendTo(out, "ft_pointer", msg);
                posed.valid = false;
            } else if (!posed.valid || tnow - posed.at >= std::chrono::milliseconds(100) ||
                       Length(eyeRaw - posed.origin) >= 0.0002 || std::fabs(std::remainder(ayaw - posed.yaw, 360.0)) >= 0.04 ||
                       std::fabs(apitch - posed.pitch) >= 0.04) {
                // The driver keeps the last pose: only a change goes out (see "Unchanged frames" at the top).
                char msg[160];
                std::snprintf(msg, sizeof msg, "pose %.5f %.5f %.5f %.4f %.4f", eyeRaw.x, eyeRaw.y, eyeRaw.z, ayaw, apitch);
                SendTo(out, "ft_pointer", msg);
                posed = {true, tnow, eyeRaw, ayaw, apitch};
            }
            if (hold.src == Src::Ray) hold.aimSent = true;  // its press can go out next frame (see above)
        }

        // A held-back press (see the top): held still long enough, it's a real press (a drag);
        // released, it's a click where the pointer is now (this frame's pose has gone out).
        // Released meanwhile: nothing to click, and the click it was due (pressRight: a right one) is
        // forgotten too, or the next press after the pointer wakes would go out as a right click.
        // A "hide" read a loop after the release is too late: the click has gone out by then
        // (input-relay.py stand_down).
        if (!active) aimHeld = aimRight = clickPress = pressRight = aimHand = confirmLesson = false;
        if (aimHeld && !aimHand && nudgeMoved < 0.2 && tnow - aimSince >= std::chrono::duration<double>(gazeHold)) {
            aimHeld = false;
            gazeBack = true;
            pressRight = aimRight;  // the right button's: a real right press (see the top)
            aimRight = false;
            pressLeft();
        }
        // A keyboard click held still for POINTER_GAZE_HOLD: a real press, then the head drags
        // (from where it is now).
        if (aimHeld && aimHand && hold.promote && !hold.engaged &&
            tnow - aimSince >= std::chrono::duration<double>(gazeHold)) {
            aimHeld = aimHand = false;
            hold.pressed = hold.grip = true;
            if (hmd.bPoseIsValid) {
                const Vec3 f = Normalize({-hm[0][2], -hm[1][2], -hm[2][2]});
                hold.refYaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
                hold.refPitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            }
            gazeBack = gazeOn;
            pressRight = hold.right;
            pressLeft();
            if (debug) std::printf("hold key %s: pressed (held still)\n", hold.right ? "right" : "left");
            if (debug) std::fflush(stdout);
        }
        // The chord (see "Gaze pinch" at the top): a left still held back is pressed now, where the pointer
        // is (this frame's pose has gone out); the right button goes down when due and up with its pinch.
        if (chordLeft) {
            chordLeft = false;
            if (hold.src == Src::Hand && hold.gaze.On() && aimHeld) {
                aimHeld = aimHand = false;
                hold.pressed = true;
                gazeBack = gazeOn;
                pressRight = false;
                pressLeft();
                hold.gaze.pressHit = lastHit;
            }
        }
        if (chordBtn.On()) {
            const int change = chordBtn.Tick(secsOf(tnow));
            if (change > 0) chordPinch.pressHit = lastHit;
            chordSend(change);
            if (chordBtn.Finished()) chordOver(false, "");
        }
        // A gaze pinch held still for POINTER_GAZE_HOLD: a real press at the locked gaze, then the hand
        // drags (see "Gaze pinch" at the top).
        // (on a screen's resize handle or grab bar: sooner, POINTER_GAZE_CONTROL_HOLD)
        if (hold.src == Src::Hand && hold.gaze.On() && aimHeld && hold.gaze.Promote(secsOf(tnow), FrameControl(lastHit))) {
            aimHeld = aimHand = false;
            hold.pressed = true;
            gazeBack = gazeOn;
            pressRight = false;
            pressLeft();
            hold.gaze.pressHit = lastHit;
        }
        if (clickPress) {
            clickPress = false;
            pressLeft();
            clickRelease = true;
            clickReleaseAt = tnow + std::chrono::milliseconds(40);
        } else if (clickRelease && tnow >= clickReleaseAt) {
            clickRelease = false;
            releaseLeft();
        }
        // A gaze pinch's click went out (or was dropped with the pointer): its log line.
        if (gazeClick.On() && !clickPress) {
            std::printf("%s %s: %s\n", gazeClick.Right() ? "middle pinch" : "pinch", gazeClickSide ? "right" : "left",
                        gazeClick.Says(secsOf(tnow), !clickRelease, "the pointer went", lastHit, lessonSaid).c_str());
            std::fflush(stdout);
            gazeClick = {};
        }

        controllerButtons.Poll(
            [&](const char *button, bool down) {
                SendTo(out, "frametop_relay", std::string("vrbtn ") + button + (down ? " 1" : " 0"));
            },
            inGame);

        vr::VREvent_t ev;
        while (sys->PollNextEvent(&ev, sizeof ev)) {
            // The dashboard's overlays come and go with it, and a game brings its own.
            if (ev.eventType == vr::VREvent_DashboardActivated || ev.eventType == vr::VREvent_DashboardDeactivated ||
                ev.eventType == vr::VREvent_DashboardOverlayCreated || ev.eventType == vr::VREvent_SceneApplicationChanged)
                overlays.Kick();
            if (ev.eventType == vr::VREvent_TrackedDeviceActivated || ev.eventType == vr::VREvent_TrackedDeviceDeactivated)
                ours = vr::k_unTrackedDeviceIndexInvalid;
            if (ev.eventType == vr::VREvent_Quit) {
                sys->AcknowledgeQuit_Exiting();

                overlays.Stop();
                vr::VR_Shutdown();
                return 0;
            }
        }
        // Idle (see the top): wait for a command instead. Not with anything still to finish (the
        // claim pulse, a click's release, a press).
        const bool idle = !active && !handsTrack && !claimPending && !claimHeld && !clickRelease && !leftHeld &&
                          hold.src == Src::None && !chordBtn.On();
        if (idle) {
            pollfd p{in, POLLIN, 0};
            poll(&p, 1, controllerButtons.Watching(inGame) ? 20 : 250);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
}
