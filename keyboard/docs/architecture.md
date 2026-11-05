# Architecture

Full Keyboard for Steam Frame is a native, manually launched overlay for dashboard and local-app input. Default behavior leaves the stock keyboard untouched. The optional [native replacement bridge](native-replacement.md) preserves Steam's typing target and restores native presentation independently of the app. It does not route events to streamed VR apps. Internal namespaces, executable names, paths, and overlay IDs remain `framekeyboard`.

## Components

| Component | Responsibility | Implementation |
| --- | --- | --- |
| Configuration | Profile discovery, validation, selection, reload and persistence | Runtime JSON catalog with compiled fallback |
| Layout model | Physical key IDs, bounds, fallback labels and actions | File-defined geometry |
| Language model | Keymap, modifier-level legends, shaping and backend compatibility | Language profiles and XKB legends |
| Key state | Pointer capture, key transitions, modifiers, repeat and cancellation | C++20, independent of renderer/backend |
| Renderer | Draw key faces/sides/legends and animations | Cairo/Pango |
| VR panel | Own overlay, pose, size, pointer events and texture submission | OpenVR + Vulkan |
| VR lifecycle | Coordinate presentation, routing, cancellation and readiness | Host-testable `VrLifecycle` with runtime effect callbacks |
| Input backend | Translate physical actions into press/release events | Gamescope Unicode characters plus native libei controls; physical uinput remains available |

## Rendering choice

Cairo/Pango draw into a CPU image, and the vendored Vulkan texture transport keeps a persistent OpenVR-compatible texture. Provenance and licenses are recorded in [third-party sources](../third_party/README.md).

The renderer draws an image, then updates the GPU texture. The compositor reuses the submitted texture between updates. Short press animations need a bounded redraw loop; idle keys do not require a continuous CPU paint loop. The VR event loop targets a 16.667 ms period while visible, including its processing time, and 250 ms while hidden. A changed replacement session wakes the hidden sleep early so native-close confirmation does not consume another idle period. Identical helper replies keep the original sleep deadline without renewing render-loop progress. Background startup prepares the hidden texture before advertising takeover readiness. It reads the dashboard pose every visible iteration; hidden tracking retries remain limited to 4 Hz. Hidden panels skip native controller-component queries. Stationary, fully aligned panels skip transform updates. These intervals control polling, not compositor presentation; SteamVR presents the existing texture independently.

`VrLifecycle` combines the existing replacement-presentation and tracking policies.
It orders cancellation before route changes, isolates dismissed/fallback requests,
retains pending shows and reports readiness only from runtime observations. The
OpenVR shell supplies effects for App input, overlay presentation and device
cancellation. Geometry, rendering, raw pointer events and independent watchdogs
stay outside the coordinator. [Lifecycle requirements](vr-lifecycle.md) define
private-receiver acceptance scenarios, including actual App holds, queued text
and two-controller ownership. Host tests do not establish physical VR behavior.

The image is retained between paints. Hover/press/release updates clear and redraw only damaged key regions, including overlapping neighboring ink in the original drawing order. Cairo recording surfaces measure ink bounds when the visual model changes; the bounds cover the entire press travel and include text outside the face rectangle. Profile snapshots compare values rather than pointer addresses so Reload cannot reuse stale geometry. Modifier/lock legends, composition/candidates, settings, status, and profile changes use full redraws. Pixel-comparison tests run the same interaction sequence against an always-full renderer.

`PanelTexture` prefers Cairo's native premultiplied BGRA bytes on little-endian systems, using `VK_FORMAT_B8G8R8A8_UNORM` and `VROverlayFlags_IsPremultiplied`. It checks Vulkan format support and OpenVR flag acceptance, and falls back to straight RGBA conversion if setup or submission fails. Rejected native textures stay alive until VR shutdown. Both GPU images still receive full uploads; partial CPU painting does not introduce stale alternate textures. The visible 60 Hz pose loop and 80 ms Graphite animation duration are unchanged.

