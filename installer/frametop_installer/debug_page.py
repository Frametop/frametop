"""The Debug button: one text file with what a report of a problem needs, for the user to send.

host/debug-report.sh collects it on the host and saves it in Downloads. A small window of its own
runs it, then says where the file is and who to send it to, and can show it in the file manager
or copy its text. It's a window, not a page, so the installer's own page (an install's progress
too) carries on under it. Closing it while a report is being made only hides it: Debug brings it
back, rather than starting a second report on the host, and shows the report that finished while
it was hidden. The installer sends nothing anywhere:
the user does.
"""
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from . import host, selfupdate  # noqa: E402
from .widgets import button, in_thread, label, vbox  # noqa: E402

SCRIPT = "debug-report.sh"
TIMEOUT = 540   # more than the script's own timeouts add up to (debug-report.sh)
CONTACT = ("Send it to Deej__ on Discord (a direct message), or email it to dj_anuz@protonmail.com, "
           "with what you did, what you expected, and what happened.")


class ReportError(Exception):
    pass


def collect(version, run=host.run, stage=host.stage):
    """Run the report on the host; returns the file it wrote. Raises ReportError, saying why not."""
    try:
        folder = stage()
    except OSError as e:
        raise ReportError(f"couldn't put the installer's scripts in {host.BIN}: {e}") from e
    code, out, err = run(["bash", str(folder / SCRIPT), version], timeout=TIMEOUT)
    for line in reversed(out.decode("utf-8", "replace").splitlines()):
        if line.startswith("wrote "):
            return Path(line[len("wrote "):].strip())
    why = err.decode("utf-8", "replace").strip().splitlines()
    raise ReportError(why[-1] if why else f"it ended with exit status {code}")


class DebugPage:
    """Mixed into the Window: the Debug button makes a new report in a window of its own, or brings
    back the one still being made."""

    debug_window = None

    def show_debug(self):
        w = self.debug_window
        if w is None or not (w.busy or w.unseen):
            if w is not None:
                w.destroy()
            w = self.debug_window = DebugWindow(self)
        w.unseen = False
        w.present()


class DebugWindow(Gtk.Window):
    def __init__(self, parent):
        super().__init__(transient_for=parent, title="Debug report", default_width=760, default_height=520,
                         hide_on_close=True)
        self.busy = False
        self.unseen = False   # it ended while its window was hidden
        self.path = None
        self.note = None
        self.report_start()

    def report_show(self, *widgets, buttons=()):
        box = vbox(spacing=14, margin_top=24, margin_bottom=24, margin_start=28, margin_end=28)
        for w in widgets:
            box.append(w)
        if buttons:
            row = Gtk.Box(spacing=10, halign=Gtk.Align.END, margin_top=8)
            for b in buttons:
                row.append(b)
            box.append(row)
        self.set_child(Gtk.ScrolledWindow(vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER, child=box))

    def report_start(self):
        if self.busy:
            return
        self.busy = True
        self.report_show(Gtk.Spinner(spinning=True, height_request=48),
                         label("Making a debug report...", css=("title-3",), xalign=0.5),
                         label("It looks at SteamOS, SteamVR, Frametop's install, its settings and services, "
                               "what's running, and each feature's logs. That usually takes under a minute.", css=("dim-label",),
                               xalign=0.5))
        own = selfupdate.own_version() or "a local build"
        in_thread(lambda: collect(own), self.report_done, self.report_failed)

    def report_done(self, path):
        self.busy, self.unseen = False, not self.get_visible()
        self.path = path
        self.note = label("", css=("dim-label",))
        self.report_show(label("Debug report saved", css=("title-1",)),
                         label(f"It's {path.name}, in your {path.parent.name or 'home'} folder:"),
                         label(str(path), css=("monospace",), selectable=True),
                         label(CONTACT, css=("heading",)),
                         label("It has the headset's SteamOS and SteamVR versions, Frametop's install and "
                               "settings, what's running, and each feature's logs, NPU hand tracking's too. Passwords, tokens, email addresses, network "
                               "addresses, Bluetooth addresses and the headset's serial number are hidden. Names "
                               "you gave things, such as your PCs', stay in. Nothing is sent: you send the file.",
                               css=("dim-label",)),
                         self.note,
                         buttons=[button("Copy text", self.report_copy),
                                  button("Show in folder", self.report_open_folder),
                                  button("Close", self.close, css=("suggested-action",))])

    def report_failed(self, error):
        self.busy, self.unseen = False, not self.get_visible()
        why = str(error) if isinstance(error, ReportError) else f"{error.__class__.__name__}: {error}"
        self.report_show(label("The debug report didn't finish", css=("title-1",)),
                         label(f"{why}. You can also make it in a terminal on the headset:"),
                         label(f"bash {host.BIN / SCRIPT}", css=("monospace",), selectable=True),
                         label(CONTACT, css=("dim-label",)),
                         buttons=[button("Close", self.close),
                                  button("Try again", self.report_start, css=("suggested-action",))])

    def report_copy(self):
        try:
            text = self.path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            return self.note.set_label(f"Couldn't read it ({e.strerror}). Send the file instead.")
        self.get_clipboard().set_content(Gdk.ContentProvider.new_for_value(text))
        self.note.set_label(f"Copied ({len(text) // 1000} KB of text). Discord takes it better as the file; "
                            "an email can take the text.")

    def report_open_folder(self):
        def opened(launcher, result):
            try:
                launcher.open_containing_folder_finish(result)
            except GLib.Error as e:
                self.note.set_label(f"Couldn't open the folder ({e.message}). The file is at {self.path}.")
        Gtk.FileLauncher.new(Gio.File.new_for_path(str(self.path))).open_containing_folder(self, None, opened)
