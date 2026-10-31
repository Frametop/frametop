---------------------------- MODULE G3Keys ----------------------------
(* G3 "held keys come up where they went down" (the DEE-222 class), read from Frametop at 78e93b1.
   One key or button on one device, followed from the hand to every place that can hold it down:
     seat       ft-screens' own keyboard state (key_held), which routes a release  compositor.c 817-845
     kwin       KWin, ft-screens' client, has it down                             compositor.c 826-861
     lost       a release ft-screens owes KWin until a surface has focus      screens/held-keys.h, compositor.c 825-861
     remote     a remote display's keyboard (ft_remote_key)                   compositor.c 966-969, 533-537
     frunner    Frunner's search (spot_send)                                  compositor.c 959-964
     vdev       the relay's virtual keyboard or mouse (uinput, kept in the
                systemd fd store across relay restarts)                       input/input-relay.py 473-505
     drv        the pointer driver's button (via the helper, "btn trigger")   input-relay.py 867-893
   Kind:
     "key"    a local pass-through keyboard, a key that isn't a modifier   input-relay.py 2163-2176, 1726-1758
     "mod"    the same, a modifier (a new relay lets go of those at start) input-relay.py 2042-2050
     "host"   a host's keyboard over the network (remote displays' input) input-relay.py 2171-2176
     "button" a pointer-role mouse button (left)                          input-relay.py 2178-2194
   Restarts: the relay may die and come back (systemd restarts it). Toggles: pause and POINTER=0/1. *)
EXTENDS Naturals

CONSTANTS Kind, Restarts, Toggles

