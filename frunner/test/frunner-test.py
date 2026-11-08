#!/usr/bin/env python3
"""Frunner, offline: the search's ranking (spotsearch.py), then ft-frunner itself against a
fake ft-screens and ft-floatd (sockets of its own, so it's safe next to the live desktop), with
made-up apps and remote displays in a temporary home, and a fake ft-layout for a PC's apps and
windows. No panel, no SteamVR.

  frunner/test/frunner-test.py
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import spotsearch  # noqa: E402

failed = 0


def check(what, got, want):
    global failed
    ok = got == want
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"))


# ---------------------------------------------------------------- the ranking

Item = spotsearch.Item
items = [Item("app", "firefox", "Firefox", "Web Browser", ["internet", "www"], "firefox"),
         Item("app", "org.kde.konsole", "Konsole", "Terminal", ["shell", "cmd"], "konsole"),
         Item("app", "code", "Visual Studio Code", "Text Editor", ["ide"], "code"),
         Item("app", "org.kde.kate", "Kate", "Advanced Text Editor", [], "kate"),
         Item("app", "google-chrome", "Google Chrome", "Web Browser", [], "google-chrome-stable"),
         Item("remote", "remote:desk-oled", "desk-pc - OLED", "Remote display", ["remote", "display"])]


def titles(q, usage=None):
    return [i.title for i in spotsearch.search(items, q, usage, now=1000)]


check("a name's start comes first", titles("fi")[:1], ["Firefox"])
check("a word's start: chrome finds Google Chrome", titles("chrome")[:1], ["Google Chrome"])
check("initials: vsc finds Visual Studio Code", titles("vsc")[:1], ["Visual Studio Code"])
check("a generic name: terminal finds Konsole", titles("terminal"), ["Konsole"])
check("a keyword: www finds Firefox", titles("www"), ["Firefox"])
check("fuzzy, in order: frfx finds Firefox", titles("frfx"), ["Firefox"])
check("keywords don't match fuzzily", titles("wbx"), [])
check("several words must each match", titles("google chr"), ["Google Chrome"])
check("...and none that doesn't", titles("google zzz"), [])
check("a remote display by its host and name", titles("desk oled"), ["desk-pc - OLED"])
check("an empty query finds nothing", titles("  "), [])
check("accents fold: kâte finds Kate", titles("kâte")[:1], ["Kate"])
both = titles("text editor")
check("text editor: both editors", sorted(both), ["Kate", "Visual Studio Code"])
usage = {"code": {"count": 10, "last": 990}}
check("what's used often and lately comes first", titles("text editor", usage)[:1], ["Visual Studio Code"])
check("use() counts", spotsearch.use({}, "x", now=5), {"x": {"count": 1, "last": 5}})
check("drop_word", (spotsearch.drop_word("hello world"), spotsearch.drop_word("hello "), spotsearch.drop_word("")),
      ("hello ", "", ""))
check("US text fallback", (spotsearch.US_TEXT[30], spotsearch.US_TEXT[57]), (("a", "A"), (" ", " ")))

# ---------------------------------------------------------------- ft-frunner

try:
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import Gio  # noqa: F401
# AttributeError: a "gi" that isn't PyGObject (the release image's test stage has one).
except (ImportError, ValueError, AttributeError):
    print("skip  ft-frunner itself: no PyGObject here")
    sys.exit(1 if failed else 0)

tag = f"ftsw{os.getpid()}"
home = tempfile.mkdtemp(prefix="ft-frunner-test-")
apps = os.path.join(home, "share", "applications")
os.makedirs(apps)
os.makedirs(os.path.join(home, ".config"))
os.makedirs(os.path.join(home, "layout"))
with open(os.path.join(home, "layout", "ft-layout"), "w") as f:
    f.write("#!/bin/sh\n")
os.chmod(os.path.join(home, "layout", "ft-layout"), 0o755)
for name, body in {
    "firefox.desktop": "Name=Firefox\nGenericName=Web Browser\nExec=true\nActions=new-window;new-private-window;\n"
                       "[Desktop Action new-window]\nName=New Window\nExec=true\n"
                       "[Desktop Action new-private-window]\nName=New Private Window\nExec=true\n",
    "editor.desktop": "Name=Editor\nGenericName=Text Editor\nExec=true\n",
    "hidden.desktop": "Name=Hidden Thing\nExec=true\nNoDisplay=true\n",
    # what ft-layout write_launchers writes for a profile (Gio skips it unless its program exists)
    "frametop-profile-work-desk.desktop": f"Name=Frametop: Work desk\nExec={home}/layout/ft-layout open "
                                          "\"Work desk\"\nX-Frametop-Profile=true\n",
}.items():
    head, _, actions = body.partition("[")
    with open(os.path.join(apps, name), "w") as f:
        f.write("[Desktop Entry]\nType=Application\n" + head + ("[" + actions if actions else ""))
with open(os.path.join(home, ".config", "frametop-commands.json"), "w") as f:
    json.dump({"commands": [{"name": "stop-framecorder", "run": f"echo ran > {home}/ran"}]}, f)
with open(os.path.join(home, ".config", "frametop-layout.json"), "w") as f:
    json.dump({"screens": [{}, {}], "hosts": [
        {"name": "desk-pc", "address": "10.0.0.2", "displays": [
            {"id": "desk-oled", "label": "OLED", "client": "c", "app": "primary"},
            {"id": "desk-side", "label": "Side", "client": "c", "app": "primary", "hidden": True}]},
        {"name": "oma7", "address": "192.0.2.30", "windows": {"port": 48989}, "displays": [
            {"id": "oma7-window-55aa20", "label": "GIMP", "client": "frametop-oma7-win1", "app": "window:0x55aa20",
             "window": True, "port": 48989, "screen": 102}]}]}, f)
# ft-layout window, faked: oma7's catalog, and launch adding a window panel to the layout as the real one does
FAKE_LAYOUT = os.path.join(home, "fake-ft-layout")
with open(FAKE_LAYOUT, "w") as f:
    f.write(f"""#!{sys.executable}
