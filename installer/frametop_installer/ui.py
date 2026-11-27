"""The Frametop Installer's window (GTK 4, styled by libadwaita when it's there, as in the Flatpak).

Pages, in order: the version (newest stable or newest experimental), the parts (checkboxes,
all ticked), your password if a part needs it (or setting one, as SteamOS starts without), then
the install's progress, which follows the install's service until it ends. Opened while an
install runs, it goes straight to the progress; opened after one stopped without finishing (the
headset restarted or crashed), it offers to run the same install again.

The version page's Uninstall Frametop runs uninstall.sh, which the app carries, from the window
(uninstall_page.py). It's always there, offline too, so a broken or half-done install can still
be taken out; uninstall.sh --plan says when nothing is left.

Opening it also looks for updates: a newer Frametop Installer is offered first (update_page.py),
and the version page says when a newer Frametop than the installed one is out.

The Debug button in the title bar, on every page, opens a window that saves a debug report to
send (debug_page.py).
"""
import os
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

try:
    gi.require_version("Adw", "1")
    from gi.repository import Adw  # noqa: E402
except (ValueError, ImportError):  # SteamOS's host has GTK 4 but no libadwaita
    Adw = None

from . import host, job, options, releases  # noqa: E402
from .debug_page import DebugPage  # noqa: E402
from .password_pages import PasswordPages  # noqa: E402
from .progress_page import ProgressPage  # noqa: E402
from .uninstall import outcome as uninstall_outcome  # noqa: E402
from .uninstall_page import UninstallPages  # noqa: E402
from .update_page import UpdatePage  # noqa: E402
from .widgets import boxed_list, button, check_row, in_thread, label, vbox  # noqa: E402

APP_ID = "io.github.frametop.Installer"
CHANNEL_NAMES = {"stable": "Latest stable", "experimental": "Latest experimental"}
CHANNEL_DETAIL = {"stable": "From main: tested releases.",
                  "experimental": "From experimental: the newest features, less tested."}


