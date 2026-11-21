"""Dictation's notice in the headset (ft-screens' vrvoice.cpp): what ft-voiced tells @ft_screens.

"voice listen hold|toggle|command LEVEL" while it listens (LEVEL: the mic's, 0..1), at most every
NOTICE_EVERY seconds; "voice typing" while the last words are decoded and typed; "voice off". ft-screens
hides the pill when the words stop coming, so a lost "off" or a crash can't leave it up. Datagrams,
never blocking: no ft-screens (no desktop), nothing happens.
"""
import math
import socket
import time

SCREENS = "\0ft_screens"
NOTICE_EVERY = 0.12  # s: about 8 a second, enough for the level bars and ft-screens' 1.5 s watch
FULL_RMS = 0.06  # a speaking voice's RMS on the Frame's mic: the bars at full height


def level_of(rms):
    """0..1 for the bars: square root, so quiet speech still moves them."""
    return min(1.0, math.sqrt(max(0.0, rms) / FULL_RMS))


class Notice:
    def __init__(self, send=None, clock=time.monotonic):
        self.sock = None
        self.send = send or self._send
        self.clock = clock
        self.mode = None  # "hold", "toggle" or "command" while it listens
        self.last = 0.0

    def _send(self, text):
        try:
            if self.sock is None:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
            self.sock.sendto(text.encode(), SCREENS)
        except OSError:
            pass  # ft-screens not running

    def listening(self, mode):
        self.mode = mode
        self.last = self.clock()
        self.send(f"voice listen {mode} 0")

    def level(self, rms):
        """A chunk's RMS while it listens; sent at most every NOTICE_EVERY."""
        now = self.clock()
        if self.mode is None or now - self.last < NOTICE_EVERY:
            return
        self.last = now
        self.send(f"voice listen {self.mode} {level_of(rms):.2f}")

    def typing(self):
        self.mode = None
        self.send("voice typing")

    def off(self):
        self.mode = None
        self.send("voice off")
