------------------------------ MODULE G8Verify ------------------------------
\* G8 at 78e93b1: how long the testbench's fakes last after the test that made them.
\* One verify run (.claude/skills/verify-frametop/scripts/verify-frametop launch, guard,
\* snapshot, cleanup) drives the headset through the testbench (testbench/frame-testbench):
\* the input proxy's overrides inside vrserver (worn on or off, the head pose, HMD buttons and
\* the synthetic controllers: testbench/native/input_state.cpp) and the fake trackers
\* (frame-testbench-fakes, testbench/frame_testbench/fakes.py). The run holds
\* $XDG_RUNTIME_DIR/verify-frametop.json, a lease of LEASE (30 min) from its last launch, guard
\* or snapshot. The rule: a fake never outlives the test that made it.
\* Left out, and checked elsewhere: the fake-worn guard and the host input locks
\* (verify-fake-worn-guard's FakeWornGuard.tla, re-run read-only for G8), the input relay's
\* testbench devices (one command each, a 3 s timeout: input/input-relay.py HOST_TIMEOUT), two
\* agents at once (launch's claim takes a flock), SteamVR's standby and the displays' power.
\* Since the G8 fixes (DEE-281, DEE-282) this is the fixed code: the proxy's overrides end after
\* OVERRIDE_TTL (300 s) with no command but status (TtlEnds), and a status question no longer
\* restarts the fakes' idle hour (StatusPoll). FixLease and FixPoll are no longer used; to see
\* the old breaks, check out f85dff9.
CONSTANTS
    AgentCanDie,  \* the agent's session ends mid-run: a killed shell, a stalled approval
                  \* prompt, the context running out. Its cleanup never runs.
    StatusPolls,  \* something keeps asking the testbench for status with no run driving: a
                  \* later agent's doctor or launch (T status), an MCP client's status tool
    FixLease,     \* unused since the V1 fix (TtlEnds is the code now)
    FixPoll       \* unused since the V2 fix (StatusPoll restarts nothing now)

VARIABLES
    driver,  \* "agent": a live agent drives the run; "none": nobody does (never started, done, or died)
    run,     \* verify-frametop.json: "free", "held" (lease running), "stale" (lease ran out)
    worn,    \* the proxy's proximity override: "real" (the sensor), "on", "off"
    pose,    \* any other proxy override: head pose, HMD buttons, synthetic controllers
    fakes,   \* frame-testbench-fakes runs: fake eyes for ft-gaze, ft-hands parked, fake hands
    quiet,   \* the fakes have had no command for IDLE_EXIT (an hour)
    wearer   \* someone wears the headset (the real proximity sensor)
vars == <<driver, run, worn, pose, fakes, quiet, wearer>>

TypeOK ==
    /\ driver \in {"agent", "none"}
    /\ run \in {"free", "held", "stale"}
    /\ worn \in {"real", "on", "off"}
    /\ pose \in BOOLEAN /\ fakes \in BOOLEAN /\ quiet \in BOOLEAN /\ wearer \in BOOLEAN

Overrides == worn # "real" \/ pose
Clean == ~Overrides /\ ~fakes

Init ==
    /\ driver = "none" /\ run = "free" /\ worn = "real" /\ pose = FALSE
    /\ fakes = FALSE /\ quiet = FALSE /\ wearer = FALSE

\* ---- the agent ----

\* V launch: no live lease, the doctor passes: not worn, no override or fake left over
Launch ==
    /\ driver = "none" /\ run # "held" /\ ~wearer /\ Clean
    /\ driver' = "agent" /\ run' = "held"
    /\ UNCHANGED <<worn, pose, fakes, quiet, wearer>>

\* V guard && T ...: a drive step only while nobody wears the headset. Any T command is a
\* command to the fakes (start=True for gaze look, hand set, replay).
Drive ==
    /\ driver = "agent" /\ run = "held" /\ ~wearer
    /\ \E w \in {"real", "on", "off"}, p \in BOOLEAN, f \in BOOLEAN :
          worn' = w /\ pose' = p /\ fakes' = (fakes \/ f)
    /\ quiet' = FALSE
    /\ UNCHANGED <<driver, run, wearer>>

\* V cleanup RUN (or V guard finding a wearer: cleanup --force, exit 2): T release (the fakes,
\* then the proxy), compositor auto, the lease freed
Cleanup ==
    /\ driver = "agent"
    /\ driver' = "none" /\ run' = "free" /\ worn' = "real" /\ pose' = FALSE /\ fakes' = FALSE
    /\ quiet' = FALSE
    /\ UNCHANGED wearer

AgentDies ==
    /\ AgentCanDie /\ driver = "agent"
    /\ driver' = "none"
    /\ UNCHANGED <<run, worn, pose, fakes, quiet, wearer>>

\* ---- time and the system ----

\* lock_holder: idle > LEASE marks the run stale; nothing else happens (verify-frametop 169)
LeaseRunsOut ==
    /\ driver = "none" /\ run = "held"
    /\ run' = "stale"
    /\ UNCHANGED <<driver, worn, pose, fakes, quiet, wearer>>

\* V1 fix (input_state.cpp tick): OVERRIDE_TTL with no command but status ends every override,
\* whoever drives (an agent that stops for 5 minutes gets them back with its next command)
TtlEnds ==
    /\ Overrides
    /\ worn' = "real" /\ pose' = FALSE
    /\ UNCHANGED <<driver, run, fakes, quiet, wearer>>

QuietHour ==
    /\ fakes /\ ~quiet /\ driver = "none"
    /\ quiet' = TRUE
    /\ UNCHANGED <<driver, run, worn, pose, fakes, wearer>>

\* fakes.py serve 815: an hour without a command ends them; close() takes their files back
FakesIdleExit ==
    /\ fakes /\ quiet
    /\ fakes' = FALSE /\ quiet' = FALSE
    /\ UNCHANGED <<driver, run, worn, pose, wearer>>

\* fakes.py serve: a status question is answered, and since the V2 fix restarts nothing (it
\* doesn't renew the proxy's TTL either)
StatusPoll ==
    /\ StatusPolls /\ fakes /\ driver = "none" /\ quiet
    /\ UNCHANGED vars

\* ---- people ----

WearerOn ==
    /\ ~wearer /\ wearer' = TRUE
    /\ UNCHANGED <<driver, run, worn, pose, fakes, quiet>>

WearerOff ==
    /\ wearer /\ wearer' = FALSE
    /\ UNCHANGED <<driver, run, worn, pose, fakes, quiet>>

\* someone runs T release or V cleanup by hand, or restarts SteamVR (the proxy starts with no
\* overrides). Nobody has to: no fairness.
ByHand ==
    /\ driver = "none" /\ ~Clean
    /\ worn' = "real" /\ pose' = FALSE /\ fakes' = FALSE /\ quiet' = FALSE
    /\ run' = IF run = "held" THEN run ELSE "free"
    /\ UNCHANGED <<driver, wearer>>

Next ==
    \/ Launch \/ Drive \/ Cleanup \/ AgentDies
    \/ LeaseRunsOut \/ TtlEnds \/ QuietHour \/ FakesIdleExit \/ StatusPoll
    \/ WearerOn \/ WearerOff \/ ByHand

\* Fairness for the system only, and for the hard rule "every run ends with V cleanup".
Spec == Init /\ [][Next]_vars
        /\ WF_vars(Cleanup) /\ WF_vars(LeaseRunsOut) /\ WF_vars(TtlEnds)
        /\ WF_vars(QuietHour) /\ WF_vars(FakesIdleExit)

\* ---- rules ----

\* A run starts on a clean headset (launch's doctor): holds
LaunchClean == [][Launch => Clean]_vars

\* Drive steps happen only with nobody in the headset (V guard): holds
NoDriveOnWearer == [][Drive => ~wearer]_vars

\* The proxy's overrides end once nobody drives the run (V1)
OverridesEnd == [](driver = "none" => <>~Overrides)

\* The fake trackers end once nobody drives the run (V2)
FakesEnd == [](driver = "none" => <>~fakes)

\* Someone who puts the headset on gets their own head, proximity and trackers back (V1, V2)
WearerGetsReal == [](wearer /\ driver = "none" => <>(Clean \/ ~wearer))
=============================================================================
