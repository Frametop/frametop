------------------------------ MODULE G8Guard ------------------------------
\* G8, read-only reuse: verify-fake-worn-guard's FakeWornGuard.tla (copied, not edited) with
\* fairness for the system's own steps (the command in flight, the guard unit, ExecStopPost, the
\* proxy coming back) and one liveness rule. People's commands, streams, kills and hangs get none.
EXTENDS FakeWornGuard

FairSpec == Spec
    /\ WF_vars(OnCheck) /\ WF_vars(OnStartGuard) /\ WF_vars(OnWait) /\ WF_vars(OnFake)
    /\ WF_vars(OnVerify) /\ WF_vars(OffSend) /\ WF_vars(StopOldGuard)
    /\ WF_vars(GuardStarts) /\ WF_vars(GuardLetsGo) /\ WF_vars(GuardCleanup) /\ WF_vars(ProxyRecovers)

\* The guard always restores the real wearer: once the wearer is faked with no live guard and no
\* command running, the fake ends (or a new guard takes it over) with no command needed.
GuardRestoresReal == [](faked /\ ~GuardAlive /\ pc = "idle" => <>(~faked \/ GuardAlive))
=============================================================================
