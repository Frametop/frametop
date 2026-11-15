# G3 Input and pointer

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers the input relay (device roles, grabs, bindings, the typing target, the fd store), ft-screens' keys, the pointer helper's laser and its one trigger, the driver, Full Keyboard and ft-textinput, voice typing, and gamepad mode.

Models:

- `G3-input-Keys.tla` (module `G3Keys`): every key and button that goes down comes up on the target it went down on. Configs: `Buttons`, `ButtonsRestart`, `Host`, `Keys`, `Mods`, `Stay`.
- `G3-input-Laser.tla` (module `G3Laser`): one owner for each hand's laser, and one holder of the trigger. Configs: `Code`, and `Waits` (let-go waits for a touch to end).

Results at `78e93b1`:

| Model and config | Holds | Breaks |
| --- | --- | --- |
| Keys Buttons | TypeOK, DesktopComesUp (144 states) | VirtualComesUp (K1, 3 steps), DriverComesUp (K2, 3 steps) |
| Keys ButtonsRestart | TypeOK, DesktopComesUp (72) | VirtualComesUp (K6), DriverComesUp (K7) |
| Keys Host | TypeOK, DriverComesUp (550) | DesktopComesUp (K3), VirtualComesUp (K6) |
| Keys Keys | TypeOK, VirtualComesUp, DriverComesUp (300) | DesktopComesUp (K3, 4 steps) |
| Keys Mods | Every rule (180) | None |
| Keys Stay | Every rule (96) | None |
| Laser Code | TypeOK, OneOwnerPerHand (302) | OneTriggerHolder (K4, 3 steps), HolderHasTrigger (K4, 4 steps), NoOrphanTrigger (K5, 5 steps) |
| Laser Waits | TypeOK, OneOwnerPerHand, NoOrphanTrigger (178) | OneTriggerHolder, HolderHasTrigger (K4) |

The DEE-222 stuck-keys fix (`956ed81`) is drawn green.

Results with the fixes (`4606ad0`): every rule holds in every config, 34 runs. States per run: Keys 52 (Buttons) to 252 (Host); Laser 82 (Code) and 70 (Waits). The models now describe the fixed code. To see the old breaks, check out `f85dff9` and run its copy of the models. Undoing K4 and K5 in the Laser model, or K6 and K7 in the Keys model, brings the old counts and breaks back, so the rules aren't vacuous.

The Laser model's `Waits` config (let-go paths wait for a touch) was the other way to fix K5. The fix took the code's way instead: the touch goes, with its trigger up. Both configs hold.

The chart (`G3-input.mmd`, `.svg`) still draws K1 to K7 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| K1 | DEE-263 | VirtualComesUp | A mouse button pressed while paused or with POINTER=0 went to the virtual mouse. A resume or POINTER=1 while it was held sent its release to the helper, and the virtual mouse kept it down | Fixed in `4606ad0`. The release of a button in `node.held` goes to the virtual device it went down on, whatever pointer mode or the binding says now |
| K2 | DEE-264 | DriverComesUp (minor) | POINTER=0 while a pointer-mode button was held dropped the Pointer with no stand_down, and the laser's trigger stayed down | Fixed in `4606ad0`. `load_config` stands the old pointer down (its driver buttons come up, then `hide pause`) |
| K3 | DEE-265 | DesktopComesUp | A relay restart while a key was down on the desktop. The new relay let go of modifiers and mouse buttons only, so KWin kept the key down and repeated it | Fixed in `4606ad0`. A relay that starts sends ft-screens `keys release`. ft-screens passes it on to Frunner, which lets go of its keys and stops a repeat |
| K4 | DEE-266 | OneTriggerHolder, HolderHasTrigger | The mouse's button asked nobody. Pressed during a touch or a ray pinch, it shared the one trigger and leftHeld, and the first let-go ended both drags | Fixed in `4606ad0`. The helper's mouse press ends a touch (trigger up first if down) and a ray pinch (released, no click), then presses |
| K5 | DEE-267 | NoOrphanTrigger | The pointer was let go while a touch held the trigger. The touch was forgotten with no trigger up, and the next wake connected the controller pressed | Fixed in `4606ad0`. The touch goes with `btn trigger 0` when it held the trigger |
| K6 | DEE-268 | VirtualComesUp | The virtual devices came back from the fd store with keys down after a relay restart, and the new relay's `node.held` was empty | Fixed in `4606ad0`. A reused virtual keyboard and mouse release every key at start (the kernel drops releases of keys that aren't down). A reused gamepad releases its buttons and centres its sticks |
| K7 | DEE-269 | DriverComesUp | A relay restart while a pointer-mode button was down. The release never reached the helper, and leftHeld and the driver's trigger stayed down until the next click | Fixed in `4606ad0`. A relay that starts sends the helper a release for every driver button. The helper ignores a release for a mouse button it doesn't have down, so a touch or ray hold keeps the trigger |

Design choices in these fixes (Daenon can override each):

- K3: a relay that starts releases every key (a full `keys release`), not only modifiers and mouse buttons. A key held through a relay restart comes up and has to be pressed again.
- K4: a mouse press ends any touch on a SteamVR panel, hovering or pressed, and any ray pinch, then takes the trigger. The touch comes back with ft-screens' next word after the mouse lets go.
- K2: POINTER=0 also hides the pointer (`hide pause`), so the virtual controller gives the laser back at once.
