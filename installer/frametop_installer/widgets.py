"""The Frametop Installer's small GTK pieces: labels, buttons, list rows, and a password field
with an on-screen keypad for a controller's laser."""
import sys
import threading
import traceback

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk, Pango  # noqa: E402


def idle(fn, *args):
    GLib.idle_add(lambda: (fn(*args), False)[1])


def in_thread(work, done, failed=None):
    """Run work() off the main loop, then done(result) on it. If work raises, failed(error) runs
    instead, so a page waiting on it can say so; with no failed, the error is printed."""
    def run():
        try:
            result = work()
        except Exception as e:
            if failed is None:
                traceback.print_exc(file=sys.stderr)
            else:
                idle(failed, e)
            return
        idle(done, result)
    threading.Thread(target=run, daemon=True).start()


def label(text="", css=(), xalign=0, wrap=True, **kw):
    w = Gtk.Label(label=text, xalign=xalign, wrap=wrap, **kw)
    if wrap:
        w.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
    for c in css:
        w.add_css_class(c)
    return w


def button(text, on_click, css=()):
    b = Gtk.Button(label=text)
    for c in css:
        b.add_css_class(c)
    b.connect("clicked", lambda *_: on_click())
    return b


def vbox(spacing=12, **kw):
    return Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing, **kw)


def boxed_list():
    lb = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
    lb.add_css_class("boxed-list")
    return lb


def check_row(check, title, detail, extra=None):
    """A list row: a checkbox (or radio) with a title and a dimmed line under it."""
    texts = vbox(spacing=2, hexpand=True)
    t = label(title, css=("heading",))
    texts.append(t)
    sub = label(detail, css=("dim-label",))
    texts.append(sub)
    if extra:
        texts.append(extra)
    row = Gtk.Box(spacing=12, margin_top=10, margin_bottom=10, margin_start=12, margin_end=12)
    check.set_valign(Gtk.Align.CENTER)
    row.append(check)
    row.append(texts)
    lbr = Gtk.ListBoxRow(child=row, activatable=True)
    lbr.connect("activate", lambda *_: check.activate() if check.get_sensitive() else None)
    return lbr, sub


class Keypad(Gtk.Box):
    """Keys to click with a controller's laser, for the password field: in VR, SteamVR's own
    keyboard doesn't always reach a desktop window, and clicks do. A real keyboard types as usual.
    (From FrameDrop's window, framedrop/installer/progress.py.)"""

    LETTERS = ("1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm")
    SYMBOLS = ("!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~")

    def __init__(self, entry):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4, halign=Gtk.Align.CENTER)
        self.entry = entry
        self.shift = self.symbols = False
        self.rows = []
        for _ in range(4):
            row = Gtk.Box(spacing=4, halign=Gtk.Align.CENTER, homogeneous=True)
            self.rows.append(row)
            self.append(row)
        bottom = Gtk.Box(spacing=4, halign=Gtk.Align.CENTER)
        for text, action in (("Shift", self.toggle_shift), ("!#1", self.toggle_symbols),
                             ("Space", lambda: self.type(" ")), ("Delete", self.backspace)):
            b = button(text, action)
            b.set_size_request(260 if text == "Space" else 110, 48)
            bottom.append(b)
            if text == "!#1":
                self.symbols_key = b
        self.append(bottom)
        self.fill()

    def fill(self):
        for row, keys in zip(self.rows, self.SYMBOLS if self.symbols else self.LETTERS):
            while (child := row.get_first_child()) is not None:
                row.remove(child)
            for k in keys:
                k = k.upper() if self.shift and not self.symbols else k
                b = button(k, lambda k=k: self.type(k))
                b.set_size_request(56, 48)
                row.append(b)

    def type(self, text):
        self.entry.set_text(self.entry.get_text() + text)
        self.entry.set_position(-1)

    def backspace(self):
        self.entry.set_text(self.entry.get_text()[:-1])
        self.entry.set_position(-1)

    def toggle_shift(self):
        self.shift = not self.shift
        self.fill()

    def toggle_symbols(self):
        self.symbols = not self.symbols
        self.symbols_key.set_label("abc" if self.symbols else "!#1")
        self.fill()


def password_field(placeholder, on_enter, keypads):
    """A password entry with a Keypad button that shows the on-screen keys under it."""
    entry = Gtk.PasswordEntry(show_peek_icon=True, placeholder_text=placeholder, hexpand=True)
    entry.connect("activate", lambda *_: on_enter())
    keypad = Keypad(entry)
    keypad.set_visible(False)
    keypads.append(keypad)
    toggle = button("Keypad", lambda: keypad.set_visible(not keypad.get_visible()))
    row = Gtk.Box(spacing=8)
    row.append(entry)
    row.append(toggle)
    box = vbox(spacing=8)
    box.append(row)
    box.append(keypad)
    return entry, box
