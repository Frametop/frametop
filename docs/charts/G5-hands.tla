---------------------------- MODULE G5Hands ----------------------------
(* G5 Hands: when hand tracking runs and when it is parked, at 78e93b1, with the fixes since.
   HANDS_MODE=frametop. The session starts and stops tracking with the desktop
   (hands/handsmode.py plan 289-318, session/frametop-session.sh 117-123). The input
   relay pauses for VR games (input/game_pause.py): without hand interaction it stops
   the hand units (stop_units 543-550), with it (POINTER_HANDS=1 or HANDS_NEAR=on,
   input-relay.py 1191) it keeps them and parks them instead (set_hands 701-707,
   update_park 714-733), except while the dashboard is open (the helper's "vrdash",
   ft-pointer.cpp 2594-2597). ft-hands processes no frames while run_dir/park exists
   (hands/track/main.cpp 890-917), and ft-camd stops copying for it.
   ft-cutouts (hands/ft-cutouts) runs the same two programs as transient units that
   conflict with the tracking units.

   With the G5 fixes (H5-1 to H5-4, DEE-274..277) and G1's L2 (DEE-253), this describes the
   fixed code. update_park makes the run folder (H5-1). Hand interaction switched off mid-pause
   stops what the pause kept (H5-2). The park is written whenever Frametop is paused, so it
   also holds ft-cutouts' tracker (H5-3). The helper says "vrdash" every 5 s, in a game or not
   (H5-4). A desktop that starts mid-pause has its hand tracking stopped again (L2). The
   constants L2, FixPark and DashFix no longer switch anything. They stay so the configs that
   showed each bug still load, and to see those bugs check out f85dff9.

   Not here: HANDS_MODE=always and off (always never parks, off never starts by itself),
   the job queue and timing of the pause (G1), SteamVR stopping mid-game (G1), the
   hand toggle (Meta+H), recording (never parks), lost datagrams. *)
EXTENDS Naturals

CONSTANTS
  DirAtStart,    \* /run/user/UID/frametop-hands exists when the model starts (hands ran since login)
  HandsOn,       \* hand interaction on in frametop.conf at the start
  ConfChanges,   \* hand interaction can be switched while Frametop runs (Input Settings)
  CutoutsUsed,   \* ft-cutouts on can be run
  Crashes,       \* the pointer helper and the input relay can die and come back
  L2,            \* was G1's L2 (a desktop starting mid-pause starts tracking). Fixed: unused
  FixPark,       \* was the H5-1 fix as a switch. Now the code: unused
  DashFix        \* was the H5-4 fix as a switch. Now the code: unused

