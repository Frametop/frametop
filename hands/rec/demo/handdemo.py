#!/usr/bin/env python3
"""ft-handdemo: records the hand recorder's demo clips, one per part of parts.json, as the headset
shows them (Room View passthrough, from SteamVR's headset view on /dev/video99 through vr-record).

  hands/rec/demo/ft-handdemo [--out DIR] [--from PART] [--here] [--dry]

The window shows the next part and the motions to make. The headset's right-side button:
  press   start: 3, 2, 1, then the panels hide and the recording starts (say the part's number);
          during a recording: stop it and go on to the next part
  twice   during a recording: stop it and record the same part again; otherwise back one part
  hold    end the session (a recording running is kept)
The window's buttons do the same.

While a take records, nothing but passthrough and your hands is in view: Frametop's panels hide
(ft-screens "pause on"), hand tracking is parked (no drawn hands, no laser) and gaze mode is off.
Each comes back as it was after the take, and when the app quits.

Each take is DIR/<part>-take<N>.mp4 (with the mic, for the spoken number), listed in DIR/takes.jsonl.
Trimming and the GIFs are done afterwards (sheets.py, gifs.py; README.md).

GTK 4 + libadwaita on the host's Python. By default the window opens on Frametop's desktop (its
KWin, found as ft_layout.py finds it), so it shows on a panel in the headset; --here opens it where
the app was started.
"""
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import session as hs  # noqa: E402  (hands/rec/session.py: the headset button)

UID = os.getuid()
RUN = f"/run/user/{UID}"
PARK_PATH = f"{RUN}/frametop-hands/park"   # input/game_pause.py's: ft-hands idles while it exists
VR_RECORD_STATE = f"{RUN}/vr-record"
COUNTDOWN = 3


def frametop_wayland():
    """Frametop's KWin socket as an absolute path (plasmashell's environment), or None."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() != "plasmashell":
                    continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = dict(e.split("=", 1) for e in f.read().decode(errors="replace").split("\0") if "=" in e)
        except OSError:
            continue
        run, wl = env.get("XDG_RUNTIME_DIR", ""), env.get("WAYLAND_DISPLAY")
        if run.endswith("/frametop") and wl:
            return wl if wl.startswith("/") else os.path.join(run, wl)
    return None


def host_env(**extra):
    """The environment for vr-record and friends: the host's runtime dir and bus (Frametop's desktop
    has its own XDG_RUNTIME_DIR, where pactl and pw-record find no PipeWire)."""
    env = dict(os.environ, XDG_RUNTIME_DIR=RUN, DBUS_SESSION_BUS_ADDRESS=f"unix:path={RUN}/bus")
    for k in ("XDG_CONFIG_HOME", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update(extra)
    return env


def ask(name, command, timeout=1.0):
    """A datagram to an abstract socket and its reply, or None."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.bind("")
        s.settimeout(timeout)
        s.sendto(command.encode(), "\0" + name)
        return s.recv(65536).decode(errors="replace").strip()
    except OSError:
        return None
    finally:
        s.close()


def find_vr_record():
    for p in (os.environ.get("HANDDEMO_VR_RECORD"), os.path.expanduser("~/Desktop/Projects/vr-recorder/vr-record"),
              shutil.which("vr-record")):
        if p and os.access(p, os.X_OK):
            return p
    return None


class CleanView:
    """Only passthrough and the hands in view while a take records; put back as it was after."""

    def __init__(self, log, dry=False):
        self.log, self.dry = log, dry
        self.active = False
        self.was_paused = self.gaze_was_on = self.parked = None

    def enter(self):
        if self.active or self.dry:
            self.active = True
            return
        self.active = True
        state = ask("ft_screens", "pause state")
        self.was_paused = bool(state and state.endswith("paused"))
        if state is not None and not self.was_paused:
            ask("ft_screens", "pause on")
        gaze = ask("ft_pointer_helper", "gaze ?")
        self.gaze_was_on = gaze == "ok on"
        if self.gaze_was_on:
            ask("ft_pointer_helper", "gaze off")
        self.parked = False
        if not os.path.exists(PARK_PATH):
            try:
                os.makedirs(os.path.dirname(PARK_PATH), exist_ok=True)
                open(PARK_PATH, "w").close()
                self.parked = True
            except OSError as e:
                self.log("can't park hand tracking: %s" % e)
        self.log("clean view: panels %s, gaze %s, hands %s" % (
            "already paused" if self.was_paused else "hidden" if state else "(no ft-screens)",
            "off" if self.gaze_was_on else "was off", "parked" if self.parked else "left as they were"))

    def leave(self):
        if not self.active:
            return
        self.active = False
        if self.dry:
            return
        if self.parked:
            try:
                os.unlink(PARK_PATH)
            except FileNotFoundError:
                pass
        if self.gaze_was_on:
            ask("ft_pointer_helper", "gaze on")
        if self.was_paused is False:
            ask("ft_screens", "pause off")
        self.log("clean view undone")


