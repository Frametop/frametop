# SteamVR dashboard

A press of a controller's system button opens SteamVR's dashboard (the Steam library) in front of
the Frametop panels, with Frametop's "Enter Gamepad Mode" pill above it. Another press closes
it.

## Sub-features

- `dash-open` opens the dashboard from a controller's system button.
- `dash-close` closes it with a second press.
- `dash-gamepad-pill` shows Frametop's Gamepad Mode pill while the dashboard is open.
- `dash-laser-click` aims the controller's laser at the dashboard and clicks with the trigger.

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
- **Laser click.** With the dashboard open from this controller, move it a little
  (`T controller right set --position 0.2 1.03 -0.4`, then back to `1.02`), wait 1 s and
  `V snapshot 3-aimed --capture`: the capture shows the laser ending on a library tile. Pull and
  let go of the trigger (`T controller right trigger 1.0 --click on`, 0.15 s,
  `T controller right trigger 0 --click off`), wait 2.5 s, `V snapshot 4-clicked --capture`:
  the capture shows that game's page (proved 2026-10-27: Astroneer's page opened). Press and
  release `b` to go back to the library.
- **Close.** Press and release system again, wait 2 s, `V snapshot 5-closed`. Those overlays
  are no longer visible.
- **Put the controller down.** Run `T controller right reset`.

## Gotchas

- The headset's own system button (`T hmd-button /input/system/click`) does not open the
  dashboard (no visible change on 2026-10-27); use a controller's.
- The overlays take up to 2 s to show or hide: snapshot after the wait, not at once.
- Leaving the dashboard open hands controller input to Steam: close it before the next feature.
  `V cleanup` doesn't close it, so close it before cleanup.
- The laser shows only on the controller that opened the dashboard. A controller connected
  while it was already open (after a cleanup, say) gets none: close and reopen it with that
  controller's system button.
- Frametop's floating windows (`frametop.float.N`, such as Remote Displays) can sit in front of
  the dashboard, with real settings under the laser. Read the aimed capture before pulling the
  trigger, and only click where the laser ends on the dashboard. A cursor dot that stays put
  when the controller moves is a desktop cursor, not the laser.
- Tile clicks are safe (a game's page). Never click Play, Install or the store.
