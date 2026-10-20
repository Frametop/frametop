# Experimental native keyboard replacement

In Settings → Experimental, enable **Replace native keyboard**, then Apply and save. It is off by default. Full Keyboard must already be running with typing enabled. With replacement enabled, X hides the panel and leaves Full Keyboard resident for the next native request. Replacement alone changes no startup settings or Steam files. The separate **Start with SteamOS** checkbox is off by default and can only be edited while replacement is checked.

The current replacement bridge requires Python 3 and Steam's existing debugger at `127.0.0.1:8080`. It uses that local connection to install temporary keyboard hooks in Steam's UI. Ordinary Full Keyboard typing works without the debugger. If the debugger or required interfaces are unavailable, replacement stays inactive and native requests keep using Steam's keyboard. The saved preference is retained for a later available connection.

SteamOS can separately provide port 8081 through `steam-web-debug-portforward.service` for remote debugging from another computer. Full Keyboard does not connect to port 8081. It opens no debugging listener and leaves SteamOS's ports, forwarding services and Developer Mode settings unchanged. Installation adds no administrator or port-configuration step.

The bridge checks that its debugger endpoint on port 8080 is bound only to loopback. Port 8081 is unused and does not affect replacement availability. A Steam UI update may disable replacement until compatibility is restored.

When replacement cannot run, Settings reports a fixed reason: unavailable debugger, blocked local debugger access, incompatible Steam keyboard interfaces, another active replacement, unavailable helper, or lost connection. It also confirms that Steam's keyboard remains available. No raw exception, field value or input packet is displayed. A successful reconnection clears the notice.

Replacement writes `[native-replacement]` diagnostics to the app's stdout. Launcher
starts appear in Steam's `steam_output.log`; boot starts appear in
`journalctl --user -u framekeyboard-autostart.service`. Lines contain only fixed
helper/session codes, request numbers and retry delays. Install and repeated
reply timeouts have distinct codes. Field values, typed keys, input packets and
helper stderr are never forwarded. The bridge retains the latest 32 sequenced
transitions so multiple changes between polls can be reported once. Output is
limited to eight lines per second. An unchanged failure and retry delay is
reported at most once per minute.

## When it runs

Checking the option and choosing Apply and save starts the bundled helper and a temporary watchdog inside the running Steam UI. It does not install a background service. Unchecking it and applying stops the helper, removes the Steam hooks/watchdog, and restores native presentation. The app's internal safety thread stays idle while replacement is off and ends when the app exits.

With replacement enabled, the top-right X dismisses the current typing session and hides the panel. Full Keyboard and its bridge keep running, ready to show our panel on the next supported native request. Dismissing X does not bring back the stock keyboard. Dismissals are scoped to their request, and duplicate native close notifications cannot create new sessions. Pressing a supported native keyboard button while our acknowledged panel is open dismisses both presentations, including after closing an app and returning to the dashboard. Explicit Steam field-focus requests transfer the typing target and keep our panel open. The bridge distinguishes these requests through the existing field-manager show path, rather than treating a stale field reference as new focus.

**Quit Full Keyboard** appears in Settings only while replacement is enabled in VR. Use it to exit the resident app and restore native fallback. With replacement disabled, X exits normally and the separate Quit action is absent. Desktop preview keeps its ordinary exit behavior.

The preference is saved, so launching Full Keyboard again recreates the bridge when replacement is enabled. After an explicit Quit or uninstall, native requests use Steam's keyboard until Full Keyboard is launched again. Without **Start with SteamOS**, a crash or reboot also requires a manual launch. The native button summons a running resident; it does not launch a stopped app. Relaunching the existing resident app reopens/recenters its panel so Settings remains reachable.

Repeated debugger, compatibility or helper failures before a usable state back off from 5 seconds to a 60-second retry interval. A successful state, re-enabling replacement, or an explicit relaunch/recenter request resets the delay. A busy replacement or lost established connection retains the 5-second retry so temporary ownership and transport interruptions can recover promptly.

## Start with SteamOS

Check **Replace native keyboard**, then **Start with SteamOS**, and choose Apply and save. The startup checkbox is disabled while replacement is unchecked. Both saved preferences must be on for startup to run. Turning replacement off removes boot startup; turning it on again restores startup if its saved checkbox was selected. Unchecking startup alone removes it while replacement keeps working in the current session. Back discards pending edits.

