---- MODULE G1Lifecycle ----
\* Frametop G1, lifecycle and presence, as the code at frametop 78e93b1 (experimental) does it,
\* with the L1-L7 fixes (DEE-252..258) since: see "Fixed" in docs/charts/README.md.
\* Read-only model: it describes the code and changes nothing. The copy kept for the reader is
\* ~/frametop-charts/charts/G1-lifecycle.tla; TLC needs the file named after the module, so the
\* runs use this copy, G1Lifecycle.tla.
\*
\* Two halves, as HostChart.tla slices HostInputLease.tla, so each fits TLC and the chart:
\*  - the pause (PausePart): SteamVR start and stop, the pointer helper's hello and "vrgame", the
\*    input relay (input/input-relay.py, input/game_pause.py) and its restarts with the state file
\*    /run/user/UID/frametop-pause.json, its worker's jobs, the gaze unit, hand tracking, the
\*    desktop (desktops.sh, session/frametop-session.sh, ft-screens' "pause on|off"), the relay's
\*    3D mouse and the helper's virtual controller, and gamepad mode;
\*  - presence (WornPart): the headset's activity level, the pointer helper's worn answer
\*    (pointer/helper/ft-pointer.cpp, HEADSET_OFF_AFTER 1.5 s), ft-powerd's away (power/ft-powerd.cpp,
\*    1 s), and the displays (DISPLAY_OFF_MIN, SteamVR's standby, powerd-brightness).
\* Time in the worn half goes in steps: t = 0 under 1 s since the level changed, 1 from 1 s to
\* 1.5 s, 2 from 1.5 s to 5 s, 3 from 5 s (SteamVR's standby).
\*
\* Left out: remote desktop (VNC, stopped and started like the desktop), the hand view unit,
\* parking (hand interaction on: hand tracking stays up, parked), HANDS_MODE=always, the sounds,
\* PAUSED_ACTIONS, pause_auto off (it is on here), datagrams other than "pause off" being lost.
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    DeskMode,       \* pause_desktop: "hide" (the default) or "close"
    AutoStart,      \* AUTOSTART=1: frametop-autostart.service starts the desktop with SteamVR
    HandsWithDesk,  \* HANDS_MODE=frametop, hand interaction off: the session starts hand tracking, pausing stops it
    PausePart,      \* the pause half's own steps on
    WornPart,       \* the worn and display half's own steps on
    Crashes,        \* the relay, ft-gazed and ft-powerd can die and come back
    Lossy,          \* the relay's "pause off" to ft-screens can be lost (a full socket: EAGAIN)
    OffMin,         \* DISPLAY_OFF_MIN > 0
    MaxJobs         \* the worker's queue, bounded for TLC (CONSTRAINT JobsBound)

VARIABLES
    svr,      \* SteamVR: "down" | "up"
    helper,   \* frametop-pointer (the pointer helper): "down" | "starting" (before its vrhello) | "up"
    gaze,     \* frametop-gaze: "down" | "up" | "restarting" (Restart=on-failure, 3 s)
    hands,    \* frametop-camd + frametop-hands: "down" | "up"
    game,     \* a SteamVR scene app (a VR game) runs
    relay,    \* frametop-input-relay: "down" | "up"
    paused,   \* GamePause.paused
    ends,     \* GamePause.ends_with_game
    rgame,    \* GamePause.game: the relay's idea of whether a game runs
    due,      \* GamePause.resume_at is set (RESUME_DELAY after the game ended)
    mouse,    \* state["pointer"]: the relay feeds the 3D mouse (None while paused)
    ractive,  \* the relay's Pointer.active
    ptr,      \* the helper's pointer awake: its virtual controller has the laser
    pad,      \* gamepad mode on (vrpad.GamepadMode)
    jobs,     \* the pause worker's queue: "pause" | "resume" | "enforce"
    stU,      \* Worker.stopped["units"]: what the resume starts again
    stD,      \* Worker.stopped["desktop"]: "none" | "hidden" | "closed"
    file,     \* STATE_PATH as last saved
    desk,     \* the desktop: "down" | "starting" (session up, ft-screens not listening yet) | "up"
    spaused,  \* ft-screens' g_paused
    told,     \* the relay has seen this ft-screens (screens_seen) since either started
    lost,     \* the pause closed the desktop and nothing has started it since (cleared when SteamVR stops)
    lvl,      \* the headset's activity level: "on" | "off" (idle, standby)
    t,        \* time since lvl last changed, in steps (above)
    hOff0,    \* the helper started with the headset off ("idle at the start: off already")
    powerd,   \* frametop-power (ft-powerd): "down" | "up"
    pAway,    \* ft-powerd's away
    pFresh,   \* ft-powerd started less than a second ago
    disp,     \* the displays: "on" | "darkPd" (ft-powerd's timeout) | "darkSvr" (SteamVR's standby)
    saved,    \* ~/.cache/frametop/powerd-brightness exists
    pdOff     \* ft-powerd's off flag

svrVars == <<svr, helper, gaze, hands, game>>
pauseVars == <<paused, ends, due, spaused, pad, ptr, ractive, mouse, jobs, file>>
wornVars == <<lvl, t, hOff0, powerd, pAway, pFresh, disp, saved, pdOff>>
vars == <<svr, helper, gaze, hands, game, relay, paused, ends, rgame, due, mouse, ractive, ptr, pad,
          jobs, stU, stD, file, desk, spaused, told, lost, lvl, t, hOff0, powerd, pAway, pFresh, disp,
          saved, pdOff>>

Units == {"gaze", "hands"}
FileOf(p, e, u, d) == [paused |-> p, ends |-> e, units |-> u, desk |-> d]
Min(a, b) == IF a < b THEN a ELSE b

\* The helper's answer to "gaze ? headset": worn, not away. Away once the level has been idle
\* 1.5 s, or at once when the helper started with the headset off.
HelperWorn == helper = "up" /\ (lvl = "on" \/ (t < 2 /\ ~hOff0))
WornAt(t2) == lvl = "on" \/ (t2 < 2 /\ ~hOff0)

Init ==
    /\ svr = "down" /\ helper = "down" /\ gaze = "down" /\ hands = "down" /\ game = FALSE
    \* The relay starts before SteamVR (Before=steamvr.service) and runs without it.
    /\ relay = "up" /\ paused = FALSE /\ ends = FALSE /\ rgame = FALSE /\ due = FALSE
    /\ mouse = TRUE /\ ractive = FALSE /\ ptr = FALSE /\ pad = FALSE
    /\ jobs = <<>> /\ stU = {} /\ stD = "none" /\ file = FileOf(FALSE, FALSE, {}, "none")
    /\ desk = "down" /\ spaused = FALSE /\ told = FALSE /\ lost = FALSE
    /\ lvl = "on" /\ t = 1 /\ hOff0 = FALSE /\ powerd = "down" /\ pAway = FALSE /\ pFresh = FALSE
    /\ disp = "on" /\ saved = FALSE /\ pdOff = FALSE

\* ---- The relay's pause() and resume() (game_pause.py GamePause), in one step each ----
\* pause(): ends_with_game = game and pause_auto; tell_screens(True); the relay's part
\* (pause_changed: gamepad.stop, Pointer.stand_down's "hide pause", which goes every time since L1's
\* fix, pointer None);
\* the worker's "pause" job; save().
DoPause(g) ==
    /\ paused' = TRUE /\ ends' = g /\ due' = FALSE
    /\ spaused' = IF desk = "up" THEN TRUE ELSE spaused
    /\ pad' = FALSE
    /\ ptr' = FALSE
    /\ ractive' = FALSE /\ mouse' = FALSE
    /\ jobs' = Append(jobs, "pause")
    /\ file' = FileOf(TRUE, g, stU, stD)

\* resume(): tell_screens(False) (one datagram, fire and forget); the pointer back; the worker's
\* "resume" job; save() with paused false, so a relay that starts after this ignores "stopped".
DoResume ==
    /\ paused' = FALSE /\ ends' = FALSE /\ due' = FALSE
    /\ spaused' \in IF desk = "up" THEN (IF Lossy THEN {FALSE, spaused} ELSE {FALSE}) ELSE {spaused}
    /\ mouse' = TRUE
    /\ jobs' = Append(jobs, "resume")
    /\ file' = FileOf(FALSE, FALSE, stU, stD)
    /\ UNCHANGED <<pad, ptr, ractive>>

\* ---- You ----
\* pause_toggle (a button, a key combination, the gesture, input/ft-pause).
YouToggle ==
    /\ PausePart /\ relay = "up"
    /\ IF paused THEN DoResume ELSE DoPause(rgame)
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, wornVars>>

YouStartGame ==
    /\ PausePart /\ svr = "up" /\ ~game /\ game' = TRUE
    /\ UNCHANGED <<svr, helper, gaze, hands, relay, rgame, stU, stD, desk, told, lost, pauseVars, wornVars>>

YouEndGame ==
    /\ game /\ game' = FALSE
    /\ UNCHANGED <<svr, helper, gaze, hands, relay, rgame, stU, stD, desk, told, lost, pauseVars, wornVars>>

\* desktops.sh start (by hand, Display Settings, an update restarting the desktop).
YouStartDesktop ==
    /\ PausePart /\ svr = "up" /\ desk = "down" /\ desk' = "starting"
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, spaused, told, lost, pauseVars, wornVars>>

\* desktops.sh stop: ft-screens ends, the session's trap says "handsmode down".
YouStopDesktop ==
    /\ PausePart /\ desk = "up"
    /\ desk' = "down" /\ spaused' = FALSE /\ told' = FALSE /\ pad' = FALSE
    /\ hands' = IF HandsWithDesk THEN "down" ELSE hands
    /\ UNCHANGED <<svr, helper, gaze, game, relay, rgame, stU, stD, lost, paused, ends, due, ptr,
                   ractive, mouse, jobs, file, wornVars>>

\* The mouse moves enough to wake the 3D mouse: the relay sends motion, the helper wakes (not
\* with the headset off).
YouMoveMouse ==
    /\ PausePart /\ relay = "up" /\ mouse /\ HelperWorn /\ ~(ractive /\ ptr)
    /\ ractive' = TRUE /\ ptr' = TRUE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   pad, mouse, jobs, file, wornVars>>

\* POINTER_IDLE with no mouse use: the relay sends "hide idle".
MouseIdles ==
    /\ ractive /\ ractive' = FALSE /\ ptr' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   pad, mouse, jobs, file, wornVars>>

\* A real controller moves: the helper lets the laser go (the relay doesn't hear of it).
YouMoveController ==
    /\ ptr /\ ptr' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   pad, mouse, ractive, jobs, file, wornVars>>

\* Enter Gamepad Mode on a panel (ft-screens' "gamepad desktop"), refused while paused or in a game.
YouStartGamepad ==
    /\ PausePart /\ relay = "up" /\ desk = "up" /\ ~paused /\ ~rgame /\ ~pad /\ pad' = TRUE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   ptr, ractive, mouse, jobs, file, wornVars>>

YouEndGamepad ==
    /\ pad /\ pad' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   ptr, ractive, mouse, jobs, file, wornVars>>

\* ---- SteamVR and its units ----
\* steamvr.service starts: the units it wants start with it (frametop-pointer, frametop-gaze,
\* frametop-power; frametop-autostart, which starts the desktop with AUTOSTART=1). Hand tracking
\* isn't wanted by it with HANDS_MODE=frametop. ft-powerd first puts back a brightness it left off.
SteamVRStarts ==
    /\ svr = "down" /\ svr' = "up" /\ helper' = "starting" /\ gaze' = "up"
    /\ desk' = IF AutoStart /\ desk = "down" THEN "starting" ELSE desk
    /\ powerd' = "up" /\ pAway' = FALSE /\ pFresh' = TRUE /\ pdOff' = FALSE /\ saved' = FALSE
    /\ disp' = IF saved /\ disp = "darkPd" THEN "on" ELSE disp
    /\ UNCHANGED <<hands, game, relay, rgame, stU, stD, spaused, told, lost, pauseVars, lvl, t, hOff0>>

\* SteamVR stops (or restarts, or crashes): PartOf stops its units; ft-screens ends on VREvent_Quit,
\* so the desktop goes; ft-powerd turns the displays back on as it stops.
SteamVRStops ==
    /\ svr = "up" /\ svr' = "down"
    /\ helper' = "down" /\ gaze' = "down" /\ hands' = "down" /\ game' = FALSE
    /\ desk' = "down" /\ spaused' = FALSE /\ told' = FALSE /\ lost' = FALSE
    /\ ptr' = FALSE /\ pad' = FALSE
    /\ powerd' = "down" /\ pAway' = FALSE /\ pFresh' = FALSE /\ pdOff' = FALSE
    /\ disp' = IF powerd = "up" /\ pdOff THEN "on" ELSE disp
    /\ saved' = IF powerd = "up" /\ pdOff THEN FALSE ELSE saved
    /\ UNCHANGED <<relay, rgame, stU, stD, paused, ends, due, ractive, mouse, jobs, file, lvl, t, hOff0>>

\* The helper is up and says "vrhello": the relay sends the mapped buttons, and while paused it
\* queues "enforce" (stop the units SteamVR just started). A relay that's down misses it.
HelperHello ==
    /\ helper = "starting" /\ svr = "up" /\ helper' = "up" /\ hOff0' = (lvl = "off")
    /\ jobs' = IF relay = "up" /\ paused THEN Append(jobs, "enforce") ELSE jobs
    /\ UNCHANGED <<svr, gaze, hands, game, relay, rgame, stU, stD, desk, spaused, told, lost,
                   paused, ends, due, pad, ptr, ractive, mouse, file, lvl, t, powerd, pAway,
                   pFresh, disp, saved, pdOff>>

\* The helper's "vrgame 1|0" (on a change and every 5 s) reaches the relay: game_state(). Only a
\* change does anything there. A game starting pauses (pause_auto) and ends gamepad mode; a game
\* ending sets resume_at for a pause that ends with it.
HelperSaysGame ==
    /\ PausePart /\ relay = "up" /\ helper = "up" /\ game # rgame
    /\ rgame' = game
    /\ \/ /\ game /\ paused
          /\ due' = FALSE /\ pad' = FALSE
          /\ UNCHANGED <<paused, ends, spaused, ptr, ractive, mouse, jobs, file>>
       \/ /\ game /\ ~paused
          /\ DoPause(TRUE)
       \/ /\ ~game
          /\ due' = (paused /\ ends)
          /\ UNCHANGED <<paused, ends, spaused, pad, ptr, ractive, mouse, jobs, file>>
    /\ UNCHANGED <<svrVars, relay, stU, stD, desk, told, lost, wornVars>>

\* GAME_STALE: no "vrgame" for 12 s (the helper is gone) ends the game in the relay.
GameGoesStale ==
    /\ PausePart /\ relay = "up" /\ rgame /\ helper # "up"
    /\ rgame' = FALSE /\ due' = (paused /\ ends)
    /\ UNCHANGED <<svrVars, relay, stU, stD, desk, told, lost, paused, ends, spaused, pad, ptr,
                   ractive, mouse, jobs, file, wornVars>>

\* RESUME_DELAY after the game ended.
ResumeDelayPasses ==
    /\ PausePart /\ relay = "up" /\ due
    /\ DoResume
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, wornVars>>

\* ft-gazed dies; systemd restarts it 3 s later (Restart=on-failure, RestartSec=3).
GazeDies ==
    /\ Crashes /\ PausePart /\ gaze = "up" /\ gaze' = "restarting"
    /\ UNCHANGED <<svr, helper, hands, game, relay, rgame, stU, stD, desk, told, lost, pauseVars, wornVars>>

GazeRestarts ==
    /\ gaze = "restarting" /\ svr = "up" /\ gaze' = "up"
    /\ UNCHANGED <<svr, helper, hands, game, relay, rgame, stU, stD, desk, told, lost, pauseVars, wornVars>>

\* ---- The relay's own life ----
\* It dies (or an install restarts it): its threads and its queue go with it. The helper keeps
\* its virtual controller; the relay's virtual pad goes.
RelayDies ==
    /\ Crashes /\ PausePart /\ relay = "up" /\ relay' = "down"
    /\ paused' = FALSE /\ ends' = FALSE /\ rgame' = FALSE /\ due' = FALSE
    /\ mouse' = FALSE /\ ractive' = FALSE /\ pad' = FALSE
    /\ jobs' = <<>> /\ stU' = {} /\ stD' = "none" /\ told' = FALSE
    /\ UNCHANGED <<svrVars, file, desk, spaused, lost, ptr, wornVars>>

\* It starts (Restart=on-failure, 1 s): load() takes the state file; paused, it queues "enforce".
\* Not paused but with a list of what a pause stopped (a resume whose job didn't run), it queues
\* "resume" (L5's fix). load_config(): the pointer only when not paused, and a paused relay's
\* pointer stands down, so the helper lets go (L1's fix). A new Pointer starts inactive; resume_at
\* is unset; the game is taken as running when the pause ends with it (L6's fix), else unknown.
RelayStarts ==
    /\ relay = "down" /\ relay' = "up"
    /\ LET leftover == ~file.paused /\ (file.units # {} \/ file.desk = "closed")
       IN /\ paused' = file.paused /\ ends' = (file.paused /\ file.ends)
          /\ rgame' = (file.paused /\ file.ends)
          /\ stU' = IF file.paused \/ leftover THEN file.units ELSE {}
          /\ stD' = IF file.paused \/ leftover THEN file.desk ELSE "none"
          /\ jobs' = IF file.paused THEN <<"enforce">> ELSE IF leftover THEN <<"resume">> ELSE <<>>
    /\ mouse' = ~file.paused
    /\ ptr' = IF file.paused THEN FALSE ELSE ptr
    /\ UNCHANGED <<svrVars, due, ractive, pad, file, desk, spaused, told, lost, wornVars>>

\* ---- The pause worker, one job at a time, then save() ----
\* "pause": close the desktop (pause_desktop close, ft-screens running: desktops.sh stop) or mark
\* it hidden; then stop_units(): every unit that is active, or activating (a restart systemd has
\* queued: L4's fix), stops and goes on the list.
StopUnits(g0, h0) ==
    LET sg == g0 \in {"up", "restarting"}
        sh == h0 = "up" /\ HandsWithDesk
    IN /\ gaze' = IF sg THEN "down" ELSE g0
       /\ hands' = IF sh THEN "down" ELSE h0
       /\ stU' = stU \cup (IF sg THEN {"gaze"} ELSE {}) \cup (IF sh THEN {"hands"} ELSE {})

WorkerPauses ==
    /\ relay = "up" /\ jobs # <<>> /\ Head(jobs) = "pause" /\ jobs' = Tail(jobs)
    /\ LET closing == DeskMode = "close" /\ desk = "up"
           h0 == IF closing /\ HandsWithDesk THEN "down" ELSE hands
           d2 == IF DeskMode = "close" THEN (IF closing THEN "closed" ELSE stD) ELSE "hidden"
       IN /\ desk' = IF closing THEN "down" ELSE desk
          /\ spaused' = IF closing THEN FALSE ELSE spaused
          /\ told' = IF closing THEN FALSE ELSE told
          /\ lost' = (lost \/ closing)
          /\ pad' = IF closing THEN FALSE ELSE pad
          /\ stD' = d2
          /\ StopUnits(gaze, h0)
          /\ file' = FileOf(paused, ends, stU', d2)
    /\ UNCHANGED <<svr, helper, game, relay, paused, ends, rgame, due, mouse, ractive, ptr, wornVars>>

\* "enforce" (paused, and SteamVR or the relay just started): stop_units() only.
WorkerEnforces ==
    /\ relay = "up" /\ jobs # <<>> /\ Head(jobs) = "enforce" /\ jobs' = Tail(jobs)
    /\ StopUnits(gaze, hands)
    /\ file' = FileOf(paused, ends, stU', stD)
    /\ UNCHANGED <<svr, helper, game, relay, paused, ends, rgame, due, mouse, ractive, ptr, pad,
                   stD, desk, spaused, told, lost, wornVars>>

\* "resume": systemctl --user start the listed units, only with SteamVR running (L3's fix: camd and
\* hands have no Requisite=steamvr.service), and with HANDS_MODE=frametop, hand tracking only with
\* the desktop running (a closed desktop starts it itself). A closed desktop starts again
\* (desktops.sh start; without SteamVR ft-screens can't).
WorkerResumes ==
    /\ relay = "up" /\ jobs # <<>> /\ Head(jobs) = "resume" /\ jobs' = Tail(jobs)
    /\ gaze' = IF "gaze" \in stU /\ svr = "up" /\ gaze = "down" THEN "up" ELSE gaze
    /\ hands' = IF "hands" \in stU /\ svr = "up" /\ desk = "up" THEN "up" ELSE hands
    /\ desk' = IF stD = "closed" /\ svr = "up" /\ desk = "down" THEN "starting" ELSE desk
    /\ stU' = {} /\ stD' = "none"
    /\ file' = FileOf(paused, ends, {}, "none")
    /\ UNCHANGED <<svr, helper, game, relay, paused, ends, rgame, due, mouse, ractive, ptr, pad,
                   spaused, told, lost, wornVars>>

\* ---- The desktop ----
\* ft-screens is listening: a new one starts unpaused; the session says "handsmode up", then
\* "ft-pause enforce" (L2's fix): a paused relay queues "enforce", which stops that hand tracking.
DesktopUp ==
    /\ desk = "starting" /\ svr = "up"
    /\ desk' = "up" /\ spaused' = FALSE /\ told' = FALSE /\ lost' = FALSE
    /\ hands' = IF HandsWithDesk THEN "up" ELSE hands
    /\ jobs' = IF relay = "up" /\ paused THEN Append(jobs, "enforce") ELSE jobs
    /\ UNCHANGED <<svr, helper, gaze, game, relay, rgame, stU, stD, paused, ends, due, pad, ptr,
                   ractive, mouse, file, wornVars>>

\* ft-screens says "keyboard ..." each second; the relay sees a new pid (screens_seen) and, paused,
\* tells it "pause on".
RelaySeesScreens ==
    /\ relay = "up" /\ desk = "up" /\ ~told
    /\ told' = TRUE /\ spaused' = (spaused \/ paused)
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, lost, paused, ends, due, pad, ptr,
                   ractive, mouse, jobs, file, wornVars>>

\* ft-screens' report each second now says whether it's paused; one out of step with the relay is
\* told again (screens_report, L7's fix). A lost correction is followed by the next report, so the
\* model takes the one that gets through.
ScreensBehind == relay = "up" /\ desk = "up" /\ told /\ spaused # paused
ScreensReport ==
    /\ ScreensBehind
    /\ spaused' = paused
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, pad, ptr,
                   ractive, mouse, jobs, file, wornVars>>

\* ---- Presence: the headset and the displays ----
YouTakeHeadsetOff ==
    /\ WornPart /\ lvl = "on" /\ lvl' = "off" /\ t' = 0
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, hOff0, powerd,
                   pAway, pFresh, disp, saved, pdOff>>

\* Putting it on ends SteamVR's standby, which lights the displays it turned off.
YouPutHeadsetOn ==
    /\ WornPart /\ lvl = "off" /\ lvl' = "on" /\ t' = 0 /\ hOff0' = FALSE
    /\ disp' = IF disp = "darkSvr" THEN "on" ELSE disp
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, powerd, pAway,
                   pFresh, saved, pdOff>>

\* Time passes. The helper's answer follows the level (it releases the pointer once away);
\* ft-powerd's away flips once the level has disagreed with it for a second (and the headset
\* back on counts as use: the displays it turned off come on); 5 s off is SteamVR's standby.
TimePasses ==
    /\ WornPart
    /\ LET t2 == IF lvl = "on" THEN Min(t + 1, 1) ELSE Min(t + 1, 3)
           awayNow == lvl = "off"
           flip == powerd = "up" /\ awayNow # pAway /\ (t2 >= 1 \/ pFresh)
           wake == flip /\ ~awayNow /\ pdOff
       IN /\ (t2 # t \/ pFresh)
          /\ t' = t2
          /\ pAway' = IF flip THEN awayNow ELSE pAway
          /\ pFresh' = FALSE
          /\ ptr' = IF helper = "up" /\ ~WornAt(t2) THEN FALSE ELSE ptr
          /\ disp' = IF wake THEN "on"
                     ELSE IF awayNow /\ t2 = 3 /\ svr = "up" /\ disp = "on" THEN "darkSvr" ELSE disp
          /\ saved' = IF wake THEN FALSE ELSE saved
          /\ pdOff' = IF wake THEN FALSE ELSE pdOff
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, paused, ends, due, spaused,
                   pad, ractive, mouse, jobs, file, lvl, hOff0, powerd>>

\* DISPLAY_OFF_MIN without use: ft-powerd saves the brightness, then writes 0.
PowerdTimesOut ==
    /\ WornPart /\ OffMin /\ powerd = "up" /\ ~pAway /\ disp = "on"
    /\ disp' = "darkPd" /\ saved' = TRUE /\ pdOff' = TRUE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, lvl, t, hOff0,
                   powerd, pAway, pFresh>>

\* Movement or input: ft-powerd turns the displays back on.
YouUseIt ==
    /\ WornPart /\ powerd = "up" /\ pdOff
    /\ disp' = "on" /\ saved' = FALSE /\ pdOff' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, lvl, t, hOff0,
                   powerd, pAway, pFresh>>

\* ft-powerd is killed (no chance to turn the displays on); Restart=on-failure brings it back,
\* and it puts back the brightness it saved.
PowerdDies ==
    /\ Crashes /\ WornPart /\ powerd = "up" /\ powerd' = "down" /\ pdOff' = FALSE /\ pAway' = FALSE
    /\ pFresh' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, lvl, t, hOff0,
                   disp, saved>>

PowerdRestarts ==
    /\ powerd = "down" /\ svr = "up" /\ powerd' = "up" /\ pAway' = FALSE /\ pFresh' = TRUE
    /\ disp' = IF saved /\ disp = "darkPd" THEN "on" ELSE disp
    /\ saved' = FALSE
    /\ UNCHANGED <<svrVars, relay, rgame, stU, stD, desk, told, lost, pauseVars, lvl, t, hOff0, pdOff>>

Next ==
    \/ YouToggle \/ YouStartGame \/ YouEndGame \/ YouStartDesktop \/ YouStopDesktop
    \/ YouMoveMouse \/ MouseIdles \/ YouMoveController \/ YouStartGamepad \/ YouEndGamepad
    \/ SteamVRStarts \/ SteamVRStops \/ HelperHello \/ HelperSaysGame \/ GameGoesStale
    \/ ResumeDelayPasses \/ GazeDies \/ GazeRestarts \/ RelayDies \/ RelayStarts
    \/ WorkerPauses \/ WorkerEnforces \/ WorkerResumes \/ DesktopUp \/ RelaySeesScreens \/ ScreensReport
    \/ YouTakeHeadsetOff \/ YouPutHeadsetOn \/ TimePasses \/ PowerdTimesOut \/ YouUseIt
    \/ PowerdDies \/ PowerdRestarts

Spec == Init /\ [][Next]_vars

JobsBound == Len(jobs) <= MaxJobs

\* ---- Rules ----
TypeOK ==
    /\ svr \in {"down", "up"} /\ helper \in {"down", "starting", "up"}
    /\ gaze \in {"down", "up", "restarting"} /\ hands \in {"down", "up"}
    /\ relay \in {"down", "up"} /\ stU \subseteq Units /\ stD \in {"none", "hidden", "closed"}
    /\ desk \in {"down", "starting", "up"} /\ lvl \in {"on", "off"} /\ t \in 0..3
    /\ disp \in {"on", "darkPd", "darkSvr"} /\ powerd \in {"down", "up"}
    /\ \A i \in 1..Len(jobs) : jobs[i] \in {"pause", "resume", "enforce"}

\* Nothing is on its way: the relay runs with an empty queue, the helper said hello, the desktop
\* isn't starting and the relay has seen it, no resume is due, ft-screens' next report isn't
\* about to correct it (ScreensReport), and the relay's game agrees with the helper's (or a gone
\* helper's game went stale).
Quiet ==
    /\ relay = "up" /\ jobs = <<>> /\ helper # "starting" /\ gaze # "restarting"
    /\ desk # "starting" /\ (desk = "up" => told) /\ ~due /\ ~ScreensBehind
    /\ (helper = "up" => rgame = game) /\ (helper # "up" => ~rgame)

\* Paused, the relay neither feeds the 3D mouse nor runs gamepad mode.
MouseFreeWhilePaused == relay = "up" /\ paused => ~mouse /\ ~ractive /\ ~pad
\* Paused, the helper's virtual controller doesn't hold the laser.
PointerFreeWhilePaused == relay = "up" /\ paused => ~ptr
\* Gamepad mode only outside a pause and a game.
GamepadOutsideGames == pad => relay = "up" /\ ~paused /\ ~rgame
\* Paused and quiet: the gaze unit and hand tracking don't run.
GazeOffWhilePaused == Quiet /\ paused => gaze # "up"
HandsOffWhilePaused == Quiet /\ paused => hands # "up"
\* Hand tracking (HANDS_MODE=frametop) runs only with a desktop, and only with SteamVR.
HandsFollowDesktop == Quiet => (hands = "up" => desk = "up" /\ svr = "up")
\* A running desktop is hidden exactly while paused.
DesktopMatchesPause == Quiet /\ desk = "up" => spaused = paused
\* After a resume, with SteamVR up, the gaze unit runs again.
GazeBackAfterResume == Quiet /\ ~paused /\ svr = "up" => gaze # "down"
\* After a resume, with SteamVR up, the desktop the pause closed is back.
ClosedDesktopComesBack == Quiet /\ ~paused /\ svr = "up" => ~lost
\* A pause that ends with its game doesn't outlast the game.
NoStuckGamePause == Quiet /\ paused /\ ends => game

\* The helper and ft-powerd disagree about the headset only in their debounce windows: taking it
\* off, ft-powerd says away from 1 s and the helper from 1.5 s; putting it on, the helper says
\* worn at once and ft-powerd 1 s later; and the second after ft-powerd starts.
WornAgreesSoon == helper = "up" /\ powerd = "up" /\ HelperWorn = pAway => t < 2 \/ pFresh
\* Displays ft-powerd turned off are ones it will turn on: it runs and knows, or the saved file says.
DarkHasOwner == disp = "darkPd" => (powerd = "up" /\ pdOff) \/ saved
\* Away (by the helper), the virtual controller is let go.
AwayReleasesPointer == helper = "up" /\ ~HelperWorn => ~ptr
====
