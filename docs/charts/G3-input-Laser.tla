---------------------------- MODULE G3Laser ----------------------------
(* G3 "one owner per hand's laser", read from Frametop at 78e93b1 (pointer/helper/ft-pointer.cpp,
   rayowner.h, handoff.h, pointer/driver_ft_pointer.cpp). Frametop has one laser, the pointer
   driver's virtual controller, with one trigger. Each hand can drive it three ways: its controller
   (SteamVR's own laser then), its hand ray (rayowner.h: one owner, the other hand takes over), or a
   touch on a SteamVR panel (ft-screens' "touch", which poses the laser while it lasts). The mouse
   presses the same trigger ("btn trigger" from the relay), and so do ray pinches (pressLeft).
   The rules: a hand has one owner at a time, and the driver's trigger is down exactly while
   someone holds it, by one holder at a time. *)
EXTENDS Naturals, FiniteSets

CONSTANTS LetGoChecksTouch  \* FALSE: the code. TRUE: the pointer's let-go paths wait for a touch

Sides == {"L", "R"}
None == "none"
Other(s) == IF s = "L" THEN "R" ELSE "L"

VARIABLES
  ctl,        \* [side -> BOOLEAN] that hand holds its controller (handoff.h Owner::Controller)
  near,       \* [side -> BOOLEAN] that hand is near a panel (ft-screens' "near"): no ray, it touches
  ray,        \* the hand ray that drives the pointer (rayowner.h Owner), or none
  touch,      \* the hand on a SteamVR panel (touch.side), or none
  tDown,      \* the touch has the trigger down (touch.down)
  rayHold,    \* a ray pinch holds the trigger (hold.src == Ray, hold.pressed)
  mouseHolds, \* the mouse's button is down and pressed the trigger (leftButton -> pressLeft)
  leftHeld,   \* the helper's one flag for a press it made (pressLeft/releaseLeft)
  active,     \* the helper's pointer is on (the virtual controller connected, its laser ours)
  drv         \* the driver's trigger (state_.buttons[0]; "hide" keeps it)

vars == <<ctl, near, ray, touch, tDown, rayHold, mouseHolds, leftHeld, active, drv>>

TypeOK == /\ ctl \in [Sides -> BOOLEAN] /\ near \in [Sides -> BOOLEAN]
          /\ ray \in Sides \cup {None} /\ touch \in Sides \cup {None}
          /\ {tDown, rayHold, mouseHolds, leftHeld, active, drv} \subseteq BOOLEAN

Init == /\ ctl = [s \in Sides |-> FALSE] /\ near = [s \in Sides |-> FALSE]
        /\ ray = None /\ touch = None /\ tDown = FALSE /\ rayHold = FALSE /\ mouseHolds = FALSE
        /\ leftHeld = FALSE /\ active = FALSE /\ drv = FALSE

\* releasePointer (and the "last used wins", headset-off and no-hand-role releases): "btn a 0",
\* "hide"; the trigger stays as it was
LetGo == active' = FALSE

\* handoff.h: the hand picks up its controller. Its ray leaves at once (!may: rayLeave(controller),
\* which lets the pointer go unless the mouse is in use), its ray hold ends as lost, and ft-screens
\* (in.allowed) ends its touch ("touch end": the trigger up if down).
CtlOn(s) ==
  /\ ~ctl[s] /\ ctl' = [ctl EXCEPT ![s] = TRUE] /\ near' = [near EXCEPT ![s] = FALSE]
  /\ IF ray = s
     THEN /\ ray' = None /\ rayHold' = FALSE
          /\ leftHeld' = (leftHeld /\ ~rayHold)
          /\ IF touch = s THEN touch' = None /\ tDown' = FALSE ELSE UNCHANGED <<touch, tDown>>
          /\ drv' = IF rayHold \/ (touch = s /\ tDown) THEN FALSE ELSE drv
          /\ IF active /\ ~mouseHolds /\ (LetGoChecksTouch => touch \in {None, s}) THEN LetGo ELSE UNCHANGED active
     ELSE /\ UNCHANGED <<ray, rayHold, leftHeld, active>>
          /\ IF touch = s THEN touch' = None /\ tDown' = FALSE /\ drv' = (drv /\ ~tDown)
             ELSE UNCHANGED <<touch, tDown, drv>>
  /\ UNCHANGED mouseHolds
CtlOff(s) ==
  /\ ctl[s] /\ ctl' = [ctl EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<near, ray, touch, tDown, rayHold, mouseHolds, leftHeld, active, drv>>

\* "Last used wins" (a controller moves), the headset off, no hand role: the pointer goes. Not
\* while a ray drives it or holds (hold.src, raySide), but nothing asks about a touch.
ControllerMoves ==
  /\ active /\ ray = None /\ ~rayHold
  /\ LetGoChecksTouch => touch = None
  /\ LetGo
  /\ UNCHANGED <<ctl, near, ray, touch, tDown, rayHold, mouseHolds, leftHeld, drv>>

\* a hand comes near a panel: the ray owner leaves at once (rayLeave, no let-go); it goes away again,
\* and ft-screens ends a touch it had ("touch end": the trigger up if down)
Near(s) ==
  /\ ~ctl[s] /\ ~near[s] /\ near' = [near EXCEPT ![s] = TRUE]
  /\ IF ray = s THEN ray' = None /\ rayHold' = FALSE /\ leftHeld' = (leftHeld /\ ~rayHold) /\ drv' = (drv /\ ~rayHold)
     ELSE UNCHANGED <<ray, rayHold, leftHeld, drv>>
  /\ UNCHANGED <<ctl, touch, tDown, mouseHolds, active>>
Far(s) ==
  /\ near[s] /\ near' = [near EXCEPT ![s] = FALSE]
  /\ IF touch = s THEN touch' = None /\ tDown' = FALSE /\ drv' = (drv /\ ~tDown) ELSE UNCHANGED <<touch, tDown, drv>>
  /\ UNCHANGED <<ctl, ray, rayHold, mouseHolds, leftHeld, active>>

\* rayowner.h: a hand's ray takes the pointer (its own raised, a pinch, the grace over); rayEnter wakes it
RayTakes(s) ==
  /\ ~ctl[s] /\ ~near[s] /\ ray # s /\ ~rayHold
  /\ ray' = s /\ active' = TRUE
  /\ UNCHANGED <<ctl, near, touch, tDown, rayHold, mouseHolds, leftHeld, drv>>
\* the owner's hand isn't tracked any more (an "off": rayLeave(true) lets the pointer go unless the
\* mouse is in use), its hold ends as lost
RayLost ==
  /\ ray # None /\ ray' = None /\ rayHold' = FALSE
  /\ leftHeld' = (leftHeld /\ ~rayHold) /\ drv' = (drv /\ ~rayHold)
  /\ IF active /\ ~mouseHolds /\ (LetGoChecksTouch => touch = None) THEN LetGo ELSE UNCHANGED active
  /\ UNCHANGED <<ctl, near, touch, tDown, mouseHolds>>
\* beginRayHold checks hold, leftHeld and the gaze's holds; its press needs the pose (aimSent),
\* which waits while a touch has the laser
RayPress ==
  /\ ray # None /\ active /\ ~rayHold /\ ~leftHeld /\ touch = None
  /\ rayHold' = TRUE /\ leftHeld' = TRUE /\ drv' = TRUE
  /\ UNCHANGED <<ctl, near, ray, touch, tDown, mouseHolds, active>>
RayRelease ==
  /\ rayHold /\ rayHold' = FALSE
  /\ leftHeld' = FALSE /\ drv' = IF leftHeld THEN FALSE ELSE drv  \* endHold: if (leftHeld) releaseLeft()
  /\ UNCHANGED <<ctl, near, ray, touch, tDown, mouseHolds, active>>

\* ft-screens' touch: none starts while the other hand's is down, or a ray or the mouse holds
TouchStarts(s) ==
  /\ near[s] /\ ~ctl[s] /\ touch # s
  /\ IF touch = None THEN ~rayHold /\ ~leftHeld ELSE ~tDown
  /\ touch' = s /\ tDown' = FALSE /\ active' = TRUE  \* wake (or claim the laser back)
  /\ UNCHANGED <<ctl, near, ray, rayHold, mouseHolds, leftHeld, drv>>
TouchPress ==
  /\ touch # None /\ active /\ ~tDown /\ tDown' = TRUE /\ drv' = TRUE
  /\ UNCHANGED <<ctl, near, ray, touch, rayHold, mouseHolds, leftHeld, active>>
TouchUp ==
  /\ tDown /\ tDown' = FALSE /\ drv' = FALSE
  /\ UNCHANGED <<ctl, near, ray, touch, rayHold, mouseHolds, leftHeld, active>>
\* the main loop, line 2378: "if (!active && touch.side >= 0) touch = Touch{};" (no "btn trigger 0")
TouchForgotten ==
  /\ ~active /\ touch # None /\ touch' = None /\ tDown' = FALSE
  /\ UNCHANGED <<ctl, near, ray, rayHold, mouseHolds, leftHeld, active, drv>>

\* the relay's "btn trigger 1|0" (a mouse button in pointer mode): it wakes the pointer, and
\* leftButton presses (pressLeft) and releases (releaseLeft) with nothing else asked
MousePress ==
  /\ ~mouseHolds /\ mouseHolds' = TRUE /\ leftHeld' = TRUE /\ drv' = TRUE /\ active' = TRUE
  /\ UNCHANGED <<ctl, near, ray, touch, tDown, rayHold>>
MouseRelease ==
  /\ mouseHolds /\ mouseHolds' = FALSE /\ leftHeld' = FALSE /\ drv' = IF leftHeld THEN FALSE ELSE drv
  /\ UNCHANGED <<ctl, near, ray, touch, tDown, rayHold, active>>

Next == \/ \E s \in Sides : CtlOn(s) \/ CtlOff(s) \/ Near(s) \/ Far(s) \/ RayTakes(s) \/ TouchStarts(s)
        \/ ControllerMoves \/ RayLost \/ RayPress \/ RayRelease \/ TouchPress \/ TouchUp \/ TouchForgotten
        \/ MousePress \/ MouseRelease

Spec == Init /\ [][Next]_vars

\* a hand drives the laser one way at a time: its controller, its ray, or its touch
OneOwnerPerHand == \A s \in Sides : ~(ctl[s] /\ (ray = s \/ touch = s)) /\ ~(ray = s /\ touch = s)
\* one press at a time on the one trigger
Holders == {h \in {"touch", "ray", "mouse"} : (h = "touch" /\ tDown) \/ (h = "ray" /\ rayHold) \/ (h = "mouse" /\ mouseHolds)}
OneTriggerHolder == Cardinality(Holders) <= 1
\* the trigger is down while someone holds it, and only then
HolderHasTrigger == Holders # {} => drv
NoOrphanTrigger == drv => Holders # {}
=============================================================================
