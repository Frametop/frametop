#!/usr/bin/env python3
"""Let the 3D mouse's treadmill device move SteamVR's floating windows.

SteamVR's dashboard (vrwebhelper's systemui page) starts a floating window's move only for a
press from the head or a hand: UndockedOverlay.startFloatingWindowMove reads the pressing
device from the mouse event's CapsLock/NumLock bits and gives up on a treadmill. So the 3D
mouse, a treadmill since 2026-10-09 (docs/design.md, "Treadmill role"), clicked and resized
those windows but couldn't move them. treadmill-move.js wraps the grab bar's handler so a press
with no device moves the window with /user/treadmill. This puts it into the dashboard over the
Chrome DevTools Protocol, again whenever the dashboard's page is new (SteamVR restarted).

It also tells ft-screens (@ft_screens, "steamvr <gamepad> <theater app>") what only the
dashboard's page knows: whether SteamVR's controllers are in Gamepad mode (Enter Gamepad Mode, so
the pad goes to a game), and which Steam game shows in a theater window (a flatscreen game, which
isn't a scene app). ft-screens then keeps its panels from turning SteamVR's laser on while the pad
is in a game (screens/vr.cpp, "controllers on the screens"). That's read only, five times a
second, on one connection.

The dashboard's debugger is off by default. SteamVR turns it on with VRWebHelper.DebuggerEnabled
and listens on VRWebHelper.DebuggerPort, on 127.0.0.1 only. Not 8087: SteamOS's
steamvr-web-debug-portforward.service forwards 0.0.0.0:8088 to 127.0.0.1:8087, which would put
the dashboard's debugger on the network. Any local program can use the port, as with Steam's
own on 8080 (steam/steamui.py).

  pointer/dashpatch/ft-dashpatch.py on      # turn the debugger on (from SteamVR's next start)
  pointer/dashpatch/ft-dashpatch.py off     # turn it off again, if this turned it on
  pointer/dashpatch/ft-dashpatch.py status  # the settings, and whether the patch is in
  pointer/dashpatch/ft-dashpatch.py run     # keep the patch in (frametop-dashpatch.service)

With SteamVR running the settings go through vrcmd, since SteamVR keeps its settings in memory
and writes over the file; without it, they go into SteamVR's settings file.
"""
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "steam"))
from steamui import _WebSocket  # noqa: E402  (Frametop's small CDP client)

HOME = Path.home()
SECTION = "VRWebHelper"
PORT = 8089
PATCH = HERE / "treadmill-move.js"
STEAMVR_BIN = "/opt/steamvr/bin/linuxarm64"
STEAMVR_SETTINGS = (HOME / ".config/openvr/config/steamvr.vrsettings",
                    HOME / ".steam/steam/config/steamvr.vrsettings")
# There when this turned the debugger on, so `off` leaves a debugger someone else turned on alone.
MARK = HOME / ".local/state/frametop/pointer/dashboard-debugger"
# A terminal in the desktop has the desktop's own config folder; vrcmd needs SteamVR's.
ENV = dict(os.environ, XDG_CONFIG_HOME=str(HOME / ".config"), LD_LIBRARY_PATH=STEAMVR_BIN)


def steamvr_up():
    return subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0


def vrcmd(*args):
    return subprocess.run([f"{STEAMVR_BIN}/vrcmd", *args], capture_output=True, text=True, timeout=15, env=ENV).stdout


def settings_file():
    return next((p for p in STEAMVR_SETTINGS if p.exists()), STEAMVR_SETTINGS[0])


def get():
    """(DebuggerEnabled, DebuggerPort): a bool or None, an int or None."""
    if steamvr_up():
        on = re.search(rf"^{SECTION}\.DebuggerEnabled=(\w+)", vrcmd("--settings-bool", f"{SECTION}.DebuggerEnabled"), re.M)
        port = re.search(rf"^{SECTION}\.DebuggerPort=(-?\d+)", vrcmd("--settings-int", f"{SECTION}.DebuggerPort"), re.M)
        return (on.group(1) in ("true", "1") if on else None), (int(port.group(1)) if port else None)
    try:
        s = json.loads(settings_file().read_text()).get(SECTION, {})
    except (OSError, ValueError, AttributeError):
        return None, None
    on, port = s.get("DebuggerEnabled"), s.get("DebuggerPort")
    return (on if isinstance(on, bool) else None), (port if isinstance(port, int) else None)