The HTML in `design/` is a keycap design artifact only. Settings uses native card geometry rather than that older HTML toolbar. It is not an embedded browser requirement for the native app.

## Configuration and selection

Global pin, Num Lock and numpad visibility preferences live beside the active selection and favorites. Hidden numpad keys are excluded from painting and hit testing. The remaining geometry keeps its full-size scale, centered in a narrower case. The fixed texture and saved dashboard anchor remain centered, with transparent side space excluded by the OpenVR intersection mask. Composition controls fit the narrower case; settings cards use the full case width.

[Configuration](configuration.md) defines independent layout, language and theme profiles. Native rendering consumes the selected model, not hardcoded US keys or graphite colors. The VR settings panel uses the same catalog and validation as the host preview. Prepare a complete candidate before activation, release keys using the old mapping, and atomically replace geometry and hit regions. Persist the selection only after successful activation.

The language model validates required physical keys. The normal Frame path composes characters locally and sends Unicode. Only physical uinput and external JIS use an explicit `--target-language` declaration. Derived legends must match the symbols actually delivered. Locale names alone do not establish language support. Keep IME/composition and UI translation separate from physical key geometry.

Settings geometry and scrolling live in `SettingsUi`. App supplies three data-defined cards for languages/layouts, general preferences and experimental native replacement; the same model accepts extra cards and fields in tests. Card and field clips are shared with control hit testing. An open dropdown owns clicks and scroll input; outside dismissal consumes its click. Scroll changes invalidate pressed controls, and cancellation clears scrollbar ownership and menus.

Pointer targets distinguish keyboard keys from UI controls. Their hover sets are also separate in the renderer, so a custom key ID can match a control ID without activating or highlighting that control. A control press activates only when released over the same control target.

Controls carry typed actions with candidate indices or selected profile IDs as
payloads. A press captures that action alongside its visual identity; release
dispatches it without parsing an ID string. `Options` uses enums for operation
and input backend, with textual names confined to command-line parsing.

The logical case remains 1600 x 600. Cairo and the Vulkan upload include 200 transparent pixels above and below it so five-row dropdowns can escape the case. Preview and OpenVR events convert texture coordinates back to case coordinates. The symmetric margins preserve the case center, physical width and dashboard placement math. OpenVR intersection masks admit the case plus the open dropdown, excluding unused transparent margins. PNG exports include a visible movement line below the case; popup captures include both margins.

Favorites use layout/language identity. App keeps the saved default separate from the current runtime selection so cycling and immediate pin saves do not overwrite it. Apply and save persists profiles, favorites and pin state atomically while preserving unknown top-level fields. Profile preparation happens before input cancellation or active-state changes. The optional `use_drag_to_move` preference defaults to false. In that mode a white line below the case starts movement with the laser trigger. Enabling it selects the existing grip movement and hides the line. The line shares the panel texture and has its own fixed intersection-mask rectangle. Its idle color is native `#3d4450`; hover/capture interpolates to white and 1.1 times the width over 80 ms using CSS ease-out. The renderer schedules paints only until that transition completes, including when settings is open. The larger stroke remains inside the same padded hit region. Capture belongs to one tracked head/controller device and cursor; mouse-up, tracking loss, dashboard hiding and focus changes release it. Frame trigger-component samples also recover a release missed by pointer events. After observing a held trigger, release is detected immediately. If a quick click ends before the first physical poll, `TriggerReleaseGuard` allows 150 ms for stale initial samples, then requires 50 ms of consecutive released samples. Missing/invalid data or a gap over 100 ms restarts that released-sample window. Every new capture resets the guard; AUX and grip movement retain their separate release paths. Pin gates both movement methods and removes the line in the VR layer; dashboard following continues through the existing placement path.

Preference snapshots go to a dedicated `SettingsWriter` worker. Atomic file writes and fsync never run during key-down or pin clicks. One writer preserves ordering and coalesces pending snapshots to the newest state; completion errors return to App during its normal tick. Shutdown drains the final write after key cleanup. Reload defers to a later tick while writes or startup reconciliation are pending, keeping the VR loop running. Repeated requests coalesce, and any newer saves also finish before Reload reads configuration, so an older save cannot overwrite the reloaded file later. Profile preparation still runs on App's thread once storage is idle. Deferred preparation failures return through the normal UI status without automatic retries; save failures remain visible.

