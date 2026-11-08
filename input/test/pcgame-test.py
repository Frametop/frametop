#!/usr/bin/env python3
"""Offline test of input/pc_game.py: when Steam Link's PC game makes Frametop give way. The hosts
and their answers are stubs and the flag file is a temporary one, so nothing reaches a PC or the
live Frametop.

  input/test/pcgame-test.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import pc_game  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


tmp = tempfile.mkdtemp(prefix="pcgame-test-")
flag = os.path.join(tmp, "frametop-pcgame")
HOSTS = [{"name": "desk-pc", "address": "192.0.2.20", "direct": ["10.35.78.20"], "route": "auto"},
         {"name": "laptop", "address": "192.0.2.30"}]
answers = {}   # address -> answer
asked = []
logs = []


def ask(address, host):
    asked.append((address, host["address"]))
    return answers.get(address)


def flag_text():
    try:
        with open(flag) as f:
            return f.read()
    except FileNotFoundError:
        return None


p = pc_game.PcGame(logs.append, ask=ask, hosts=lambda: HOSTS, path=flag)
check("before Steam Link: nothing written", (p.step(0.0), flag_text(), asked), (False, None, []))

p.link_state(True, 1.0)
answers["10.35.78.20"] = {"steamvr": True, "scene": "home"}
check("Steam Link, PC in SteamVR Home: no game", (p.step(1.0), flag_text()), (False, "0\n"))
check("...asked the dongle first, with the host's token", asked[0], ("10.35.78.20", "192.0.2.20"))
check("...and not its network address once the dongle answered", ("192.0.2.20", "192.0.2.20") in asked, False)

answers["10.35.78.20"] = {"steamvr": True, "scene": "game", "app": "Half-Life: Alyx"}
check("a PC game: give way", (p.step(4.0), flag_text()), (True, "1\n"))
check("...said once", sum("runs a PC game" in m for m in logs), 1)
p.step(7.0)
check("...not again while it runs", sum("runs a PC game" in m for m in logs), 1)

p.configure({"pc_games": "visible"})
check("pc_games visible: Frametop stays over it", (p.step(8.0), flag_text()), (False, "0\n"))
p.configure({"pc_games": "nonsense"})
check("a bad pc_games value: hide", p.step(9.0), True)
p.configure({})
check("no pc_games: hide", p.mode, "hide")

answers.clear()
answers["192.0.2.30"] = {"scene": "game", "app": "Bonelab"}
check("the dongle silent, the network address silent, the other PC runs one", p.step(10.0), True)
check("...named", logs[-1], "Steam Link: laptop runs a PC game (Bonelab)")

answers.clear()
check("no host answers (an older Vibepollo): no game", (p.step(11.0), flag_text()), (False, "0\n"))
check("...the game's end said", logs[-1], "Steam Link: the PC game ended")

answers["10.35.78.20"] = {"scene": "game"}
p.step(12.0)
check("the helper silent for 12 s: Steam Link counts as over", (p.step(25.0), flag_text()), (False, "0\n"))
n = len(asked)
os.unlink(flag)
p.step(28.0)
check("...and then nothing more is asked or written", (len(asked), flag_text()), (n, None))

p.link_state(True, 30.0)
p.step(30.0)
p.link_state(False, 31.0)
check("vrlink 0: over at once", (p.step(31.0), flag_text()), (False, "0\n"))
check("...said, and the game's end", logs[-2:], ["Steam Link: ended", "Steam Link: the PC game ended"])

check("route dongle: only the dongle", pc_game.addresses({"address": "a", "direct": ["d"], "route": "dongle"}), ["d"])
check("route network: only the address", pc_game.addresses({"address": "a", "direct": ["d"], "route": "network"}), ["a"])
check("no dongle: the address", pc_game.addresses({"address": "a"}), ["a"])

with open(os.path.join(tmp, "layout.json"), "w") as f:
    f.write('{"hosts": [{"name": "x", "address": "192.0.2.9"}, {"name": "no address"}, 5]}')
check("layout hosts: those with an address", [h["name"] for h in pc_game.layout_hosts(os.path.join(tmp, "layout.json"))], ["x"])
check("no layout: none", pc_game.layout_hosts(os.path.join(tmp, "missing.json")), [])

for name in os.listdir(tmp):
    os.unlink(os.path.join(tmp, name))
os.rmdir(tmp)
print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