import json, sys
args = sys.argv[1:]
with open({os.path.join(home, "layout-calls")!r}, "a") as f:
    f.write(" ".join(args) + "\\n")
if args[:2] == ["window", "catalog"]:
    print("forgot oma7-window-1: its window closed on oma7")
    print(json.dumps({{"host": "oma7", "locked": False, "apps": [{{"id": "foot.desktop", "name": "Foot", "icon": "foot"}},
                                                               {{"id": "broken.desktop", "name": "Broken"}}],
                      "windows": [{{"address": "0x55aa10", "class": "foot", "title": "a terminal", "output": None,
                                   "panel": None, "screen": None}},
                                  {{"address": "0x55aa20", "class": "gimp", "title": "GIMP", "output": "FT-W1",
                                   "panel": "oma7-window-55aa20", "screen": 102}}]}}))
elif args[:2] == ["window", "launch"] and args[3] == "broken.desktop":
    print("error: oma7: no app broken.desktop")
    sys.exit(1)
elif args[:2] == ["window", "launch"]:
    path = {os.path.join(home, ".config", "frametop-layout.json")!r}
    layout = json.load(open(path))
    layout["hosts"][1]["displays"].append({{"id": "oma7-window-77", "client": "frametop-oma7-win2", "app": "window:0x77",
                                           "window": True, "port": 48989, "screen": 107}})
    json.dump(layout, open(path, "w"))
    print("arranged 1 remote display(s)")
    print("oma7-window-77")