The VR writer commits replacement/startup flags only after unit configuration succeeds. A unit failure saves the other snapshot fields while preserving the two prior saved flags, and returns a startup warning separately from storage errors. A JSON write failure still rolls the unit transaction back and reports unsaved preferences. Reconciliation remains a separate operation that never writes JSON.

VR startup queues one reconciliation of valid saved startup preferences on that worker. This refreshes owned service templates after an update without requiring Apply or Reload. Matching files skip manager reload; malformed settings preserve the existing unit. Previews never configure startup.

Deliberate VR exits return 0 for manual launches and 3 for boot launches or processes carrying `FRAMEKEYBOARD_SERVICE`. The boot flag covers older units without the marker; the inherited marker covers service-managed update successors that omit the boot flag. Runtime shutdown and watchdog SIGTERM remain eligible for service restart.

`UpdateStartup` captures background-update intent from a valid inherited acknowledgement socket and a nonempty service marker before consuming the descriptor environment. Main passes that intent separately from the boot-only `--autostart` flag. With replacement enabled, App uses it for hidden presentation without rechecking boot opt-in; without replacement it opens normally. Prepared-update shutdown explicitly disposes the bridge without restoration and cancels the exact-overlay guard. Default stop and EOF still restore native. The helper drops queued input before applying the stop disposition, and the replacement destructor waits for helper cleanup before update activation.

## Input and focus

`OutputController` owns the enabled gate, ordered output queue, delivered-key
references, text retry buffer and repeat scheduling. `KeyboardState` retains
pointer captures, visual state and sticky modifier interpretation. Each accepted
native press snapshots its chord; Unicode presses snapshot the composed text.
Up to 32 output entries can wait for delivery, with at most 32 Unicode characters
per text entry. Accepted queued keys receive the normal pressed visual and haptic.
Releases do not require queue space. A queued click sends a balanced down/up;
already-delivered keys release immediately, including while another result waits.
Repeats begin only after initial delivery completes and never catch up after a stall.

The controller consumes text and native-control results before advancing to the
next entry. Steam named controls acknowledge delivery through the helper; local
native controls complete when submitted to libei/uinput. Physical holds must end
before a following Unicode commit. A failure stops the queue and releases delivered
holds. The failed text and consecutive queued text before the first native action
remain for one explicit Enter retry, in transport-sized chunks. Native actions and
text after that boundary are discarded with a status, so a discarded Tab or Enter
cannot join text intended for separate fields. A synchronous preedit rejection
leaves the original editor responsible for its unchanged preedit.

Cancellation, hide, input reset and observed target/request or route changes clear
queued output and retries before another session can receive input. A late result
cannot advance the new session. An unowned Wayland ordering barrier left by
cancellation still rejects output presses with a compositor-wait status; local
locks and preedit edits remain available. Transport-specific protocol state and
device cleanup stay in the sinks. No queue state is persisted or logged.

The installed launcher uses the pinned libei client through the running Gamescope socket. The client is compiled into the app because SteamOS libei 1.4.1 can crash on an aborted negotiation; system libraries remain untouched. A keyboard seat is bound, output starts only after device resume, and a pause or removed device clears held state. Resuming or receiving a replacement keyboard can restore typing. Disconnected native and Unicode sockets reconnect in the background with a 1-second delay doubling up to 30 seconds, without replaying old holds or text. While waiting, App reports "Keyboard paused or disconnected". An interruption flag survives a pause/resume pair within one dispatch, and blocks backend output until the app clears its old UI holds. Connection state is tracked even while the dashboard disables typing. Existing evdev key mapping is shared with uinput; compositor repeat replaces synthetic EV_KEY repeat in the libei path. Development previews remain input-disabled. Apply/Reload releases held keys and restores typing only for a live backend, available text transport and an explicitly enabled launch. Physical-only modes additionally check their target declaration; no pause/resume button is needed.

