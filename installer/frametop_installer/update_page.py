"""The installer's own update: when the window opens it looks for a newer Frametop Installer,
offers it, installs it and opens the new one (selfupdate.py does the work)."""
import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk  # noqa: E402

from . import host, selfupdate  # noqa: E402
from .widgets import button, in_thread, label  # noqa: E402


class UpdatePage:
    """Mixed into the Window: uses its page(), get, dry_run, busy, close() and its application."""

    def look_for_installer_update(self, then):
        """Go on with then(), after offering a newer installer if there is one. Nothing stops
        then(): no newer one, a local build, GitHub out of reach, or Not now."""
        own = selfupdate.own_version()
        if not own:
            return then()
        self.page("loading", Gtk.Spinner(spinning=True, height_request=48),
                  label("Looking for a newer Frametop Installer...", css=("title-3",), xalign=0.5))
        in_thread(lambda: selfupdate.check(own, self.get, host.installed()[1] == "experimental"),
                  lambda update: self.show_installer_update(update, own, then) if update else then(),
                  lambda error: then())

    def show_installer_update(self, update, own, then, error=""):
        where = selfupdate.installation() or "user"
        widgets = [label("A newer Frametop Installer is out", css=("title-1",)),
                   label(f"This is Frametop Installer {own}. Version {update.version} is out. Updating takes "
                         "a few seconds, and the installer opens again by itself.")]
        if where == "system":
            widgets.append(label("It's installed for every user of this headset, so SteamOS asks for your "
                                 "password, as it did when you installed it.", css=("dim-label",)))
        if self.dry_run:
            widgets.append(label("Test run: the update is downloaded and checked, not installed.",
                                 css=("dim-label",)))
        if error:
            widgets.append(label(f"The update didn't install: {error}. You can go on with this version, or "
                                 "download FrametopInstaller.flatpak from Frametop's releases page and open "
                                 "it.", css=("error",)))
        self.page("update", *widgets,
                  buttons=[button("Not now", then),
                           button("Try again" if error else "Update", lambda: self.update_installer(update, own, then),
                                  css=("suggested-action",))])

    def update_installer(self, update, own, then):
        if self.busy:
            return
        self.busy = True
        dry_run = self.dry_run
        where = selfupdate.installation() or "user"
        self.page("loading", Gtk.Spinner(spinning=True, height_request=48),
                  label(f"Updating to Frametop Installer {update.version}...", css=("title-3",), xalign=0.5))

        def work():
            path = selfupdate.download(update)
            if not dry_run:
                selfupdate.install(path, where)

        def done(_):
            self.busy = False
            if dry_run:
                return then()
            if selfupdate.reopen():
                return self.get_application().quit()
            self.page("update", label(f"Frametop Installer {update.version} is installed", css=("title-1",)),
                      label("Close this window and open Frametop Installer again to use it."),
                      buttons=[button("Close", self.close, css=("suggested-action",))])

        def failed(e):
            self.busy = False
            text = str(e) if isinstance(e, selfupdate.UpdateError) else f"{e.__class__.__name__}: {e}"
            self.show_installer_update(update, own, then, error=text)
        in_thread(work, done, failed)
