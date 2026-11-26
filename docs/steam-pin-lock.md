# Steam PIN protection (experimental)

**Experimental, opt-in; basic app-preserving lock/unlock wearer-tested.** The running
desktop is unchanged until the new components are installed, restarted and
`STEAM_PIN_LOCK=1` is set in the configuration file. Default is off while this
internal Steam API integration is experimental. Use the screens backend; the
gamescope backend is refused in protected mode rather than silently unprotected.

## Behavior

- Steam's existing PIN remains the only unlock authority. No second password.
- Unknown, locked, stale, disconnected or reset state denies access. A fresh,
  valid snapshot with loaded PIN settings, no active lock, no mandatory prompt
  and no reset permits access, including after observer startup/restart. There is
  no requirement to lock/unlock again. **It never requests a PIN
  screen, locks or unlocks Steam, or submits credentials. Steam alone owns those
  actions.** An absent prompt without initialized settings is not enough.
- Screens, floating windows, popups, controls and compositor-owned guides hide;
  desktop input is rejected. Full Keyboard hides and cancels input, leaving
  Steam's own keyboard available. Frunner and the hand/gaze panel views also hide.
- Pending presses are released before focus is cleared; locked overlay events
  are drained, including the first unlocked tick. Hand gesture/hold state resets.
- The compositor, nested KWin and apps remain running, with all application
  memory and unsaved work intact. Layout/profile metadata may update behind the
  gate; normal hide/show/resume cannot override the security gate. Locking never
  stops or restarts the desktop, uses SIGSTOP or freezes application cgroups.
- **Remote desktop serving and notification forwarding are unavailable for the
  entire protected session**, even when unlocked. These independent channels are
  not secured by hiding VR panels, so this first version refuses them rather than
  allowing a bypass. Incoming remote-display pictures use the same VR gate.
- Audio and background jobs continue, as with a conventional desktop lock. This
  is display/input protection, not a media mute or a suspension of applications.

The installed `session/frametop-pin-lock.service` publishes a private, boot-local
lease. Consumers fail closed without it. A lease is dated from the start of its
Steam query, expires in 750 ms, requires a live publisher and rejects evidence
spanning a suspend (boot and monotonic clocks disagree). A slow or failed query
cannot renew access using old data. No unlock token survives reboot.

This is **bounded-latency**, not an atomic lock transaction with Steam: normal
queries run every 150 ms, expiry is 750 ms, and an overlay's next processing tick
must hide it. A hung renderer cannot enforce its gate until it runs again. Do not
claim protection against a compromised/hung SteamVR compositor or same-user code.
Measure actual lock latency and test runtime-failure behavior before deployment.

## What was found

The nested KWin starts with its own lock screen disabled. Autostart waits for
SteamVR, not for Steam authentication. Linux logind's locked hint is not evidence
of the Steam six-digit PIN state. Steam owns the host screen-saver D-Bus name on
the investigated Frame, but its advertised GetActive method returns an error.

Steam's SharedJSContext exposes `securitystore.IsLockScreenActive()`. Existing
Frametop tools already use the local Chrome DevTools connection through
`steam/steamui.py`. Steam's UI source initializes active lock props to null before
it loads settings and mounts its initial PIN screen: **false alone is not proof
of unlock**. A dialog can also be cancellable, or a PIN-reset flow can clear it.

The current policy requires loaded settings with both lock-preference booleans,
defined lock props, a valid UI generation and explicit boolean state flags before
accepting an inactive lock. A live probe in an already-unlocked session reported
PIN configured, inactive lock, no mandatory prompt and no reset. The observer now
accepts that current snapshot without requiring a previously observed lock cycle.

**Unverified initialization assumption:** no cold-boot test has demonstrated
whether those same loaded-settings/inactive flags can briefly occur before an
initial mandatory prompt mounts. The risk was inferred from initialization code,
not observed in a headset startup test. This revision trades the former extra
lock-cycle requirement for trusting Steam's explicit current state once settings
are loaded; it is not a verified authentication-completed signal. Keep the feature
experimental/default-off until that startup ordering has been validated.