This creates only a user-local `framekeyboard-autostart.service` and a link under `~/.config/systemd/user/steamvr.service.wants/`. It follows the Frame's existing SteamVR service and retries after crashes or runtime shutdown. It does not start SteamVR itself, require administrator access or alter Steam files. Startup begins with the panel hidden, including while the debugger reconnects; the app, replacement helper and watchdog run normally. The native keyboard button or supported field focus summons it. A duplicate boot start cannot summon an already running panel. Launching Full Keyboard manually still summons/recenters it and reaches Settings. Applying or reloading settings migrates the application's old gamescope-session startup link automatically.

Settings Quit pauses the app until the next SteamVR start or SteamOS login if both options remain enabled. Disable Start with SteamOS and apply to keep it off across those starts. Deliberate VR exits use status 3, which the unit treats as successful without an automatic restart; runtime shutdown uses normal exit status and is retried. The unit preserves independent fallback helpers and in-app update handoff when the main process exits. Startup reads the saved settings before opening input or VR, so a stale unit cannot activate it with either option off. If startup fails, Steam's keyboard remains available. Preview/image exports never install a startup unit.

Repeated startup failures use five restart-delay steps from 3 seconds up to 60 seconds. These backoff directives require systemd 254 or later; older managers ignore them and retain the 3-second delay. VR launches check for a running SteamVR server before parsing profiles or connecting input. Existing owned units receive the new settings through startup reconciliation.

## Coverage

Service-started keyboards retain service supervision through an in-app update or rollback. The original process stays registered until systemd confirms the replacement's PID. Subsequent crashes and SteamVR shutdown still use the startup unit's recovery policy. A manual app launch remains independent of the startup service.

Requests come from Steam's shared VR keyboard status, including Steam field focus and dashboard summons. Steam field takeover requires a matching window, a target-bound input route and a native Show/Hide provider that can restore that target. A direct field callback is preferred. Otherwise, the bridge requires an editable input or textarea in the manager's own document, verified by Steam's retained field reference and validity predicate. Unknown targets stay native. Steam's global controller-text API can send input to a local app behind the dashboard, so replacement never uses it. Frame-local app requests retain their supported replacement path.

Frame-local Gamescope desktop/window requests are also supported when their metadata matches the dashboard's minimal, single-line request with flags 13 and SteamVR dispatch disabled. These use the ordinary local libei/Unicode backend. An app-scoped native guard must first resolve the exact requesting overlay and arm recovery. It can reopen native for that overlay through OpenVR, independently of Python and the Steam UI. If that guard is unavailable, the request stays native. Unknown targets, mismatched Steam owners, other external request modes and buffered SteamVR requests remain native. A native keyboard already open when the bridge connects stays native until dismissed. Streamed VR-app input remains outside the project scope.

The bridge closes native presentation through its owning provider and temporarily preserves the Steam manager's logical open state and target. It suppresses only the corresponding physical-close cleanup. A real target change, explicit dismissal or another native request still receives the appropriate cleanup and request generation. Supported Steam fields receive committed Unicode and plain Backspace, Enter, Tab and arrow actions, preserving the field's Enter callback. Callback routes temporarily bind the renderer override to the retained callback and restore it after each call. DOM routes edit and move the caret only in the retained owner's document, even when that window lacks compositor focus. They never read existing field text. Every dispatch revalidates the target; blur ends the session without reviving the old field. In Steam-field sessions, physical modifiers, Copy/Paste, shortcuts and unsupported controls are blocked with a notice; use Steam keyboard for those actions. Shift/AltGr used to compose Unicode remain available. Local-app sessions keep ordinary libei shortcuts. External JIS and buffered external requests are excluded.

The dashboard keyboard button can also open our panel before a field is selected. Plain text then has no recipient and is discarded rather than saved for a later field. Selecting a supported Steam field creates a new request and routes subsequent typing to it. A new button summon can restore Steam's last field only when Steam still considers it valid; idle polling never restores a dismissed field.

PgUp and PgDn are unsupported in Steam replacement sessions, including a dashboard summon with no field selected. They show the control-unavailable notice in either case. Use the normal controller controls to scroll the dashboard. This limitation does not indicate that a blurred search field still owns input; clicking outside search must end its session and dismiss the panel.

Some Steam field predicates check the shared context's document instead of the owner's document. The bridge also records a field reference when Steam selects it synchronously during that owner's DOM focus or click event. This association remains valid only for the same connected, editable, active element and reference. A ref selected outside that event cannot acquire an association merely because an input is focused.