VARIABLES
  phys,        \* the key is physically down
  relayUp,     \* input-relay.py runs
  grabbed,     \* the relay holds the device's grab (EVIOCGRAB): only it sees the key
  typing,      \* typing goes to the desktop (desktop_until; ft-screens' keys_desktop)
  screensDown, \* the relay told ft-screens the key went down (screens_down)
  seat, kwin, lost, \* ft-screens' keyboard has it down, KWin has it down, ft-screens owes KWin its release
  focus,       \* a desktop surface has keyboard focus
  remote, remoteHeld,   \* a remote display has the typing (key_remote), and has the key down
  frunner, frunnerHeld, \* Frunner is open, and has the key down
  vdev, nodeHeld,       \* the virtual device has it down, the relay's node.held says so
  drv, drvDown,         \* the driver button is down, the relay's Pointer.driver_down says so
  paused, pointerOn     \* Frametop is paused, POINTER=1 (pointer_conf)

vars == <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld,
          frunner, frunnerHeld, vdev, nodeHeld, drv, drvDown, paused, pointerOn>>

Keyboard == Kind \in {"key", "mod", "host"}
\* the relay reads the device: a pass-through keyboard only while grabbed (else Steam reads it raw)
Reaches == relayUp /\ (Kind \in {"key", "mod"} => grabbed)
Active == pointerOn /\ ~paused  \* state["pointer"]

TypeOK == \A v \in {phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld,
                    frunner, frunnerHeld, vdev, nodeHeld, drv, drvDown, paused, pointerOn} : v \in BOOLEAN

Init == /\ phys = FALSE /\ relayUp = TRUE /\ typing \in BOOLEAN /\ focus \in BOOLEAN
        /\ grabbed = (IF Kind \in {"key", "mod"} THEN typing ELSE Kind = "button")
        /\ screensDown = FALSE /\ seat = FALSE /\ kwin = FALSE /\ lost = FALSE
        /\ remote = FALSE /\ remoteHeld = FALSE /\ frunner = FALSE /\ frunnerHeld = FALSE
        /\ vdev = FALSE /\ nodeHeld = FALSE /\ drv = FALSE /\ drvDown = FALSE
        /\ paused = FALSE /\ pointerOn \in (IF Kind = "button" THEN BOOLEAN ELSE {FALSE})

\* ft-screens' handle_key for a press: Frunner, else the remote, else the desktop if it types there
\* and a screen has focus ("ok typing goes to Steam", "ok no focus"); send_key ignores a second press
ScreensPress ==
  IF frunner THEN /\ frunnerHeld' = TRUE /\ UNCHANGED <<seat, kwin, remoteHeld>>
  ELSE IF remote THEN /\ remoteHeld' = TRUE /\ UNCHANGED <<seat, kwin, frunnerHeld>>
  ELSE IF focus /\ typing THEN /\ seat' = TRUE /\ kwin' = TRUE /\ UNCHANGED <<remoteHeld, frunnerHeld>>
  ELSE UNCHANGED <<seat, kwin, remoteHeld, frunnerHeld>>
\* and a release: the desktop's own key (key_held) always goes to the desktop; send_key lets go of
\* it on the seat, and with no screen focused KWin gets it with the next focus (ft_lost_keys_add)
ScreensRelease ==
  IF frunner /\ ~seat THEN /\ frunnerHeld' = FALSE /\ UNCHANGED <<seat, kwin, lost, remoteHeld>>
  ELSE IF remote /\ ~seat THEN /\ remoteHeld' = FALSE /\ UNCHANGED <<seat, kwin, lost, frunnerHeld>>
  ELSE IF seat /\ focus THEN /\ seat' = FALSE /\ kwin' = FALSE /\ UNCHANGED <<lost, remoteHeld, frunnerHeld>>
  ELSE IF seat THEN /\ seat' = FALSE /\ lost' = TRUE /\ UNCHANGED <<kwin, remoteHeld, frunnerHeld>>
  ELSE UNCHANGED <<seat, kwin, lost, remoteHeld, frunnerHeld>>
ScreensSame == UNCHANGED <<seat, kwin, lost, remoteHeld, frunnerHeld>>

Press ==
  /\ ~phys /\ phys' = TRUE
  /\ IF ~Reaches THEN UNCHANGED <<screensDown, seat, kwin, lost, remoteHeld, frunnerHeld, vdev, nodeHeld, drv, drvDown>>
     ELSE IF Keyboard THEN
       /\ screensDown' = TRUE /\ ScreensPress /\ UNCHANGED <<lost, drv, drvDown>>
       \* a host's key also goes to Steam, by the virtual keyboard, while typing doesn't go to the desktop
       /\ IF Kind = "host" /\ ~typing THEN vdev' = TRUE /\ nodeHeld' = TRUE ELSE UNCHANGED <<vdev, nodeHeld>>
     ELSE IF Active THEN  \* pointer mode: the helper's "btn trigger 1"
       /\ drv' = TRUE /\ drvDown' = TRUE /\ UNCHANGED <<screensDown, vdev, nodeHeld>> /\ ScreensSame
     ELSE  \* paused or POINTER=0: the virtual mouse
       /\ vdev' = TRUE /\ nodeHeld' = TRUE /\ UNCHANGED <<screensDown, drv, drvDown>> /\ ScreensSame
  /\ UNCHANGED <<relayUp, grabbed, typing, focus, remote, frunner, paused, pointerOn>>

Release ==
  /\ phys /\ phys' = FALSE
  /\ IF ~Reaches THEN UNCHANGED <<screensDown, seat, kwin, lost, remoteHeld, frunnerHeld, vdev, nodeHeld, drv, drvDown>>
     ELSE IF Keyboard THEN
       /\ screensDown' = FALSE /\ ScreensRelease /\ UNCHANGED <<drv, drvDown>>
       /\ IF Kind = "host" /\ nodeHeld THEN vdev' = FALSE /\ nodeHeld' = FALSE ELSE UNCHANGED <<vdev, nodeHeld>>
     ELSE IF Active THEN  \* do_action(.., 0): "btn trigger 0", and the relay's loop goes on (continue)
       /\ drv' = FALSE /\ drvDown' = FALSE /\ UNCHANGED <<screensDown, vdev, nodeHeld>> /\ ScreensSame
     ELSE
       /\ vdev' = FALSE /\ nodeHeld' = FALSE /\ UNCHANGED <<screensDown, drv, drvDown>> /\ ScreensSame
  /\ UNCHANGED <<relayUp, grabbed, typing, focus, remote, frunner, paused, pointerOn>>

\* reconcile_desktop_keys, every second: a key the desktop has down that no device holds comes up
Reconcile ==
  /\ relayUp /\ Keyboard /\ screensDown /\ ~phys
  /\ screensDown' = FALSE /\ ScreensRelease
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, focus, remote, frunner, vdev, nodeHeld, drv, drvDown, paused, pointerOn>>

\* typing moves between the desktop and Steam (a click, a panel, a pause); the grab follows in ApplyRoles
TypingFlip ==
  /\ Keyboard /\ typing' = ~typing
  /\ UNCHANGED <<phys, relayUp, grabbed, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>
\* apply_roles: a pass-through keyboard's grab waits for its keys to come up
ApplyRoles ==
  /\ relayUp /\ Kind \in {"key", "mod"} /\ grabbed # typing /\ ~phys
  /\ grabbed' = typing
  /\ UNCHANGED <<phys, relayUp, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>

FocusLeave ==
  /\ Keyboard /\ focus /\ focus' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, remote, remoteHeld, frunner, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>
\* keyboard_enter sends the releases owed (ft_lost_keys_take)
FocusEnter ==
  /\ Keyboard /\ ~focus /\ focus' = TRUE
  /\ IF lost THEN kwin' = FALSE /\ lost' = FALSE ELSE UNCHANGED <<kwin, lost>>  \* (seat is up: no press without focus)
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, remote, remoteHeld, frunner, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>
\* a click on a remote display: it takes the typing, Frunner goes (spot_dismiss)
RemoteOn ==
  /\ Keyboard /\ ~remote /\ remote' = TRUE /\ frunner' = FALSE /\ frunnerHeld' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remoteHeld, vdev, nodeHeld,
                 drv, drvDown, paused, pointerOn>>
\* a click elsewhere: set_key_remote(-1) blurs it, and ft_remote_blur lets go of its keys
RemoteOff ==
  /\ Keyboard /\ remote /\ remote' = FALSE /\ remoteHeld' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, frunner, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>
\* Frunner opens (it blurs the remote) and closes (its window, and what it held, go)
FrunnerOpen ==
  /\ Keyboard /\ ~frunner /\ frunner' = TRUE /\ remote' = FALSE /\ remoteHeld' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, frunnerHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>
FrunnerClose ==
  /\ Keyboard /\ frunner /\ frunner' = FALSE /\ frunnerHeld' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld,
                 vdev, nodeHeld, drv, drvDown, paused, pointerOn>>

\* pause_changed: on pause, stand_down lets go of the driver buttons and the pointer goes (None)
Pause ==
  /\ Toggles /\ Kind = "button" /\ ~paused /\ paused' = TRUE
  /\ IF pointerOn THEN drv' = FALSE /\ drvDown' = FALSE ELSE UNCHANGED <<drv, drvDown>>
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner,
                 frunnerHeld, vdev, nodeHeld, pointerOn>>
Resume ==
  /\ Toggles /\ Kind = "button" /\ paused /\ paused' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner,
                 frunnerHeld, vdev, nodeHeld, drv, drvDown, pointerOn>>
\* load_config with POINTER=0 drops pointer_conf (no stand_down); POINTER=1 makes a new Pointer
PointerOff ==
  /\ Toggles /\ Kind = "button" /\ pointerOn /\ pointerOn' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner,
                 frunnerHeld, vdev, nodeHeld, drv, drvDown, paused>>
PointerOn ==
  /\ Toggles /\ Kind = "button" /\ ~pointerOn /\ pointerOn' = TRUE /\ drvDown' = FALSE
  /\ UNCHANGED <<phys, relayUp, grabbed, typing, screensDown, seat, kwin, lost, focus, remote, remoteHeld, frunner,
                 frunnerHeld, vdev, nodeHeld, drv, paused>>

\* the relay dies: its grabs go with it and it forgets screens_down, node.held and driver_down.
\* ft-screens (a datagram socket) never hears; the uinput devices stay in the fd store.
RelayDies ==
  /\ Restarts /\ relayUp /\ relayUp' = FALSE /\ grabbed' = FALSE
  /\ screensDown' = FALSE /\ nodeHeld' = FALSE /\ drvDown' = FALSE
  /\ UNCHANGED <<phys, typing, seat, kwin, lost, focus, remote, remoteHeld, frunner, frunnerHeld, vdev, drv, paused, pointerOn>>
\* a new relay: modifiers (and mouse buttons, on ft-screens' screens) come up on the desktop.
\* A pointer device is grabbed at once; a pass-through keyboard waits for ApplyRoles.
RelayStarts ==
  /\ Restarts /\ ~relayUp /\ relayUp' = TRUE
  /\ grabbed' = (Kind = "button")
  /\ IF Kind = "mod" THEN ScreensRelease ELSE ScreensSame
  /\ UNCHANGED <<phys, typing, screensDown, focus, remote, frunner, frunnerHeld, vdev, nodeHeld, drv, drvDown,
                 paused, pointerOn>>

Next == Press \/ Release \/ Reconcile \/ TypingFlip \/ ApplyRoles \/ FocusLeave \/ FocusEnter \/ RemoteOn
        \/ RemoteOff \/ FrunnerOpen \/ FrunnerClose \/ Pause \/ Resume \/ PointerOff \/ PointerOn
        \/ RelayDies \/ RelayStarts

Spec == Init /\ [][Next]_vars

\* The key is up, the relay runs and has nothing left to reconcile: then nothing has it down,
\* save a release ft-screens owes KWin until it has focus again (DEE-222).
Settled == ~phys /\ relayUp /\ ~screensDown
DesktopComesUp == Settled => (~seat /\ (~kwin \/ lost) /\ ~remoteHeld /\ ~frunnerHeld)
VirtualComesUp == Settled => ~vdev
DriverComesUp  == Settled => ~drv
=============================================================================
