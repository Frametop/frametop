---- MODULE G6RemoteExit ----
\* G6: RemoteStreams (verify-remote-streams, 78e93b1) with ft-stream's way out split in two, and a
\* way out that can hang. Copied, not changed in place: the skill's model stays as it is.
\*   stream/ft-stream.cpp Stream(), after the loop: g_mic.Stop, g_hostin.Stop(kDrop), LiStopConnection
\*   ("stopping": ft-screens still has "live"), then "state lost", dec.Close and, for "monitor",
\*   Disconnect Monitor ("releasing"). Either part may not return ("hung").
\*   screens/remote.c: stop_stream sets quit_at; ft_remote_tick SIGKILLs a pid FT_REMOTE_QUIT_MS
\*   (20 s) after quit_at (e15b65f), and only then; leaving() is pid != 0 and (fd < 0 or "lost").
\* With the R2 and R3 fixes (DEE-259, DEE-260; R23Fix): ft-stream says "state lost" before
\* LiStopConnection, and ft-screens sets quit_at when it hears "lost" (or the socket closes), so a
\* way out that hangs gets the SIGKILL too. ft-stream's own 25 s exit watchdog (R4, DEE-261) isn't
\* modelled: it only matters once ft-screens is gone.
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Slots, Settings,
          KillAfterQuit,  \* TRUE at 78e93b1 (e15b65f); FALSE: as before it, SIGTERM only
          R23Fix          \* TRUE with the R2 and R3 fixes; FALSE: the code at 78e93b1

