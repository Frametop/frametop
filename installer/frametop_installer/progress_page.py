"""The progress page: follows the install's service through its log until it ends, answers a
sudo that comes after the window was opened again, and shows done or failed."""
import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

from . import host, job  # noqa: E402
from .askpass import Helper  # noqa: E402
from .widgets import button, in_thread, label, password_field, vbox  # noqa: E402

CRASH_NOTE = ("If your Steam Frame restarts or crashes while Frametop installs, just open Frametop "
              "Installer again and choose Install again: it picks up where it stopped.")


class ProgressPage:
    """Mixed into the Window: uses its page(), stack, helper, password, busy, keypads, dry_run,
    close(), show_versions(), install_again() and forget_password()."""

    def show_progress(self, reattach=False):
        self.progress = job.Progress()
        self.offset = 0
        self.finished = False
        state = job.load_state() or {}
        self.state = state
        version = state.get("version", "")
        self.status = label(f"Installing Frametop {version}", css=("title-1",))
        self.step = label("Starting...", css=("heading",))
        self.bar = Gtk.ProgressBar(show_text=False)
        self.download = label("", css=("dim-label",))
        note = vbox(spacing=6)
        note.add_css_class("card")
        for w in (label("If it restarts or crashes", css=("heading",), margin_top=10, margin_start=14,
                        margin_end=14),
                  label(CRASH_NOTE, margin_start=14, margin_end=14),
                  label("You can close this window and take the headset off: the install keeps going. "
                        "Open Frametop Installer again to see how it's doing."
                        + (" Keep this window open if the install asks for your password."
                           if state.get("sudo") else ""),
                        css=("dim-label",), margin_bottom=10, margin_start=14, margin_end=14)):
            note.append(w)
        self.note = note
        self.log = Gtk.TextView(editable=False, cursor_visible=False, monospace=True,
                                wrap_mode=Gtk.WrapMode.WORD_CHAR, left_margin=8, right_margin=8,
                                top_margin=6, bottom_margin=6)
        logscroll = Gtk.ScrolledWindow(child=self.log, min_content_height=260, vexpand=True)
        self.logscroll = logscroll
        expander = Gtk.Expander(label="Details", child=logscroll)
        # Asked for the password by the install, without it (the window was opened again)
        self.keypads = []
        self.ask_entry, ask_field = password_field("Your SteamOS password, for the next step", self.answer_ask,
                                                   self.keypads)
        self.ask_error = label("", css=("error",), visible=False)
        self.ask_box = vbox(spacing=8, visible=False)
        for w in (label("The install needs your password for its next step.", css=("heading",)), ask_field,
                  self.ask_error, Gtk.Box(spacing=8, halign=Gtk.Align.END)):
            self.ask_box.append(w)
        btns = self.ask_box.get_last_child()
        btns.append(button("Skip that part", self.skip_ask))
        btns.append(button("OK", self.answer_ask, css=("suggested-action",)))
        self.done_buttons = Gtk.Box(spacing=10, halign=Gtk.Align.END)
        self.done_buttons.append(button("Close", self.close))
        self.page("progress", self.status, self.step, self.bar, self.download, self.note, self.ask_box,
                  expander, self.done_buttons)
        if state.get("sudo") and self.helper is None:
            try:
                self.helper = Helper(self.ask_again)
                if self.password:
                    self.helper.give(self.password)
            except OSError as e:
                self.add_log([f"(the password helper didn't start: {e}; the parts that need sudo will be skipped)"])
        self.ticks = 0
        self.unit = "activating"
        GLib.timeout_add(500, self.tick)
        self.tick()

    def ask_again(self):
        if self.password and self.helper:
            self.helper.give(self.password)
            return
        self.ask_box.set_visible(True)
        self.ask_entry.grab_focus()

    def answer_ask(self):
        text = self.ask_entry.get_text()
        if not text or self.busy:
            return
        pw = bytearray(text.encode())
        self.ask_entry.set_text("")
        self.busy = True

        def done(ok):
            self.busy = False
            if ok and self.helper:
                self.forget_password()
                self.password = pw
                self.helper.give(pw)
                self.ask_box.set_visible(False)
            else:
                pw[:] = bytes(len(pw))
                self.ask_error.set_label("sudo didn't take that password.")
                self.ask_error.set_visible(True)
        in_thread(lambda: self.dry_run or host.check_password(pw), done)

    def skip_ask(self):
        if self.helper:
            self.helper.refuse()
        self.ask_box.set_visible(False)

    def add_log(self, lines):
        if not lines:
            return
        buf = self.log.get_buffer()
        buf.insert(buf.get_end_iter(), "".join(line + "\n" for line in lines))
        adj = self.logscroll.get_vadjustment()
        GLib.idle_add(lambda: (adj.set_value(adj.get_upper()), False)[1])

    def tick(self):
        if self.finished or self.stack.get_visible_child_name() != "progress":
            return False
        try:
            with open(job.LOG, "rb") as f:
                f.seek(self.offset)
                data = f.read()
                self.offset += len(data)
        except FileNotFoundError:
            data = b""
        if data:
            self.add_log(self.progress.feed(data))
        p = self.progress
        self.step.set_label(p.step)
        self.download.set_label(p.download)
        self.download.set_visible(bool(p.download))
        if p.fraction > 0:
            self.bar.set_fraction(p.fraction)
        else:
            self.bar.pulse()
        self.ticks += 1
        if self.ticks % 4 == 1:
            in_thread(job.unit_state, self.unit_seen)
        return True

    def unit_seen(self, unit):
        if self.finished:
            return
        self.unit = unit
        rc = job.result()
        where = job.where_things_are(self.state, unit, rc)
        if where == "running":
            return
        self.add_log(self.progress.flush())
        self.finish(where, rc)

    def finish(self, where, rc):
        self.finished = True
        self.forget_password()
        if self.helper:
            self.helper.close()
            self.helper = None
        self.ask_box.set_visible(False)
        self.note.set_visible(where != "done")
        state = dict(self.state or {})
        state["seen"] = True
        job.save_state(state)
        self.state = state
        while (child := self.done_buttons.get_first_child()) is not None:
            self.done_buttons.remove(child)
        if where == "done":
            self.bar.set_fraction(1)
            self.status.set_label(f"Frametop {state.get('version', '')} is installed")
            self.step.set_label("Done. " + ("SteamVR restarts now." if state.get("restart") and not state.get("dry_run")
                                            else "Restart SteamVR once, or reboot the headset, so it loads "
                                                 "Frametop. Then open Launch a program, then Desktop."))
            self.done_buttons.append(button("Close", self.close, css=("suggested-action",)))
        else:
            self.status.set_label("The install stopped before it finished")
            why = (f"It ended with exit status {rc}. The details below say why." if rc is not None else
                   "It stopped without finishing: the headset may have restarted, or something ended it.")
            self.step.set_label(why + " Install again picks up where it stopped.")
            self.done_buttons.append(button("Change choices", self.show_versions))
            self.done_buttons.append(button("Close", self.close))
            self.done_buttons.append(button("Install again", self.install_again, css=("suggested-action",)))
