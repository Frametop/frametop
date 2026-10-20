# VR lifecycle requirements

`VrLifecycle` coordinates the existing presentation and tracking policies. The
OpenVR shell supplies runtime observations and effects. Input state, rendering,
pose geometry and independent watchdogs retain their existing owners.

The host acceptance scenarios exercise these requirements through real App input
and private receivers. They do not require a particular list of internal states
or callbacks.

1. Changing the typing target releases held keys to the old receiver and discards
   queued input before the new receiver is selected. This applies even when both
   receivers use the same transport. An unchanged target must not interrupt a hold.
2. A hidden dashboard or panel cannot deliver pointer input, repeats or queued
   text. Hiding cancels movement and pointer captures. Reopening permits fresh
   input, without replaying the cancelled work.
3. Closing the panel dismisses the current request while the process remains
   resident. Late status for that request cannot reopen or acknowledge it. A
   fresh request may open the panel. Choosing native fallback has the same
   request isolation but asks Steam to preserve native input rather than dismiss.
4. A request is acknowledged only after successful presentation with usable
   placement/tracking, an open dashboard, actual compositor visibility and a
   ready input backend. Replacement typing waits for takeover acknowledgement.
   A failed presentation cannot consume a pending show or advertise success.
5. Recenter/show requests survive temporarily closed dashboards and missing
   tracking. Tracking recovery restores readiness without reviving a dismissed
   or expired request. Background startup stays hidden until requested.
6. Focus or tracking-space loss cancels input and movement. Losing one pointer
   device releases that device's keys without releasing the other hand. A lost
   movement owner ends movement; an unrelated device loss or laser leave does
   not end a valid movement capture.

These checks complement the existing output, bridge, movement and tracking tests.
They cannot establish physical OpenVR presentation or controller behavior. Runtime
timings, watchdogs, frame pacing and independent native recovery remain unchanged.
