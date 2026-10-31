---------------------------- MODULE G2Desktop ----------------------------
(* G2 Desktop and panels: when a desktop screen shows, at 78e93b1. One screen of ft-screens
   (screens/vr.cpp UpdateVisibility 1236-1257, ModeVisible 1177-1197) and the programs that hide
   it: the hide/show switch (g_manual), the visibility mode and "ingames" (EffectiveMode
   1162-1164), conceal (s.alone), the pause (g_paused, from the input relay), a full gaze
   calibration (gaze/gazecheck.py 620-624, 1262-1266), and ft-layout, which sends the settings
   at desktop start (layout/ft_layout.py apply_screens 741-782). Remote access (VNC: krdp + Xvnc,
   session/remote-ctl.sh) runs with the desktop and stops for a pause (input/game_pause.py
   552-592).

   Two modes stand for four: "always" (and in a VR game with ingames hide, the dashboard mode,
   EffectiveMode) and "dashboard" (toggle and gesture act the same here: shown only by the
   switch or by something the user does). ingames is hide (the default). The pause hides
   (pause_desktop=hide); closing the desktop is G1's. Not here: floating windows (ft-floatd),
   wrist pins, remote displays (G6), several screens, ft-screens dying (Plasma ends with it). *)
EXTENDS Naturals

CONSTANTS
  CfgMode,       \* the visibility mode in layout.json: "always" or "dashboard"
  Concealed,     \* the screen is hidden on its own in layout.json (or its profile)
  Hotkey,        \* the user hides and shows the screens (screens_toggle, Meta+Shift+H)
  Cal,           \* a full gaze calibration can run
  GazedDies,     \* ft-gazed can die during a calibration (G4's Z1)
  AutoPause,     \* the relay pauses for a VR game (auto_pause)
  Lossy,         \* the relay's "pause on|off" datagrams can be lost (G1's L7)
  Remote,        \* REMOTE=1: remote access starts with the desktop
  HeadFix,       \* the P1 fix: ft-layout sends the settings even without a head pose
  CalFix,        \* the P2 fix: the calibration hides with a flag of its own, not the switch
  StartFix       \* the P3 fix: a desktop starting mid-pause leaves remote access to the resume

MODES == {"always", "dashboard"}

VARIABLES
  desk,      \* the desktop (ft-screens) runs
  pending,   \* ft-layout start (frametop-session.sh 89) hasn't sent the settings yet
  head,      \* SteamVR has a head pose (the headset on and awake)
  game,      \* a VR game runs
  dash,      \* the SteamVR dashboard is open
  rPaused,   \* the relay is paused
  byGame,    \* ... for the game, and resumes when it ends
  sPaused,   \* ft-screens' g_paused
  fresh,     \* the relay hasn't seen this ft-screens yet (screens_seen)
  sMode,     \* ft-screens' g_mode
  manual,    \* ft-screens' g_manual
  uManual,   \* (ghost) the switch as the user set it, in the configured mode
  alone,     \* ft-screens' s.alone (conceal)
  cal,       \* a full calibration is open
  saved,     \* gazecheck's screens_shown: "none", or whether it read the screens as shown
  calHide,   \* the CalFix flag in ft-screens
  vnc,       \* krdp and Xvnc run
  rStopped   \* the pause stopped remote access (stopped["remote"]): the resume starts it

vars == <<desk, pending, head, game, dash, rPaused, byGame, sPaused, fresh, sMode, manual,
          uManual, alone, cal, saved, calHide, vnc, rStopped>>

TypeOK ==
  /\ {desk, pending, head, game, dash, rPaused, byGame, sPaused, fresh, manual, uManual, alone, cal,
      calHide, vnc, rStopped} \subseteq BOOLEAN
  /\ sMode \in MODES /\ saved \in {"none", "shown", "hidden"}

(* EffectiveMode: in a VR game, always acts like dashboard. *)
Eff(m) == IF game /\ m = "always" THEN "dashboard" ELSE m
(* ModeVisible without the pause. *)
ModeVis(m, sw) == IF Eff(m) = "always" THEN ~sw ELSE sw \/ dash
(* "hide"/"show" (vr.cpp 5992-5998): the switch that gives want in mode m. *)
Sw(m, want) == IF Eff(m) = "always" THEN ~want ELSE want

