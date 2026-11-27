"""The uninstall pages: what to delete (from uninstall.sh --plan), the password if Frametop's
system files need it, how it ended, and Finish uninstalling after the restart it needed.

uninstall.sh goes in two steps, as in a terminal. Step 1 stops Frametop from starting; what runs
keeps running until the headset restarts, so step 2 (deleting the releases, the code and what was
picked) waits for that. The window offers the restart, and opened again after it, offers to
finish with the same choices (state.json's flags).
"""
import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

from . import host, job, uninstall  # noqa: E402
from .widgets import boxed_list, button, check_row, in_thread, label, password_field  # noqa: E402

REMOVE_APP = ("Frametop Installer stays, so you can install Frametop again. To remove it too, uninstall it "
              "in Discover, or run this in a terminal: flatpak uninstall --delete-data io.github.frametop.Installer")


class UninstallPages:
    """Mixed into the Window: uses its page(), stack, state, dry_run, busy, password, keypads,
    launch(), close(), show_versions(), show_versions_again(), went_wrong(), log_tail(), say() and
    forget_password(); and the progress page's status, step, bar, note and done_buttons."""

    def show_uninstall(self, flags=None):
        """Look at what's installed (uninstall.sh --plan), then show the choices. With FLAGS
        (Finish uninstalling), go on with those instead."""
        if flags is None:
            self.un_finishing, self.un_dry, self.un_picks = False, False, {}
        self.page("loading", Gtk.Spinner(spinning=True, height_request=48),
                  label("Looking at what Frametop installed...", css=("title-3",), xalign=0.5))

        def work():
            host.stage()
            code, out, err = host.run(["/usr/bin/bash", str(host.BIN / "uninstall.sh"), "--plan"], timeout=180)
            if code != 0:
                raise RuntimeError(err.decode("utf-8", "replace").strip() or f"uninstall.sh --plan ended with {code}")
            return uninstall.parse_plan(out.decode("utf-8", "replace"))
        in_thread(work, lambda plan: self.uninstall_planned(plan, flags), self.went_wrong)

    def uninstall_planned(self, plan, flags):
        self.plan = plan
        if not plan.anything:
            if flags is not None:   # finished some other way (a terminal): nothing to offer next time
                self.forget_installer_files()
            return self.page("message", label("Frametop isn't installed", css=("title-1",)),
                             label("Nothing of Frametop is left on this headset. " + REMOVE_APP),
                             buttons=[button("Install Frametop", self.show_versions), button("Close", self.close)])
        if flags is None:
            return self.uninstall_choices()
        if plan.running and not plan.step1:   # Finish uninstalling, before the restart
            return self.show_restart_needed(plan.running)
        self.uninstall_password_or_go(flags)

    def uninstall_choices(self):
        plan = self.plan
        lb = boxed_list()
        self.un_checks = {}
        for key, (_, size) in plan.asks.items():
            title, detail = uninstall.CHOICES[key]
            check = Gtk.CheckButton()
            row, _ = check_row(check, f"{title} ({size})" if size else title, detail)
            lb.append(row)
            self.un_checks[key] = (check, row)
            check.connect("toggled", self.uninstall_toggled, key)
        self.uninstall_sync()
        widgets = [label("Uninstall Frametop", css=("title-1",)),
                   label("This gives the launcher's Desktop back to the stock desktop, and removes Frametop's "
                         "services, SteamVR driver, menu entries and system files, and the releases. Pick what "
                         "else goes.")]
        if plan.asks:
            widgets.append(lb)
        if plan.running:
            widgets.append(label("Frametop is running now, so this goes in two steps. Now it stops Frametop from "
                                 "starting, and what runs keeps running. Then you restart the headset, and open "
                                 "Frametop Installer again to finish: that deletes the rest.", css=("heading",)))
        if plan.sudo:
            widgets.append(label("Removing Frametop's system files needs your SteamOS password (sudo): the next "
                                 "page asks for it.", css=("dim-label",)))
        self.page("uninstall", *widgets,
                  buttons=[button("Back", self.show_versions_again),
                           button("Uninstall", self.uninstall_picked, css=("destructive-action",))])

    def uninstall_toggled(self, check, key):
        if not self.un_syncing:
            self.un_picks[key] = check.get_active()
            self.uninstall_sync()

    def uninstall_sync(self):
        self.un_syncing = True
        try:
            for key, (ticked, can_change) in uninstall.boxes(self.plan, self.un_picks).items():
                check, row = self.un_checks[key]
                check.set_active(ticked)
                row.set_sensitive(can_change)
        finally:
            self.un_syncing = False

    def uninstall_picked(self):
        self.uninstall_password_or_go(uninstall.flags(self.plan, self.un_picks))

    def uninstall_password_or_go(self, flags):
        if self.plan.sudo and not self.password:
            return self.show_uninstall_password(flags)
        self.begin_uninstall(self.password if self.plan.sudo else None, flags)

    def begin_uninstall(self, pw, flags):
        dry_run = self.dry_run or self.un_dry
        self.launch(pw, lambda: job.start_uninstall(flags, pw is not None, dry_run=dry_run),
                    "Starting the uninstall...", "The uninstall didn't start")

    def show_uninstall_password(self, flags):
        self.pw_error = label("", css=("error",), visible=False)
        self.keypads = []
        back = button("Not now", self.close) if self.un_finishing else button("Back", self.uninstall_choices)
        skip = button("Skip those", lambda: self.begin_uninstall(None, flags))
        if not (self.dry_run or self.un_dry or host.password_set()):
            return self.page("password", label("Your password", css=("title-1",)),
                             label("Removing Frametop's system files needs sudo, and your user has no password. "
                                   "Set one in a terminal with passwd, then open Frametop Installer again. Or skip "
                                   "them: they stay, and the rest goes."),
                             buttons=[back, skip])

        def go():
            text = self.pw_entry.get_text()
            if not text or self.busy:
                return
            pw = bytearray(text.encode())
            self.pw_entry.set_text("")
            self.busy = True
            self.say("Checking it...")

            def done(ok):
                self.busy = False
                if not ok:
                    pw[:] = bytes(len(pw))
                    return self.say("sudo didn't take that password. Try again, or skip those.")
                self.forget_password()
                self.password = pw
                self.begin_uninstall(pw, flags)
            in_thread(lambda: self.dry_run or self.un_dry or host.check_password(pw), done, lambda _: done(False))

        self.pw_entry, field = password_field("Your SteamOS password", go, self.keypads)
        self.page("password", label("Your password", css=("title-1",)),
                  label("Frametop's system files (its eye tracker's frame grabber, NPU hand tracking, the "
                        "Bluetooth fixes, hand tracking's camera access) need your SteamOS password (sudo) to "
                        "remove. Type it once now. Skip those, and they stay while the rest goes."),
                  field,
                  label("It's kept in this window's memory until the uninstall ends, and never written anywhere.",
                        css=("dim-label",)),
                  self.pw_error, buttons=[back, skip, button("Uninstall", go, css=("destructive-action",))])
        self.pw_entry.grab_focus()

    # --- how it ended

    def uninstall_finished(self, rc):
        """The progress page, once uninstall.sh ended with RC."""
        out = uninstall.outcome(rc)
        add = self.done_buttons.append
        self.note.set_visible(False)
        if out == "restart":
            self.status.set_label("Restart the headset to finish")
            self.step.set_label("Frametop no longer starts. What runs now stops when the headset restarts. After "
                                "the restart, open Frametop Installer again and choose Finish uninstalling: it "
                                "deletes what's left, with the choices you made.")
            add(button("Close", self.close))
            add(button("Restart now", self.restart_headset, css=("destructive-action",)))
        elif out in ("done", "leftovers"):
            self.bar.set_fraction(1)
            if out == "done":
                self.status.set_label("Frametop is uninstalled")
                self.step.set_label(REMOVE_APP)
                self.forget_installer_files()
            else:
                self.status.set_label("Frametop is uninstalled, except what needs your password")
                self.step.set_label("The system files that need sudo stay; the details below say which. Try again "
                                    "to remove them.")
                add(button("Try again", self.show_uninstall))
            add(button("Close", self.close, css=("suggested-action",)))
        else:
            self.status.set_label("The uninstall stopped before it finished")
            self.step.set_label((f"It ended with exit status {rc}. The details below say why." if rc is not None else
                                 "It stopped without finishing: the headset may have restarted, or something "
                                 "ended it.") + " Try again picks up where it stopped.")
            add(button("Close", self.close))
            add(button("Try again", self.show_uninstall, css=("suggested-action",)))

    def show_uninstall_state(self, where, state, rc):
        """Opened after an uninstall, not running now: finish it, or say how it ended."""
        out = uninstall.outcome(rc)
        if out == "restart":
            return self.show_finish_uninstall(state)
        if where != "unfinished":   # a result, shown once
            job.save_state(dict(state, seen=True))
        if out == "done":
            self.forget_installer_files()
            return self.page("resume", label("Frametop is uninstalled", css=("title-1",)), label(REMOVE_APP),
                             buttons=[button("Install Frametop", self.show_versions),
                                      button("Close", self.close, css=("suggested-action",))])
        why = {"leftovers": "Everything went, except the system files that need your password (sudo).",
               "failed": f"It ended with exit status {rc}.",
               "unfinished": "It stopped without finishing: the headset may have restarted or crashed."}[out]
        log = label(self.log_tail(), css=("monospace", "dim-label"), wrap=True, selectable=True)
        self.page("resume", label("The last uninstall didn't finish", css=("title-1",)),
                  label(f"{why} Try again looks at what's left, and removes it."),
                  Gtk.Expander(label="The end of its log", child=log),
                  buttons=[button("Install Frametop", self.show_versions),
                           button("Try again", self.show_uninstall, css=("suggested-action",))])

    def show_finish_uninstall(self, state):
        when = GLib.DateTime.new_from_unix_local(state.get("started", 0)).format("%e %b, %H:%M").strip()

        def go():
            self.un_finishing, self.un_dry, self.un_picks = True, bool(state.get("dry_run")), {}
            self.show_uninstall(flags=list(state.get("flags", [])))
        self.page("resume", label("Finish uninstalling Frametop", css=("title-1",)),
                  label(f"You started uninstalling Frametop on {when}, and Frametop no longer starts. Once the "
                        "headset has restarted, finish here: it deletes what's left, with the choices you made."),
                  buttons=[button("Not now", self.close),
                           button("Finish uninstalling", go, css=("destructive-action",))])

    def show_restart_needed(self, running):
        self.page("restart", label("Restart the headset first", css=("title-1",)),
                  label(f"Frametop's programs still run from before ({running}), so what's left can't go yet. "
                        "Restart the headset, then open Frametop Installer again and choose Finish uninstalling."),
                  buttons=[button("Close", self.close),
                           button("Restart now", self.restart_headset, css=("destructive-action",))])

    def restart_headset(self):
        if self.test_run():
            return self.show_message("Test run", "A test run doesn't restart the headset.")

        def failed(_=None):
            self.show_message("Couldn't restart the headset",
                              "Restart it from Steam's power menu, then open Frametop Installer again.")
        in_thread(lambda: host.run(["systemctl", "reboot"], timeout=30)[0],
                  lambda code: failed() if code != 0 else None, failed)

    def test_run(self):
        """This window's, or the uninstall's it follows (opened again while one ran)."""
        return bool(self.dry_run or self.un_dry or (self.state or {}).get("dry_run"))

    def forget_installer_files(self):
        """After it's all gone, the installer's own scripts, log and state go too (uninstall.sh left
        them, as it ran from there). A test run leaves them."""
        if not self.test_run():
            in_thread(lambda: host.run(["rm", "-rf", "--", str(host.CACHE)], timeout=30), lambda _: None,
                      lambda _: None)
