#!/usr/bin/env python3
"""Frametop Tour, offline: the done flag (ft_tour.py and the ft-tour wrapper's --first-run and
--reset, in a temporary home), what's installed, the key table, and the pages for each set of
features. No window, no container.

  tour/test/tour-test.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
TOUR = os.path.dirname(HERE)
sys.path.insert(0, TOUR)
import ft_tour

failed = 0


def check(what, got, want):
    global failed
    ok = got == want
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"))


tmp = tempfile.mkdtemp(prefix="ft-tour-test-")

# ---------------------------------------------------------------- the flag

flag = os.path.join(tmp, "state", "tour.json")
check("no flag: not done", ft_tour.is_done(flag), False)
ft_tour.mark_done(flag, how="skipped")
check("marked: done", ft_tour.is_done(flag), True)
with open(flag) as f:
    saved = json.load(f)
check("flag says how", saved["how"], "skipped")
check("flag has the tour's version", saved["version"], ft_tour.TOUR_VERSION)
with open(flag, "w") as f:
    json.dump({"version": ft_tour.TOUR_VERSION - 1}, f)
check("flag from an older tour: not done", ft_tour.is_done(flag), False)
with open(flag, "w") as f:
    f.write("not json")
check("broken flag: not done", ft_tour.is_done(flag), False)
with open(flag, "w") as f:
    f.write("[]")
check("flag that isn't an object: not done", ft_tour.is_done(flag), False)
ft_tour.reset(flag)
check("reset: not done", ft_tour.is_done(flag), False)
ft_tour.reset(flag)
check("reset twice is fine", os.path.exists(flag), False)

# The CLI and the wrapper, with a home of their own (DONE_PATH is under ~).
home = os.path.join(tmp, "home")
os.makedirs(home)
env = dict(os.environ, HOME=home)
home_flag = os.path.join(home, ".local", "state", "frametop", "tour.json")
run = lambda *argv: subprocess.run(argv, env=env, capture_output=True, text=True, timeout=30, check=False)
check("cli done: not yet", run(sys.executable, "-I", os.path.join(TOUR, "ft_tour.py"), "done").returncode, 1)
os.makedirs(os.path.dirname(home_flag))
ft_tour.mark_done(home_flag)
check("cli done: done", run(sys.executable, "-I", os.path.join(TOUR, "ft_tour.py"), "done").returncode, 0)
wrapper = os.path.join(TOUR, "ft-tour")
if os.path.exists("/usr/bin/python3"):
    r = run(wrapper, "--first-run")
    check("ft-tour --first-run when done: exits at once", (r.returncode, r.stdout, r.stderr), (0, "", ""))
    r = run(wrapper, "--reset")
    check("ft-tour --reset", (r.returncode, os.path.exists(home_flag)), (0, False))
r = run(wrapper, "--what")
check("ft-tour with a wrong option", r.returncode, 2)

# ---------------------------------------------------------------- what's here

units = os.path.join(tmp, "units")
os.makedirs(units)
f = ft_tour.features(units, conf={})
check("nothing installed", (f["gaze"], f["hands"], f["voice"], f["gazeOn"], f["handsPointer"], f["floats"]),
      (False, False, False, False, False, True))
for unit in ("frametop-gaze.service", "frametop-hands.service", "frametop-voice.service"):
    open(os.path.join(units, unit), "w").close()
f = ft_tour.features(units, conf={"POINTER_GAZE": "1", "POINTER_HANDS": "1", "FLOAT_SLOTS": "0"})
check("everything installed and on, no floating windows",
      (f["gaze"], f["hands"], f["voice"], f["gazeOn"], f["handsPointer"], f["floats"]), (True, True, True, True, True, False))

conf = os.path.join(tmp, "frametop.conf")
with open(conf, "w") as fh:
    fh.write("# a comment\nPOINTER_GAZE=1   # gaze\nDASHBOARD_APPS = frametop\n")
check("read_conf", ft_tour.read_conf(conf), {"POINTER_GAZE": "1", "DASHBOARD_APPS": "frametop"})

# ---------------------------------------------------------------- the key table

names = {36: "KEY_J", 37: "KEY_K", 33: "KEY_F", 15: "KEY_TAB", 57: "KEY_SPACE", 30: "KEY_A"}
mods = {29: "Ctrl", 42: "Shift", 56: "Alt", 125: "Meta"}
right = {97: "Right Ctrl", 54: "Right Shift", 100: "Right Alt", 126: "Right Meta"}
name = lambda c: ft_tour.combo_name(c, names, mods, right)
check("Meta tap", name("125"), "Meta tap")
check("Right Ctrl tap", name("97"), "Right Ctrl tap")
check("Meta+J", name("125+36"), "Meta+J")
check("Meta+Shift+F", name("42+125+33"), "Meta+Shift+F")
check("Meta+Alt+Shift+Tab", name("42+56+125+15"), "Meta+Alt+Shift+Tab")
check("Meta+Space", name("125+57"), "Meta+Space")
check("unknown code", name("125+999"), "Meta+999")

labels = {"steam_menu": "Steam menu", "gaze_left": "Left click at the gaze", "frunner": "Frunner",
          "float_toggle": "Float a window", "dictate": "Dictate"}
bound = {"125": "steam_menu", "125+36": "gaze_left", "42+125+33": "float_toggle", "125+57": "frunner",
         "97": "dictate", "125+30": "command:konsole", "125+37": "none"}
rows = ft_tour.bindings(bound, lambda a: labels.get(a, a), name)
check("key table: everyday ones first, none left out", [r["action"] for r in rows],
      ["Steam menu", "Frunner", "Left click at the gaze", "Float a window", "Dictate", "command:konsole"])
check("key table row", rows[0], {"combo": "Meta tap", "action": "Steam menu"})

# ---------------------------------------------------------------- the pages

KNOWN = {"frunner", "spin", "remote", "gaze:calibrate", "gaze:quickcal"}
INPUT = {"devices", "buttons", "controllers", "games", "keyboard", "voice", "pointer", "ignore", "gaze", "hands", "bluetooth"}
DISPLAY = {"screens", "layout", "visibility", "power"}


def known(action):
    kind, _, page = action.partition(":")
    return action in KNOWN or (kind == "input" and page in INPUT) or (kind == "display" and page in DISPLAY)


none = ft_tour.features(os.path.join(tmp, "empty"), conf={})
everything = ft_tour.features(units, conf={"POINTER_GAZE": "1", "POINTER_HANDS": "1"})
bare, full = ft_tour.pages(none, rows), ft_tour.pages(everything, rows)
ids = [p["id"] for p in full]
check("pages, in order", ids, ["welcome", "screens", "floating", "frunner", "keys", "gaze", "hands", "hands-far",
                               "pinning", "profiles", "pause", "settings", "more", "done"])
check("the same pages with nothing installed", [p["id"] for p in bare], ids)
for pages in (bare, full):
    for p in pages:
        check(f"page {p['id']}: has a title, intro, icon",
              all(isinstance(p.get(k), str) and p[k] for k in ("title", "intro", "icon")), True)
        check(f"page {p['id']}: rows are headings, pairs, or pairs with an action",
              all((isinstance(r, str) and r) or (len(r) in (2, 3) and all(isinstance(x, str) and x for x in r))
                  for r in p["rows"]), True)
        check(f"page {p['id']}: a heading isn't last", not p["rows"] or not isinstance(p["rows"][-1], str), True)
        check(f"page {p['id']}: actions are known",
              [a["id"] for a in p["actions"] if not known(a["id"])]
              + [r[2] for r in p["rows"] if not isinstance(r, str) and len(r) == 3 and not known(r[2])], [])
        check(f"page {p['id']}: its videos are here", all(os.path.exists(c["file"]) and c["caption"] for c in p["clips"]),
              True)
by = {p["id"]: p for p in bare}
check("gaze not installed: says how", "gaze/run.sh install" in by["gaze"]["intro"], True)
check("gaze not installed: nothing to try", (by["gaze"]["actions"], by["gaze"].get("practice")), ([], ""))
check("hands not installed: says how", "hands/run.sh install" in by["hands"]["intro"], True)
check("gaze not installed: no gaze pinch rows", "With gaze mode on" in by["hands-far"]["rows"], False)
check("hands not installed: says how, far too", "hands/run.sh install" in by["hands-far"]["intro"], True)
check("no voice: no dictation row", any("Dictation" in r[0] for r in by["more"]["rows"]), False)
by = {p["id"]: p for p in full}
check("gaze installed: a target", by["gaze"]["practice"], "gaze")
check("gaze on: no note", by["gaze"].get("note"), None)
check("gaze rows: Meta+J and Meta+K first", by["gaze"]["rows"][:2], ["Clicking", ["Tap Meta+J, or Meta+K",
                                                                                 "A left or right click where you look"]])
check("hands and gaze: pinch at the gaze", any("look at something and pinch" in r[0].lower()
                                               for r in by["hands-far"]["rows"] if not isinstance(r, str)), True)
check("hands far: a target", by["hands-far"]["practice"], "hands")
check("the pages with videos", [p["id"] for p in full if p["clips"]],
      ["screens", "floating", "frunner", "gaze", "hands", "hands-far"])
check("settings: a row for every Input Settings page",
      {r[2][6:] for r in by["settings"]["rows"] if not isinstance(r, str) and r[2].startswith("input:")}, INPUT)
check("settings: a row for every Display Settings page",
      {r[2][8:] for r in by["settings"]["rows"] if not isinstance(r, str) and r[2].startswith("display:")}, DISPLAY)
check("hands installed: a target", by["hands"]["practice"], "hands")
check("every clip in tour/clips is shown somewhere",
      sorted(n[:-5] for n in os.listdir(os.path.join(TOUR, "clips")) if n.endswith(".webp")),
      sorted(os.path.basename(c["file"])[:-5] for p in full for c in p["clips"]))
check("voice: dictation row first", by["more"]["rows"][0][0].startswith("Dictation"), True)
check("keyboard page has the key table", by["keys"]["keys"], rows)
check("floating page: the dashboard choice", by["floating"]["toggle"], "dashboardApps")
check("pause page: one controller", any("one controller" in r[0] for r in by["pause"]["rows"]), True)
off = {p["id"]: p for p in ft_tour.pages(ft_tour.features(units, conf={"FLOAT_SLOTS": "0"}), rows)}
check("gaze installed, off: a note", "off" in off["gaze"]["note"], True)
check("hands installed, off: a note", "off" in off["hands"]["note"], True)
check("no floating windows: no dashboard choice", off["floating"]["toggle"], "")

print("all ok" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