Visible == desk /\ ~sPaused /\ ~alone /\ ~calHide /\ ModeVis(sMode, manual)
(* What the settings and the user ask for: not paused, not concealed, the configured mode with
   the user's switch, and not while a calibration runs. *)
Wanted == desk /\ ~rPaused /\ ~Concealed /\ ~cal /\ ModeVis(CfgMode, uManual)

Init ==
  /\ desk = FALSE /\ pending = FALSE /\ head \in BOOLEAN /\ game = FALSE /\ dash = FALSE
  /\ rPaused = FALSE /\ byGame = FALSE /\ sPaused = FALSE /\ fresh = FALSE
  /\ sMode = "always" /\ manual = FALSE /\ uManual = FALSE /\ alone = FALSE
  /\ cal = FALSE /\ saved = "none" /\ calHide = FALSE /\ vnc = FALSE /\ rStopped = FALSE

(* desktops.sh start (the dashboard's Desktop entry, or autostart.sh when SteamVR starts; neither
   checks the pause): a new ft-screens with its defaults (vr.cpp 528-548), ft-layout start
   pending, and with REMOTE=1 remote-ctl.sh start (frametop-session.sh 246-249). *)
DeskStarts ==
  /\ ~desk /\ desk' = TRUE /\ pending' = TRUE /\ fresh' = TRUE
  /\ sMode' = "always" /\ manual' = FALSE /\ uManual' = FALSE /\ alone' = FALSE /\ sPaused' = FALSE
  /\ calHide' = FALSE
  /\ IF Remote /\ ~(StartFix /\ rPaused) THEN vnc' = TRUE /\ UNCHANGED rStopped
     ELSE IF Remote THEN rStopped' = TRUE /\ UNCHANGED vnc
     ELSE UNCHANGED <<vnc, rStopped>>
  /\ UNCHANGED <<head, game, dash, rPaused, byGame, cal, saved>>

(* The desktop ends (desktops.sh stop, SteamVR stopping): cleanup runs remote-ctl.sh stop (140).
   A calibration ends with it. *)
DeskStops ==
  /\ desk /\ desk' = FALSE /\ pending' = FALSE /\ vnc' = FALSE /\ calHide' = FALSE
  /\ cal' = FALSE /\ saved' = "none"
  /\ UNCHANGED <<fresh, head, game, dash, rPaused, byGame, sPaused, sMode, manual, uManual, alone, rStopped>>

(* ft-layout start / apply --wait: apply_screens asks "head" before it sends anything (756); with
   no head pose that raises, and the caller only logs it (1955-1958, 2031-2038): no visibility,
   no ingames, no conceal. *)
LayoutStart ==
  /\ desk /\ pending /\ pending' = FALSE
  /\ IF head \/ HeadFix THEN sMode' = CfgMode /\ manual' = FALSE /\ uManual' = FALSE /\ alone' = Concealed
     ELSE UNCHANGED <<sMode, manual, uManual, alone>>
  /\ UNCHANGED <<fresh, desk, head, game, dash, rPaused, byGame, sPaused, cal, saved, calHide, vnc, rStopped>>

(* Meta+Shift+R, the reset button, a settings change: ft-layout again, with a head pose. *)
YouReset ==
  /\ desk /\ head /\ ~pending
  /\ sMode' = CfgMode /\ manual' = FALSE /\ uManual' = FALSE /\ alone' = Concealed
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, rPaused, byGame, sPaused, cal, saved, calHide, vnc, rStopped>>

HeadChanges ==
  /\ head' = ~head
  /\ UNCHANGED <<fresh, desk, pending, game, dash, rPaused, byGame, sPaused, sMode, manual, uManual, alone,
                 cal, saved, calHide, vnc, rStopped>>

(* The hotkey (relay -> "toggle"): the user picks what they want to see. *)
YouHideShow ==
  /\ Hotkey /\ desk /\ \E want \in BOOLEAN :
       manual' = Sw(sMode, want) /\ uManual' = Sw(CfgMode, want)
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, rPaused, byGame, sPaused, sMode, alone, cal, saved, calHide, vnc, rStopped>>

DashChanges ==
  /\ dash' = ~dash
  /\ UNCHANGED <<fresh, desk, pending, head, game, rPaused, byGame, sPaused, sMode, manual, uManual, alone,
                 cal, saved, calHide, vnc, rStopped>>

(* The relay pauses: "pause on" (tell_screens 669-673), remote access stops if it runs
   (game_pause.py 560-564). *)
PauseNow(g) ==
  /\ rPaused' = TRUE /\ byGame' = g
  /\ IF desk THEN sPaused' \in (IF Lossy THEN BOOLEAN ELSE {TRUE}) ELSE UNCHANGED sPaused
  /\ IF vnc THEN vnc' = FALSE /\ rStopped' = TRUE ELSE UNCHANGED <<vnc, rStopped>>

(* The resume (572-592): "pause off", remote-ctl.sh start if the pause stopped it (with no
   desktop it has no remote-capable file: exit 3). *)
ResumeNow ==
  /\ rPaused' = FALSE /\ byGame' = FALSE /\ rStopped' = FALSE
  /\ IF desk THEN sPaused' \in (IF Lossy THEN {FALSE, sPaused} ELSE {FALSE}) ELSE UNCHANGED sPaused
  /\ IF rStopped /\ desk THEN vnc' = TRUE ELSE UNCHANGED vnc

YouPause ==
  /\ ~rPaused /\ PauseNow(FALSE)
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, sMode, manual, uManual, alone, cal, saved, calHide>>

YouResume ==
  /\ rPaused /\ ResumeNow
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, sMode, manual, uManual, alone, cal, saved, calHide>>

(* A game starts or ends; ft-screens' UpdateGame (1168-1175) resets the switch either way. *)
GameStarts ==
  /\ ~game /\ game' = TRUE /\ manual' = FALSE /\ uManual' = FALSE
  /\ IF AutoPause /\ ~rPaused THEN PauseNow(TRUE) ELSE UNCHANGED <<rPaused, byGame, sPaused, vnc, rStopped>>
  /\ UNCHANGED <<fresh, desk, pending, head, dash, sMode, alone, cal, saved, calHide>>

GameEnds ==
  /\ game /\ game' = FALSE /\ manual' = FALSE /\ uManual' = FALSE
  /\ IF rPaused /\ byGame THEN ResumeNow ELSE UNCHANGED <<rPaused, byGame, sPaused, vnc, rStopped>>
  /\ UNCHANGED <<fresh, desk, pending, head, dash, sMode, alone, cal, saved, calHide>>

(* A new ft-screens says where typing goes; while paused the relay sends "pause on" again
   (screens_seen 675-688). Nothing sends "pause off" again. *)
RelaySees ==
  /\ desk /\ fresh /\ fresh' = FALSE
  /\ IF rPaused THEN sPaused' = TRUE ELSE UNCHANGED sPaused
  /\ UNCHANGED <<desk, pending, head, game, dash, rPaused, byGame, sMode, manual, uManual, alone,
                 cal, saved, calHide, vnc, rStopped>>

(* A full calibration starts: "state" gives g_mode, not the mode in effect (vr.cpp 2606, 6254),
   and the switch; gazecheck reads shown = (manual 0 in always, 1 otherwise), then "hide". *)
CalStarts ==
  /\ Cal /\ desk /\ ~cal /\ cal' = TRUE
  /\ IF CalFix THEN calHide' = TRUE /\ UNCHANGED <<manual, saved>>
     ELSE /\ saved' = (IF (sMode = "always") = ~manual THEN "shown" ELSE "hidden")
          /\ manual' = Sw(sMode, FALSE) /\ UNCHANGED calHide
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, rPaused, byGame, sPaused, sMode, uManual, alone, vnc, rStopped>>

(* It closes (1262-1266): "show" if it read the screens as shown. *)
CalCloses ==
  /\ cal /\ cal' = FALSE /\ saved' = "none" /\ calHide' = FALSE
  /\ IF saved = "shown" THEN manual' = Sw(sMode, TRUE) ELSE UNCHANGED manual
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, rPaused, byGame, sPaused, sMode, uManual, alone, vnc, rStopped>>

(* ft-gazed dies mid-calibration (G4 Z1): the calibration is over and nothing shows them again. *)
GazedCrash ==
  /\ GazedDies /\ cal /\ cal' = FALSE /\ saved' = "none"
  /\ UNCHANGED <<fresh, desk, pending, head, game, dash, rPaused, byGame, sPaused, sMode, manual, uManual,
                 alone, calHide, vnc, rStopped>>

Next ==
  \/ DeskStarts \/ DeskStops \/ LayoutStart \/ YouReset \/ HeadChanges \/ YouHideShow \/ DashChanges
  \/ YouPause \/ YouResume \/ GameStarts \/ GameEnds \/ RelaySees \/ CalStarts \/ CalCloses \/ GazedCrash

(* The programs' own steps happen: ft-layout start finishes, the relay re-sends "pause on", a
   calibration closes. Nothing forces the user, the headset, games or crashes. *)
Spec == Init /\ [][Next]_vars /\ WF_vars(LayoutStart) /\ WF_vars(RelaySees) /\ WF_vars(CalCloses)

(* A screen hidden for a reason comes back once the reason is gone (G1 L7 and G4 Z1 are this
   kind): wanted but not shown, it shows, or stops being wanted. *)
HiddenComesBack == [](Wanted /\ ~Visible => <>(Visible \/ ~Wanted))

(* A screen shown against the settings (concealed, the mode, a game) doesn't stay shown. *)
ShownOnlyWanted == [](Visible /\ ~Wanted => <>(~Visible \/ Wanted))

(* Remote access doesn't run through a pause (the pause stops it for the game's sake). *)
VncOffInPause == [](rPaused /\ vnc => <>(~rPaused \/ ~vnc))

(* Remote access the pause stopped comes back with the resume. *)
VncComesBack == [](Remote /\ desk /\ ~rPaused /\ ~vnc => <>(vnc \/ rPaused \/ ~desk))
=============================================================================
