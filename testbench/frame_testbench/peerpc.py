"""A fake peer's split compute PC in a Frametop Together room (peersim.py, docs/peers.md): the
program that stands for the PC, and the windows of it the peer has in the room. The questions
to ft-peer-sim that are about it:

  pc [COUNT]   the peer's PC comes to its Frame, and COUNT of its windows (1 to 4; as many as
               it had, or 1) are panels in the room, a metre wide, in a row above the peer's
               own panel. Said again, it changes how many
  unpc         the windows leave the room, and the PC goes

The PC is the program the peer is given to start (peersim.py: `ft-pcsend`, which calls the
peer's own ft-together with a code that one made for it, as a real PC's sender does:
docs/together.md, "A PC's panels"). Its pictures are a loop that was encoded once, a second of
the test picture that counts, sent only for the windows someone watches and passed on to them
by the peer's ft-together. No encoder runs for them, however many windows there are.

The windows stay where they are put here: where someone else puts one isn't taken, and nothing
is given on them (a PC's panels take no input yet).

The rules only: what it asks and starts goes through what it is given.
"""
from .peerwords import whole

FIRST = 3               # a PC's panels are its person's numbers 3 to 6 (together/room/panels.h)
MOST = 4
SIZE = (640, 360)       # the loop's pictures (together/assets/pcloop-640x360.bin)
METRES = 1.0            # how wide a window stands in the room
APART = 1.1             # from one window's middle to the next
ABOVE = 1.0             # the row's middle over the peer's own panel's
QUESTIONS = 'pc [COUNT] | unpc'


class Pc:
    """One fake peer's PC. `ask(text)` asks the peer's ft-together and gives the answer;
    `start(address, port, code)` runs the program that stands for the PC and `stop(child)` ends
    it; `at` is the middle of the peer's own panel, which the windows stand above."""

    def __init__(self, ask, start, stop, at):
        self.ask, self.start, self.stop, self.at = ask, start, stop, tuple(at)
        self.child, self.windows = None, 0

    def words(self, window):
        x = self.at[0] + (window - (MOST - 1) / 2) * APART
        return 'share %d %d %d %g %g %g 1.000000 0.000000 0.000000 0.000000 %g 0 Window %d' % (
            FIRST + window, *SIZE, x, self.at[1] + ABOVE, self.at[2], METRES, window + 1)

    def running(self):
        return self.child is not None and self.child.poll() is None

    def call(self):
        """The PC calls the peer's ft-together, which says where it takes calls and makes the
        code. The answer that kept it from calling, when one did."""
        here, code = self.ask('here').split(), self.ask('sender').split()
        for reply in (here, code):
            if reply[:1] != ['ok']:
                return ' '.join(reply) or 'error no answer'
        if len(here) < 3 or not here[2].isdigit() or len(code) != 2:
            return 'error no answer'
        self.child = self.start(here[1], int(here[2]), code[1])
        return 'ok'

    def come(self, count):
        if not self.running():
            reply = self.call()
            if reply != 'ok':
                return reply
        for window in range(count):
            reply = self.ask(self.words(window))
            if not reply.startswith('ok'):
                return reply   # (the windows it had stay)
            self.windows = max(self.windows, window + 1)
        self.leave(count)
        return 'ok'

    def leave(self, keep=0):
        while self.windows > keep:
            self.windows -= 1
            self.ask('unshare %d' % (FIRST + self.windows))

    def rest(self):
        """Its windows out of the room: before the peer leaves or joins. The PC stays with it."""
        self.leave()

    def go(self):
        self.leave()
        self.stop(self.child)
        self.child = None
        return 'ok'

    def state(self):
        return {'pc': None if self.child is None else {'running': self.running(), 'windows': self.windows}}

    def act(self, what, rest):
        """The answer to one of the questions above, or None when it isn't one of them. A
        ValueError when it is one with words that can't be."""
        if what == 'pc' and len(rest) <= 1:
            return self.come(whole(rest[0], 1, MOST) if rest else self.windows or 1)
        if what == 'unpc' and not rest:
            return self.go()
        return None