Use logical names such as `Enter`, `ControlLeft`, `KeyC`, and `NumpadEnter`. Every backend owns its mapping. Steam key-state values are not Linux evdev values. The native keyboard must not use Steam's contextual Enter callback for its dedicated Enter action.

uinput follows the compositor's input routing; it does not address a chosen window. A target overlay ID does not prove keyboard focus. Preserve focus while interacting with the panel and verify delivery in KDE and the outer floating-window session separately.

Copy/Paste are shortcut actions, initially Ctrl+C and Ctrl+V for browser targets. They do not read passwords or clipboard contents. Terminal shortcuts and clipboard sharing across sessions are separate requirements.

## Packaging and failure cleanup

Releases live under `~/.local/share/framekeyboard/releases`, with a `current` symlink and a launcher in `~/.local/bin`. Config lives under `~/.config/framekeyboard`. The package enables no startup by default. The explicit Start with SteamOS option maintains a user-local session unit on the preference worker. It launches hidden replacement only when both saved options are on; independent fallback and update helpers survive main-process handoff. No kernel module or Steam patch is installed.

Production replacement stages a unique new directory, including for a same-version
reinstall, and atomically switches `current`. The installer holds the instance lock
throughout, so no running keyboard can lose its release files. Installer and updater
share root/name-bound production markers and conservative deletion checks. Only the
replaced marked release is removed, never `current`, development deployments or
unmarked historical releases. In-app updates retain the old release and settings
snapshot through startup probation and service adoption; success removes them,
while failure restores the old release. Cleanup errors cannot roll back a confirmed
app. No permanent `previous` link is maintained; manual rollback reinstalls an older
package. Failed-update preference snapshots remain available for recovery.

Every held key has a cancellation path. Disconnect, focus loss, hiding, close, and drag capture release input. Overlapping controller presses share backend key references, including lock-key toggles. After a drag timeout or tracking loss, cleanup clears the application's dragging state even if the motion tracker already stopped. Cleanup is idempotent.

Optional replacement uses independent readiness leases and watchdogs to restore the stock keyboard. Per-user instance IPC accepts only a recenter request, not arbitrary input or text.

## Instance ownership and placement

VR launches acquire `VrInstance` before touching OpenVR. A later launch connects to the owner's private local socket, sends only a recenter request, waits for acknowledgment, and exits. The render loop handles requests on its own thread; it releases keys, returns to the keyboard view, resets placement beneath the current dashboard, and shows its own overlay. This avoids duplicate overlay keys and avoids reassigning SteamVR's generated application PID through a second VR connection.

`PanelPlacement` contains pure transform math with no VR calls. Its initial width is 0.95 m; recenter keeps the chosen width and applies a 50-degree backward pitch from upright, independent of headset pitch/roll. Recenter reads the live bottom center of `valve.steam.gamepadui.main` through `GetTransformForOverlayCoordinates`, normalizes its scaled basis, and puts the keyboard's top edge 6 cm below it with its center 24 cm toward the viewer. The hidden stock keyboard is deliberately ignored because its transform can lag behind dashboard movement. If the dashboard pose is unavailable, placement falls back to 0.85 m ahead and 0.65 m below the headset. While visible, dashboard movement updates the panel's relative pose at a target 60 Hz without repainting the texture. During a grab the hand owns the pose and updates its offset. The last dashboard anchor is saved with placement, so close/reopen follows subsequent dashboard movement. `PanelPlacement` stores the world transform directly; only recenter constructs a default rotation. The main-view resize icons queue bounded width changes without rebuilding the pose; controller grabbing handles positioning. The VR loop consumes them, updates the transform/width, and checks compositor visibility independently of its cached state. Tracking loss clears held keys; a valid pose after a brief loss restores visibility without moving the panel. Tracking-origin reset events still request recentering. `placement_store.cpp` validates and atomically persists the standing-space transform, width and tracking-universe ID on orderly shutdown. A cold launch restores the saved placement once tracking is valid. A different known universe recenters the pose while preserving saved width. An invalid file falls back to defaults. Legacy placement files without a dashboard anchor retain their world-space pose until reset. This is not a cross-room spatial anchor.

