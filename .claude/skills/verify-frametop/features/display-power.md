# Display power

Frametop can turn the headset's displays off while the Frame keeps running, to save power and
keep it reachable remotely (ft-powerd, the Power tab), and back on. Driven on 2026-10-27 with
`worn on`: `on` -> `off` -> `on`, each within 1 s.

## Sub-features

- `power-status` reports the displays on, off, standby (SteamVR's standby, asked for by
  ft-powerd) or away (SteamVR's standby because the headset came off), idle time and the off
  timeout.
- `power-off` turns the displays off.
- `power-on` turns them back on.

## How to get to it (user POV)

- Frametop's Power tab, or the idle timeout.

## Driving it with the testbench

Preconditions:

- `V snapshot 1-before`: `power.displays` is `on` (with `T worn on`) or `away`.

- **Off.** Run `T power off`, wait 1 s, `V snapshot 2-off`: `power.displays` is `off`.
- **On.** Run `T power on`, wait 1 s, `V snapshot 3-on`: `power.displays` is back to what it was.

### Standby (tracking paused)

With `DISPLAY_OFF_STANDBY=1` (the default) `power off` asks SteamVR for its standby instead of
only turning the backlight off, but never while the testbench fakes a wearer, so the steps above
still end in `off`. To drive the standby itself, nobody may wear the headset and `T worn` must be
released (no `frametop-fake-worn.json`). Then fake the wearer through the driver, which leaves
no flag and which the headset's own driver undoes 15-20 s later:

- `python3 -c 'import socket; s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.sendto(b"worn 1", "\0ft_pointer")'`,
  wait 2 s: `T power status` says `on`.
- **Standby.** `T power off` within 10 s, wait 8 s: `power.displays` is `standby`, vrserver.txt
  has a new `0 - entering standby`, and frametop-power's log says `standby: SteamVR took it`.
- **Wake.** `T power on`: the log says `standby ended: asked to` and `power.displays` is `on`
  within 2 s (and `away` again once the headset's driver has corrected the wearer).

## Gotchas

- Never send the driver `worn` more than once per step: every write wakes vrserver for about
  6 s (pointer/driver/worn.h).

- `V cleanup` turns the displays back on if the run turned them off, but only when its baseline
  had them on or away.
- Displays off can look like a crash to someone who picks the headset up: never leave them off.
