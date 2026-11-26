"""The Debug button: one text file with what a report of a problem needs, for the user to send.

host/debug-report.sh collects it on the host and saves it in Downloads. This page runs it, then
says where the file is and who to send it to, and can show it in the file manager or copy its
text. The installer sends nothing anywhere: the user does.
"""
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from . import host, selfupdate  # noqa: E402
from .widgets import button, in_thread, label  # noqa: E402

SCRIPT = "debug-report.sh"
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
    code, out, err = run(["bash", str(folder / SCRIPT), version], timeout=300)
    for line in reversed(out.decode("utf-8", "replace").splitlines()):
        if line.startswith("wrote "):
            return Path(line[len("wrote "):].strip())
    why = err.decode("utf-8", "replace").strip().splitlines()
    raise ReportError(why[-1] if why else f"it ended with exit status {code}")


class DebugPage:
    """Mixed into the Window: uses its page(), stack and close(). While the debug page is up, other
    pages that finish (an install's progress, a lookup) are made but not shown: Back shows the
    last of them."""

    on_debug = False
    debugging = False
    debug_back = None
    debug_path = None

    def reveal(self, name):
        """Show page NAME, unless the debug page is up: then it's the page Back goes to."""
        if self.on_debug and name != "debug":
            self.debug_back = name
        else:
            self.stack.set_visible_child_name(name)

    def show_debug(self):
        if self.debugging:
            return
        if not self.on_debug:
            self.debug_back = self.stack.get_visible_child_name()
        self.on_debug = self.debugging = True
        self.page("debug", Gtk.Spinner(spinning=True, height_request=48),
                  label("Making a debug report...", css=("title-3",), xalign=0.5),
                  label("It looks at SteamOS, SteamVR, Frametop's install, its settings and services, and "
                        "what's running. That takes up to a minute.", css=("dim-label",), xalign=0.5))
        own = selfupdate.own_version() or "a local build"
        in_thread(lambda: collect(own), self.debug_done, self.debug_failed)

    def debug_done(self, path):
        self.debugging = False
        self.debug_path = path
        self.debug_note = label("", css=("dim-label",))
        where = label(str(path), css=("monospace",), selectable=True)
        self.page("debug", label("Debug report saved", css=("title-1",)),
                  label(f"It's {path.name}, in your {path.parent.name or 'home'} folder:"), where,
                  label(CONTACT, css=("heading",)),
                  label("It has the headset's SteamOS and SteamVR versions, Frametop's install, settings and "
                        "logs, and what's running. Passwords, tokens, email addresses, Bluetooth addresses "
                        "and the headset's serial number are left out. Nothing is sent: you send the file.",
                        css=("dim-label",)),
                  self.debug_note,
                  buttons=[button("Copy text", self.copy_debug), button("Show in folder", self.open_debug_folder),
                           button("Back", self.debug_close, css=("suggested-action",))])

    def debug_failed(self, error):
        self.debugging = False
        why = str(error) if isinstance(error, ReportError) else f"{error.__class__.__name__}: {error}"
        self.page("debug", label("The debug report didn't finish", css=("title-1",)),
                  label(f"{why}. You can also make it in a terminal on the headset:"),
                  label(f"bash {host.BIN / SCRIPT}", css=("monospace",), selectable=True),
                  label(CONTACT, css=("dim-label",)),
                  buttons=[button("Back", self.debug_close),
                           button("Try again", self.show_debug, css=("suggested-action",))])

    def debug_close(self):
        if self.debugging:
            return
        self.on_debug = False
        if self.debug_back and self.stack.get_child_by_name(self.debug_back):
            self.reveal(self.debug_back)
        else:
            self.open_where_things_are()

    def copy_debug(self):
        try:
            text = self.debug_path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            return self.debug_note.set_label(f"Couldn't read it ({e.strerror}). Send the file instead.")
        self.get_clipboard().set_content(Gdk.ContentProvider.new_for_value(text))
        self.debug_note.set_label(f"Copied ({len(text) // 1000} KB of text). Discord takes it better as the "
                                  "file; an email can take the text.")

    def open_debug_folder(self):
        def opened(launcher, result):
            try:
                launcher.open_containing_folder_finish(result)
            except GLib.Error as e:
                self.debug_note.set_label(f"Couldn't open the folder ({e.message}). The file is at "
                                          f"{self.debug_path}.")
        launcher = Gtk.FileLauncher.new(Gio.File.new_for_path(str(self.debug_path)))
        launcher.open_containing_folder(self, None, opened)
