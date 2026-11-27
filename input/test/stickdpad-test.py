#!/usr/bin/env python3
"""Offline test of gamepad mode's left stick as d-pad presses, for a Steam game that reads its
controller through Steam Input's actions: which games those are (input/steamactions.py, on a
temporary Steam folder), the stick's presses (vrpad.StickDpad), and gamepad mode using them for
such a game's panel only (vrpad.GamepadMode, with a recording pad: no uinput, no Steam).

  input/test/stickdpad-test.py
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import steamactions  # noqa: E402
import vrpad  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


# ---- which games read their controller through Steam Input's actions
tmp = tempfile.mkdtemp(prefix="stickdpad-test-")
steam, other = os.path.join(tmp, "Steam"), os.path.join(tmp, "sd card")


def install(library, app, folder, actions):
    """A game in a library: its manifest, its folder, and with `actions` its action list."""
    os.makedirs(os.path.join(library, "steamapps", "common", folder, "controller_config" if actions else ""))
    with open(os.path.join(library, "steamapps", f"appmanifest_{app}.acf"), "w") as f:
        f.write('"AppState"\n{\n\t"appid"\t\t"%d"\n\t"name"\t\t"A Game"\n\t"installdir"\t\t"%s"\n}\n' % (app, folder))
    if actions:
        open(os.path.join(library, "steamapps", "common", folder, "controller_config", f"game_actions_{app}.vdf"), "w").close()


install(steam, 2868840, "Slay the Spire 2", actions=True)
install(steam, 620, "Portal 2", actions=False)
install(other, 4103250, "On The Card", actions=True)
with open(os.path.join(steam, "steamapps", "libraryfolders.vdf"), "w") as f:
    f.write('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' % (steam, other))
with open(os.path.join(steam, "steamapps", "appmanifest_777.acf"), "w") as f:
    f.write('"AppState"\n{\n\t"installdir"\t\t"Gone"\n}\n')  # its folder was deleted

check("a game's folder, from its manifest", steamactions.install_dir(2868840, steam),
      os.path.join(steam, "steamapps", "common", "Slay the Spire 2"))
check("a game with an action list: Steam Input's actions", steamactions.native(2868840, steam), True)
check("a game without one: a plain pad", steamactions.native(620, steam), False)
check("a game in another library", steamactions.native(4103250, steam), True)
check("a manifest whose folder is gone", (steamactions.install_dir(777, steam), steamactions.native(777, steam)), (None, False))
check("a game Steam doesn't have", steamactions.native(5, steam), False)
check("no app id", steamactions.native(0, steam), False)
check("no Steam there at all", steamactions.native(2868840, os.path.join(tmp, "nowhere")), False)
shutil.rmtree(tmp)

# ---- the stick's presses
UP, DOWN, LEFT, RIGHT = vrpad.PAD_UP, vrpad.PAD_DOWN, vrpad.PAD_LEFT, vrpad.PAD_RIGHT
ON, OFF = vrpad.STICK_ON, vrpad.STICK_OFF
stick = vrpad.StickDpad()
check("at rest: nothing", stick.feed(0, 0), 0)
check("short of the threshold: nothing", stick.feed(ON - 1, -(ON - 1)), 0)
check("right", stick.feed(ON, 0), RIGHT)
check("held between the thresholds: still right", stick.feed(OFF, 0), RIGHT)
check("back under the lower one: released", stick.feed(OFF - 1, 0), 0)
check("left, up (the stick is up-positive), down", [stick.feed(-32768, 0), stick.feed(0, 32767), stick.feed(0, -32768)],
      [LEFT, UP, DOWN])
check("a diagonal is two", stick.feed(-30000, 30000), LEFT | UP)
check("straight across: the other side, never both", stick.feed(32767, 0), RIGHT)

# ---- gamepad mode: only for such a game's panel
pads, said, logs, asked = [], [], [], []


class FakePad:
    def __init__(self, identity):
        self.identity, self.sent = identity, []
        pads.append(self)

    def send(self, state):
        self.sent.append(state)

    def close(self):
        self.sent.append("removed")


def actions(app):
    asked.append(app)
    return app == 2868840


mode = vrpad.GamepadMode(logs.append, lambda to, m: said.append(m), FakePad, lambda on: None, sound=False,
                         steam_actions=actions)
mode.screens_word("game", 1.0, None, 2868840)
check("a Steam Input game's panel: said once in the log", [m for m in logs if "left stick" in m],
      ["gamepad mode: steam:2868840 reads its controller through Steam Input, which has nothing for a game on a "
       "panel: the left stick presses the d-pad too"])
mode.reader_state((0, 0, 0, 0, 0, 0, 0), 1.1)
mode.reader_state((0, 0, 0, 32767, 0, 0, 0), 1.2)
check("its left stick right: the d-pad's right, and the stick as it is", pads[-1].sent[-1], (RIGHT, 0, 0, 32767, 0, 0, 0))
mode.reader_state((vrpad.PAD_A, 0, 0, 0, 30000, 0, 0), 1.3)
check("up with A held", pads[-1].sent[-1], (vrpad.PAD_A | UP, 0, 0, 0, 30000, 0, 0))
mode.reader_state((0, 0, 0, 0, 0, 32767, 32767), 1.4)
check("the right stick presses nothing", pads[-1].sent[-1], (0, 0, 0, 0, 0, 32767, 32767))
mode.screens_word("game", 2.0, None, 2868840)
check("ft-screens' word each second: asked about the game once", asked, [2868840])
mode.screens_word(None, 3.0)

mode.screens_word("game", 10.0, None, 2868840)
mode.reader_state((0, 0, 0, -32768, 0, 0, 0), 10.1)
check("a stick held over as the mode comes on waits for its release, as a held button does",
      pads[-1].sent[-1], (0, 0, 0, -32768, 0, 0, 0))
mode.reader_state((0, 0, 0, 0, 0, 0, 0), 10.2)
mode.reader_state((0, 0, 0, -32768, 0, 0, 0), 10.3)
check("...and presses from the next push", pads[-1].sent[-1], (LEFT, 0, 0, -32768, 0, 0, 0))
mode.screens_word("game", 11.0, None, 620)
check("the pill moved to another game's panel: the first pad lets go and goes, a new one comes",
      (pads[-2].sent[-2:], len(pads), [m for m in logs if "now for" in m]),
      ([vrpad.NEUTRAL, "removed"], 3, ["gamepad mode: now for a gamescope app's panel"]))
mode.reader_state((0, 0, 0, 0, 0, 0, 0), 11.1)
mode.reader_state((0, 0, 0, 32767, 0, 0, 0), 11.2)
check("a game that reads the pad itself: the stick is only the stick", pads[-1].sent[-1], (0, 0, 0, 32767, 0, 0, 0))
mode.screens_word(None, 12.0)

for target, app, label in (("game", 0, "a gamescope app that isn't Steam's"), ("desktop", 0, "a desktop panel")):
    del asked[:]
    mode.screens_word(target, 20.0, None, app)
    mode.reader_state((0, 0, 0, 0, 0, 0, 0), 20.1)
    mode.reader_state((0, 0, 0, 0, -32768, 0, 0), 20.2)
    check(f"{label}: the stick is only the stick, and nothing is asked", (pads[-1].sent[-1], asked),
          ((0, 0, 0, 0, -32768, 0, 0), []))
    mode.screens_word(None, 21.0)

plain = vrpad.GamepadMode(logs.append, lambda to, m: None, FakePad, lambda on: None, sound=False)
plain.screens_word("game", 30.0, None, 2868840)
plain.reader_state((0, 0, 0, 0, 0, 0, 0), 30.1)
plain.reader_state((0, 0, 0, 32767, 0, 0, 0), 30.2)
check("nothing to ask (the default): the stick is only the stick", pads[-1].sent[-1], (0, 0, 0, 32767, 0, 0, 0))

print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
