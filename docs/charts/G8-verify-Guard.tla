------------------------------ MODULE G8Guard ------------------------------
\* G8, read-only reuse: verify-fake-worn-guard's FakeWornGuard.tla (copied, not edited) with
\* fairness for the system's own steps (the command in flight, the guard unit, ExecStopPost, the
\* proxy coming back) and one liveness rule. People's commands, streams, kills and hangs get none.
EXTENDS FakeWornGuard

\* V3 fix (DEE-283), through V1's: the input proxy's own TTL ends a faked wearer
\* (testbench/native/input_state.cpp tick). Its clock runs through hangs: only a command that
\* reaches the proxy renews it, and a release that reaches it ends the fake anyway. So a proxy
\* that is up again and again acts at a tick past the TTL: strong fairness, at any point of a
\* command (a command still on its way hasn't renewed it). The skill's model is left as it is;
\* this step joins its Next here.
TtlEnds ==
    /\ proxy = "up" /\ faked
    /\ faked' = FALSE
    /\ UNCHANGED <<proxy, flag, guard, lock, stream, pc, asked, seen, saidTest, lease>>

FairSpec == Init /\ [][Next \/ TtlEnds]_vars /\ SF_vars(TtlEnds)
    /\ WF_vars(OnCheck) /\ WF_vars(OnStartGuard) /\ WF_vars(OnWait) /\ WF_vars(OnFake)
    /\ WF_vars(OnVerify) /\ WF_vars(OffSend) /\ WF_vars(StopOldGuard)
    /\ WF_vars(GuardStarts) /\ WF_vars(GuardLetsGo) /\ WF_vars(GuardCleanup) /\ WF_vars(ProxyRecovers)

\* The guard always restores the real wearer: once the wearer is faked with no live guard and no
\* command running, the fake ends (or a new guard takes it over) with no command needed.
GuardRestoresReal == [](faked /\ ~GuardAlive /\ pc = "idle" => <>(~faked \/ GuardAlive))
=============================================================================
