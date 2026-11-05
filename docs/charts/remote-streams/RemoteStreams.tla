---- MODULE RemoteStreams ----
\* ft-screens' remote displays (screens/remote.c): one remote display N of one host, the slots of
\* g_remotes that can run its ft-stream, ft-layout's "remote N start|stop" (route change, Connected
\* off and on, a profile), ft_remote_tick's start (host_busy), and ft-stream's life: it connects,
\* goes live, ends, and on the way out releases a Remote Monitor ("Disconnect Monitor", a stream
\* operation on the host) before it exits and ft_remote_child reaps it.
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Slots, Settings

VARIABLES
    used,     \* [slot -> the slot is display N's (r->used)]
    set,      \* [slot -> the settings it was started with]
    restart,  \* [slot -> a start is due (r->restart_at # 0)]
    proc,     \* [slot -> its ft-stream: "none", "starting", "live", "releasing", "exited" (not reaped)]
    said      \* [slot -> r->state: "queued", "starting", "live", "lost"]

vars == <<used, set, restart, proc, said>>

Running(s) == proc[s] \in {"starting", "live"}
Mine == {s \in Slots : used[s]}

Init == /\ used = [s \in Slots |-> FALSE] /\ set = [s \in Slots |-> CHOOSE v \in Settings : TRUE]
        /\ restart = [s \in Slots |-> FALSE] /\ proc = [s \in Slots |-> "none"]
        /\ said = [s \in Slots |-> "lost"]

\* stop_stream: "quit", the socket closed, SIGTERM; ft-stream goes out the way it always does
Quit(s) == IF Running(s) THEN "releasing" ELSE proc[s]

\* ft-layout: remote N start, with settings v
YouStart(v) ==
    \/ /\ Mine = {}
       /\ \E s \in Slots :
            /\ ~used[s] /\ proc[s] = "none"
            /\ used' = [used EXCEPT ![s] = TRUE] /\ set' = [set EXCEPT ![s] = v]
            /\ restart' = [restart EXCEPT ![s] = TRUE] /\ said' = [said EXCEPT ![s] = "queued"]
            /\ UNCHANGED proc
    \/ \E s \in Mine :   \* other settings: it starts over once the old one has gone
            /\ set[s] # v
            /\ proc' = [proc EXCEPT ![s] = Quit(s)] /\ set' = [set EXCEPT ![s] = v]
            /\ restart' = [restart EXCEPT ![s] = TRUE] /\ said' = [said EXCEPT ![s] = "queued"]
            /\ UNCHANGED used

\* ft-layout: remote N stop (it keeps its pid until the exit is reaped)
YouStop == \E s \in Mine :
    /\ proc' = [proc EXCEPT ![s] = Quit(s)] /\ used' = [used EXCEPT ![s] = FALSE]
    /\ restart' = [restart EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<set, said>>

\* host_busy: another of the host's streams that is starting (the 20 s limit is left out), or one on
\* its way out (told to quit or lost, not reaped yet: it may be releasing a Remote Monitor)
Leaving(o) == proc[o] # "none" /\ (~used[o] \/ proc[o] \in {"releasing", "exited"})
HostBusy(s) == \E o \in Slots \ {s} : (used[o] /\ proc[o] # "none" /\ said[o] = "starting") \/ Leaving(o)

\* ft_remote_tick -> start_stream
TickStarts == \E s \in Slots :
    /\ used[s] /\ restart[s] /\ proc[s] = "none" /\ ~HostBusy(s)
    /\ proc' = [proc EXCEPT ![s] = "starting"] /\ restart' = [restart EXCEPT ![s] = FALSE]
    /\ said' = [said EXCEPT ![s] = "starting"]
    /\ UNCHANGED <<used, set>>

\* ft-stream says "state live" (only while ft-screens still listens: used)
StreamLive == \E s \in Slots :
    /\ proc[s] = "starting"
    /\ proc' = [proc EXCEPT ![s] = "live"]
    /\ said' = IF used[s] THEN [said EXCEPT ![s] = "live"] ELSE said
    /\ UNCHANGED <<used, set, restart>>

\* it can't connect: it exits at once, no Disconnect Monitor
CantConnect == \E s \in Slots :
    /\ proc[s] = "starting"
    /\ proc' = [proc EXCEPT ![s] = "exited"] /\ said' = [said EXCEPT ![s] = "lost"]
    /\ UNCHANGED <<used, set, restart>>

\* the connection ends: out through LiStopConnection and Disconnect Monitor
StreamEnds == \E s \in Slots :
    /\ proc[s] = "live"
    /\ proc' = [proc EXCEPT ![s] = "releasing"] /\ said' = [said EXCEPT ![s] = "lost"]
    /\ UNCHANGED <<used, set, restart>>

ReleaseDone == \E s \in Slots :
    /\ proc[s] = "releasing"
    /\ proc' = [proc EXCEPT ![s] = "exited"]
    /\ UNCHANGED <<used, set, restart, said>>

\* SIGCHLD: ft_remote_child
Reaped == \E s \in Slots :
    /\ proc[s] = "exited"
    /\ proc' = [proc EXCEPT ![s] = "none"]
    /\ restart' = IF used[s] /\ ~restart[s] THEN [restart EXCEPT ![s] = TRUE] ELSE restart
    /\ UNCHANGED <<used, set, said>>

Next == (\E v \in Settings : YouStart(v)) \/ YouStop \/ TickStarts \/ StreamLive \/ CantConnect
     \/ StreamEnds \/ ReleaseDone \/ Reaped

\* slots and settings are interchangeable (model values)
Perms == Permutations(Slots) \cup Permutations(Settings)

Spec == Init /\ [][Next]_vars

\* Rules
TypeOK == Cardinality(Mine) <= 1
\* One ft-stream per display: never two of display N's streams at once.
OneStreamPerDisplay == Cardinality({s \in Slots : proc[s] # "none"}) <= 1
\* One stream operation at a time on the host: a start and a Remote Monitor release never overlap.
OneHostOperation == Cardinality({s \in Slots : proc[s] \in {"starting", "releasing"}}) <= 1
\* A stopped display stays stopped: no start is due and nothing of it streams.
StoppedStaysStopped == \A s \in Slots : ~used[s] => (~restart[s] /\ ~Running(s))
\* A display that should stream and has no ft-stream has a start due.
NeverStranded == \A s \in Slots : (used[s] /\ proc[s] = "none") => restart[s]
====
