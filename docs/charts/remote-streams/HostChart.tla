---- MODULE HostChart ----
\* The chart's slice of HostInputLease (the whole model is too big to draw; the skill's check runs
\* all of it): the switch stays on, and the helper counts only an off of 3 s or more.
EXTENDS HostInputLease
ChartNext == HeadsetOff \/ HeadsetOn \/ HostEscape \/ Age \/ HelperAnswers
          \/ StreamStarts \/ StreamLive \/ StreamEnds \/ ReadSwitch \/ TickIdle \/ TickLive
LongOffsOnly == onAfter => cleared
====