""")
os.chmod(FAKE_LAYOUT, 0o755)


class Fake:
    """A socket that answers like ft-screens or ft-floatd and keeps what it got."""

    def __init__(self, name, answer):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("\0" + name)
        self.sock.settimeout(0.1)
        self.answer, self.got, self.stop = answer, [], False
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while not self.stop:
            try:
                data, sender = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            text = data.decode()
            self.got.append(text)
            reply = self.answer(text)
            if sender and reply is not None:
                self.sock.sendto(reply.encode(), sender)

    def take(self):
        got, self.got = self.got, []
        return got


pins = {}


def screens_answer(text):
    if text == "remotes":
        return "ok 1 101:c:live:2560x1440"
    if text == "screens":
        return "ok 2 1:1920x1080:1.600 2:1920x1080:1.600"
    if text == "head":
        return "ok 0 1.6 0 0.00"
    if text.startswith("get "):
        n = int(text.split()[1])
        x = {1: -1.0, 2: 0.0}.get(n, 0.5)  # screen 2 is straight ahead (-z)
        return f"ok {x} 1.6 -1.5  1 0 0  0 1 0  0 0 1  1.6 0.9 0.000 {pins.get(n, 'none')}"
    if text.startswith("spin to "):
        return "error pinned" if pins.get(int(text.split()[2]), "none") != "none" else "ok 12.0"
    if text == "vrkeyboard show":
        return "ok opened"
    return "ok"


windows = [{"id": "w-old", "app": "firefox", "class": "firefox", "title": "Old tab", "panel": 1, "floating": False,
            "minimized": False, "activeAt": 100},
           {"id": "w-new", "app": "firefox", "class": "firefox", "title": "New tab", "panel": 4, "floating": True,
            "minimized": False, "activeAt": 200}]


def float_answer(text):
    if text == "spotlist":
        return "ok " + json.dumps(windows)
    return "ok"


def relay_answer(text):
    if text == "action pause_toggle":
        return json.dumps({"t": "error", "error": "paused"})
    return json.dumps({"t": "action", "action": text.split()[1]})


screens = Fake(f"{tag}_screens", screens_answer)
floatd = Fake(f"{tag}_float", float_answer)
relay = Fake(f"{tag}_relay", relay_answer)
env = dict(os.environ, HOME=home, XDG_DATA_HOME=os.path.join(home, "share"), XDG_DATA_DIRS=os.path.join(home, "none"),
           XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_CACHE_HOME=os.path.join(home, ".cache"),
           FT_COMMANDS_NO_SYSTEMD="1", FT_FRUNNER_LAYOUT=FAKE_LAYOUT)
daemon = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(HERE), "ft_frunner.py"), "--no-panel",
                           "--socket", f"{tag}_sw", "--screens", f"{tag}_screens", "--float", f"{tag}_float", "--relay", f"{tag}_relay"],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
me = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
me.bind("")
me.settimeout(2)


def ask(text):
    me.sendto(text.encode(), f"\0{tag}_sw")
    return me.recv(4096).decode()


def tell(text):
    me.sendto(text.encode(), f"\0{tag}_sw")
    time.sleep(0.05)


KEYS = {c: code for code, (c, _) in spotsearch.US_TEXT.items()}


def type_text(text):
    for ch in text:
        tell(f"key {KEYS[ch]} 1")
        tell(f"key {KEYS[ch]} 0")


def press(*codes):
    for c in codes:
        tell(f"key {c} 1")
    for c in reversed(codes):
        tell(f"key {c} 0")


ENTER, ESC, SHIFT, DOWN, UP, TAB, BACKSPACE = 28, 1, 42, 108, 103, 15, 14
try:
    for _ in range(50):  # until it's up
        try:
            if ask("status").startswith("ok"):
                break
        except OSError:
            time.sleep(0.1)
    check("starts closed", ask("status").split()[1], "closed")
    check("toggle opens it", ask("toggle keyboard"), "ok")
    check("...and takes the typing from ft-screens", [m for m in screens.take() if m.startswith("frunner")],
          ["frunner on"])
    time.sleep(0.2)
    type_text("fir")
    check("typing goes into the query", ask("status"), "ok open list fir")
    press(BACKSPACE)
    check("Backspace", ask("status"), "ok open list fi")
    type_text("x")
    tell(f"key {BACKSPACE} 1")
    tell("keys release")  # a relay that restarted with Backspace down
    time.sleep(0.6)  # past REPEAT_DELAY_MS: a held key would have repeated by now
    check("keys release: a held Backspace comes up, and doesn't repeat", ask("status"), "ok open list fi")
    tell(f"key {BACKSPACE} 0")
    type_text("r")
    floatd.take()
    screens.take()
    press(ENTER)
    check("Enter on a running app: its most recent window is activated", floatd.take(), ["activate w-new"])
    got = screens.take()
    check("...typing back from frunner, and the lazy susan spins its panel to the front",
          [m for m in got if m.startswith(("frunner", "spin"))], ["frunner off", "spin to 4"])
    check("...and it closed", ask("status").split()[1], "closed")

    ask("open")
    type_text("edi")
    floatd.take()
    press(ENTER)
    check("Enter on an app that isn't running: it launches, floating", floatd.take(), ["launch editor.desktop"])

    ask("open")
    type_text("hidden")
    check("NoDisplay apps aren't found", json.loads(ask("rows")[3:]), [])
    press(ESC)
    check("Esc closes", ask("status").split()[1], "closed")

    ask("open")
    time.sleep(0.2)
    type_text("firefox")
    press(SHIFT, ENTER)
    check("Shift+Enter opens the app's menu", json.loads(ask("rows")[3:]),
          ["Firefox", "New Window", "New Private Window", "Open new window", "Open docked on this screen", "Windows",
           "New tab", "Old tab"])
    press(DOWN)
    floatd.take()
    press(ENTER)
    check("its own actions: New Private Window", floatd.take(), ["launch firefox.desktop action=new-private-window"])

    ask("open")
    time.sleep(0.2)
    type_text("firefox")
    press(SHIFT, ENTER)
    for _ in range(2):
        press(DOWN)
    floatd.take()
    press(ENTER)
    check("Open new window: the app's own new-window action", floatd.take(),
          ["launch firefox.desktop action=new-window"])

    ask("open")
    time.sleep(0.2)
    type_text("firefox")
    press(SHIFT, ENTER)
    for _ in range(3):
        press(DOWN)
    floatd.take()
    press(ENTER)
    check("Open docked on this screen: the screen straight ahead", floatd.take(), ["launch firefox.desktop screen=2"])

    ask("open")
    time.sleep(0.2)
    type_text("firefox")
    press(SHIFT, ENTER)
    press(UP)
    press(ESC)
    check("Esc in a menu goes back to the results", ask("status"), "ok open list firefox")
    press(SHIFT, ENTER)
    for _ in range(5):
        press(DOWN)  # past the Windows section to the second window
    floatd.take()
    screens.take()
    press(ENTER)
    check("a window from the menu: that one", floatd.take(), ["activate w-old"])
    check("...spun to", [m for m in screens.take() if m.startswith("spin")], ["spin to 1"])

    ask("open")
    type_text("desk-pc")
    check("remote displays as {host} - {display}, not the hidden one", json.loads(ask("rows")[3:]), ["desk-pc - OLED"])
    screens.take()
    press(ENTER)
    check("Enter on a connected remote display: spun to", [m for m in screens.take() if m.startswith("spin")],
          ["spin to 101"])

    pins[4] = "left"
    ask("open")
    type_text("firefox")
    screens.take()
    press(ENTER)
    got = screens.take()
    check("a pinned panel: typing goes there where it is", [m for m in got if m.startswith(("spin", "front"))],
          ["spin to 4", "front 4"])
    pins.clear()

    ask("toggle mouse")
    check("from a mouse or controller: Frametop's keyboard opens too", "vrkeyboard show" in screens.take(), True)
    ask("toggle mouse")
    check("...and closes with it", "vrkeyboard close" in screens.take(), True)

    ask("open")
    tell("dismiss click")
    check("ft-screens' dismiss closes it", ask("status").split()[1], "closed")
    check("...without telling ft-screens again", [m for m in screens.take() if m == "frunner off"], [])

    ask("open")
    type_text("fi")
    tell("click 0")
    check("a click on a row opens it", ask("status").split()[1], "closed")
    ask("open")
    tell("outside")
    check("a click beside it closes it", ask("status").split()[1], "closed")

    ask("open")
    type_text("work desk")
    check("a profile's launcher is found as a profile", json.loads(ask("rows")[3:]), ["Frametop: Work desk"])
    press(SHIFT, ENTER)
    check("...its menu opens the profile (through ft-layout, not ft-floatd)", json.loads(ask("rows")[3:]),
          ["Frametop: Work desk", "Open profile"])
    press(ESC)
    press(ESC)

    ask("open")
    type_text("stop frame")
    check("your commands are found by name", json.loads(ask("rows")[3:]), ["stop-framecorder"])
    press(ENTER)
    check("Enter runs a command and closes", ask("status").split()[1], "closed")
    ran = os.path.join(home, "ran")
    for _ in range(40):
        if os.path.exists(ran):
            break
        time.sleep(0.05)
    check("...in the background", os.path.exists(ran), True)

    # The desktop's layout changes while it runs (Input Settings or System Settings): the next
    # open types on the new one. Key 21 is Y on a US layout and Z on German QWERTZ.
    ask("open")
    press(21)
    check("a US layout without kxkbrc", ask("status"), "ok open list y")
    press(ESC)
    with open(os.path.join(home, ".config", "kxkbrc"), "w") as f:
        f.write("[Layout]\nLayoutList=de\nUse=true\nVariantList=\n")
    ask("open")
    press(21)
    check("...and the desktop's new layout from the next open", ask("status"), "ok open list z")
    press(ESC)
    os.remove(os.path.join(home, ".config", "kxkbrc"))
    ask("open")
    press(21)
    check("...and back to US when it goes", ask("status"), "ok open list y")
    press(ESC)

    with open(os.path.join(home, ".config", "frametop-frunner.json")) as f:
        counts = json.load(f)["usage"]
    check("usage is counted", sorted(counts), ["command:stop-framecorder", "editor", "firefox", "remote:desk-oled"])

    # Frametop's own actions (DEE-172): in the search, and as the OS menu.
    ask("open")
    check("an empty query lists nothing", json.loads(ask("rows")[3:]), [])
    type_text("hand")
    check("Frametop's actions are found by name", json.loads(ask("rows")[3:]), ["Hand tracking on or off"])
    relay.take()
    press(ENTER)
    check("Enter asks the input relay to do it", relay.take(), ["action hands_toggle"])
    check("...and closes", ask("status").split()[1], "closed")

    screens.take()
    check("toggle ... actions opens the OS menu", ask("toggle left actions"), "ok")
    check("...Frametop's keyboard too, from a controller", "vrkeyboard show" in screens.take(), True)
    check("...with every action listed, in the menu's order", json.loads(ask("rows")[3:]),
          ["Steam menu", "Hide or show the screens", "Spin to the next panel", "Spin to the previous panel",
           "Put every floating window back", "Hand tracking on or off", "Pause for a VR game", "Let go of every key"])
    type_text("spin")
    check("typing searches as usual", json.loads(ask("rows")[3:]),
          ["Spin to the next panel", "Spin to the previous panel"])
    for _ in range(4):
        press(BACKSPACE)
    check("...and back to an empty query, the menu again", len(json.loads(ask("rows")[3:])), 8)
    press(SHIFT, ENTER)
    check("Shift+Enter: its menu", json.loads(ask("rows")[3:]), ["Steam menu", "Do it"])
    press(ESC)
    relay.take()
    press(DOWN)
    press(ENTER)
    check("Down, Enter: the second action", relay.take(), ["action screens_toggle"])
    check("...and it closed", ask("status").split()[1], "closed")
    ask("toggle keyboard actions")
    check("toggle again closes it", ask("toggle keyboard actions"), "ok closed")

    ask("open mouse actions")
    for _ in range(6):
        press(DOWN)
    relay.take()
    press(ENTER)
    check("an action the relay refuses (paused): asked once, and closed", (relay.take(), ask("status").split()[1]),
          (["action pause_toggle"], "closed"))
    ask("open")
    check("a plain open after the OS menu lists nothing again", json.loads(ask("rows")[3:]), [])
    press(ESC)

    # A PC's apps and windows (DEE-351): oma7 has a Frametop windows instance.
    calls = os.path.join(home, "layout-calls")

    def layout_calls():
        for _ in range(40):
            if os.path.exists(calls):
                break
            time.sleep(0.05)
        with open(calls) as f:
            got = f.read().splitlines()
        os.remove(calls)
        return got
    layout_calls()  # the opens before this one asked too
    ask("open")
    time.sleep(0.5)  # its catalog, asked in the background
    check("each open asks the PC for its apps and windows", layout_calls(), ["window catalog oma7"])
    type_text("foot")
    check("its apps and windows are found, by name and by the window's app", json.loads(ask("rows")[3:]),
          ["Foot", "a terminal"])
    screens.take()
    press(ENTER)
    check("Enter on a PC app launches it as a panel", layout_calls(), ["window launch oma7 foot.desktop"])
    time.sleep(0.3)
    check("...and spins to its panel", [m for m in screens.take() if m.startswith("spin")], ["spin to 107"])
    ask("open")
    time.sleep(0.3)
    layout_calls()
    type_text("terminal")
    press(ENTER)
    check("Enter on a PC window opens it as a panel", layout_calls(), ["window open oma7 0x55aa10"])
    ask("open")
    time.sleep(0.3)
    layout_calls()
    type_text("gimp")
    check("a PC window's panel isn't also listed as a remote display", json.loads(ask("rows")[3:]), ["GIMP"])
    screens.take()
    press(ENTER)
    check("Enter on a PC window with a panel spins to it", [m for m in screens.take() if m.startswith("spin")],
          ["spin to 102"])
    check("...and asks ft-layout nothing", os.path.exists(calls), False)
    ask("open")
    time.sleep(0.3)
    layout_calls()
    type_text("gimp")
    press(SHIFT, ENTER)
    check("its menu", json.loads(ask("rows")[3:]), ["GIMP", "Go to its panel", "Back to oma7"])
    press(DOWN)
    press(ENTER)
    check("Back to oma7 closes its panel", layout_calls(), ["window close oma7-window-55aa20"])
    ask("open")
    time.sleep(0.3)
    layout_calls()
    type_text("broken")
    press(ENTER)
    check("a launch the PC refuses: tried once", layout_calls(), ["window launch oma7 broken.desktop"])
finally:
    daemon.terminate()
    out = daemon.communicate(timeout=5)[0]
    screens.stop = floatd.stop = relay.stop = True
if failed:
    print(out)
print("all passed" if not failed else f"{failed} failed")
sys.exit(1 if failed else 0)
