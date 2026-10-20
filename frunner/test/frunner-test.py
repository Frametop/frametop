#!/usr/bin/env python3
"""Frunner, offline: the search's ranking (spotsearch.py), then ft-frunner itself against a
fake ft-screens and ft-floatd (sockets of its own, so it's safe next to the live desktop), with
made-up apps and remote displays in a temporary home. No panel, no SteamVR.

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
for name, body in {
    "firefox.desktop": "Name=Firefox\nGenericName=Web Browser\nExec=true\nActions=new-window;new-private-window;\n"
                       "[Desktop Action new-window]\nName=New Window\nExec=true\n"
                       "[Desktop Action new-private-window]\nName=New Private Window\nExec=true\n",
    "editor.desktop": "Name=Editor\nGenericName=Text Editor\nExec=true\n",
    "hidden.desktop": "Name=Hidden Thing\nExec=true\nNoDisplay=true\n",
}.items():
    head, _, actions = body.partition("[")
    with open(os.path.join(apps, name), "w") as f:
        f.write("[Desktop Entry]\nType=Application\n" + head + ("[" + actions if actions else ""))
with open(os.path.join(home, ".config", "frametop-layout.json"), "w") as f:
    json.dump({"screens": [{}, {}], "hosts": [
        {"name": "desk-pc", "address": "10.0.0.2", "displays": [
            {"id": "desk-oled", "label": "OLED", "client": "c", "app": "primary"},
            {"id": "desk-side", "label": "Side", "client": "c", "app": "primary", "hidden": True}]}]}, f)


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


screens = Fake(f"{tag}_screens", screens_answer)
floatd = Fake(f"{tag}_float", float_answer)
env = dict(os.environ, HOME=home, XDG_DATA_HOME=os.path.join(home, "share"), XDG_DATA_DIRS=os.path.join(home, "none"),
           XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_CACHE_HOME=os.path.join(home, ".cache"))
daemon = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(HERE), "ft_frunner.py"), "--no-panel",
                           "--socket", f"{tag}_sw", "--screens", f"{tag}_screens", "--float", f"{tag}_float"],
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
    type_text("desk")
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

    with open(os.path.join(home, ".config", "frametop-frunner.json")) as f:
        counts = json.load(f)["usage"]
    check("usage is counted", sorted(counts), ["editor", "firefox", "remote:desk-oled"])
finally:
    daemon.terminate()
    out = daemon.communicate(timeout=5)[0]
    screens.stop = floatd.stop = True
if failed:
    print(out)
print("all passed" if not failed else f"{failed} failed")
sys.exit(1 if failed else 0)