`IsDashboardVisible` gates both rendering and interaction. The loop checks it before pointer events and before repeat, releases held keys before disabling `OutputController`, cancels preedit/grabs and drains hidden pointer events. The gate also covers Apply, Reload and relaunch, so they cannot enable hidden output. The overlay does not set `MakeOverlaysInteractiveIfVisible`; it relies on dashboard interaction only and cannot deliberately claim scene inputs after closure. No active-VR-app backend is provided, so the panel remains hidden outside the dashboard.

Controller line dragging and grip dragging capture a controller-to-panel transform at grab time, then composes each tracked controller pose with that fixed offset. Frame's dashboard masks modern grip actions and legacy controller-state polling. `GrabInput` instead reads the native `button_grip` render component for the known Frame controller models. A synthetic all-released controller state supplies its neutral component pose; the live component's relative rotation measures physical squeeze. `GripLatch` applies press/release hysteresis and requires a released sample after startup or a missing sample. This model-specific path must be rechecked when Valve changes controller models. It does not alter runtime input settings.

Laser hover gates a grab, and releasing grip, losing input/tracking, hiding or recentering cancels it. Only the captured controller owns movement; leaving the hover target during a drag does not release it. The UI cancels held keys and suppresses typing while dragging. The keyboard opens one OpenVR connection after checking that the VR server is already running.

`App::down` and `App::up` report accepted key press/release transitions. `App::move` reports entering a different key when that pointer is not holding a key. Only the VR layer sends feedback: 25 ms/full amplitude for press and 8 ms/0.35 amplitude for release, both at 150 Hz, 4 ms at 240 Hz/0.1 amplitude for hover. Clicks take priority over hover in the same frame. This keeps haptics out of rendering and input delivery; duplicate/rejected events, stationary hover, dragging and repeat do not pulse.

During a grab, `GrabInput` also reads the native thumbstick render component. `ComponentAxis` calibrates neutral/full-up rotations using synthetic axis states. It extracts the signed twist around that axis, excluding sideways tilt. The controller model’s tip supplies the laser direction. `PanelDrag` integrates only the captured controller’s vertical stick value into its relative translation with a dead zone, elapsed-time cap and depth bounds. This keeps rotation, size and the lateral grab offset intact. Grab input uses neither action manifests nor global input overrides.

The optional boolean `auto_leveling` defaults to true and is exposed in General settings. `HorizonAlignment` bypasses correction when it is disabled. Applying or reloading a change adopts the displayed pose as the raw placement and resets the easing clock, so disabling cannot expose an older raw roll and enabling starts smoothly. `HorizonAlignment` corrects only local roll within 5 degrees of the standing-space horizon, using 500 ms smoothstep easing. It preserves the panel normal and center. The VR loop keeps raw placement separate from displayed placement so corrections never accumulate in the grab transform. Grabs capture the displayed pose and bypass horizon correction entirely. Normal grip release starts a fresh 500 ms animation, including when the hand was stationary before release. Leaving the range eases the correction out. Cancellation freezes the visible pose, recenter resets assistance, and shutdown saves the displayed pose. Panels facing straight up/down skip alignment because horizon roll is undefined.

`KeyHaptics` retains the controller owning each accepted pointer press and sends release feedback to that owner. Each controller has its own feedback queue; clicks take priority over hover, and hover is suppressed until a click ends. Release uses a shorter pulse because equal API values felt stronger on release on Frame. `VrHaptics` uses an output-only action manifest and `TriggerHapticVibrationAction` restricted to the event controller’s hand source. Missing/unknown controllers receive no pulse; there is no fallback to the overlay-wide laser haptic API. The haptic action set binds no controller inputs and changes no global input settings.

