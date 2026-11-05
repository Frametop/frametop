---------------------------- MODULE G4Gaze ----------------------------
(***************************************************************************)
(* G4 Gaze: the gaze service's lifecycle at 78e93b1 (gaze/ft-gazed, its    *)
(* checks in gaze/gazecheck.py, our tracker gaze/tracker/ft-eyes), with    *)
(* the Z1, Z2 and Z4 fixes (DEE-270, DEE-271, DEE-273) since.               *)
(*                                                                         *)
(* ft-gazed runs ft-gaze (the eye tracker's samples) and, with our tracker, *)
(* ft-eyes, only while the gaze is in use (awake): gaze mode on with the     *)
(* headset worn (the pointer helper's "gaze ? headset"), a check open or    *)
(* asked for. IDLE_AFTER without use, both stop. A check asked for while    *)
(* idle waits (pending) until the tracker sends or WAKE_SETTLE passes.      *)
(*                                                                         *)
(* Gaze mode on without a calibration opens the full calibration once per   *)
(* arming (full_armed). Arming: the service starting, gaze mode coming on   *)
(* (on_gaze_on), the headset going on (the don: eyes back after AWAY_MIN    *)
(* without, only with no check open since 3a796b5: DonGuard), and the      *)
(* tracker having a calibration (need_full's "if cal").                     *)
(*                                                                         *)
(* Our tracker sends no gaze before its first calibration. Since d39fbb9    *)
(* (Blind) that calibration runs "blind": eyes seen by SteamVR's tracker    *)
(* and ft-eyes answering are enough. Before it, can_run needed a gaze, so   *)
(* a fresh install could never calibrate: the 2026-10-05 deadlock.          *)
(*                                                                         *)
(* ft-gazed knows our tracker's calibration only from ft-eyes' status       *)
(* reply (said), asked every second and stale after 5 s ("none" there).     *)
(* A full calibration hides Frametop's screens (ft-screens "hide", a plain  *)
(* toggle) and shows them again in close(), if they were shown. Since Z1's  *)
(* fix, saved is also a file (SCREENS_HIDDEN), so it outlives a crash.      *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    Own,          \* TRUE: our tracker (GAZE_TRACKER=own or auto with it installed); FALSE: SteamVR's
    Blind,        \* the blind first calibration (d39fbb9); FALSE is the code before it
    DonGuard,     \* no re-arm at the don while a check is open (3a796b5); FALSE is before it
    MmapKnown,    \* ft-gaze knows eye-server.mmap's layout (EyeFile::Detect)
    Crashes,      \* ft-gazed can die (Restart=on-failure brings it back 3 s later)
    Cooperative,  \* the user takes every dot and never quits or ignores a check
    Blips,        \* the eyes can come back between EYES_GONE (2 s) and AWAY_MIN (3 s)
    FitSays,      \* calib-fit's answer updates what ft-gazed knows (said) before need_full. Was a
                  \* what-if; since Z2's fix the code does it whatever this says
    Asks          \* the user can use Calibrate (Input Settings, ft-gazectl calibrate)

VARIABLES
    svc,      \* ft-gazed: "up" | "down"
    gazeOn,   \* the pointer helper's gaze mode
    worn,     \* someone wears the headset (the helper's worn, SteamVR's eyes seen)
    vOn,      \* ft-gazed's view of gaze mode (Checks.gaze_on): "?" | "on" | "off"
    vWorn,    \* ...and of the headset (Checks.headset)
    awake,    \* ft-gaze (and ft-eyes) wanted: Service.awake
    settled,  \* WAKE_SETTLE has passed since waking (Service.waking() is FALSE)
    feed,     \* ft-gaze runs
    eyesP,    \* ft-eyes runs (our tracker)
    cal,      \* the tracker in use has a calibration (ft-eyes' own, or calibration.json)
    said,     \* our tracker's calibration as ft-gazed last heard: "none" | "nocal" | "cal"
    check,    \* "none" | "quick" | "full" | "fitting" (ft-eyes has the dots: calib-fit)
    armed,    \* Checks.full_armed
    pending,  \* a calibration asked for while idle (Checks.pending)
    away,     \* Checks.away: no eyes for AWAY_MIN
    hidden,   \* ft-screens hides the screens (its g_manual, set by the calibration's "hide")
    saved,    \* Checks.screens_shown: they were shown when the calibration hid them
    extra     \* a full calibration opened by itself while the tracker already had one

vars == <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, check, armed, pending,
          away, hidden, saved, extra>>

Init ==
    /\ svc = "up" /\ gazeOn \in BOOLEAN /\ worn \in BOOLEAN /\ vOn = "?" /\ vWorn = FALSE
    /\ awake = FALSE /\ settled = FALSE /\ feed = FALSE /\ eyesP = FALSE
    /\ cal = FALSE /\ said = "none" /\ check = "none" /\ armed = TRUE /\ pending = FALSE
    /\ away = TRUE /\ hidden = FALSE /\ saved = FALSE /\ extra = FALSE

-----------------------------------------------------------------------------
\* What ft-gazed sees.
EyesSeen == svc = "up" /\ feed /\ worn /\ MmapKnown          \* mmap1's "unc" under EYE_LOST
GazeFlows == IF Own THEN svc = "up" /\ feed /\ worn /\ eyesP /\ cal   \* ft-eyes maps pupils only with a calibration
             ELSE EyesSeen
Calibrated == IF Own THEN said ELSE IF cal THEN "cal" ELSE "nocal"    \* Checks.calibrated(): "none" is None
IsBlind == Own /\ Calibrated = "nocal"                                \* Checks.blind()
CanRun == IF Blind /\ IsBlind THEN EyesSeen ELSE EyesSeen /\ GazeFlows   \* Checks.can_run()
Waking == ~awake \/ ~settled                                          \* Service.waking()
InUse == (vOn = "on" /\ vWorn) \/ check # "none" \/ pending           \* Service.use_reason()
Opens == CanRun /\ (~Own \/ eyesP)                                     \* start("full"): calib-start answered

\* close(): the panel goes, and the screens come back if the calibration hid them.
Close == /\ check' = "none"
         /\ hidden' = IF check \in {"full", "fitting"} /\ saved THEN FALSE ELSE hidden
         /\ saved' = IF check \in {"full", "fitting"} THEN FALSE ELSE saved

OpenFull == /\ check' = "full" /\ hidden' = TRUE /\ saved' = ~hidden

-----------------------------------------------------------------------------
\* The user.
PutOn == /\ ~worn /\ worn' = TRUE /\ UNCHANGED <<svc, gazeOn, vOn, vWorn, awake, settled, feed, eyesP, cal, said,
                                                 check, armed, pending, away, hidden, saved, extra>>
TakeOff == /\ worn /\ worn' = FALSE /\ UNCHANGED <<svc, gazeOn, vOn, vWorn, awake, settled, feed, eyesP, cal, said,
                                                   check, armed, pending, away, hidden, saved, extra>>
\* The relay's gaze_toggle or Input Settings' Gaze page.
GazeToggle == /\ gazeOn' = ~gazeOn /\ UNCHANGED <<svc, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said,
                                                  check, armed, pending, away, hidden, saved, extra>>
\* "calibrate" (Input Settings, ft-gazectl): pending while idle, else it opens now if it can.
AskCalibrate ==
    /\ Asks /\ svc = "up" /\ check = "none" /\ ~pending
    /\ IF Waking
       THEN /\ pending' = TRUE /\ UNCHANGED <<check, hidden, saved>>
       ELSE /\ Opens /\ OpenFull /\ UNCHANGED pending
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, armed, away, extra>>
\* Each dot clicked: with our tracker the dots go to ft-eyes (calib-fit), SteamVR's fit here.
TakeDots ==
    /\ check = "full" /\ EyesSeen /\ (GazeFlows \/ Own)   \* ours goes blind without its gaze (OWN_GONE)
    /\ IF Own THEN /\ check' = "fitting" /\ UNCHANGED <<cal, hidden, saved>>
       ELSE /\ cal' = TRUE /\ Close
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, said, armed, pending, away, extra>>
QuickDone ==
    /\ check = "quick" /\ EyesSeen /\ Close
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, armed, pending, away, extra>>
\* Ignored (QUICK_TIMEOUT, CLICK_IDLE): closed unfinished, not re-armed.
Ignore ==
    /\ ~Cooperative /\ check \in {"quick", "full"} /\ Close
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, armed, pending, away, extra>>
\* calquit: with no calibration still, gaze mode goes off (POINTER_GAZE=0, "gaze off").
Quit ==
    /\ ~Cooperative /\ check = "full" /\ Close
    /\ IF Calibrated = "nocal" THEN gazeOn' = FALSE /\ vOn' = "off" ELSE UNCHANGED <<gazeOn, vOn>>
    /\ UNCHANGED <<svc, worn, vWorn, awake, settled, feed, eyesP, cal, said, armed, pending, away, extra>>

-----------------------------------------------------------------------------
\* ft-gazed.
\* periodic's "gaze ? headset" and the helper's reply (on_readable): on_gaze_on arms.
Poll ==
    /\ svc = "up"
    /\ LET on == IF gazeOn THEN "on" ELSE "off" IN
       /\ <<vOn, vWorn>> # <<on, worn>>
       /\ vOn' = on /\ vWorn' = worn
       /\ armed' = IF vOn = "off" /\ gazeOn THEN TRUE ELSE armed
    /\ UNCHANGED <<svc, gazeOn, worn, awake, settled, feed, eyesP, cal, said, check, pending, away, hidden, saved, extra>>
\* update_awake.
Wake ==
    /\ svc = "up" /\ ~awake /\ InUse /\ awake' = TRUE /\ settled' = FALSE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, feed, eyesP, cal, said, check, armed, pending, away, hidden, saved, extra>>
Idle ==   \* IDLE_AFTER with nothing in use: stop_helper, stop_eyes
    /\ svc = "up" /\ awake /\ ~InUse /\ awake' = FALSE /\ feed' = FALSE /\ eyesP' = FALSE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, settled, cal, said, check, armed, pending, away, hidden, saved, extra>>
Settle ==
    /\ svc = "up" /\ awake /\ ~settled /\ settled' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, feed, eyesP, cal, said, check, armed, pending, away, hidden, saved, extra>>
FeedStarts ==   \* run(): start_helper while awake
    /\ svc = "up" /\ awake /\ ~feed /\ feed' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, eyesP, cal, said, check, armed, pending, away, hidden, saved, extra>>
EyesStart ==    \* periodic: start_eyes while eyes_wanted()
    /\ Own /\ svc = "up" /\ awake /\ ~eyesP /\ eyesP' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, cal, said, check, armed, pending, away, hidden, saved, extra>>
\* ft-eyes' status reply (on_own), or none for 5 s once it stopped.
Status ==
    /\ Own /\ svc = "up"
    /\ LET now == IF ~eyesP THEN "none" ELSE IF cal THEN "cal" ELSE "nocal" IN
       said # now /\ said' = now
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, check, armed, pending, away, hidden, saved, extra>>
\* periodic's away watch: no eyes for AWAY_MIN, then DON_DELAY of eyes is the headset going on.
GoneAway ==
    /\ svc = "up" /\ ~EyesSeen /\ ~away /\ away' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, check, armed, pending, hidden, saved, extra>>
Don ==
    /\ svc = "up" /\ EyesSeen /\ away /\ away' = FALSE
    /\ armed' = IF check = "none" \/ ~DonGuard THEN TRUE ELSE armed
    \* auto_quick: the one-dot check if calibrated (QUICK_COOLDOWN aside)
    /\ check' = IF check = "none" /\ vOn = "on" /\ CanRun /\ Calibrated = "cal" THEN "quick" ELSE check
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, pending, hidden, saved, extra>>
\* need_full, every second: "if cal: full_armed = True", then open once per arming.
ArmWhenCal ==
    /\ svc = "up" /\ Calibrated = "cal" /\ ~armed /\ armed' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, check, pending, away, hidden, saved, extra>>
NeedFull ==
    /\ svc = "up" /\ vOn = "on" /\ check = "none" /\ armed /\ Calibrated = "nocal" /\ Opens
    /\ OpenFull /\ armed' = FALSE /\ extra' = (extra \/ cal)
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, pending, away>>
\* run_pending: once the tracker sends (and ours has said), or WAKE_SETTLE after waking.
RunPending ==
    /\ svc = "up" /\ pending
    /\ (CanRun /\ (~Own \/ Calibrated # "none")) \/ ~Waking
    /\ pending' = FALSE
    /\ IF check = "none" /\ Opens THEN OpenFull ELSE UNCHANGED <<check, hidden, saved>>
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, armed, away, extra>>
\* tick: no eyes for EYES_GONE closes a check ("the headset came off"); not while ft-eyes fits.
\* Since Z4's fix that counts as away at once, so eyes back before AWAY_MIN are a don too (Blips
\* no longer matters).
EyesGone ==
    /\ check \in {"quick", "full"} /\ ~EyesSeen /\ Close
    /\ away' = TRUE
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, cal, said, armed, pending, extra>>
\* calib-fit's answer (or its deadline): fitted() closes. ft-eyes saved it if it was running, and
\* its "ok" sets what ft-gazed holds of its status (Z2's fix).
FitDone ==
    /\ check = "fitting" /\ cal' = (cal \/ eyesP) /\ Close
    /\ said' = IF eyesP THEN "cal" ELSE said
    /\ UNCHANGED <<svc, gazeOn, worn, vOn, vWorn, awake, settled, feed, eyesP, armed, pending, away, extra>>
\* ft-gazed dies (an exception, a kill): its children quit with their stdin, nothing calls close().
Crash ==
    /\ Crashes /\ svc = "up" /\ svc' = "down"
    /\ feed' = FALSE /\ eyesP' = FALSE /\ awake' = FALSE /\ check' = "none" /\ pending' = FALSE
    /\ UNCHANGED <<gazeOn, worn, vOn, vWorn, settled, cal, said, armed, away, hidden, saved, extra>>
\* systemd starts it again: a new Service and Checks, nothing remembered but SCREENS_HIDDEN:
\* the screens a dead calibration hid are shown again (Z1's fix).
Restart ==
    /\ svc = "down" /\ svc' = "up"
    /\ vOn' = "?" /\ vWorn' = FALSE /\ said' = "none" /\ armed' = TRUE /\ away' = TRUE /\ saved' = FALSE
    /\ hidden' = (hidden /\ ~saved)
    /\ UNCHANGED <<gazeOn, worn, awake, settled, feed, eyesP, cal, check, pending, extra>>

System == Poll \/ Wake \/ Idle \/ Settle \/ FeedStarts \/ EyesStart \/ Status \/ GoneAway \/ Don
          \/ ArmWhenCal \/ NeedFull \/ RunPending \/ EyesGone \/ FitDone \/ Crash \/ Restart
User == PutOn \/ TakeOff \/ GazeToggle \/ AskCalibrate \/ TakeDots \/ QuickDone \/ Ignore \/ Quit
Next == System \/ User

Fairness ==
    /\ WF_vars(Poll) /\ WF_vars(Wake) /\ WF_vars(Settle) /\ WF_vars(FeedStarts) /\ WF_vars(EyesStart)
    /\ WF_vars(Status) /\ WF_vars(GoneAway) /\ WF_vars(Don) /\ WF_vars(ArmWhenCal) /\ WF_vars(NeedFull)
    /\ WF_vars(RunPending) /\ WF_vars(EyesGone) /\ WF_vars(FitDone) /\ WF_vars(Restart)
    /\ WF_vars(TakeDots) /\ WF_vars(QuickDone)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
TypeOK ==
    /\ svc \in {"up", "down"} /\ vOn \in {"?", "on", "off"} /\ said \in {"none", "nocal", "cal"}
    /\ check \in {"none", "quick", "full", "fitting"}
    /\ {gazeOn, worn, vWorn, awake, settled, feed, eyesP, cal, armed, pending, away, hidden, saved, extra} \subseteq BOOLEAN

\* Idle means ft-gaze and ft-eyes are stopped (the half core with gaze mode off).
IdleStops == ~awake => (~feed /\ ~eyesP)
\* A check never runs on a stopped tracker: a check keeps the service awake.
CheckKeepsAwake == check # "none" => awake
\* No calibration open: Frametop's screens are not hidden by one. While ft-gazed is down (systemd
\* brings it back 3 s on) they may still be: its next start shows them (Z1).
ScreensComeBack == (svc = "up" /\ check = "none") => ~hidden
\* A full calibration opens by itself only for a tracker that has none.
FullOnlyWhenNeeded == ~extra

\* With someone in the headset and gaze mode on, a missing calibration gets made
\* (or the user takes the headset off or turns gaze mode off).
CalibrationComes == (gazeOn /\ worn /\ ~cal) ~> (cal \/ ~gazeOn \/ ~worn)
\* Gaze mode on and worn wakes the tracker.
TrackerWakes == (gazeOn /\ worn) ~> (feed \/ ~gazeOn \/ ~worn)
=============================================================================
