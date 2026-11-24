"""Frametop Settings' Remote display keys page (RemoteKeysPage.qml): the key combinations a remote
display's computer gets while you type on it, the input rules' "remote_key_bindings"
(conf/ft_remote_keys.py says what the table means; the input relay acts on it).

The list, and what an entry can't be, are plain functions; RemoteKeys is the page's object
("backend.remoteKeys"). It reads and saves the rules, captures key combinations, names them and
says things in the window only through what ft_input_settings' Backend hands it, so it runs
without a relay in input-settings/test/test_remote_keys.py.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, Signal, Slot

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "conf"))
import ft_remote_keys  # noqa: E402  (the table's meaning and its defaults)

RELEASE_KEYS = "release_keys"


def rows(table, bindings, name, label):
    """The page's list, by what you press: one entry per combination in table ({pressed: sent}).
    bindings is Frametop's own key combinations ({combo: action}); name(combo) and label(action)
    say them as the Keyboard page does. instead is what Frametop does with the combination
    everywhere else ("" when it does nothing of its own)."""
    out = []
    for pressed, sent in sorted(table.items(), key=lambda entry: name(entry[0]).casefold()):
        action = bindings.get(pressed)
        out.append({"pressed": pressed, "sent": sent, "same": sent == pressed, "pressedLabel": name(pressed),
                    "sentLabel": name(sent), "instead": label(action) if action not in (None, "key", "none") else ""})
    return out


def releases(pressed, bindings):
    """Whether the relay takes pressed as release_keys: that combination, or it with more
    modifiers held (one may be stuck too; the relay's release_combo)."""
    *mods, key = pressed.split("+")
    for combo, action in bindings.items():
        *held, last = combo.split("+")
        if action == RELEASE_KEYS and held and last == key and set(held) <= set(mods):
            return True
    return False


def problem(pressed, sent, bindings, sendable, name):
    """Why pressed can't go to the computer as sent ("" when it can)."""
    if not ft_remote_keys.pressed_ok(pressed):
        return "That isn't a key combination a keyboard can press."
    if releases(pressed, bindings):
        return f"{name(pressed)} lets go of every key, for one stuck on the computer too: it always stays Frametop's."
    if sent != pressed and not sendable(sent):
        return f"{name(sent)} can't be sent: volume, power and sleep keys stay on the headset."
    return ""


def put(rules, pressed, sent, sendable):
    """Add or change one entry in the rules, in place. A file with no table of its own starts
    from the defaults, so adding one entry doesn't take the others away."""
    rules[ft_remote_keys.RULES_KEY] = {**ft_remote_keys.read(rules, sendable), pressed: sent}


def drop(rules, pressed, sendable):
    """Remove one entry from the rules, in place (from the defaults too, which then become the
    file's own table)."""
    table = ft_remote_keys.read(rules, sendable)
    table.pop(pressed, None)
    rules[ft_remote_keys.RULES_KEY] = table


class RemoteKeys(QObject):
    """The page's state: the list, and the entry being made (what you press, what the computer
    gets, and which of the two is being captured).

    rules(): the input rules as they read now. edit(change): change them under the file's lock
    and tell the relay (Backend._edit_rules); it returns not_saved when that failed, having said
    why. Whoever made this calls refresh() when the rules change, after an edit from here too. capture(then, dropped) takes the next key combination pressed on any keyboard, and
    cancel() gives that up. bindings(rules) is Frametop's own key combinations, name(combo) and
    label(action) say them, sendable(combo) is the relay's limit on keys it taps, and
    say(text, is_error) shows a line in the window."""
    changed = Signal()
    draftChanged = Signal()

    def __init__(self, rules, edit, not_saved, capture, cancel, bindings, name, label, sendable, say, parent=None):
        super().__init__(parent)
        self._rules, self._edit, self._not_saved = rules, edit, not_saved
        self._capture, self._cancel = capture, cancel
        self._bindings, self._name, self._label, self._sendable, self._say = bindings, name, label, sendable, say
        self._pressed = ""    # the new entry's pressed side ("" until captured)
        self._sent = ""       # its sent side ("": the same keys)
        self._capturing = ""  # "pressed" or "sent" while that side is being captured

    def refresh(self):
        """The rules changed, here or elsewhere (a combination bound on the Keyboard page): the
        list and what's wrong with the new entry are read again."""
        self.changed.emit()
        self.draftChanged.emit()

    @Property("QVariantList", notify=changed)
    def rows(self):
        rules = self._rules()
        return rows(ft_remote_keys.read(rules, self._sendable), self._bindings(rules), self._name, self._label)

    @Property(bool, notify=changed)
    def isDefault(self):
        """Whether the list is the defaults: the rules have no table of their own."""
        return not isinstance(self._rules().get(ft_remote_keys.RULES_KEY), dict)

    @Property(str, notify=draftChanged)
    def capturing(self):
        return self._capturing

    @Property(str, notify=draftChanged)
    def pressed(self):
        return self._pressed

    @Property(str, notify=draftChanged)
    def pressedLabel(self):
        return self._name(self._pressed) if self._pressed else ""

    @Property(str, notify=draftChanged)
    def sentLabel(self):
        """The other keys the computer gets for the new entry, in words ("": the same keys)."""
        return self._name(self._sent) if self._sent else ""

    @Property(str, notify=draftChanged)
    def problem(self):
        """Why the new entry can't be added as it stands ("" when it can, or there's none yet)."""
        if not self._pressed:
            return ""
        return problem(self._pressed, self._sent or self._pressed, self._bindings(self._rules()), self._sendable,
                       self._name)

    @Slot(str)
    def capture(self, side):
        """Take the next key combination pressed as the new entry's "pressed" or "sent" side."""
        if side not in ("pressed", "sent"):
            return
        self._capture(lambda combo: self._captured(side, combo), self._capture_dropped)
        self._capturing = side  # (after: taking a capture drops this page's earlier one, which clears it)
        self.draftChanged.emit()

    def _captured(self, side, combo):
        self._capturing = ""
        if side == "pressed":
            self._pressed = combo
        else:
            self._sent = combo
        if self._sent == self._pressed:
            self._sent = ""  # the same keys
        self.draftChanged.emit()

    def _capture_dropped(self):
        self._capturing = ""
        self.draftChanged.emit()

    @Slot()
    def cancelCapture(self):
        if self._capturing:
            self._cancel()
            self._capture_dropped()

    @Slot()
    def sendSame(self):
        """The new entry goes to the computer as the keys you press."""
        self._sent = ""
        self.draftChanged.emit()

    @Slot()
    def add(self):
        pressed, sent = self._pressed, self._sent or self._pressed
        if not pressed or self.problem:
            return
        if self._edit(lambda rules: put(rules, pressed, sent, self._sendable)) is self._not_saved:
            return
        self._pressed, self._sent = "", ""
        self.draftChanged.emit()
        self._say(f"{self._name(pressed)} goes to the computer" + ("" if sent == pressed else f" as {self._name(sent)}"),
                  False)

    @Slot(str)
    def remove(self, pressed):
        if self._edit(lambda rules: drop(rules, pressed, self._sendable)) is self._not_saved:
            return
        self._say(f"{self._name(pressed)} stays Frametop's on a remote display", False)

    @Slot()
    def reset(self):
        """Back to the defaults: the rules lose their own table."""
        if self._edit(lambda rules: rules.pop(ft_remote_keys.RULES_KEY, None)) is self._not_saved:
            return
        self._say("Remote display keys: back to the defaults", False)
