"""Bring Frametop's keyboard back after it was quit.

Quitting Full Keyboard from its own panel ends it, and its unit leaves it closed until SteamVR
starts again (keyboard/FRAMETOP.md): a text field or one of Steam's fields doesn't open a keyboard
someone quit. Asking for it does. The Open/close keyboard action (a mapped button, the palm's
Quick Settings, Frametop Settings' Keyboard page) with no keyboard running starts the unit again,
and the keyboard opens once it says hello. Before this, only a SteamVR restart brought it back.

The input relay owns one KeyboardRevive: ask() from the action, take() on the keyboard's hello.
"""
import subprocess

UNIT = "frametop-keyboard.service"
# A start limit the unit ran into (5 starts in 2 minutes) would refuse the start: someone asking
# for the keyboard clears it first. $0 is the unit.
START = 'systemctl --user reset-failed "$0" 2>/dev/null; exec systemctl --user start --no-block "$0"'
WAIT = 20.0  # seconds a started keyboard has to say hello and still open (a release's first start loads slowly)
AGAIN = 5.0  # seconds before another press starts the unit again (the first start may still be on its way)


def start_unit():
    """Start the keyboard's unit without waiting for it. Its outcome goes to the unit's own log."""
    subprocess.Popen(["/bin/sh", "-c", START, UNIT], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


class KeyboardRevive:
    """Which start the relay asked for, and whether the keyboard it brings still has to open."""

    def __init__(self, start=start_unit):
        self.start = start
        self.asked = None  # when the unit was last started here
        self.open_until = 0.0  # a keyboard that says hello before then opens

    def ask(self, now):
        """The keyboard was asked for and isn't running: start it, and open it when it's there.
        True if this started the unit (False: a start from a moment ago is still on its way)."""
        self.open_until = now + WAIT
        if self.asked is not None and now - self.asked < AGAIN:
            return False
        self.asked = now
        self.start()
        return True

    def take(self, now):
        """The keyboard said its first hello: whether it's the one asked for, which opens. Once."""
        due = now < self.open_until
        self.open_until = 0.0
        return due