## Presentation and fallback

A prepared in-app update explicitly closes its retained request and cancels the local-app recovery guard before handing off. It discards queued input and does not reopen native. Ordinary Quit, disable, helper loss and timeouts keep native recovery. An unrelated native keyboard already opened by another owner is preserved.

While the app's render/input loop is healthy, the bridge prepares a transparent, noninteractive native popup texture and gates its show/focus calls. A supported request closes native presentation through the original provider. Our panel becomes eligible only after that close is confirmed, removing the native scene's collision and movement line before takeover. Each new request or field target still needs acknowledgement from our visible, input-ready panel within 400 ms. A failed native close or late acknowledgement leaves or restores the stock keyboard. Browser HideWindow is never used because it leaves separate dashboard geometry alive and can trigger focus-driven reopening. Stock controller keyboard actions are blocked while replacement owns the retained session.

Background startup prepares our hidden texture before takeover readiness. Changed helper session metadata wakes the hidden frame sleep, including while native close is still pending. Ordinary idle replies retain the original frame deadline. Waiting alone cannot renew readiness or acknowledge a panel; only a completed render/input frame can do that.

When Steam clears a field, its replacement session ends and the panel hides.
Selecting another dashboard frame also invalidates the old request, even if
its hidden document retains an active input. While the panel is open, it follows
the selected Steam frame or a previously acquired local-app frame. Local targets
are remembered only for this bridge lifetime, with a bounded frame-to-overlay
map. Returning to one creates a fresh request, cancels queued input and requires
the independent native guard to verify that exact overlay is active before local
input resumes. A first request in a local app still comes through its normal
keyboard button. Unknown or unsupported frames cannot inherit another app's
typing route. Dismissal, native fallback and lease expiry prevent automatic
reopening. The guard never restores a followed local app after it ceases to be
the active dashboard overlay.
Clearing a field does not prove which app has compositor focus, so the bridge
never changes that session into local input. Ending the session advances the
request generation and cancels queued text and holds. Focus another Steam field
or summon the keyboard again to start a new session with fresh acknowledgement.
Fallback also closes a field session whose target has disappeared;
it never reopens a native keyboard for that missing field, including when the
lease expires before the next field-change poll.

The stock dashboard handle is owned separately by SteamVR. Closing the actual presentation is required to remove it; CSS alone cannot do that. Native show/close ordering can still change. Confirm no flash, stale texture, collision or remaining handle in the headset before treating the prototype as complete visual replacement.

An independent watchdog in Steam's SharedJSContext expires the lease after 900 ms without new render-loop progress. Traffic from the helper alone cannot renew it. Readiness timeout, unsupported targets, backend loss, explicit escape, helper disconnect or app failure restore native presentation. An in-process watchdog hides our overlay after a 700 ms main-loop stall so it cannot cover native recovery. Its private health pipe reports watchdog progress independently of rendering; that health never renews the native takeover lease.

While replacement runs, the helper also monitors the main loop, including when the panel is hidden. Three seconds without a completed frame triggers SIGTERM to its own original parent, followed by SIGKILL after a one-second grace if the main loop still has not progressed. A live overlay watchdog alone cannot postpone this recovery. Profile preparation, language conversion and settings flushes explicitly declare slow work. With a live watchdog, they get an allowance of at most 30 seconds from the last completed frame. Repeated operations and watchdog heartbeats cannot extend that frame's allowance. Native fallback still proceeds during the allowance; only a completed frame can restore takeover readiness. For profile and storage work, if the watchdog also stalls, the ordinary three-second deadline applies. Termination releases the instance lock and compositor input devices, so a later manual launch can start a fresh app. It never terminates Steam or another application. These bounded allowances can still terminate unusually slow work that exceeds their limit; sleep/wake and recovery latency require device acceptance.

System resume and SteamVR runtime waits have separate bounded recovery allowances. The helper detects system suspend from the difference between Linux's BOOTTIME and MONOTONIC clocks and gives the main loop 30 seconds after resume to complete a frame. Repeated wakes without a completed frame cannot extend that grace. Main-thread OpenVR calls and texture submission declare runtime waits before entering the call. A wait can last at most 30 seconds from the last completed frame, including when the overlay watchdog is blocked on SteamVR too. Returning from a call leaves at most three seconds to complete the frame, within that same hard limit. Runtime calls cannot renew the native takeover lease. Ordinary main-loop deadlocks still use the three-second limit; a runtime wait exceeding its allowance is terminated rather than left resident forever. These allowances reduce false hang detection during wake/restart and do not promise recovery from an indefinitely stalled SteamVR service.