VARIABLES
    used,     \* [slot -> the slot is display N's (r->used)]
    set,      \* [slot -> the settings it was started with]
    restart,  \* [slot -> a start is due (r->restart_at # 0)]
    proc,     \* [slot -> "none", "starting", "live", "stopping", "releasing", "hungLive", "hungLost", "exited"]
    said,     \* [slot -> r->state: "queued", "starting", "live", "lost"]
    quit,     \* [slot -> it was asked to end, its socket is closed (fd < 0)]
    clock     \* [slot -> r->quit_at # 0: a SIGKILL is coming FT_REMOTE_QUIT_MS after it]

vars == <<used, set, restart, proc, said, quit, clock>>

Running(s) == proc[s] \in {"starting", "live"}
Hung(s) == proc[s] \in {"hungLive", "hungLost"}
Mine == {s \in Slots : used[s]}

Init == /\ used = [s \in Slots |-> FALSE] /\ set = [s \in Slots |-> CHOOSE v \in Settings : TRUE]
        /\ restart = [s \in Slots |-> FALSE] /\ proc = [s \in Slots |-> "none"]
        /\ said = [s \in Slots |-> "lost"] /\ quit = [s \in Slots |-> FALSE]
        /\ clock = [s \in Slots |-> FALSE]

\* stop_stream: "quit", disconnect (fd -1), SIGTERM, quit_at (if it has a pid)
Quit(s) == /\ proc' = [proc EXCEPT ![s] = IF Running(s) THEN "stopping" ELSE proc[s]]
           /\ quit' = [quit EXCEPT ![s] = proc[s] # "none"]
           /\ clock' = [clock EXCEPT ![s] = clock[s] \/ proc[s] # "none"]

YouStart(v) ==
    \/ /\ Mine = {}
       /\ \E s \in Slots :
            /\ ~used[s] /\ proc[s] = "none"
            /\ used' = [used EXCEPT ![s] = TRUE] /\ set' = [set EXCEPT ![s] = v]
            /\ restart' = [restart EXCEPT ![s] = TRUE] /\ said' = [said EXCEPT ![s] = "queued"]
            /\ UNCHANGED <<proc, quit, clock>>
    \/ \E s \in Mine :
            /\ set[s] # v
            /\ Quit(s) /\ set' = [set EXCEPT ![s] = v]
            /\ restart' = [restart EXCEPT ![s] = TRUE] /\ said' = [said EXCEPT ![s] = "queued"]
            /\ UNCHANGED used

YouStop == \E s \in Mine :
    /\ Quit(s) /\ used' = [used EXCEPT ![s] = FALSE]
    /\ restart' = [restart EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<set, said>>

\* leaving(): a pid, and its socket closed or it said "lost"
Leaving(o) == proc[o] # "none" /\ (quit[o] \/ ~used[o] \/ said[o] = "lost" \/ proc[o] = "exited")
HostBusy(s) == \E o \in Slots \ {s} : (used[o] /\ proc[o] # "none" /\ said[o] = "starting") \/ Leaving(o)

TickStarts == \E s \in Slots :
    /\ used[s] /\ restart[s] /\ proc[s] = "none" /\ ~HostBusy(s)
    /\ proc' = [proc EXCEPT ![s] = "starting"] /\ restart' = [restart EXCEPT ![s] = FALSE]
    /\ said' = [said EXCEPT ![s] = "starting"] /\ quit' = [quit EXCEPT ![s] = FALSE]
    /\ clock' = [clock EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<used, set>>

StreamLive == \E s \in Slots :
    /\ proc[s] = "starting"
    /\ proc' = [proc EXCEPT ![s] = "live"]
    /\ said' = IF used[s] /\ ~quit[s] THEN [said EXCEPT ![s] = "live"] ELSE said
    /\ UNCHANGED <<used, set, restart, quit, clock>>

\* a 404, can't connect: "state lost ...", exit 1 at once
CantConnect == \E s \in Slots :
    /\ proc[s] = "starting"
    /\ proc' = [proc EXCEPT ![s] = "exited"]
    /\ said' = IF quit[s] THEN said ELSE [said EXCEPT ![s] = "lost"]
    /\ clock' = IF R23Fix THEN [clock EXCEPT ![s] = TRUE] ELSE clock
    /\ UNCHANGED <<used, set, restart, quit>>

\* the host or the network ends it (connectionTerminated): out of the loop, into LiStopConnection.
\* At 78e93b1 nothing is sent yet: ft-screens still has "live". With R23Fix, "state lost" goes
\* first, and ft-screens starts the SIGKILL clock.
StreamEnds == \E s \in Slots :
    /\ proc[s] = "live"
    /\ proc' = [proc EXCEPT ![s] = "stopping"]
    /\ said' = IF R23Fix /\ ~quit[s] THEN [said EXCEPT ![s] = "lost"] ELSE said
    /\ clock' = IF R23Fix THEN [clock EXCEPT ![s] = TRUE] ELSE clock
    /\ UNCHANGED <<used, set, restart, quit>>

\* LiStopConnection returned: "state lost" (at 78e93b1; with R23Fix it went before), then
\* dec.Close and Disconnect Monitor
Stopped == \E s \in Slots :
    /\ proc[s] = "stopping"
    /\ proc' = [proc EXCEPT ![s] = "releasing"]
    /\ said' = IF quit[s] THEN said ELSE [said EXCEPT ![s] = "lost"]
    /\ clock' = IF R23Fix THEN [clock EXCEPT ![s] = TRUE] ELSE clock
    /\ UNCHANGED <<used, set, restart, quit>>

ReleaseDone == \E s \in Slots :
    /\ proc[s] = "releasing"
    /\ proc' = [proc EXCEPT ![s] = "exited"]
    /\ UNCHANGED <<used, set, restart, said, quit, clock>>

\* a part of the way out never returns (a stuck LiStopConnection, decoder close or HTTP call)
Hang == \E s \in Slots :
    /\ proc[s] \in {"stopping", "releasing"}
    /\ proc' = [proc EXCEPT ![s] = IF proc[s] = "stopping" THEN "hungLive" ELSE "hungLost"]
    /\ UNCHANGED <<used, set, restart, said, quit, clock>>

\* ft_remote_tick: 20 s after quit_at, SIGKILL (e15b65f). At 78e93b1 only a stream that was
\* asked to end has quit_at; with R23Fix, one that said "lost" too.
Kill == KillAfterQuit /\ \E s \in Slots :
    /\ clock[s] /\ proc[s] \notin {"none", "exited"}
    /\ proc' = [proc EXCEPT ![s] = "exited"]
    /\ UNCHANGED <<used, set, restart, said, quit, clock>>

Reaped == \E s \in Slots :
    /\ proc[s] = "exited"
    /\ proc' = [proc EXCEPT ![s] = "none"] /\ quit' = [quit EXCEPT ![s] = FALSE]
    /\ clock' = [clock EXCEPT ![s] = FALSE]
    /\ restart' = IF used[s] /\ ~restart[s] THEN [restart EXCEPT ![s] = TRUE] ELSE restart
    /\ UNCHANGED <<used, set, said>>

Env == (\E v \in Settings : YouStart(v)) \/ YouStop \/ CantConnect \/ StreamEnds \/ Hang
Sys == TickStarts \/ StreamLive \/ Stopped \/ ReleaseDone \/ Kill \/ Reaped
Next == Env \/ Sys

Perms == Permutations(Slots) \cup Permutations(Settings)

\* Fairness for the code's own steps only: you, the host and a hang get none.
Spec == Init /\ [][Next]_vars /\ WF_vars(TickStarts) /\ WF_vars(Stopped) /\ WF_vars(ReleaseDone)
        /\ WF_vars(Kill) /\ WF_vars(Reaped)

\* Rules of RemoteStreams, unchanged
TypeOK == Cardinality(Mine) <= 1
OneStreamPerDisplay == Cardinality({s \in Slots : proc[s] # "none"}) <= 1
OneHostOperation == Cardinality({s \in Slots : proc[s] \in {"starting", "releasing", "hungLost"}}) <= 1
StoppedStaysStopped == \A s \in Slots : ~used[s] => (~restart[s] /\ ~Running(s))
NeverStranded == \A s \in Slots : (used[s] /\ proc[s] = "none") => restart[s]

\* New (G6). A stream stuck on its way out has a SIGKILL coming (quit_at is set).
HungGetsKilled == \A s \in Slots : Hung(s) => clock[s]
\* ... the same, for one stuck after it said "lost" (it blocks the host's other displays too)
HungLostGetsKilled == \A s \in Slots : proc[s] = "hungLost" => clock[s]
\* Liveness. A stream asked to end ends (the slot leak, fixed in e15b65f)
AskedEnds == \A s \in Slots : (quit[s] /\ proc[s] # "none") ~> proc[s] = "none"
\* ... and so does one that ended on its own and got stuck (nobody stops it)
StuckEnds == \A s \in Slots : Hung(s) ~> proc[s] = "none"
====
