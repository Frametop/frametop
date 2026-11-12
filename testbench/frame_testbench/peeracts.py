"""A fake peer's hand and eyes in a Frametop Together room (peersim.py, docs/peers.md): what it
points at, where it looks, the panel it asks for, what it does on one it was given, and where
it puts one. The questions to ft-peer-sim that are about those:

  point PANEL X Y [down]       its pointer on the room's panel PANEL (its id), X across and Y
                               down the picture, each 0 to 1; `down` with a button held
  point off                    its pointer on no panel
  look YAW PITCH               it shares where it looks: degrees to its left and up from the
                               way its head faces, 63.5 at most each way
  look off                     it shares that no longer
  ask PANEL                    asks the panel's owner for it
  act PANEL move X Y           on a panel it was given: its pointer there, each 0 to 1
  act PANEL button B 0|1       button B (1 left, 2 right, 3 middle) let go or pressed
  act PANEL scroll RIGHT DOWN  a scroll, 120 a notch, 2400 at most
  act PANEL key CODE 0|1       a key, by its Linux code (1 to 111, not 99), let go or pressed
  put PANEL X Y Z [DEGREES]    puts someone's panel there in the room: its middle in metres,
                               turned that much to its left

Each is checked here and then said to the peer's own ft-together in the words a Frame's own
programs say them (together/README.md: the pointer datagram, `gaze`, `control`, `move`).
Whether the peer may is not asked here: its ft-together and the panel's owner say, and their
answer is passed back. A pointer and a look are kept up as a Frame's programs keep them up: the
pointer told again each second, the direction a few times a second.

The rules only: what it asks and sends goes through the two functions it is given.
"""
import struct

from .peerwords import FAR, PANELS, number, told, upright, whole

NO_PANEL = 255         # a pointer on none (proto/pointwire.h)
MOST_GAZE = 63.5       # degrees either way (proto/posewire.h)
POINT_EVERY_S = 1.0    # a resting hand's pointer is told this often (local/panelhand.h)
LOOK_EVERY_S = 0.4     # a direction stands for one second (local/gaze.h)
HAND_EVERY_S = 1.0     # what moves the pointer is said each second (local/gaze.h)
NOT_SHARED = "error you don't share where you look"   # ft-together's word (local/gazeask.cpp)
# what an act's two numbers may be (flow/control.h); a move's are parts of the picture
ACTS = {'button': ((1, 3), (0, 1)), 'scroll': ((-2400, 2400), (-2400, 2400)), 'key': ((1, 111), (0, 1))}
NO_KEY = 99            # SysRq
QUESTIONS = ('point PANEL X Y [down] | point off | look YAW PITCH | look off | ask PANEL | act PANEL move X Y | '
             'act PANEL button B 0|1 | act PANEL scroll RIGHT DOWN | act PANEL key CODE 0|1 | put PANEL X Y Z [DEGREES]')


def part(of):
    """A part of a picture's side, 0 to 1, as a pointer and a move say it."""
    return round(of * 65535)


class Acts:
    """One fake peer's hand and eyes. `ask(text)` asks its ft-together and gives the answer,
    `send(data)` hands it a datagram that has none, `clock()` is the time in seconds."""

    def __init__(self, ask, send, clock):
        self.ask, self.send, self.clock = ask, send, clock
        self.points = self.looks = None   # (panel, x, y, down), (yaw, pitch)
        self.pointed = self.looked = self.hand_said = -1e9

    def hand(self):
        """Its ft-together is told that its hand moves its pointer, as ft-roomgaze tells a
        Frame's. One that isn't told takes the pointer for the eyes', and keeps a pointer that
        only moves from the room (local/hush.h)."""
        if self.clock() - self.hand_said >= HAND_EVERY_S:
            self.hand_said = self.clock()
            self.ask('gaze pointer hand')

    def show(self):
        """The room is told where its pointer is now."""
        self.hand()
        self.pointed = self.clock()
        panel, x, y, down = self.points or (NO_PANEL, 0.0, 0.0, False)
        self.send(struct.pack('<BHBHHB', 3, 0, panel, part(x), part(y), int(down)))   # proto/pointwire.h; ft-together counts them

    def tick(self):
        now = self.clock()
        if self.points and now - self.pointed >= POINT_EVERY_S:
            self.show()
        if self.looks and now - self.looked >= LOOK_EVERY_S:
            self.looked = now
            if self.ask('gaze say %g %g' % self.looks) == NOT_SHARED:
                self.looks = None   # its room is gone, and what it shared with it

    def rest(self):
        """Its hand off every panel and its eyes ahead: before it leaves, goes away or joins."""
        if self.points:
            self.act('point', ['off'])
        if self.looks:
            self.act('look', ['off'])

    def state(self):
        return {'points': self.points and dict(zip(('panel', 'x', 'y', 'down'), self.points)),
                'looks': self.looks and list(self.looks),
                'control': told(self.ask('control')), 'gaze': told(self.ask('gaze'))}

    def act(self, what, rest):
        """The answer to one of the questions above, or None when it isn't one of them. A
        ValueError when it is one with words that can't be."""
        if what == 'point' and rest == ['off']:
            if self.points:
                self.points = None
                self.show()
            return 'ok'
        if what == 'point' and len(rest) in (3, 4) and rest[3:] in ([], ['down']):
            self.points = (whole(rest[0], 0, PANELS - 1), number(rest[1], 0, 1), number(rest[2], 0, 1), len(rest) == 4)
            self.show()
            return 'ok'
        if what == 'look' and rest == ['off']:
            self.looks = None
            return self.ask('gaze off')
        if what == 'look' and len(rest) == 2:
            looks = (number(rest[0], -MOST_GAZE, MOST_GAZE), number(rest[1], -MOST_GAZE, MOST_GAZE))
            reply = self.ask('gaze on')
            if reply.startswith('ok'):
                self.looks, self.looked = looks, self.clock()
                reply = self.ask('gaze say %g %g' % looks)
            return reply
        if what == 'ask' and len(rest) == 1:
            return self.ask('control ask %d' % whole(rest[0], 0, PANELS - 1))
        if what == 'act' and len(rest) == 4:
            return self.use(whole(rest[0], 0, PANELS - 1), *rest[1:])
        if what == 'put' and len(rest) in (4, 5):
            panel, at = whole(rest[0], 0, PANELS - 1), [number(word, -FAR, FAR) for word in rest[1:4]]
            turn = upright(number(rest[4], -360, 360) if len(rest) == 5 else 0.0)
            return self.ask('move %d %g %g %g %.6f 0 %.6f 0' % (panel, *at, turn[0], turn[2]))
        return None

    def use(self, panel, what, a, b):
        if what == 'move':
            words = (part(number(a, 0, 1)), part(number(b, 0, 1)))
        elif what in ACTS:
            words = tuple(whole(word, *bounds) for word, bounds in zip((a, b), ACTS[what]))
            if what == 'key' and words[0] == NO_KEY:
                raise ValueError
        else:
            raise ValueError
        self.hand()
        return self.ask('control act %d %s %d %d' % (panel, what, *words))