def put(on):
    if steamvr_up():
        # 1 or 0: vrcmd reads "true" as 0.
        vrcmd("--set-settings-bool", f"{SECTION}.DebuggerEnabled", "1" if on else "0")
        if on:
            vrcmd("--set-settings-int", f"{SECTION}.DebuggerPort", str(PORT))
        got_on, got_port = get()
        return got_on is on and (not on or got_port == PORT)
    path = settings_file()
    try:
        d = json.loads(path.read_text())
    except FileNotFoundError:
        d = {}
    except (OSError, ValueError) as e:
        print(f"can't read {path}: {e}", file=sys.stderr)
        return False
    s = d.setdefault(SECTION, {})
    s["DebuggerEnabled"] = on
    if on:
        s["DebuggerPort"] = PORT
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".frametop-tmp")
    tmp.write_text(json.dumps(d, indent=3) + "\n")
    tmp.replace(path)
    return True


def dashboard():
    """The systemui page's debugger URL, or None (SteamVR not up, or its debugger off)."""
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json", timeout=3) as r:
            targets = json.load(r)
    except (OSError, ValueError):
        return None
    return next((t.get("webSocketDebuggerUrl") for t in targets if t.get("title") == "systemui"), None)


def evaluate(url, expression):
    ws = _WebSocket(url)
    try:
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                            "params": {"expression": expression, "returnByValue": True}}))
        while True:
            reply = json.loads(ws.recv())
            if reply.get("id") == 1:
                break
    finally:
        ws.close()
    result = reply.get("result", {})
    if "exceptionDetails" in result:
        d = result["exceptionDetails"]
        return "error: " + (d.get("exception", {}).get("description") or d.get("text", "?"))
    return result.get("result", {}).get("value")


STATE = "JSON.stringify(window.__ftTreadmillMove || null)"

SCREENS = "\0ft_screens"
# SteamVR's system panel interaction mode (2 = Gamepad), and the app id of a Steam game in a
# theater window (0 for none). Read only. The game is gamescope's overlay
# valve.steam.desktopgame.<app id> (desktopgame.0 is always there), for as long as it runs: not
# its frame's page, which turns into Steam's own (the Steam UI's app id) while the Steam menu is used.
# Gamepad mode with a Frametop panel on top of SteamVR's focus stack (a Frametop screen or the Full
# Keyboard, clicked last) counts as laser mode: SteamVR goes to Gamepad mode by itself when the
# system focus goes (the Steam menu closing), and the pad can't drive a Frametop screen, so the
# screens turning the laser back on is what gets the controllers back (2026-10-25: stuck in Gamepad
# mode on a clicked screen, the grips dead, until the Steam menu opened). Not frametop.gamepad: the
# dashboard overlay that takes the pad for a Frametop screen (in the works).
STEAMVR_STATE = """(() => {
  const f = window.inputFocusStore && window.inputFocusStore.computedInputFocus;
  if (!f || !window.OverlayStore) return '';
  let app = 0;
  for (const o of window.OverlayStore.m_mapAllOverlays.values()) {
    const m = /^valve\\.steam\\.desktopgame\\.(\\d+)$/.exec(o.sOverlayKey || '');
    if (m && +m[1] > 0) { app = +m[1]; break; }
  }
  let ours = false;
  const top = f.m_entryAtTopOfStack && f.m_entryAtTopOfStack.m_unPanelSGID;
  for (const o of window.OverlayStore.m_mapAllOverlays.values())
    if (top && o.unStandalonePanelSGID == top) ours = /^(frametop\\.|org\\.framekeyboard\\.)/.test(o.sOverlayKey || '')
      && !/^frametop\\.(probe\\.)?gamepad/.test(o.sOverlayKey || '');
  return (f.m_eSystemPanelInteractionMode == 2 && !ours ? 1 : 0) + ' ' + app;
})()"""