class Recorder:
    """One take at a time through vr-record (hardware H.264, the mic, no system audio)."""

    def __init__(self, out, log, dry=False):
        self.out, self.log, self.dry = out, log, dry
        self.tool = find_vr_record()
        self.env = host_env(VR_RECORD_DIR=out, VR_RECORD_SYS="0", VR_RECORD_MIC="1")
        self.current = None

    def start(self):
        if self.dry:
            self.current = os.path.join(self.out, "dry.mp4")
            return True
        if not self.tool:
            self.log("vr-record not found (HANDDEMO_VR_RECORD)")
            return False
        r = subprocess.run([self.tool, "start"], env=self.env, capture_output=True, text=True, timeout=20)
        try:
            with open(os.path.join(VR_RECORD_STATE, "current")) as f:
                self.current = f.read().strip()
        except OSError:
            self.current = None
        if r.returncode or not self.current:
            self.log("vr-record start failed: %s" % (r.stdout + r.stderr).strip())
            return False
        return True

    def stop(self, name):
        """Stops the take and names it DIR/<name>.mp4; returns that path or None."""
        if self.dry:
            return os.path.join(self.out, name + ".mp4")
        r = subprocess.run([self.tool, "stop"], env=self.env, capture_output=True, text=True, timeout=120)
        src, self.current = self.current, None
        if r.returncode:
            self.log("vr-record stop: %s" % (r.stdout + r.stderr).strip())
        dst = os.path.join(self.out, name + ".mp4")
        try:
            os.replace(src, dst)
            return dst
        except (OSError, TypeError) as e:
            self.log("take not found (%s): %s" % (src, e))
            return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="session folder (default ~/Videos/Recordings/hand-demos/<date time>)")
    ap.add_argument("--from", dest="start", help="start at this part (e.g. 7a)")
    ap.add_argument("--parts", default=os.path.join(HERE, "parts.json"))
    ap.add_argument("--here", action="store_true", help="open the window here, not on Frametop's desktop")
    ap.add_argument("--dry", action="store_true", help="no recording, no clean view: try the window and button")
    a = ap.parse_args()

    if not a.here:
        wl = frametop_wayland()
        if wl:
            os.environ["WAYLAND_DISPLAY"] = wl
            os.environ.pop("DISPLAY", None)
        else:
            print("Frametop's desktop isn't running: the window opens here")
    os.environ.setdefault("GSK_RENDERER", "cairo")   # a plain window; no Vulkan through zink
    os.environ.setdefault("DISABLE_GAMESCOPE_WSI", "1")

    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Adw", "1")
    from gi.repository import Adw, Gdk, GLib, Gtk

    with open(a.parts) as f:
        parts = json.load(f)["parts"]
    out = a.out or os.path.expanduser(time.strftime("~/Videos/Recordings/hand-demos/%Y-%m-%d %H-%M-%S"))
    os.makedirs(out, exist_ok=True)
    index = next((i for i, p in enumerate(parts) if p["id"] == a.start), 0) if a.start else 0

    takes_path = os.path.join(out, "takes.jsonl")
    counts = {}
    if os.path.exists(takes_path):   # carrying on in a folder: number the takes after its own
        with open(takes_path) as f:
            for line in f:
                try:
                    t = json.loads(line)
                    counts[t["part"]] = max(counts.get(t["part"], 0), t["take"])
                except (ValueError, KeyError):
                    pass

    def log(msg):
        print(time.strftime("%H:%M:%S ") + msg, flush=True)

    clean = CleanView(log, a.dry)
    rec = Recorder(out, log, a.dry)

    css = Gtk.CssProvider()
    css.load_from_string("""
        .part-id { font-size: 64pt; font-weight: 800; }
        .part-name { font-size: 26pt; font-weight: 700; }
        .motion { font-size: 20pt; }
        .phase { font-size: 24pt; font-weight: 700; }
        .phase.recording { color: #e01b24; }
        .phase.count { color: #e5a50a; }
        .small { font-size: 13pt; }
    """)

    class App(Adw.Application):
        def __init__(self):
            super().__init__(application_id="dev.frametop.HandDemo")
            self.state, self.index = "ready", index
            self.count_left, self.started = 0, None
            self.button = None

        # ---- the window
        def do_activate(self):
            Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css,
                                                      Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            self.win = Adw.ApplicationWindow(application=self, title="Hand demo recorder")
            self.win.set_default_size(1100, 760)
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14,
                          margin_top=24, margin_bottom=24, margin_start=32, margin_end=32)
            head = Gtk.Box(spacing=24)
            self.id_label = Gtk.Label(css_classes=["part-id"], xalign=0)
            self.name_label = Gtk.Label(css_classes=["part-name"], xalign=0, wrap=True, hexpand=True)
            head.append(self.id_label)
            head.append(self.name_label)
            box.append(head)
            self.motions = Gtk.Label(css_classes=["motion"], xalign=0, yalign=0, wrap=True, vexpand=True)
            box.append(self.motions)
            self.phase = Gtk.Label(css_classes=["phase"], xalign=0, wrap=True)
            box.append(self.phase)
            row = Gtk.Box(spacing=10)
            self.b_back = Gtk.Button(label="Back")
            self.b_go = Gtk.Button(label="Start", css_classes=["suggested-action", "pill"])
            self.b_redo = Gtk.Button(label="Stop and redo")
            self.b_skip = Gtk.Button(label="Skip")
            self.b_end = Gtk.Button(label="End", css_classes=["destructive-action"])
            self.b_back.connect("clicked", lambda *_: self.gesture("double"))
            self.b_go.connect("clicked", lambda *_: self.gesture("press"))
            self.b_redo.connect("clicked", lambda *_: self.gesture("double"))
            self.b_skip.connect("clicked", lambda *_: self.move(1))
            self.b_end.connect("clicked", lambda *_: self.gesture("hold"))
            for b in (self.b_back, self.b_go, self.b_redo, self.b_skip, self.b_end):
                row.append(b)
            box.append(row)
            self.info = Gtk.Label(css_classes=["small", "dim-label"], xalign=0, wrap=True)
            box.append(self.info)
            self.win.set_content(box)
            self.win.connect("close-request", lambda *_: (self.finish(), False)[1])
            self.win.present()

            path = os.environ.get("HANDDEMO_BUTTON") or hs.find_button(hs.read_input_devices())
            if path:
                self.button = hs.ButtonReader(path, lambda g: GLib.idle_add(self.gesture, g), log).start()
            GLib.timeout_add_seconds(2, self.refresh_info)
            self.show()

        def show(self):
            p = parts[self.index] if self.index < len(parts) else None
            if p is None:
                self.id_label.set_label("")
                self.name_label.set_label("All parts recorded")
                self.motions.set_label("Hold the button or press End to finish.")
            else:
                self.id_label.set_label(p["id"])
                self.name_label.set_label(p["section"] + (": " + p["title"] if p.get("title") else ""))
                self.motions.set_label("\n".join("•  " + m for m in p["motions"]))
            ph = self.phase
            for c in ("recording", "count"):
                ph.remove_css_class(c)
            if self.state == "ready":
                n = counts.get(p["id"], 0) if p else 0
                ph.set_label(("Press the side button to start" + (" (take %d)" % (n + 1) if n else ""))
                             if p else "Done")
                self.b_go.set_label("Start")
            elif self.state == "countdown":
                ph.add_css_class("count")
                ph.set_label("Starts in %d   then say “%s”" % (self.count_left, p["id"]))
                self.b_go.set_label("Cancel")
            elif self.state == "recording":
                ph.add_css_class("recording")
                ph.set_label("● Recording %s: say “%s”, do the motions, then press" % (p["id"], p["id"]))
                self.b_go.set_label("Stop, next part")
            elif self.state == "saving":
                ph.set_label("Saving…")
            self.b_go.set_sensitive(self.state != "saving" and p is not None)
            self.b_redo.set_sensitive(self.state == "recording")
            self.b_back.set_sensitive(self.state == "ready" and self.index > 0)
            self.b_skip.set_sensitive(self.state == "ready" and p is not None)
            self.refresh_info()

        def refresh_info(self):
            nxt = parts[self.index + 1] if self.index + 1 < len(parts) else None
            room = ask("ft_handview", "state", 0.3) if not a.dry else "dry run"
            bits = ["Next: %s %s" % (nxt["id"], nxt["section"]) if nxt else "Last part",
                    "button: %s" % ("ok" if self.button and self.button.ok else "not found" if not self.button else "waiting"),
                    "takes: %d" % sum(counts.values()), out]
            if room:
                bits.insert(1, "view: " + room[:80])
            self.info.set_label("   ·   ".join(bits))
            return True

        # ---- the flow
        def gesture(self, g):
            p = parts[self.index] if self.index < len(parts) else None
            log("button %s (%s, part %s)" % (g, self.state, p["id"] if p else "-"))
            if g == "hold":
                if self.state == "recording":
                    self.stop("end")
                else:
                    self.finish()
                    self.quit()
            elif self.state == "ready":
                if g == "press" and p:
                    self.count_left = COUNTDOWN
                    self.state = "countdown"
                    GLib.timeout_add_seconds(1, self.tick)
                elif g == "double":
                    self.move(-1)
            elif self.state == "countdown":
                self.state = "ready"   # any press cancels
            elif self.state == "recording":
                self.stop("next" if g == "press" else "redo")
            self.show()
            return False

        def move(self, d):
            if self.state == "ready":
                self.index = max(0, min(len(parts), self.index + d))
                self.show()

        def tick(self):
            if self.state != "countdown":
                return False
            self.count_left -= 1
            if self.count_left > 0:
                self.show()
                return True
            self.state = "saving"   # busy while it starts
            self.show()
            threading.Thread(target=self._start, daemon=True).start()
            return False

        def _start(self):
            clean.enter()
            time.sleep(0.3)   # the panels gone before the first frame
            ok = rec.start()
            if not ok:
                clean.leave()
            self.started = time.time()
            GLib.idle_add(self._started, ok)

        def _started(self, ok):
            self.state = "recording" if ok else "ready"
            if not ok:
                self.phase.set_label("Couldn't start the recording: see the log")
            else:
                log("recording %s" % parts[self.index]["id"])
            self.show()
            return False

        def stop(self, result):
            p = parts[self.index]
            counts[p["id"]] = counts.get(p["id"], 0) + 1
            take, started = counts[p["id"]], self.started
            self.state = "saving"
            self.show()

            def work():
                stopped = time.time()
                path = rec.stop("%s-take%d" % (p["id"], take))
                clean.leave()
                entry = {"part": p["id"], "section": p["section"], "title": p.get("title"), "take": take,
                         "file": os.path.basename(path) if path else None, "start": started, "stop": stopped,
                         "result": result}
                with open(takes_path, "a") as f:
                    f.write(json.dumps(entry) + "\n")
                log("take %s #%d %s: %s" % (p["id"], take, result, path))
                GLib.idle_add(self._stopped, result)
            threading.Thread(target=work, daemon=True).start()

        def _stopped(self, result):
            self.state = "ready"
            if result == "next":
                self.index += 1
            self.show()
            if result == "end":
                self.finish()
                self.quit()
            return False

        def finish(self):
            if self.state == "recording":
                p = parts[self.index]
                counts[p["id"]] = counts.get(p["id"], 0) + 1
                path = rec.stop("%s-take%d" % (p["id"], counts[p["id"]]))
                with open(takes_path, "a") as f:
                    f.write(json.dumps({"part": p["id"], "section": p["section"], "title": p.get("title"),
                                        "take": counts[p["id"]], "file": path and os.path.basename(path),
                                        "start": self.started, "stop": time.time(), "result": "end"}) + "\n")
                self.state = "ready"
            clean.leave()
            if self.button:
                self.button.stop()
                self.button = None

    app = App()

    def on_signal():
        app.finish()
        app.quit()
        return False
    for sig in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, on_signal)
    try:
        app.run([sys.argv[0]])
    finally:
        clean.leave()
    log("session folder: %s" % out)


if __name__ == "__main__":
    main()