When an app tab such as Brave is active, Steam's hidden main dashboard overlay retains a stale transform. `DashboardAnchor` uses the main tab only while visible; the visible dashboard bar then carries that mount with its full translation and rotation across all tabs. Overlay scale is removed without flattening pitch or roll. The bar reference remains continuous until recentering to avoid orientation jumps on tab switches. Both anchor and bar pose are saved so reopening directly into an app preserves the offset. Without a prior calibration, initial placement falls back beneath the live bar. Invisible overlays are never accepted as current poses.

## Japanese composition

`JapaneseComposer` owns a bounded in-memory preedit, romaji parsing and UTF-8 kana editing. Anthy is loaded dynamically on first conversion. Its public C API provides segment candidates; no dictionary learning/commit API is called. Candidate controls, script changes and Enter update local state before committing. Composition keys use `KeyboardState` for pointer ownership, visuals, modifier consumption and haptics, with native emission/repeat suppressed for those presses. Raw Ctrl/Alt/Meta shortcuts retain their normal key path, but unfinished composition is queued before native key-down. Once accepted, the controller owns that text, allowing the next preedit to begin while delivery waits. A synchronous rejection retains the preedit and blocks the forwarding action. Gamescope text commits use an asynchronous sync callback before a following key can be sent over the separate libei socket.

`EiSink` derives the Japanese text socket only from the documented `gamescope-N-ei` naming convention, preserving its directory and compositor number. Nonstandard key sockets require an explicit `--text-socket`; there is no fallback to gamescope-0 or the default Wayland display.

`GamescopeText` uses the pinned, generated version-1 input-method protocol. It feature-checks the manager/seat and handles unavailable connections. Discovery and commits progress through nonblocking flush/dispatch. Each handshake stage and commit has a one-second deadline. A commit timeout reports failure without killing the connection. Its ordering barrier remains until the late callback arrives, including after cancellation; native releases still pass. No timed-out text is automatically resubmitted. The protocol cannot establish whether the target received text before a timeout, so an explicit retry can duplicate text. `EiSink` retries failed connections after 1, 2, 4 seconds up to 30 seconds, resets the delay on readiness, refuses text while native keys are held, and does not read target fields or clipboard data. Text and physical output share explicit launch gating. Commits are limited to 32 Unicode characters to stay below the compositor's temporary keymap capacity; local reading is limited to roughly 30 characters.

The renderer reserves space for preedit/candidates during Japanese, Chinese and Korean composition. Kana/shift labels come from the selected language profile. Settings, profile changes, target-focus loss, hiding, dragging and recentering discard unfinished composition. Laser FocusLeave releases only the identified controller's keys and retains composition because it does not change application focus. Failed text submission keeps the preedit for recovery. The protocol reports submission availability, not application acceptance; application coverage needs separate tests.

## Code map

| Area | Files |
| --- | --- |
| Configuration and validation | `src/config.cpp`, `include/framekeyboard/config.hpp` |
| Pointer/key ownership, XKB legends, uinput | `src/input.cpp` |
| Output queue, gate, delivered holds, retries and repeat | `src/output.cpp`, `include/framekeyboard/output.hpp` |
| Compositor key and text delivery | `src/ei_input.cpp`, `src/text_input.cpp` |
| Native rendering and hit regions | `src/panel.cpp` |
| Settings, profile changes and composition dispatch | `src/app.cpp`, `src/settings_ui.cpp` |
| Japanese reading and conversion | `src/japanese.cpp` |
| Preview and command-line entry | `src/preview.cpp`, `src/main.cpp` |
| Instance IPC | `src/instance.cpp` |
| Placement math and persistence | `src/placement.cpp`, `src/placement_store.cpp` |
| Grip model and haptic state | `src/grip.cpp`, `include/framekeyboard/feedback.hpp`, `include/framekeyboard/vr_haptics.hpp` |
| OpenVR lifecycle and event loop | `src/vr.cpp` |
| Tests and opt-in receiver probes | `tests/` |

## Chinese and Korean composition