After tracking loss, the hidden resident continues checking headset pose every 250 ms while the dashboard is open. A valid pose restores readiness for a fresh request even if the previous lease expired. This check does not show the panel or reopen an expired, dismissed or explicitly returned native request.

A signal-stopped app is a separate case. While monitoring its original parent, the helper checks `/proc/<pid>/stat` for uppercase `T` before applying any readiness or recovery grace. A confirmed stop goes directly to SIGKILL because the frozen watchdog cannot hide the panel and SIGTERM cannot execute while stopped. Sleeping, blocked and tracing-stop states do not trigger this path. Unreadable process state retains the ordinary recovery deadlines. Tests freeze only a disposable fixture process, never the installed keyboard.

In Steam sessions, Esc dismisses the current panel like X. Open the gear menu and select **Use Steam keyboard** to give the current request back to Steam. Timeout, disable, exit and removal reopen the original owner's native provider while keeping its Steam field target intact, then remove presentation suppression. The restored request remains native until dismissed. The helper exits on parent EOF and removes its hooks when CDP is available. If CDP is lost, the independent Steam-side lease performs restoration. A subsequent bridge can replace an expired hook instance. No surviving helper is meant to suppress native input after uninstall.

The helper never reads initial field text or clipboard contents. Text travels transiently through the private IPC/CDP connection and is not logged or saved. A shared output controller accepts up to 32 queued text/control entries in press order, with ordinary pressed visuals and haptics. Subsequent output waits for the previous text or named-control acknowledgement. Characters and modifier mappings are captured at press time. Queued clicks retain their releases; already-delivered holds release immediately, and repeats begin only after delivery completes. Unsupported Steam-field shortcuts remain unsupported and never escape through local input.

A failed or uncertain delivery stops the queue. Failed text and consecutive queued text before the first native action remain for one Enter retry while the same typing session remains valid. Queued native actions and text after that boundary are discarded with a status. Each text entry stays within the compositor's 32-character limit; one retry advances all retained entries without merging across a discarded Tab or Enter. Target/request changes, route changes, cancellation, hiding, native fallback and shutdown clear the queue and retry buffer. Native fallback takes precedence over retaining a retry for an expired field. No timeout automatically resubmits text. A dispatch already in flight can finish after release or cancellation; these operations cannot retract a request already sent to Steam. A connection loss after delivery but before acknowledgement cannot establish whether the character reached the target. Check the field before retrying in that case.

Installation has its own two-second CDP deadline. The parent allows at most three seconds from helper launch through the first state reply, including connection and native-guard setup. Repeated frame packets cannot extend that startup budget, and the bridge cannot take over before a fresh readiness handshake. A timed-out attempt receives token-scoped cleanup even without an install acknowledgement. The install expression also checks its expiry before discovery and after hook creation, disposing a late result rather than publishing it. Python and Steam run on the same device and use its wall clock for those checks. If the connection is lost, the existing independent lease and orphan-hook disposal remain the recovery backstop. Discovery stops at the first valid keyboard store; no module cache persists between installations.

Ordinary CDP calls retain their 400 ms deadline, independent of the 300 ms connection timeout. Partial frames stay buffered across read timeouts, and late replies are discarded by command ID. One timed-out status poll skips that cycle; a timed-out text/key delivery reports failure and requests native fallback. Dismissal timeouts also request fallback. Three consecutive call timeouts end the helper. After the first state reply, the parent uses its normal 800 ms freshness limit. The Steam lease and runtime watchdog deadlines are unchanged.

Status polls run every 35 ms during a request, handoff or restoration. When the bridge explicitly reports that all of those are closed, the interval is 75 ms. Panel visibility alone cannot establish idle state. Missing or inconsistent idle metadata and timed-out polls keep the fast cadence; explicit fallback requests wake polling immediately. Input and parent packets remain independent of that timer. Watchdog deadlines and native-guard traffic are unchanged. The slower idle interval can add detection latency; it does not establish a battery-life improvement.

Steam keeps an independent 50 ms watchdog tick. During quiescence, a full inspection can share a recent helper or event inspection; without those inspections, the timer performs one after 100 ms. Busy sessions, pending restoration, clock reversal and expired leases with presentation suppression still force full inspection on the watchdog tick. Helper steps, status callbacks and presentation hooks always inspect directly. The independent lease and abandoned-install cleanup deadlines remain unchanged.