class Window(PasswordPages, ProgressPage, UninstallPages, UpdatePage, DebugPage, Gtk.ApplicationWindow):
    def __init__(self, app, dry_run=False):
        super().__init__(application=app, title="Frametop Installer")
        self.set_default_size(980, 820)
        self.dry_run = dry_run
        self.releases = {}
        self.release = None
        self.picked = {p.key: True for p in options.PARTS}
        self.restart = True
        self.password = None            # bytearray, until the install ends or the window closes
        self.helper = None
        self.keypads = []
        self.busy = False
        self.state = None
        self.plan, self.un_picks, self.un_finishing, self.un_dry, self.un_syncing = None, {}, False, False, False
        self.get = releases.memo()      # GitHub's answers, each asked for once per look
        self.osr = releases.os_release()

        header = Gtk.HeaderBar()
        debug = button("Debug", self.show_debug)
        debug.set_tooltip_text("Save a debug report to send to Frametop's developer")
        header.pack_start(debug)
        self.set_titlebar(header)
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vhomogeneous=False)
        outer = vbox(spacing=0)
        if dry_run:
            banner = label("Test run: Frametop isn't downloaded or installed, and your password isn't checked.",
                           css=("warning",), xalign=0.5, margin_top=6, margin_bottom=6)
            outer.append(banner)
        scroll = Gtk.ScrolledWindow(vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER, child=self.stack)
        outer.append(scroll)
        self.set_child(outer)
        self.connect("close-request", self.closing)
        self.open_where_things_are()

    # --- pages

    def page(self, name, *widgets, buttons=()):
        """Replace page NAME with these widgets, buttons at the bottom right, and show it."""
        old = self.stack.get_child_by_name(name)
        if old:
            self.stack.remove(old)
        box = vbox(spacing=14, margin_top=24, margin_bottom=24, margin_start=28, margin_end=28)
        clamp = box
        if Adw:
            clamp = Adw.Clamp(maximum_size=860, child=box)
        for w in widgets:
            box.append(w)
        if buttons:
            row = Gtk.Box(spacing=10, halign=Gtk.Align.END, margin_top=8)
            for b in buttons:
                row.append(b)
            box.append(row)
        self.stack.add_named(clamp, name)
        self.stack.set_visible_child_name(name)
        return box

    def open_where_things_are(self):
        self.page("loading", Gtk.Spinner(spinning=True, height_request=48),
                  label("Looking up Frametop...", css=("title-3",), xalign=0.5))

        def work():
            unit = job.unit_state()
            other = job.unit_state(job.FRAMEDROP_UNIT)
            return unit, other, job.load_state(), job.result()

        def done(r):
            unit, other, state, rc = r
            where = job.where_things_are(state, unit, rc)
            self.state = state
            if other == "activating" and where != "running":
                return self.show_message("FrameDrop is installing Frametop",
                                         "Another install of Frametop is running (from FrameDrop). Let it "
                                         "finish, then open Frametop Installer again.")
            if where == "running":
                return self.show_progress(reattach=True)   # (an install isn't interrupted for an update)

            def go_on():
                if (state or {}).get("kind") == "uninstall" and (where != "new" or uninstall_outcome(rc) == "restart"):
                    return self.show_uninstall_state(where, state, rc)
                if where in ("unfinished", "failed", "done"):
                    return self.show_resume(where, state, rc)
                self.show_versions()
            self.look_for_installer_update(go_on)
        in_thread(work, done, self.went_wrong)

    def show_message(self, title, text):
        self.page("message", label(title, css=("title-1",)), label(text),
                  buttons=[button("Close", self.close)])

    def went_wrong(self, error):
        self.show_message("Something went wrong", f"{error.__class__.__name__}: {error}. Close this window "
                                                  "and open Frametop Installer again.")

    def show_versions(self):
        if not releases.is_frame(self.osr) and not self.dry_run:
            return self.show_message("This isn't a Steam Frame",
                                     "Frametop installs on a Steam Frame (SteamOS, VR variant).")
        self.page("loading", Gtk.Spinner(spinning=True, height_request=48),
                  label("Looking up the newest Frametop releases on GitHub...", css=("title-3",), xalign=0.5))
        # This look shares the answers the look for a newer installer got; the next asks afresh.
        build, get, self.get = self.osr.get("BUILD_ID", ""), self.get, releases.memo()
        in_thread(lambda: (releases.lookup_both(build, get), host.installed()), self.versions_found,
                  self.went_wrong)

    def versions_found(self, found):
        self.releases, (inst_version, inst_channel) = found
        osver = self.osr.get("VERSION_ID", "?")
        intro = label("Frametop turns your Steam Frame into a multi-screen desktop, with a 3D mouse, "
                      "Frametop Settings, and the optional parts on the next page. Pick the version to "
                      "install. Both come built: nothing compiles on the headset.")
        lb = boxed_list()
        group = None
        self.choice = {}
        for channel in ("stable", "experimental"):
            r = self.releases[channel]
            radio = Gtk.CheckButton(group=group)
            group = group or radio
            if isinstance(r, Exception):
                detail = f"{CHANNEL_DETAIL[channel]} Not available: {r}."
                radio.set_sensitive(False)
            else:
                size = f"{r.zip_size / 1e9:.1f} GB download" if r.zip_size else "about a 1.2 GB download"
                status = {"tested": f"Tested on your SteamOS ({osver}).",
                          "untested": f"Not tested on your SteamOS ({osver}) yet; {r.note}. It usually works.",
                          "broken": f"Doesn't work on your SteamOS ({osver}): {r.note}."}[r.status]
                detail = f"Frametop {r.version}. {CHANNEL_DETAIL[channel]} {status} {size}."
                if r.status == "broken":
                    radio.set_sensitive(False)
            row, _ = check_row(radio, CHANNEL_NAMES[channel], detail)
            lb.append(row)
            self.choice[channel] = radio
        usable = [c for c in ("stable", "experimental") if self.choice[c].get_sensitive()]
        prefer = "experimental" if inst_channel == "experimental" else "stable"
        if usable:
            self.choice[prefer if prefer in usable else usable[0]].set_active(True)
        title, note, news = releases.installed_note(inst_version, inst_channel, self.releases)
        widgets = [label(title, css=("title-1",)), intro, lb]
        if inst_version:
            widgets.insert(1 if news else 3, label(note, css=("heading",) if news else ("dim-label",)))
        if not usable:
            widgets.append(label("Neither version can be installed right now. Check the headset's "
                                 "internet connection, then try again.", css=("error",)))
            buttons = [button("Try again", self.show_versions, css=("suggested-action",))]
        else:
            buttons = [button("Next", self.version_picked, css=("suggested-action",))]
        buttons.insert(0, button("Uninstall Frametop", self.show_uninstall))
        self.page("version", *widgets, buttons=buttons)

    def version_picked(self):
        channel = "experimental" if self.choice["experimental"].get_active() else "stable"
        self.release = self.releases[channel]
        self.show_parts()

    def show_parts(self):
        r = self.release
        lb = boxed_list()
        self.checks = {}
        self.wanted = {}     # what each box would say if what it needs were ticked
        self.syncing = False
        for part in options.PARTS:
            kind = options.availability(part, r.supported)
            if kind == "absent":
                continue
            self.wanted[part.key] = self.picked.get(part.key, True) or kind == "always"
            check = Gtk.CheckButton(active=self.wanted[part.key])
            detail = part.detail
            if part.sudo:
                detail += " Needs your password (sudo)."
            if kind == "always":
                check.set_active(True)
                check.set_sensitive(False)
                detail += f" Frametop {r.version} always installs it."
            row, _ = check_row(check, part.title, detail)
            lb.append(row)
            self.checks[part.key] = (check, row)
            check.connect("toggled", self.part_toggled, part.key)
        self.restart_check = Gtk.CheckButton(active=self.restart)
        rrow, _ = check_row(self.restart_check, "Restart SteamVR when it's done",
                            "SteamVR has to restart once to load Frametop. That closes everything open in "
                            "VR, this window too. Untick it to restart it yourself later, or reboot.")
        restart_list = boxed_list()
        restart_list.append(rrow)
        self.parts_note = label("", css=("dim-label",))
        self.page("parts", label(f"Frametop {r.version}: what to install", css=("title-1",)),
                  label("Frametop's desktop, 3D mouse, power service and Frametop Settings always install. "
                        "These are optional; untick what you don't want."),
                  lb, restart_list, self.parts_note,
                  buttons=[button("Back", self.show_versions_again),
                           button("Next", self.parts_picked, css=("suggested-action",))])
        self.parts_changed()

    def show_versions_again(self):
        if self.releases:
            self.versions_found((self.releases, host.installed()))
        else:
            self.show_versions()

    def current_picks(self):
        return dict(self.wanted)

    def part_toggled(self, check, key):
        if not self.syncing:
            self.wanted[key] = check.get_active()
            self.parts_changed()

    def parts_changed(self):
        """A part whose parent is unticked shows unticked and can't be changed; ticking the parent
        again brings back what was picked for it."""
        picks = self.current_picks()
        self.syncing = True
        try:
            for key, (ticked, can_change) in options.boxes(picks, self.release.supported).items():
                check, row = self.checks[key]
                row.set_sensitive(can_change)
                check.set_active(ticked)
        finally:
            self.syncing = False
        need = options.needs_password(picks, self.release.supported)
        self.parts_note.set_label("The parts that need your password ask for it once, on the next page."
                                  if need else "None of these need your password.")

    def parts_picked(self):
        self.picked.update(self.current_picks())
        self.restart = self.restart_check.get_active()
        if options.needs_password(self.picked, self.release.supported):
            self.show_password()
        else:
            self.begin(None)

    # --- the install

    def begin(self, pw, version=None, channel=None, flags=None, restart=None, dry_run=None):
        """Start the install. version/flags/restart/dry_run come from the last install's state for
        Install again; otherwise from the pages."""
        dry_run = self.dry_run if dry_run is None else dry_run
        if version is None:
            version, channel = self.release.version, self.release.channel
            flags = options.install_flags(self.picked, self.release.supported)
            restart = self.restart
        self.launch(pw, lambda: job.start(version, channel, flags, restart, pw is not None, dry_run=dry_run),
                    "Starting the install...", "The install didn't start")

    def show_resume(self, where, state, rc):
        version = state.get("version", "")
        when = GLib.DateTime.new_from_unix_local(state.get("started", 0)).format("%e %b, %H:%M").strip()
        if where != "unfinished":   # a result, shown once; an unfinished install asks every time
            job.save_state(dict(state, seen=True))
        if where == "done":
            return self.page("resume", label(f"Frametop {version} is installed", css=("title-1",)),
                             label(f"The install you started on {when} finished. If you haven't yet, restart "
                                   "SteamVR once, or reboot the headset. Then open Launch a program, then "
                                   "Desktop."),
                             buttons=[button("Install or update again", self.show_versions),
                                      button("Close", self.close, css=("suggested-action",))])
        why = (f"It ended with exit status {rc}." if where == "failed" else
               "It stopped without finishing: the headset may have restarted or crashed.")
        log = label(self.log_tail(), css=("monospace", "dim-label"), wrap=True, selectable=True)
        self.page("resume", label("The last install didn't finish", css=("title-1",)),
                  label(f"You started installing Frametop {version} on {when}. {why} Install again runs the "
                        "same install with the same choices, and picks up where it stopped: the download "
                        "resumes, and the steps done already go quickly."),
                  Gtk.Expander(label="The end of its log", child=log),
                  buttons=[button("Change choices", self.show_versions),
                           button("Install again", self.install_again, css=("suggested-action",))])

    @staticmethod
    def log_tail():
        try:
            p = job.Progress()
            p.feed(job.LOG.read_bytes()[-20000:])
            p.flush()
            return "\n".join(p.lines[-25:]) or "(empty)"
        except OSError:
            return "(no log)"

    def install_again(self):
        state = self.state or job.load_state() or {}
        if not state.get("version"):
            return self.show_versions()
        # (a test run again is a test run; the window's own dry_run stays what it was started as)
        dry_run = self.dry_run or bool(state.get("dry_run"))
        if state.get("sudo") and not self.password:
            return self.show_again_password(state)
        self.begin(self.password if state.get("sudo") else None, state["version"], state.get("channel", ""),
                   state.get("flags", ["--yes"]), state.get("restart", False), dry_run=dry_run)

    # --- closing

    def forget_password(self):
        if self.password:
            self.password[:] = bytes(len(self.password))
        self.password = None

    def closing(self, *_):
        self.forget_password()
        if self.helper:
            self.helper.close()
            self.helper = None
        return False


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    dry_run = "--dry-run" in argv or os.environ.get("FRAMETOP_INSTALLER_DRY_RUN") == "1"
    cls = Adw.Application if Adw else Gtk.Application
    app = cls(application_id=APP_ID)

    def activate(a):
        if Adw:
            # Dark: a white window is glaring in VR.
            Adw.StyleManager.get_default().set_color_scheme(Adw.ColorScheme.FORCE_DARK)
        else:
            Gtk.Settings.get_default().set_property("gtk-application-prefer-dark-theme", True)
        win = a.get_active_window() or Window(a, dry_run=dry_run)
        win.present()

    app.connect("activate", activate)
    return app.run([sys.argv[0]])