def watch_steamvr():
    """Tells ft-screens SteamVR's Gamepad mode and theater game: on change and every second."""
    out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    ws, rid, last, sent = None, 0, None, 0.0
    while True:
        time.sleep(0.2)
        try:
            if ws is None:
                url = dashboard()
                if not url:
                    time.sleep(3)
                    continue
                ws = _WebSocket(url)
            rid += 1
            ws.send(json.dumps({"id": rid, "method": "Runtime.evaluate",
                                "params": {"expression": STEAMVR_STATE, "returnByValue": True}}))
            while True:
                reply = json.loads(ws.recv())
                if reply.get("id") == rid:
                    break
            value = reply.get("result", {}).get("result", {}).get("value")
        except (OSError, ValueError):
            if ws is not None:
                try:
                    ws.close()
                except OSError:
                    pass
            ws = None
            continue
        if not isinstance(value, str) or not re.fullmatch(r"[01] \d+", value):
            continue
        now = time.monotonic()
        if value != last or now - sent >= 1:
            if value != last:
                gamepad, app = value.split()
                print(f"SteamVR: {'gamepad' if gamepad == '1' else 'laser'} mode, "
                      f"{'theater game ' + app if app != '0' else 'no theater game'}", flush=True)
            try:
                out.sendto(f"steamvr {value}".encode(), SCREENS)
            except OSError:
                pass  # the desktop isn't running
            last, sent = value, now


def run():
    """Puts the patch in each new dashboard page, and checks it's still in every 30 s (the page
    can reload without a new URL)."""
    patch = PATCH.read_text()
    version = int(re.search(r"const VERSION = (\d+);", patch).group(1))
    last_url, last_check = None, 0.0
    threading.Thread(target=watch_steamvr, daemon=True).start()
    while True:
        url = dashboard()
        now = time.monotonic()
        if url and (url != last_url or now - last_check > 30):
            try:
                state = json.loads(evaluate(url, STATE) or "null")
                if not isinstance(state, dict) or state.get("version", 0) < version:
                    print(f"SteamVR's dashboard: {evaluate(url, patch)}", flush=True)
                last_url, last_check = url, now
            except (OSError, ValueError) as e:
                print(f"SteamVR's dashboard: can't reach it yet ({e})", flush=True)
                last_url = None
        time.sleep(3)


def main():
    what = sys.argv[1] if len(sys.argv) == 2 else ""
    if what == "on":
        on, port = get()
        if on and port == PORT:
            print(f"SteamVR's dashboard debugger: on already, on port {PORT}")
            return 0
        if not put(True):
            print("couldn't turn on SteamVR's dashboard debugger: the 3D mouse can't move SteamVR's windows. "
                  "Try again with: pointer/dashpatch/ft-dashpatch.py on", file=sys.stderr)
            return 1
        if not on:
            MARK.parent.mkdir(parents=True, exist_ok=True)
            MARK.write_text(f"{SECTION}.DebuggerEnabled was off; Frametop turned it on (port {PORT})\n")
        print(f"SteamVR's dashboard debugger: on, on 127.0.0.1:{PORT}, once SteamVR restarts "
              "(so the 3D mouse can move SteamVR's windows)")
        return 0
    if what == "off":
        if not MARK.exists():
            print("SteamVR's dashboard debugger: left as it is (Frametop didn't turn it on)")
            return 0
        if not put(False):
            print("couldn't turn off SteamVR's dashboard debugger. Try again with: "
                  "pointer/dashpatch/ft-dashpatch.py off", file=sys.stderr)
            return 1
        MARK.unlink()
        print("SteamVR's dashboard debugger: off again, once SteamVR restarts")
        return 0
    if what == "status":
        on, port = get()
        print(f"{SECTION}.DebuggerEnabled: {'on' if on else 'off'}" + (" (set by Frametop)" if MARK.exists() else "")
              + f", DebuggerPort: {port if port is not None else 'unset'}")
        url = dashboard()
        if not url:
            print(f"dashboard: not reachable on 127.0.0.1:{PORT}")
        else:
            try:
                print(f"dashboard patch: {evaluate(url, STATE)}")
            except (OSError, ValueError) as e:
                print(f"dashboard patch: unknown ({e})")
        return 0
    if what == "run":
        run()
        return 0
    print("usage: pointer/dashpatch/ft-dashpatch.py on|off|status|run", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
