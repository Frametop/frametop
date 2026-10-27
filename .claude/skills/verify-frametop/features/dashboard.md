# SteamVR dashboard

A press of a controller's system button opens SteamVR's dashboard (the Steam library) in front of
the Frametop panels, with Frametop's "Enter Gamepad Mode" pill above it. Another press closes
it.

## Sub-features

- `dash-open` opens the dashboard from a controller's system button.
- `dash-close` closes it with a second press.
- `dash-gamepad-pill` shows Frametop's Gamepad Mode pill while the dashboard is open.

## How to get to it (user POV)

- Press the system button on either controller.

## Driving it with the testbench

Preconditions:

- `T worn on`, `T compositor awake`, head pose facing the panels.
- The dashboard is closed: `valve.steam.gamepadui.main` isn't in the visible overlays of
  `V snapshot 1-before --capture`.

- **Connect a controller.** Pick it up. Run `T controller right set --position 0.2 0.9 -0.4`
  (standing space, like the head: at the hip, ahead).
  `T status` shows `input.controllers.right.pose_override: true`.
- **Open.** Press and release system. Run `T controller right button system on`, wait 0.2 s,
  `T controller right button system off`, wait 2 s, `V snapshot 2-open --capture`. The visible
  overlays gain `valve.steam.gamepadui.main`, `valve.steam.gamepadui.bar` and
  `valve.steam.gamepadui.frame.menu.N`; the capture shows the Steam library and "Enter Gamepad
  Mode".
- **Close.** Press and release system again, wait 2 s, `V snapshot 3-closed`. Those overlays
  are no longer visible.
- **Put the controller down.** Run `T controller right reset`.

## Gotchas

- The headset's own system button (`T hmd-button /input/system/click`) does not open the
  dashboard (no visible change on 2026-10-27); use a controller's.
- The overlays take up to 2 s to show or hide: snapshot after the wait, not at once.
- Leaving the dashboard open hands controller input to Steam: close it before the next feature.