`CjkComposer` loads a bundled engine adapter on demand with `dlopen`. The
main executable has no PyZy/libhangul startup dependency. Each adapter links its
own system library and exposes a versioned factory through a private interface.
Adapters and host are built together; engine objects are destroyed before their
module is unloaded. A missing engine rejects Apply, or falls back to English at
startup without overwriting the saved profile.

The engine keeps bounded Pinyin or two-set Hangul state. PyZy supplies Chinese
candidates and script conversion; libhangul supplies Korean syllable assembly.
The app releases physical holds and gates commits using the same transport rules
as Japanese. Failed commits retain preedit. Space or a candidate selection commits
a completed Pinyin reading; Hangul word boundaries commit before forwarding keys.
Profile Apply prepares engines before changing the active model. Missing build
support or dictionaries leaves the prior profile usable.

PyZy's learning methods are not called. Candidate selection reads its conversion
and remaining phonetic text, saves local undo state, and rebuilds the remaining
reading. Its user-cache/config paths are non-directories, so the library cannot
read or persist personal history. Korean Backspace rebuilds from the bounded key
sequence, preserving jamo-level undo across resyllabification. Neither engine
changes the system IBus selection. See [language setup](languages.md).

## Independent Unicode character output

`LanguageMap::symbol` resolves the selected XKB profile without legend overrides.
Its compose state handles dead accents locally. `App` sends printable characters
through the Gamescope text connection and retains a failed commit for retry.
Local Caps/Num toggles share pointer ownership without changing compositor locks.
Text repeat is bounded and canceled with its pointer; native controls keep the
existing backend repeat path. Numpad navigation is sent as explicit Home/arrows/etc.

`EiSink` reads the advertised keymap once per keyboard device and tracks group
updates. Ctrl/Alt letter shortcuts and dedicated Copy/Paste search that map for
the intended Latin symbol, with a conventional physical fallback where metadata
is absent. This lookup neither changes the system keymap nor reads app text.

After a libei disconnect, `EiSink` clears held keys and reports one input reset.
Reconnect uses nonblocking Unix sockets and frame-by-frame negotiation with a
three-second deadline, retrying after 1, 2, 4 seconds up to 30 seconds. A ready
keyboard resets the backoff. Reconnection also recreates the Unicode transport;
old holds and text are never replayed. The initial launch alone may wait up to
three seconds for the first keyboard.

AUX head-cursor clicks use the existing OpenVR overlay mouse events. Laser movement accepts tracked HMD and controller poses. Grip reads and haptic output remain controller-specific. Device/cursor ownership prevents a controller reusing cursor zero from releasing an HMD capture. Tracking loss and dashboard hiding cancel either source. Handle-click logs contain source metadata only, with no key coordinates or typed text.

While a controller owns a laser-line capture, `GrabInput` reads only that controller's thumbstick without querying or requiring grip. It supplies the calibrated tip ray at capture so depth follows the same laser direction as grip movement. The laser path polls no render components while idle; an AUX/HMD capture has no stick source and cannot borrow a controller's axis. Missing stick/model data returns zero depth while tracked pose movement remains available.

## Optional native keyboard replacement

`NativeReplacement` wraps the local sink and owns a temporary Python helper over a private socketpair. The VR thread exchanges bounded, nonblocking packets. The helper talks only to Steam's localhost CDP endpoint; SharedJSContext owns presentation hooks and a 900 ms lease. Only completed VR-loop iterations can renew that lease. Each status/target generation also requires a visible-panel acknowledgement within 400 ms. A separate VR watchdog hides our panel if the main loop stalls. The helper recovers a main loop that stops progressing even if the watchdog stays alive. Explicit profile/engine preparation and settings barriers have bounded slow-work deadlines. Main-thread runtime-call scopes and system-resume detection provide separate bounded grace periods, including when SteamVR blocks both threads. Independent watchdog traffic cannot renew takeover readiness or extend those deadlines. See [recovery timing](native-replacement.md#presentation-and-fallback).

