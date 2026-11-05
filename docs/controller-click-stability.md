# Controller desktop click stability

Trigger presses reach KDE immediately, but controller motion within 32 logical
pixels of the press stays at that position until release. Releasing without a motion outside this
zone delivers the click at the original position, even if the hand moved during
release. Moving outside the zone begins a normal drag immediately; returning to
the zone does not turn it back into a click. There is no hold-duration timer.

This filters overlay pointer content events on desktop monitors only, and only
presses from hand controllers start it. The 3D mouse (whose laser comes from the
`ft_pointer` virtual controller), SteamVR UI, separate screen grab bars and
floating-app title-bar carrying are unaffected. Multi-button gestures keep their
existing behavior. A motion onto another desktop monitor starts a drag;
cross-monitor motion is not stabilized.

CLI (runtime preferences, reset to 32 on desktop restart):

```sh
input/ft-clickctl status
input/ft-clickctl threshold 32
input/ft-clickctl threshold 0  # disable without a restart
```

Thresholds are 0–64 logical pixels, normalized to each panel's KDE scale.
Status reports held state, suppressed motions, stabilized clicks and drags.
Changing the threshold while a controller button is held is refused.

This is a separate contribution from desktop mouse/controller ownership. Its
hardware validation must check small controls, intentional text selection,
long presses, cross-monitor dragging and simultaneous mouse use. The existing
renderer laser remains tracked; this change stabilizes desktop input rather
than smoothing the visual laser. The default was 8 at first. That's about 0.2° on a 3.4 m wide 3440-pixel screen 2 m away, and clicking took a very still hand, so it's 32 (about 0.9°) since 2026-10-03.

`just test-c` runs the isolated gesture-state tests (`screens/tests/controller-click-test.c`).
