"""A fake peer's own panel in a Frametop Together room (peersim.py, docs/peers.md): a test
picture it shares, where the panel stands, who it gave it to and what they did on it. The
questions to ft-peer-sim that are about it:

  share [W H [X Y Z [DEGREES]]]  puts a test panel into the room: a picture of W by H pixels
                                 that counts (1280 by 720), two metres wide, its middle there
                                 in the room and turned that much to its left. Said again, it
                                 moves the panel
  unshare                        takes it back
  give PERSON                    gives the panel to the person in that place of the room
  take [PERSON]                  takes it back from them, or from everyone, and says no to
                                 their ask

The picture is the program the peer is given to start (peersim.py: `ft-sharepanel --pattern`,
the program a Frame shares a screen with, fed a test picture in place of a screen). It encodes
only while someone watches the panel.

While it shares, the peer does for its panel what a Frame's programs do for a screen
(together/README.md): it says its wearer is there each second (`control here`), without which
nothing can be given; it takes where the others put the panel (`moves`) and tells the room it
stands there now; and it takes the input the others give it (`inputs`). There is no desktop
behind the panel: the input is counted and the last of it kept, for `state`.

The rules only: what it asks, starts and hears goes through what it is given.
"""
import struct

from .peerwords import FAR, PEOPLE, number, upright, whole

PANEL = 0                   # the peer's one panel, by its number among the peer's own
WIDTHS = (640, 896, 1280, 1408, 1664, 1920)   # across, that the Frame's encoder is known to take: multiples of 128 (together/README.md)
HEIGHTS = (360, 1200)       # down, in steps of 8
SIZE = (1280, 720)
METRES = 2.0                # how wide it stands in the room
NAME = 'Test panel'
HERE_EVERY_S = 1.0          # a grant stands ten seconds after the last word (flow/control.h)
INPUTS_EVERY_S = 1.0        # ...and input comes for five after it was asked for (local/inputs.h)
LAST = 16                   # acts kept for `state`
ACT = '<BBii'               # an act's body: the panel's id, what it is, and two numbers (proto/control.h)
WHAT = {0: 'let go', 1: 'move', 2: 'button', 3: 'scroll', 4: 'key'}
QUESTIONS = 'share [W H [X Y Z [DEGREES]]] | unshare | give PERSON | take [PERSON]'


class Share:
    """One fake peer's panel. `ask(text)` asks its ft-together and gives the answer, and
    `came()` gives what its ft-together sent unasked since (the input, once asked for);
    `start(panel, size)` runs the program that makes the panel's picture and `stop(child)` ends
    it; `there()` says whether the peer's wearer counts as wearing the headset; `at` is where
    the panel stands when nobody says."""

    def __init__(self, ask, came, start, stop, there, at, clock):
        self.ask, self.came, self.start, self.stop, self.there, self.clock = ask, came, start, stop, there, clock
        self.default = tuple(at)
        self.size = self.place = self.child = None   # (w, h), (x, y, z, qw, qx, qy, qz)
        self.here_said = self.inputs_asked = -1e9
        self.inputs, self.last = 0, []

    def words(self, size, place):
        return 'share %d %d %d %g %g %g %.6f %.6f %.6f %.6f %g 0 %s' % (PANEL, *size, *place, METRES, NAME)

    def running(self):
        return self.child is not None and self.child.poll() is None

    def share(self, size, place):
        if self.size and size != self.size:
            self.unshare()   # a panel's video keeps its size: another size is another panel
        reply = self.ask(self.words(size, place))
        if not reply.startswith('ok'):
            return reply
        self.size, self.place = size, place
        if not self.running():
            self.child = self.start(PANEL, size)
        return reply

    def unshare(self):
        self.stop(self.child)
        self.size = self.place = None
        return self.ask('unshare %d' % PANEL)

    def rest(self):
        """Its panel out of the room: before it leaves or joins."""
        if self.size:
            self.unshare()

    def here(self, now=False):
        """Its wearer is there, said each second while they are: what it gave stands on that."""
        if self.there() and (now or self.clock() - self.here_said >= HERE_EVERY_S):
            self.here_said = self.clock()
            self.ask('control here')

    def moved(self):
        """Where someone who may put the panel since this was last asked: the room is told it
        stands there now, as a Frame tells it once its screen is there."""
        for word in self.ask('moves').split()[2:]:   # after `ok` and how many
            parts = word.split(':')   # the panel's number, who put it, where, and its turn
            try:
                if len(parts) != 9 or int(parts[0]) != PANEL:
                    continue
                place = (*(number(part, -FAR, FAR) for part in parts[2:5]), *(number(part, -1, 1) for part in parts[5:]))
            except ValueError:
                continue
            if self.ask(self.words(self.size, place)).startswith('ok'):
                self.place = place

    def heard(self):
        for data in self.came():
            if data[:1] == b'\x85' and len(data) == 2 + struct.calcsize(ACT):
                _, what, a, b = struct.unpack_from(ACT, data, 2)
                self.inputs += 1
                self.last = (self.last + [{'person': data[1], 'what': WHAT.get(what, str(what)), 'a': a, 'b': b}])[-LAST:]

    def tick(self):
        if not self.size:
            return
        self.here()
        if self.clock() - self.inputs_asked >= INPUTS_EVERY_S:
            self.inputs_asked = self.clock()
            self.ask('inputs')
        self.moved()
        self.heard()

    def state(self):
        self.heard()
        return {'shares': self.size and {'size': list(self.size), 'at': list(self.place[:3]), 'turn': list(self.place[3:]), 'running': self.running(),
                                         'inputs': self.inputs, 'last': self.last}}

    def act(self, what, rest):
        """The answer to one of the questions above, or None when it isn't one of them. A
        ValueError when it is one with words that can't be."""
        if what == 'share' and len(rest) in (0, 2, 5, 6):
            size = (whole(rest[0], WIDTHS[0], WIDTHS[-1]), whole(rest[1], *HEIGHTS)) if rest else self.size or SIZE
            if size[0] not in WIDTHS or size[1] % 8:
                raise ValueError
            place = self.place or (*self.default, *upright(0.0))
            if len(rest) >= 5:
                place = (*(number(word, -FAR, FAR) for word in rest[2:5]), *upright(number(rest[5], -360, 360) if len(rest) == 6 else 0.0))
            return self.share(size, place)
        if what == 'unshare' and not rest:
            return self.unshare()
        if what == 'give' and len(rest) == 1:
            person = whole(rest[0], 0, PEOPLE - 1)
            self.here(now=True)   # a peer that has just shared hasn't said it yet
            return self.ask('control give %d %d' % (PANEL, person))
        if what == 'take' and len(rest) <= 1:
            return self.ask(('control take %d %s' % (PANEL, whole(rest[0], 0, PEOPLE - 1) if rest else '')).strip())
        return None
