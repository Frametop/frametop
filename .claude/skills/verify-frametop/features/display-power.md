# Display power

Frametop can turn the headset's displays off while the Frame keeps running, to save power and
keep it reachable remotely (ft-powerd, the Power tab), and back on. Driven on 2026-10-27 with
`worn on`: `on` -> `off` -> `on`, each within 1 s.

## Sub-features

- `power-status` reports the displays on, off or away (SteamVR's standby), idle time and the
  off timeout.
- `power-off` turns the displays off.
- `power-on` turns them back on.

## How to get to it (user POV)

- Frametop's Power tab, or the idle timeout.

## Driving it with the testbench

Preconditions:

- `V snapshot 1-before`: `power.displays` is `on` (with `T worn on`) or `away`.

- **Off.** Run `T power off`, wait 1 s, `V snapshot 2-off`: `power.displays` is `off`.
- **On.** Run `T power on`, wait 1 s, `V snapshot 3-on`: `power.displays` is back to what it was.

## Gotchas

- `V cleanup` turns the displays back on if the run turned them off, but only when its baseline
  had them on or away.
- Displays off can look like a crash to someone who picks the headset up: never leave them off.