The CDP client connects to Steam's existing `127.0.0.1:8080` endpoint, bypasses HTTP proxies and rejects redirects. It opens no debugging listener. SteamOS's optional port 8081 forwarder serves remote debugging clients and is unused by this connection. The bridge checks Linux's TCP listener tables before connecting and periodically while active. It rejects replacement if a listener on 8080 is bound outside loopback, or if the checks cannot run. Port 8081 is not an availability gate. These checks do not configure ports or services. Missing or rejected debugger access leaves replacement inactive and native input available.

Supported Steam sessions deliver Unicode and plain Enter/Backspace/Tab/arrows through a retained field callback or the virtual-keyboard manager's owning document. The bridge closes native presentation completely and preserves only the corresponding manager's logical open state and typing target. It accepts requests only when that owner's provider can restore the same Steam target independently. Minimal Frame-local Gamescope requests use the local backend after an independent native guard resolves the exact overlay and arms recovery. Other external modes, buffered, mismatched and unknown requests stay native. Steam sessions block unsupported physical controls and shortcuts so they cannot reach another local app; local-app sessions retain libei routing. Asynchronous text commits are retained until acknowledged; a failed commit stays available for explicit retry, while cancellation invalidates late acknowledgements. Runtime details, compatibility and acceptance checks are in [native replacement](native-replacement.md).

Steam field takeover requires a direct callback or a connected editable input
in the manager's own document, validated by Steam's current or last field ref.
When Steam's validity predicate checks the wrong document, a ref selected
synchronously during the owner's DOM focus or click event establishes the association.
Polling or an unrelated ref change cannot establish it.
DOM edits and caret operations stay in that document without reading its text
or relying on compositor focus. Target loss ends the session; target changes
invalidate queued input. A dashboard button request without a field may show
the panel, but plain text has no recipient. The global controller-text route
is never used. Literal text and named controls are distinct IPC operations.

`ReplacementPresentation` separates resident panel visibility from process lifetime. In VR replacement mode, X releases input and emits a request-scoped dismissal that clears the retained target without reopening native presentation. Only semantic target/open changes create new generations; duplicate provider callbacks cannot create a reopen loop. A supported native keyboard button toggles an acknowledged panel off across owners, including after returning from a closed app. Explicit Steam field shows are tracked through the manager and transfer the target instead. The helper and lease remain active while hidden. A later eligible request follows the normal visible-panel readiness handshake. Explicit Settings Quit, signals and runtime shutdown still exit and withdraw replacement. Ordinary mode and desktop preview retain X-to-exit.

Losing a Steam field ends its replacement session and hides the panel. It never
authorizes a switch to compositor input, because field loss does not establish
local focus. The new request generation cancels old output and holds before a
later summon or field focus can acquire another session.

Dashboard frame selection is separate from DOM field focus. A hidden Steam
document cannot remain an input owner. A selected-frame change invalidates the
old request before another dispatch. While the panel is open, it follows a
supported Steam frame or a local frame whose exact overlay was acquired through
a native request earlier in this bridge lifetime. The map is bounded to 16
frames. Local handoff needs a fresh independent guard acknowledgement and an
OpenVR active-dashboard-overlay check; its recovery also rejects abandoned
targets. Unknown frames close the old session. X, fallback and expiry do not
create new handoffs.

## Manual release updates

`Updates` owns a private Python child and polls bounded nonblocking status packets. Settings retains the offer in process memory and resets the latest-version message after five seconds. `update/update_helper.py` verifies fixed-repository HTTPS assets, SHA-256, safe archive paths, AArch64 binaries and copied profiles before requesting restart. `main` destroys App, VR/input/replacement and the instance owner before sending activation. The helper switches versioned links under the installation lock, launches the new build with preserved arguments and waits for three seconds of completed-loop progress through a private startup socket. Startup failure restores and relaunches the prior release. A service-managed original process remains alive after cleanup until the helper verifies the successor has become systemd's MainPID; rollback transfers ownership the same way. Manual launches use the existing parent-exit protocol. Downloads and validation never run on the VR thread. See [updates](updates.md).