The observer in `steam/pin_lock.py` returns only explicit lock/configuration/reset
booleans and the UI context's time origin. It never returns the settings object,
PIN, account name, callback or authentication ticket. The default diagnostic makes
no calls to lock, unlock, change settings or submit credentials. Run locally on the Frame:

```sh
python3 steam/pin_lock.py --seconds 180
```

Output is only state changes: unknown, locked, or unlocked.
Unknown is deliberately not interpreted as unlocked. Each fresh snapshot is
classified independently; connection failure, malformed data, PIN reset and
inconsistent lock flags deny access. No previous unlocked state is reused after
a failure. A later fresh, valid inactive snapshot can recover automatically.
The default read-only probe exits after a bounded duration, at most ten minutes.
`--watch` is the installed guard mode; unlike the diagnostic it publishes access
leases, but all Steam access remains read-only. Short-lived locks entirely
between observations can be missed, so maintain the bounded-latency limitations
above rather than claiming event-perfect observation.
Initialized settings explicitly reporting no Steam PIN configured permit access;
uninitialized settings and active/reset prompts do not.

This is an internal Steam UI API, not a supported authentication API. Two real
PIN cycles were observed continuously: unknown → locked → unlocked. These only
validate detection, not the new enforcement. Cold boot, suspend/resume and the full
input/failure matrix still require wearer testing. Unit tests do not prove
the meaning or completeness of Steam's UI signal.

The feature was installed with an approved one-time
desktop/helper restart, leaving SteamVR running. Its first authentication changed
the live gate from locked to unlocked, and SteamVR reported the desktop screen
and controls visible afterward. The screen initially appeared missing because its
saved position was behind the wearer; bringing it in front resolved that without
restarting anything. In a subsequent real Steam PIN cycle, the wearer confirmed
Frametop disappeared while locked and returned after unlock. KWrite retained its
unsaved text, and KWrite, nested KWin and the compositor kept the same processes.
No screenshots were needed or published.

This validates basic visual concealment/restoration and unsaved-work preservation,
not all input routes, auxiliary panels, lock latency or failure cases. UI restart,
cold boot and the wider suspend/resume
matrix remain hardware-test gaps and must be completed before recommending
general use. This contribution covers Steam PIN protection only, not a
new remote-desktop hosting or client implementation.

An early version actively requested another Steam lock after lost proof. During
the floating-window test, the wearer received repeated prompts; logs showed
unknown states while unlocking, but the exact observation failure was not
reproduced in the next monitored cycle. That behavior was removed: Frametop is
now strictly passive. Fixed diagnostic reason codes identify invalid snapshots,
inconsistent flags, observation gaps/suspend, stale replies
and failed queries without logging raw Steam data or credentials.

In the earlier observed-lock-latch revision, an approved guard-only restart stayed denied without
creating a PIN prompt. The wearer used Steam's configured PIN-on-wake flow via
the headset power button and confirmed Frametop returned normally. Logs showed
unknown (including an observation-gap/suspend invalidation), then a real locked
state and unlocked-after-observed-lock. The original KWrite, KWin, compositor and
keyboard processes remained alive. This verifies passive recovery through that
normal sleep/wake cycle, not cold boot, prolonged suspend or all failure cases.

A further passive sleep/wake cycle started with both a floating KWrite window and
Full Keyboard confirmed visible. Read-only overlay observations showed every
Frametop panel, including Full Keyboard, hidden while denied; the desktop and
floating window returned after unlock. The wearer confirmed that result and
accepted Full Keyboard remaining dismissed rather than reopening automatically.
The wearer subsequently confirmed keyboard typing recovery in another app, and
normal summon/interaction attempts while locked did not reveal Frametop or cause
unexpected edits after unlock. These sequential lease and
overlay queries are not an atomic lock-latency measurement.

A later observer-stop test confirmed the guard inactive, an invalid access lease,
no Frametop overlays visible and unchanged compositor/KWin/keyboard processes.
The extra lock-cycle requirement after restarting that older guard prompted the
current fresh-state recovery change. That change needs its own live startup and
recovery checks; the earlier tests do not establish its cold-boot safety.

