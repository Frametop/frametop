# Gaze pointer

In gaze mode the cursor follows where the user looks, from SteamVR's eye tracker or Frametop's
own (ft-gaze, then the gaze service ft-gazed). The testbench fakes the eye tracker's output
files, so everything from ft-gaze on runs as with real eyes.

## Sub-features

- `gaze-follow` moves the gaze sample (and in gaze mode the cursor) where the eyes look.
- `gaze-lost` handles an eye the tracker can't see, or closed eyes.
- `gaze-mode` turns gaze mode on and off.

## How to get to it (user POV)

- Turn gaze mode on (Frametop Input Settings, Gaze page, or `~/.local/share/frametop/releases/current/gaze/ft-gazectl on`), look at a panel.

## Driving it with the testbench

Preconditions:

- `T worn on` and a head pose (ft-gaze only sends samples while the head is tracked).
- The gaze service is awake: send `wake 90` to `@ft_gazed` (it idles while gaze mode is off and
  nobody wears the headset), or turn gaze mode on.

- **Look.** Run `T gaze look 10 -5`, wait 1 s, `V snapshot 2-look`. In `readbacks.json`,
  `gaze.last` starts near `[10, -5]` (yaw +left, pitch +up, head-relative degrees),
  `gaze.samples` has grown since `1-before`, and `gaze.sample_age_s` is under 1.
- **Look elsewhere.** Run `T gaze look -8 4`, snapshot: `gaze.last` follows.
- **Lose an eye.** Run `T gaze eyes open lost`, snapshot: `gaze.eyes_lost` is `[false, true]`.
- **Gaze mode on (visual).** Run `~/.local/share/frametop/releases/current/gaze/ft-gazectl on` (the helper keeps it until it restarts;
  `V cleanup` puts it back), aim with `T gaze look`, and capture: the cursor sits where the eyes
  look.
- **Release.** Run `T gaze release`: ft-gaze goes back to the real tracker within 2 s.

## Gotchas

- Without `worn on` the samples stall: ft-gaze emits only with a valid head pose.
- Gaze calibration still applies to faked looks, so `last` is near the look, not equal to it.
- A replayed recording (`T replay NAME --only gaze`) is a real person's eye movement: keep
  recordings local.
- How a real look feels, and tracking quality, need a headset test.
