# 3D mouse

With pointer mode on (`POINTER=1`, the default), a mouse plugged into the Frame moves Frametop's
3D cursor over the panels, clicks and scrolls where it points, and a keyboard types where typing
goes (the desktop panel clicked last, or Steam). The testbench plugs in a keyboard and a mouse
as host devices of the input relay, which treats them like real ones.

## Sub-features

- `mouse-move` moves the cursor across and between panels.
- `mouse-click` clicks (left, right, middle; double) where the cursor is.
- `mouse-drag` holds a button through a move.
- `mouse-scroll` scrolls under the cursor.
- `keys` presses key combinations, with Frametop's bindings (`meta+space` opens frunner).
- `type` types text where typing goes.
- `pointer-recenter` brings the cursor back in front.

## How to get to it (user POV)

- Move, click and scroll a mouse paired with the Frame.
- Type on a keyboard paired with the Frame.
- Recenter from the pointer's controls.

## Driving it with the testbench

Preconditions:

- `T worn on`, `T compositor awake`, head pose facing a Frametop screen panel
  (`T pointer measure frametop.screen.1`).
- `T pointer state` shows `worn: true` and a head position.

- **Show the cursor in front.** Run `T pointer recenter`, then `T mouse move 1 0`, then
  `V snapshot 1-before --capture`. The capture shows the cursor dot.
- **Move.** Run `T mouse move 300 0`, then `V snapshot 2-moved --capture`. The dot is right of
  where it was. `journalctl --user -u frametop-input-relay` shows
  `Frame testbench Mouse (host:testbench:mouse ...): pointer` and `released ...: its host let go`.
- **Watch the events arrive (optional).** Run
  `V ask frametop_relay 'watch 5' --listen 5 & sleep 0.5; T mouse move 300 0; wait`: the replies
  after `{"t": "watching", ...}` are `{"t": "event", "id": "host:testbench:mouse",
  "role": "pointer", ...}` (moves at most one per 50 ms there).
- **Click.** With the dot on a harmless spot, run `T mouse click`, then snapshot with
  `--capture`. The capture shows the click's effect (a focused panel, a selection).
- **Keys.** Run `T keyboard keys meta+space`, wait 1.5 s, `V snapshot 3-frunner --capture`:
  `frametop.frunner` is visible. Close it with `T keyboard keys esc`; a snapshot 1 s later no
  longer lists it.
- **Type.** Click into a text field you opened for the test, run `T keyboard type -- 'testbench'`
  (it returns once the relay has typed it), and capture.

## Gotchas

- `T pointer measure` finds only panels in view of the head, and never the cursor itself
  (`frametop.pointer.cursor` says "panel not visible"); the pointer helper has no cursor
  position query, so the capture is the readback.
- Mouse moves are in mouse counts, scaled by the 3D mouse's speed settings, not pixels.
- `keys` and `type` go where typing goes, which may be a terminal on the desktop: click the
  target first, and never type commands.
- Only one testbench command at a time: two at once share the `testbench` devices.
- If a run dies mid-drag, the relay doesn't release the 3D mouse's button (a known Frametop gap,
  `testbench/docs/desktop.md`): the next click clears it.
