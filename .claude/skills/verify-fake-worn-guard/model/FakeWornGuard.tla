---------------------------- MODULE FakeWornGuard ----------------------------
\* The testbench's fake-worn guard (testbench/frame_testbench/hostinput.py, cli.py `worn`,
\* drop_unsafe_worn, release_or_keep_flag) against one ft-stream to a host with "input": true.
\* ft-stream leases the host's keyboard and mouse while it holds the host's input lock and the
\* pointer helper says "worn", and the helper can't tell a faked wearer from a real head.
\* Left out: --allow-host-input (user-approved), a host turning "input" on mid-run (a documented
\* gap), several hosts, two testbench commands at once, the verify skill's fake viewer.
CONSTANTS
    GuardCanDie,   \* SIGTERM, SIGKILL or OOM reach the guard unit from outside
    ProxyCanHang   \* the input proxy stalls: requests may act, but replies time out (4 s)

VARIABLES
    faked,   \* the input proxy fakes the wearer
    proxy,   \* "up" answers; "hung" may or may not act on a request, and its reply times out
    flag,    \* $XDG_RUNTIME_DIR/frametop-fake-worn.json: "none", "guard" (names the guard's pid)
             \* or "cli" (names a testbench command's pid, left when a release went unconfirmed)
    guard,   \* guard unit: "off", "starting", "holding", "exited" (ExecStopPost still to run)
    lock,    \* who holds frametop-input-HOST.lock: "none", "guard", "stream"
    stream,  \* ft-stream to the input host: "off", "live" (no lock), "holding"
    pc       \* the testbench command in flight
vars == <<faked, proxy, flag, guard, lock, stream, pc>>

Init ==
    /\ faked = FALSE /\ proxy = "up" /\ flag = "none" /\ guard = "off"
    /\ lock = "none" /\ stream = "off" /\ pc = "idle"

\* guard_alive(flag): the flag's pid is a running guard. The guard writes the flag only once it
\* holds the lock, and a flag naming it after it exited names a dead pid.
GuardAlive == flag = "guard" /\ guard = "holding"

\* faked after one release request (worn-release, worn 0, release). Hung: it may have acted.
Released == IF proxy = "up" THEN {FALSE} ELSE {FALSE, faked}

\* drop_unsafe_worn, the first thing every command does: a flag that names no live guard means
\* nothing holds the locks, so release the wearer. Through drop_wearer: the flag goes only once
\* the proxy confirms; a timed-out release keeps it.
Unsafe == flag # "none" /\ ~GuardAlive
DropFaked == IF Unsafe THEN Released ELSE {faked}
DropFlag == IF Unsafe /\ proxy = "up" THEN "none" ELSE flag

\* release_or_keep_flag after an unconfirmed release: the flag stays, or one naming the command
\* is written if none is left
KeepFlag == IF flag = "none" THEN "cli" ELSE flag

\* ---- you, at the testbench CLI (one command at a time) ----

YouRunWornOn ==
    /\ pc = "idle"
    /\ \E f \in DropFaked : faked' = f
    /\ flag' = DropFlag /\ pc' = "on_check"
    /\ UNCHANGED <<proxy, guard, lock, stream>>

YouRunWornOff ==   \* `worn off`, `worn physical` and `release` end the fake the same way
    /\ pc = "idle"
    /\ \E f \in DropFaked : faked' = f
    /\ flag' = DropFlag /\ pc' = "off_send"
    /\ UNCHANGED <<proxy, guard, lock, stream>>

YouRunOther ==     \* any other testbench command: only drop_unsafe_worn matters
    /\ pc = "idle" /\ Unsafe
    /\ \E f \in DropFaked : faked' = f
    /\ flag' = DropFlag
    /\ UNCHANGED <<proxy, guard, lock, stream, pc>>

\* refusal(state()): a live stream to an input host refuses; a live guard is kept; a flag naming
\* no live guard (drop_unsafe_worn's release went unconfirmed) refuses, as start_guard clears it
OnCheck ==
    /\ pc = "on_check"
    /\ pc' = IF stream # "off" THEN "idle"
             ELSE IF GuardAlive THEN "on_fake"
             ELSE IF flag # "none" THEN "idle"
             ELSE "on_stop"
    /\ UNCHANGED <<faked, proxy, flag, guard, lock, stream>>

\* start_guard: `systemctl stop` (waits for the old unit), clear_flag, systemd-run
OnStartGuard ==
    /\ pc = "on_stop" /\ guard = "off"
    /\ flag' = "none" /\ guard' = "starting" /\ pc' = "on_wait"
    /\ UNCHANGED <<faked, proxy, lock, stream>>

\* start_guard's wait: the flag names a live guard, or the unit went inactive (busy: refused)
OnWait ==
    /\ pc = "on_wait"
    /\ \/ GuardAlive /\ pc' = "on_fake"
       \/ guard = "off" /\ pc' = "idle"
    /\ UNCHANGED <<faked, proxy, flag, guard, lock, stream>>

\* bench.control('worn 1'). A timeout: release_or_keep_flag (the release times out too, so the
\* flag stays), raise.
OnFake ==
    /\ pc = "on_fake"
    /\ IF proxy = "up"
         THEN faked' = TRUE /\ flag' = flag /\ pc' = "on_verify"
         ELSE /\ \E f \in {TRUE, FALSE} : faked' = f   \* worn 1 and the release may each have acted
              /\ flag' = KeepFlag /\ pc' = "idle"
    /\ UNCHANGED <<proxy, guard, lock, stream>>

\* the check after worn 1: no live guard -> release_or_keep_flag, refuse
OnVerify ==
    /\ pc = "on_verify"
    /\ IF GuardAlive
         THEN UNCHANGED <<faked, flag>>
         ELSE /\ \E f \in Released : faked' = f
              /\ flag' = IF proxy = "up" THEN "none" ELSE KeepFlag
    /\ pc' = "idle"
    /\ UNCHANGED <<proxy, guard, lock, stream>>

\* worn 0 / worn-release / release, then clear_flag. A timeout raises before clear_flag.
OffSend ==
    /\ pc = "off_send"
    /\ \E f \in Released : faked' = f
    /\ flag' = IF proxy = "up" THEN "none" ELSE flag
    /\ pc' = "idle"
    /\ UNCHANGED <<proxy, guard, lock, stream>>

\* ---- the guard unit ----

\* takes the lock, then writes the flag; exit 3 if a stream holds it
GuardStarts ==
    /\ guard = "starting"
    /\ IF lock = "none"
         THEN lock' = "guard" /\ flag' = "guard" /\ guard' = "holding"
         ELSE guard' = "exited" /\ UNCHANGED <<lock, flag>>
    /\ UNCHANGED <<faked, proxy, stream, pc>>

\* the flag no longer names it: return 0, closing the lock
GuardLetsGo ==
    /\ guard = "holding" /\ flag # "guard"
    /\ guard' = "exited" /\ lock' = "none"
    /\ UNCHANGED <<faked, proxy, flag, stream, pc>>

\* drop_wearer: release (3 tries), then clear the flag; a release that never answers keeps it
DropWearer ==
    IF proxy = "up"
      THEN faked' = FALSE /\ flag' = "none"
      ELSE (\E f \in Released : faked' = f) /\ flag' = flag

\* SIGTERM: if the flag still names it, drop_wearer while it holds the lock; then close the lock
GuardTerm ==
    /\ guard = "holding"
    /\ IF flag = "guard" THEN DropWearer ELSE UNCHANGED <<faked, flag>>
    /\ guard' = "exited" /\ lock' = "none"
    /\ UNCHANGED <<proxy, stream, pc>>

GuardStopped == GuardCanDie /\ GuardTerm

\* start_guard's `systemctl stop` of a guard still running: SIGTERM
StopOldGuard ==
    /\ pc = "on_stop" /\ guard = "holding"
    /\ GuardTerm

\* SIGKILL or OOM: the kernel drops the flock at once; ExecStopPost runs after
GuardKilled ==
    /\ GuardCanDie /\ guard \in {"starting", "holding"}
    /\ guard' = "exited"
    /\ lock' = IF lock = "guard" THEN "none" ELSE lock
    /\ UNCHANGED <<faked, proxy, flag, stream, pc>>

\* ExecStopPost (after_guard): a flag naming no live guard (the dead one, or a command) -> drop_wearer
GuardCleanup ==
    /\ guard = "exited"
    /\ IF flag # "none" THEN DropWearer ELSE UNCHANGED <<faked, flag>>
    /\ guard' = "off"
    /\ UNCHANGED <<proxy, lock, stream, pc>>

\* ---- the world ----

StreamStarts ==
    /\ stream = "off" /\ stream' = "live"
    /\ UNCHANGED <<faked, proxy, flag, guard, lock, pc>>

StreamTakesLock ==
    /\ stream = "live" /\ lock = "none"
    /\ stream' = "holding" /\ lock' = "stream"
    /\ UNCHANGED <<faked, proxy, flag, guard, pc>>

StreamEnds ==
    /\ stream # "off" /\ stream' = "off"
    /\ lock' = IF stream = "holding" THEN "none" ELSE lock
    /\ UNCHANGED <<faked, proxy, flag, guard, pc>>

ProxyHangs ==
    /\ ProxyCanHang /\ proxy = "up" /\ proxy' = "hung"
    /\ UNCHANGED <<faked, flag, guard, lock, stream, pc>>

ProxyRecovers ==
    /\ proxy = "hung" /\ proxy' = "up"
    /\ UNCHANGED <<faked, flag, guard, lock, stream, pc>>

Next ==
    \/ YouRunWornOn \/ YouRunWornOff \/ YouRunOther
    \/ OnCheck \/ OnStartGuard \/ StopOldGuard \/ OnWait \/ OnFake \/ OnVerify \/ OffSend
    \/ GuardStarts \/ GuardLetsGo \/ GuardStopped \/ GuardKilled \/ GuardCleanup
    \/ StreamStarts \/ StreamTakesLock \/ StreamEnds \/ ProxyHangs \/ ProxyRecovers

Spec == Init /\ [][Next]_vars

\* ---- rules ----

\* A faked wearer never lends Frametop a streamed host's keyboard and mouse.
NoHostTakeover == ~(faked /\ stream = "holding")

\* When a testbench command has finished and the wearer is still faked, a flag says so (one
\* naming a live guard, which holds the locks, or one naming no live guard, which the next
\* command and after_guard release), so the guard's cleanup or the next command can undo it (drop_wearer's docstring: a flag gone
\* with the wearer still faked leaves nothing to notice). Mid-command, the command's own
\* post-check (OnVerify) covers it.
FakeIsNoticed == (faked /\ pc = "idle") => flag # "none"

TypeOK ==
    /\ faked \in BOOLEAN /\ proxy \in {"up", "hung"} /\ flag \in {"none", "guard", "cli"}
    /\ guard \in {"off", "starting", "holding", "exited"}
    /\ lock \in {"none", "guard", "stream"} /\ stream \in {"off", "live", "holding"}
    /\ (lock = "stream") <=> (stream = "holding")
    /\ (lock = "guard") <=> (guard = "holding")
=============================================================================
