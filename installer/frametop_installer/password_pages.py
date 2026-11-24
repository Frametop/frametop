"""The password pages: type it once for the parts that need sudo, set one if the user has none
(SteamOS starts without), or type it again for Install again."""
import os

from . import host, options
from .widgets import button, in_thread, label, password_field


class PasswordPages:
    """Mixed into the Window: uses its page(), picked, release, dry_run, busy, password, keypads,
    show_parts(), show_versions(), begin(), install_again() and forget_password()."""

    def sudo_parts(self):
        on = options.effective(self.picked, self.release.supported)
        return [p.title for p in options.PARTS if p.sudo and on[p.key]]

    def show_password(self, after_set=False):
        if self.dry_run and os.environ.get("FRAMETOP_INSTALLER_NO_PASSWORD") == "1" and not after_set:
            has = False
        else:
            has = self.dry_run or host.password_set()
        parts = ", ".join(self.sudo_parts())
        self.pw_error = label("", css=("error",), visible=False)
        self.keypads = []
        if has:
            self.pw_entry, field = password_field("Your SteamOS password", self.check_password, self.keypads)
            self.page("password", label("Your password", css=("title-1",)),
                      label(f"These need your SteamOS password (sudo): {parts}. Type it once now; the rest "
                            "of the install asks for nothing."),
                      field,
                      label("It's kept in this window's memory until the install ends, and never written "
                            "anywhere. Keep this window open until the install is done.", css=("dim-label",)),
                      self.pw_error,
                      buttons=[button("Back", self.show_parts),
                               button("Skip those parts", self.skip_sudo_parts),
                               button("Install", self.check_password, css=("suggested-action",))])
            self.pw_entry.grab_focus()
        else:
            self.new_pw, f1 = password_field("New password", lambda: self.new_pw2.grab_focus(), self.keypads)
            self.new_pw2, f2 = password_field("The same again", self.set_password, self.keypads)
            self.page("password", label("Set a password", css=("title-1",)),
                      label(f"These need sudo, which needs a password, and your user doesn't have one yet: "
                            f"{parts}. SteamOS starts without one. Set one here, and remember it: sudo and "
                            "SSH ask for it from now on."),
                      f1, f2, self.pw_error,
                      buttons=[button("Back", self.show_parts),
                               button("Skip those parts", self.skip_sudo_parts),
                               button("Set password", self.set_password, css=("suggested-action",))])
            self.new_pw.grab_focus()

    def say(self, text):
        self.pw_error.set_label(text)
        self.pw_error.set_visible(bool(text))

    def skip_sudo_parts(self):
        for p in options.PARTS:
            if p.sudo:
                self.picked[p.key] = False
        self.begin(None)

    def set_password(self):
        a, b = self.new_pw.get_text(), self.new_pw2.get_text()
        if not a:
            return self.say("Type a password.")
        if a != b:
            return self.say("The two don't match. Type them again.")
        if self.busy:
            return
        pw = bytearray(a.encode())
        self.new_pw.set_text("")
        self.new_pw2.set_text("")
        self.busy = True
        self.say("Setting it...")

        def done(err):
            self.busy = False
            if err:
                pw[:] = bytes(len(pw))
                return self.say(f"Couldn't set the password: {err}")
            self.verified(pw)
        in_thread(lambda: "" if self.dry_run else host.set_password(pw), done,
                  lambda e: done(f"{e.__class__.__name__}: {e}"))

    def check_password(self):
        text = self.pw_entry.get_text()
        if not text:
            return self.say("Type your password, or skip those parts.")
        if self.busy:
            return
        pw = bytearray(text.encode())
        self.pw_entry.set_text("")
        self.busy = True
        self.say("Checking it...")

        def done(ok):
            self.busy = False
            if ok:
                self.verified(pw)
            else:
                pw[:] = bytes(len(pw))
                self.say("sudo didn't take that password. Try again, or skip those parts.")
        in_thread(lambda: self.dry_run or host.check_password(pw), done, lambda _: done(False))

    def verified(self, pw):
        self.forget_password()
        self.password = pw
        self.begin(pw)

    def show_again_password(self, state):
        self.pw_error = label("", css=("error",), visible=False)
        self.keypads = []

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
                    return self.say("sudo didn't take that password. Try again.")
                self.forget_password()
                self.password = pw
                self.install_again()
            in_thread(lambda: self.dry_run or bool(state.get("dry_run")) or host.check_password(pw), done,
                      lambda _: done(False))

        self.pw_entry, field = password_field("Your SteamOS password", go, self.keypads)
        self.page("password", label("Your password", css=("title-1",)),
                  label("The parts you picked need your SteamOS password (sudo) again."), field, self.pw_error,
                  buttons=[button("Change choices", self.show_versions),
                           button("Install again", go, css=("suggested-action",))])
        self.pw_entry.grab_focus()