VARIABLES
  vr,        \* SteamVR runs
  desk,      \* the Frametop desktop runs
  game,      \* a VR game runs
  byGame,    \* the pause ends with the game (ends_with_game)
  paused,    \* Frametop is paused (the relay's state, kept in frametop-pause.json)
  dash,      \* the SteamVR dashboard is open
  helper,    \* the pointer helper runs
  hSent,     \* the helper's dashSent: "vrdash" as it last sent it
  rDash,     \* the relay's copy of the dashboard (GamePause.dashboard)
  conf,      \* hand interaction in frametop.conf
  rHands,    \* the relay's copy (GamePause.hands; worker.keep follows it)
  tracking,  \* frametop-camd and frametop-hands run
  cutouts,   \* ft-cutouts' units run
  stopped,   \* the hand units are in the pause's stopped list
  dir,       \* the run folder exists
  park,      \* run_dir/park exists
  rParked    \* the relay's GamePause.parked: what it thinks it applied

vars == <<vr, desk, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands,
          tracking, cutouts, stopped, dir, park, rParked>>

TypeOK == \A v \in {vr, desk, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands,
                    tracking, cutouts, stopped, dir, park, rParked} : v \in BOOLEAN

Init ==
  /\ vr = FALSE /\ desk = FALSE /\ game = FALSE /\ byGame = FALSE /\ paused = FALSE
  /\ dash = FALSE /\ helper = FALSE /\ hSent = FALSE /\ rDash = FALSE
  /\ conf = HandsOn /\ rHands = HandsOn
  /\ tracking = FALSE /\ cutouts = FALSE /\ stopped = FALSE
  /\ dir = DirAtStart /\ park = FALSE /\ rParked = FALSE

(* update_park: want = paused and not dashboard (H5-3: hand interaction or not). Nothing to
   do when want equals what it last applied. Parking makes the folder if it's missing (H5-1).
   force: a relay that just started (parked None). *)
ParkTo(want, force) ==
  IF ~force /\ want = rParked THEN UNCHANGED <<park, rParked, dir>>
  ELSE IF want THEN park' = TRUE /\ rParked' = TRUE /\ dir' = TRUE
  ELSE park' = FALSE /\ rParked' = FALSE /\ dir' = dir

(* The worker's stop_units (543-550): the hand units, unless kept for parking. *)
StopUnits(h) ==
  IF tracking /\ ~h THEN tracking' = FALSE /\ stopped' = TRUE
  ELSE UNCHANGED <<tracking, stopped>>

(* pause() 735-748 with the worker's pause job (552-567), pause_desktop=hide. *)
Pause(g) ==
  /\ paused' = TRUE /\ byGame' = g
  /\ ParkTo(~rDash, FALSE)
  /\ StopUnits(rHands)

(* resume() 750-761 with the worker's resume job (573-592). Starting what the pause stopped
   with SteamVR off is G1's L3: here it starts only with SteamVR. *)
ResumeNow ==
  /\ paused' = FALSE /\ byGame' = FALSE
  /\ IF rParked THEN park' = FALSE /\ rParked' = FALSE ELSE UNCHANGED <<park, rParked>>
  /\ stopped' = FALSE
  /\ IF stopped /\ vr THEN tracking' = TRUE /\ dir' = TRUE /\ cutouts' = FALSE
     ELSE UNCHANGED <<tracking, dir, cutouts>>

SteamVRStarts ==
  /\ ~vr /\ vr' = TRUE /\ helper' = TRUE /\ hSent' = FALSE
  /\ UNCHANGED <<desk, game, byGame, paused, dash, rDash, conf, rHands, tracking, cutouts, stopped, dir, park, rParked>>

(* Every unit is PartOf steamvr; the desktop session ends with it. A game ends first. *)
SteamVRStops ==
  /\ vr /\ ~game
  /\ vr' = FALSE /\ helper' = FALSE /\ hSent' = FALSE /\ desk' = FALSE /\ dash' = FALSE
  /\ tracking' = FALSE /\ cutouts' = FALSE
  /\ UNCHANGED <<game, byGame, paused, rDash, conf, rHands, stopped, dir, park, rParked>>

(* handsmode desktop up (plan 307-309): starts tracking whenever SteamVR runs. Mid-pause
   without hand interaction, "ft-pause enforce" stops it again (G1's L2 fix), drawn as one
   step: it goes on the stopped list. Starting frametop-camd stops ft-cutouts' units
   (Conflicts=). *)
DesktopUp ==
  /\ vr /\ ~desk /\ desk' = TRUE
  /\ IF paused /\ ~rHands
       THEN stopped' = TRUE /\ UNCHANGED <<tracking, dir, cutouts>>
       ELSE tracking' = TRUE /\ dir' = TRUE /\ cutouts' = FALSE /\ UNCHANGED stopped
  /\ UNCHANGED <<vr, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands, park, rParked>>

(* handsmode desktop down (plan 314-317): the tracking units, not ft-cutouts'. *)
DesktopDown ==
  /\ desk /\ desk' = FALSE /\ tracking' = FALSE
  /\ UNCHANGED <<vr, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands, cutouts, stopped, dir, park, rParked>>

YouPause ==
  /\ vr /\ ~paused /\ Pause(FALSE)
  /\ UNCHANGED <<vr, desk, game, dash, helper, hSent, rDash, conf, rHands, cutouts>>

YouResume ==
  /\ paused /\ ResumeNow
  /\ UNCHANGED <<vr, desk, game, dash, helper, hSent, rDash, conf, rHands>>

(* game_state 763-776: auto pause on a game, resume when it ends (RESUME_DELAY folded in). *)
GameStarts ==
  /\ vr /\ helper /\ ~game /\ game' = TRUE
  /\ IF ~paused THEN Pause(TRUE) ELSE UNCHANGED <<paused, byGame, park, rParked, dir, tracking, stopped>>
  /\ UNCHANGED <<vr, desk, dash, helper, hSent, rDash, conf, rHands, cutouts>>

GameEnds ==
  /\ game /\ game' = FALSE
  /\ IF paused /\ byGame THEN ResumeNow
     ELSE UNCHANGED <<paused, byGame, park, rParked, stopped, tracking, dir, cutouts>>
  /\ UNCHANGED <<vr, desk, dash, helper, hSent, rDash, conf, rHands>>

DashOpens ==
  /\ vr /\ ~dash /\ dash' = TRUE
  /\ UNCHANGED <<vr, desk, game, byGame, paused, helper, hSent, rDash, conf, rHands, tracking, cutouts, stopped, dir, park, rParked>>

DashCloses ==
  /\ dash /\ dash' = FALSE
  /\ UNCHANGED <<vr, desk, game, byGame, paused, helper, hSent, rDash, conf, rHands, tracking, cutouts, stopped, dir, park, rParked>>

(* ft-pointer.cpp: "vrdash" on a change of what it last sent, and every 5 s, in a game or
   not (H5-4). The relay's dashboard_state applies it. *)
HelperSaysDash ==
  /\ helper
  /\ hSent' = dash /\ rDash' = dash
  /\ ParkTo(paused /\ ~dash, FALSE)
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, helper, conf, rHands, tracking, cutouts, stopped>>

HelperDies ==
  /\ Crashes /\ helper /\ helper' = FALSE
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, hSent, rDash, conf, rHands, tracking, cutouts, stopped, dir, park, rParked>>

(* Restart=: a new helper starts with dashSent false (ft-pointer.cpp 1458) and says vrhello:
   the relay's helper_started (802-807) re-sends "paused" and, while paused, stops what the
   pause stops. It doesn't touch the relay's dashboard. *)
HelperStarts ==
  /\ vr /\ ~helper /\ helper' = TRUE /\ hSent' = FALSE
  /\ IF paused THEN StopUnits(rHands) ELSE UNCHANGED <<tracking, stopped>>
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, rDash, conf, rHands, cutouts, dir, park, rParked>>

(* A new relay (Restart=, the state file keeps the pause and its stopped list): dashboard
   false (613), parked None, set_hands from the config (1191). *)
RelayRestarts ==
  /\ Crashes /\ rDash' = FALSE /\ rHands' = conf
  /\ ParkTo(paused, TRUE)
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, helper, hSent, conf, tracking, cutouts, stopped>>

(* Input Settings switches hand interaction; the relay reloads (load_config, set_hands).
   Switched off mid-pause, the worker stops what the pause kept (H5-2). *)
YouSwitchHands ==
  /\ ConfChanges /\ conf' = ~conf /\ rHands' = ~conf
  /\ ParkTo(paused /\ ~rDash, FALSE)
  /\ IF paused /\ rHands /\ conf THEN StopUnits(FALSE) ELSE UNCHANGED <<tracking, stopped>>
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, helper, hSent, rDash, cutouts>>

(* ft-cutouts on: transient units that conflict with the tracking units. *)
CutoutsOn ==
  /\ CutoutsUsed /\ vr /\ ~cutouts /\ cutouts' = TRUE /\ tracking' = FALSE /\ dir' = TRUE
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands, stopped, park, rParked>>

CutoutsOff ==
  /\ cutouts /\ cutouts' = FALSE
  /\ UNCHANGED <<vr, desk, game, byGame, paused, dash, helper, hSent, rDash, conf, rHands, tracking, stopped, dir, park, rParked>>

Next ==
  \/ SteamVRStarts \/ SteamVRStops \/ DesktopUp \/ DesktopDown
  \/ YouPause \/ YouResume \/ GameStarts \/ GameEnds
  \/ DashOpens \/ DashCloses \/ HelperSaysDash \/ HelperDies \/ HelperStarts
  \/ RelayRestarts \/ YouSwitchHands \/ CutoutsOn \/ CutoutsOff

(* The helper's sends and systemd's restarts happen; nothing forces the user, games or crashes. *)
Spec == Init /\ [][Next]_vars /\ SF_vars(HelperSaysDash) /\ WF_vars(HelperStarts)

(* ft-hands processes frames: a tracker runs and isn't parked. *)
Tracks == (tracking \/ cutouts) /\ ~park
Runs == tracking \/ cutouts

(* Paused for a game with the dashboard closed, nothing tracks hands for long: they are
   stopped or parked (the pause's purpose: the CPU goes to the game). *)
TrackerIdleInPause == [](paused /\ ~dash /\ Tracks => <>(~paused \/ dash \/ ~Tracks))

(* Parked tracking comes back when it is wanted: the pause ends, or the dashboard opens
   (hands work there during a game). *)
ParkLifts == [](park /\ Runs /\ (~paused \/ dash) => <>(~park \/ ~Runs \/ (paused /\ ~dash)))
=============================================================================
