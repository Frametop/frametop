---- MODULE HostInputLease ----
\* ft-stream's host-input lease with a real wearer (stream/ft-stream.cpp HostInput::Tick, Apply,
\* Stop, TakeAnswers; stream/hostinput.h Decide, GivenAt), one stream to one input host, and the host's
\* side as docs/host-input.md describes it (engage, release lifts the escape, drop keeps it,
\* the lease runs out 1.5 s after the last). Time is one step, Age: 3 s pass.
EXTENDS Naturals, Sequences

CONSTANT MaxInbox   \* answers that can wait unread

VARIABLES
    worn,       \* the headset is on someone's head
    switchFile, \* "input": true on the host's layout entry (Remote Displays' switch)
    proc,       \* ft-stream for the host: "none", "pre" (not live yet), "live"
    switchOn,   \* the switch as ft-stream last read it (every 3 s)
    locked,     \* ft-stream holds frametop-input-HOST.lock
    asked,      \* a "gaze ? headset" is out, its answer not given yet
    inbox,      \* answers given, not read: a sequence of [w |-> worn then, old |-> older than 3 s]
    seen,       \* ft-stream's last answer: "none", "worn", "away"
    fresh,      \* ... given less than 3 s ago (hostinput::Fresh, GivenAt: the kernel's send time)
    basisOld,   \* ... was older than 3 s when it was read
    last,       \* what Tick last wanted: "hold", "drop", "release"
    owed,       \* releaseOwed
    leasing,    \* ft-stream sends engage leases
    escapedSaid,\* ft-stream heard the host's escape (HostInput::escaped)
    engaged,    \* the host lends its keyboard and mouse to the Frame
    esc,        \* the host's escape hotkey was pressed, not lifted yet
    offLong,    \* since the escape, the headset was off for 3 s or more
    cleared     \* since the escape, the headset came off (3 s+) and went on again

vars == <<worn, switchFile, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed,
          leasing, escapedSaid, engaged, esc, offLong, cleared>>

Init ==
    /\ worn = TRUE /\ switchFile = TRUE
    /\ proc = "none" /\ switchOn = FALSE /\ locked = FALSE /\ asked = FALSE /\ inbox = <<>>
    /\ seen = "none" /\ fresh = FALSE /\ basisOld = FALSE
    /\ last = "release" /\ owed = FALSE /\ leasing = FALSE /\ escapedSaid = FALSE
    /\ engaged = FALSE /\ esc = FALSE /\ offLong = FALSE /\ cleared = FALSE

host == <<engaged, esc, offLong, cleared>>

Decide(sw, live, s, f) ==
    IF ~sw THEN "release"
    ELSE IF ~live THEN "drop"
    ELSE IF f /\ s = "away" THEN "release"
    ELSE IF ~f THEN "drop"
    ELSE "hold"

\* --- the person ---
HeadsetOff == worn /\ worn' = FALSE /\ UNCHANGED <<switchFile, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing, escapedSaid, host>>
HeadsetOn == ~worn /\ worn' = TRUE /\ cleared' = (cleared \/ (esc /\ offLong))
    /\ UNCHANGED <<switchFile, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing, escapedSaid, engaged, esc, offLong>>
SwitchOff == switchFile /\ switchFile' = FALSE /\ UNCHANGED <<worn, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing, escapedSaid, host>>
SwitchOn == ~switchFile /\ switchFile' = TRUE /\ UNCHANGED <<worn, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing, escapedSaid, host>>
\* someone at the computer presses the escape hotkey: the host takes its input back
HostEscape == engaged /\ engaged' = FALSE /\ esc' = TRUE /\ offLong' = FALSE /\ cleared' = FALSE
    /\ escapedSaid' = (proc # "none")
    /\ UNCHANGED <<worn, switchFile, proc, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing>>

\* --- time ---
\* 3 s can pass only once ft-stream has caught up: it ticks every 50 ms, asks every second and
\* reads the switch every 3 s, and the helper answers at once. So a live stream with the switch
\* on has read an answer given since the headset last changed.
CaughtUp == /\ ~asked
            /\ (proc # "none" => switchOn = switchFile)
            /\ (proc = "live" /\ switchOn) => (inbox = <<>> /\ fresh /\ seen = IF worn THEN "worn" ELSE "away")
Age == /\ CaughtUp
       /\ fresh' = FALSE
       /\ inbox' = [i \in DOMAIN inbox |-> [inbox[i] EXCEPT !.old = TRUE]]
       /\ offLong' = (offLong \/ (esc /\ ~worn))
       \* the host lets go 1.5 s after the last lease
       /\ engaged' = (engaged /\ proc # "none" /\ leasing)
       /\ UNCHANGED <<worn, switchFile, proc, switchOn, locked, asked, seen, basisOld, last, owed, leasing, escapedSaid, esc, cleared>>

\* --- the pointer helper ---
HelperAnswers == asked /\ Len(inbox) < MaxInbox /\ asked' = FALSE
    /\ inbox' = Append(inbox, [w |-> worn, old |-> FALSE])
    /\ UNCHANGED <<worn, switchFile, proc, switchOn, locked, seen, fresh, basisOld, last, owed, leasing, escapedSaid, host>>

\* --- ft-stream's lifecycle (ft-screens starts it; it goes live once, and ends any time) ---
\* ft-screens starts it 2 s or more after the last one went: the host's lease (1.5 s) ran out by then
StreamStarts == proc = "none" /\ ~engaged /\ proc' = "pre" /\ switchOn' = switchFile
    /\ locked' = FALSE /\ asked' = FALSE /\ inbox' = <<>> /\ seen' = "none" /\ fresh' = FALSE /\ basisOld' = FALSE
    /\ last' = "release" /\ owed' = FALSE /\ leasing' = FALSE /\ escapedSaid' = FALSE
    /\ UNCHANGED <<worn, switchFile, host>>
StreamLive == proc = "pre" /\ proc' = "live"
    /\ UNCHANGED <<worn, switchFile, switchOn, locked, asked, inbox, seen, fresh, basisOld, last, owed, leasing, escapedSaid, host>>
\* the connection ends (or ft-screens stops it): Stop(kDrop) on the way out, the lock goes with it
StreamEnds == proc # "none" /\ proc' = "none" /\ locked' = FALSE /\ leasing' = FALSE
    /\ engaged' = (engaged /\ ~leasing)
    /\ UNCHANGED <<worn, switchFile, switchOn, asked, inbox, seen, fresh, basisOld, last, owed, escapedSaid, esc, offLong, cleared>>

\* Apply(want) then the engage, as one Tick. s, f: the answer it decides on.
\* Send(release) lifts the escape and pays what's owed; Send(drop) keeps the escape.
TickWith(want, s, f) ==
    LET owed1 == owed \/ (want = "release" /\ last # "release")
        stops == want # "hold" /\ (leasing \/ (want = "release" /\ (escapedSaid \/ owed1)))
        relNow == (stops /\ want = "release") \/ (want = "hold" /\ owed1)
        owed2 == owed1 /\ ~relNow
        engage == want = "hold" /\ ~owed2
    IN /\ last' = want
       /\ owed' = owed2
       /\ leasing' = IF want # "hold" THEN (leasing /\ ~stops) ELSE (leasing \/ engage)
       /\ escapedSaid' = (escapedSaid /\ ~relNow)
       /\ esc' = (esc /\ ~relNow)
       /\ engaged' = IF engage THEN ~(esc /\ ~relNow) ELSE (engaged /\ ~stops /\ ~relNow)

\* Tick while the switch is off or the stream isn't live: Apply(Decide), Unlock
TickIdle == proc # "none" /\ (~switchOn \/ proc = "pre")
    /\ TickWith(Decide(switchOn, proc = "live", seen, fresh), seen, fresh)
    /\ locked' = FALSE
    /\ UNCHANGED <<worn, switchFile, proc, switchOn, asked, inbox, seen, fresh, basisOld, offLong, cleared>>

\* Tick that reads the switch (every 3 s): off releases in the same Tick; on waits for TickLive
ReadSwitch == proc # "none" /\ switchOn # switchFile /\ switchOn' = switchFile
    /\ (IF switchFile THEN UNCHANGED <<locked, last, owed, leasing, escapedSaid, engaged, esc>>
        ELSE TickWith("release", seen, fresh) /\ locked' = FALSE)
    /\ UNCHANGED <<worn, switchFile, proc, asked, inbox, seen, fresh, basisOld, offLong, cleared>>

\* Tick holding the lock: ask (every second), read every answer that came, decide, lease
TickLive == proc = "live" /\ switchOn
    /\ locked' = TRUE
    /\ asked' = TRUE
    /\ LET n == Len(inbox)
           s == IF n > 0 THEN (IF inbox[n].w THEN "worn" ELSE "away") ELSE seen
           \* an answer counts from when it was given: one that waited 3 s or more is stale
           f == IF n > 0 THEN ~inbox[n].old ELSE fresh
       IN /\ seen' = s /\ fresh' = f
          /\ basisOld' = IF n > 0 THEN inbox[n].old ELSE basisOld
          /\ inbox' = <<>>
          /\ TickWith(Decide(TRUE, TRUE, s, f), s, f)
    /\ UNCHANGED <<worn, switchFile, proc, switchOn, offLong, cleared>>

Next == HeadsetOff \/ HeadsetOn \/ SwitchOff \/ SwitchOn \/ HostEscape \/ Age \/ HelperAnswers
     \/ StreamStarts \/ StreamLive \/ StreamEnds \/ ReadSwitch \/ TickIdle \/ TickLive

Spec == Init /\ [][Next]_vars

\* Rules
TypeOK == /\ proc \in {"none", "pre", "live"} /\ seen \in {"none", "worn", "away"}
          /\ last \in {"hold", "drop", "release"} /\ Len(inbox) <= MaxInbox
          /\ (engaged => ~esc)
\* Away means given back: once ft-stream has read a fresh "away", the host has its input.
AwayGivesBack == (proc = "live" /\ locked /\ fresh /\ seen = "away") => ~engaged
\* No lease on an old answer: with nobody wearing the headset, the host lends its input only on
\* an answer that was less than 3 s old when ft-stream read it.
NoStaleLease == (engaged /\ ~worn) => ~basisOld
\* Off and on lifts the escape: once the headset was off 3 s or more and went on again, the
\* Frame doesn't keep leasing a host that is still escaped.
OffOnLiftsEscape == ~(cleared /\ esc /\ leasing /\ proc = "live" /\ worn)
====