For local Gamescope recovery, the native guard uses a 1.2 second deadline measured from real app progress. This allows Steam's 900 ms presentation lease to expire first, so recovery cannot be mistaken for a new toggle. Explicit fallback restores immediately after withdrawing the Steam hook's logical session. On app/helper EOF or failure the guard restores the exact overlay. OpenVR closes a keyboard when its requesting client exits, so a guard that has restored native stays alive until the native session closes. It then exits. This is temporary request ownership, with no installed service or startup entry. X cancels recovery and keeps native closed.

Use Settings → Quit Full Keyboard for normal shutdown. If terminating from a shell, signal the app's exact PID rather than matching every process containing `framekeyboard`. A broad pattern such as `pkill -f framekeyboard` can terminate the independent native guard too. Steam's expiring hook still withdraws suppression, but automatic recovery of the previous local-app request cannot run without that guard. Press the native keyboard button again after suppression has expired. That new request remains native while Full Keyboard is stopped. The lease intervals are design deadlines, not guaranteed wall-clock recovery bounds during runtime stalls or headset sleep.

The host `timing-ordering` test reads the actual C++, Python and JavaScript timing
expressions and checks these required relationships. Missing or ambiguous source
matches fail the test, so a refactor cannot silently disable a check. Values remain
in their current source files; this adds no runtime configuration or timer changes.

| Required relationship | Current values | Purpose |
| --- | --- | --- |
| Bridge receipt/frame freshness and native-guard frame freshness < Steam lease | 700 ms < 900 ms | Stale app progress stops qualifying for takeover before the lease expires |
| Overlay hide < Steam lease | 700 ms < 900 ms | Withdraw our panel before native recovery |
| Request acknowledgement < Steam lease | 400 ms < 900 ms | Bound unacknowledged presentation within the lease |
| CDP call < each C++ text/key confirmation < helper reply freshness < Steam lease | 400 ms < 600 ms < 800 ms < 900 ms | Resolve or fail individual deliveries before broader session expiry |
| Steam lease < both native-guard arm/renew deadlines < ordinary parent recovery < declared slow-work ceiling | 900 ms < 1.2 s < 3 s < 30 s | Expire suppression before reopening native, and allow fallback before ordinary hang termination |

These are ordering checks. Scheduling delays, blocked runtime calls and suspend
still require the headset acceptance checks described above.

## Validation

Automated tests use fake Steam objects, fake input sinks and private socketpairs. They cover automatic-field metadata, same-window target changes, Unicode acknowledgement, failed commits, controller guarding, readiness deadlines, lease expiry, restoration, hook ownership and helper cleanup. A further 128 deterministic event sequences combine target changes, shared providers, missed close acknowledgements, pre-status shows, stale input, progress/readiness changes, dismissal and recovery. They check each unacknowledged request's deadline after time advances, expired-lease visibility, exclusive presentation, input rejection and restoration of original functions. Failure output includes a replayable seed and event trace. Recovery tests exercise bounded slow work, a stuck main loop with a live watchdog, signal escalation, helper exit and acquisition of the released instance lock by a new process. Actual process termination is confined to an isolated test executable with fake Steam/OpenVR interfaces. Host CJK suites may skip when their optional engines are absent; ARM64 tests run against the Frame's installed libraries.

Before relying on replacement, test in the headset with harmless text:

- Steam search and another Steam field that opens the keyboard automatically; dashboard keyboard button; desktop and floating local-app fields.
- Unicode, held Backspace, held printable keys, Enter, Tab, arrows, modifiers, Copy/Paste and local composition.
- No visible native keys or native movement handle, no flash on repeated summons, and no stock controller actions beneath our panel.
- X hides without exiting; the next native request reopens ours. Settings Quit exists only with replacement enabled.
- Use Steam keyboard, disable, explicit Settings Quit, crash, helper/CDP loss, backend loss, main-loop stall and full process suspension. Native must remain visible and clickable, including after repeated summon presses.
- Request changes while readiness or text is pending; Steam restart/update; app removal followed by native summons.
- With replacement enabled, headset sleep/wake and a SteamVR restart. Within the bounded recovery period, a responding runtime must not be mistaken for a permanent app hang. Verify native fallback and then a fresh Full Keyboard summon; runtime restart may still require manually relaunching the app if OpenVR disconnects.

This document defines prototype behavior and checks. It does not claim those physical acceptance checks have passed.