The fresh-state revision was loaded with a guard-only restart while Steam was
already unlocked. It immediately reported unlocked/steam-unlocked and readbacks
showed the desktop and floating windows visible, without a new Steam lock cycle
or PIN prompt. The wearer confirmed it looked correct. The compositor, KWin and
keyboard retained their processes. This checks warm observer startup, not the
unverified cold-boot ordering above.

## Intended boundary

The goal is to prevent a person using a PIN-locked headset from viewing or
interacting with an authenticated Frametop desktop. Steam remains the unlock
authority. No second PIN, credential storage or Frametop password dialog.

This is not a boundary against malicious code already running as the same Linux
user, privileged access, other unrestricted Steam applications or disk access.
The existing local DevTools endpoint itself permits same-user UI access. Steam's
PIN is not disk encryption or a Linux login boundary.

## Required before general use

1. Validate actual lock/unlock, cancellation, incorrect PIN (entered only in
   Steam), reset, UI restarts, suspend/resume and boot. Confirm no initial false
   loaded-settings/inactive state can authorize content before Steam's initial
   mandatory prompt. Verify fresh-state recovery in an unlocked session and
   fail-closed recovery when the settings/lock flags are unavailable.
2. Verify the independent security gate, not ordinary hide/pause. Start denied
   before displaying any sensitive texture or accepting input. Normal
   visibility, profiles, resume and manual startup must not override it.
3. Enforce at every output/input path: screens, floating windows and popups,
   previews/captures, keyboard/clipboard/voice, controller/hand/pointer input,
   notifications and remote desktop. Audit auxiliary overlays and queued events;
   remote services and their start paths must also fail closed. Decide and document
   audio behavior, rather than silently leaving a privacy leak.
4. Use bounded, fresh state with context identity. Lost observer, stale response,
   Steam restart, disconnect or resume invalidates access. Recheck before content
   presentation and input dispatch. Polling/IPC latency is not atomic; measure and
   document it, and prefer an event source that revokes access synchronously where
   available. Do not claim instantaneous protection from a periodic poll.
5. Verify that lock/unlock preserves running apps and unsaved work. Stopping the
   desktop or applications is not an acceptable lock fallback for this feature.
6. Add lifecycle/model tests for lock races, restarts, lost messages and attempts
   to show/resume while denied. Add Steam-update API compatibility checks and
   wearer validation before enabling or publishing.

## Offline checks

```sh
python3 -m unittest discover -s steam/tests -v
```

Thirty-seven tests cover valid inactive startup, mandatory vs cancellable prompts, relocking,
context reload, disconnect, invalid snapshots and booleans, reset/configuration
changes, inconsistent props and avoiding credential fields in the expression.
They also exercise the guard loop using fake Steam, atomic/private lease writing,
missing/bad/stale/dead-publisher leases, unsafe permissions, symlinks and FIFO
rejection. The C test checks matching expiry/suspend rules and actual private-file
reads without touching the real lease. All tests are offline and never send keys
to the production desktop.

The guard-loop tests use deterministic clocks to check scheduling gaps, a suspend
within the lease interval, slow authentication responses and slow no-PIN responses.
None may reuse stale evidence to permit access.
They also reject every guard-side Steam expression except the read-only snapshot,
check that repeated unknown states never request prompts, and verify passive
recovery after a failed observation or UI restart uses a new valid snapshot rather
than an old unlock or a forced lock cycle. Missing/uninitialized settings stay denied.

Integration tripwires check overlay show-call coverage, that the lock path cannot
stop applications, release-before-focus-clear ordering and the independent export
channel guards. They supplement, rather than replace, wearer testing.

An input-relay restart test currently fails its K3 expectation on untouched
upstream as well. The control protocol's recorded fixture assumes UID 0; it
matches when run as the image's default root user, not UID 1000. These limitations
must not be reported as a fully green repository suite.
