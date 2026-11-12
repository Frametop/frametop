"""ft-peer-sim: someone in a Frametop Together room who isn't there (docs/peers.md).

A fake peer is Frametop's own programs with nobody wearing them: an ft-together of its own
(the real session code, on a port and a socket of its own), `ft-avatars --pretend` standing in
for a tracked wearer, `ft-roomvoice --hum` for their voice, and `ft-sharepanel --pattern` for a
screen they share. This program keeps them and answers questions on @ft_peer_sim_N, one datagram a question and one back:

  state                 ok and JSON: what the peer does, and its ft-together's own state
  join HOST PORT CODE   calls a room's owner (ft-together's join)
  leave                 leaves the room
  pose NAME             stands as one of POSES
  walk RIGHT BACK DEG   stands that far from the spot the room gave it, turned that much
  wear [NAME]           has the model NAME of its avatars folder on, or none
  speak HZ | speak off  hums that note, or is quiet
  away game|off|back    as ft-together's; nobody stands there while it is away
  point, look, ask, act, put
                        its hand and its eyes in the room (peeracts.py has them)
  share, unshare, give, take
                        a test panel of its own in the room (peershare.py has them)
  quit                  leaves, ends its programs and itself

  ft-peer-sim N [--name NAME] [--port PORT] [--loopback] [--models DIR] [--tag TAG]
              [--ft-together PATH] [--ft-avatars PATH] [--ft-roomvoice PATH] [--ft-sharepanel PATH]

N is 1 to 3. --loopback is ft-together's: no Tailscale, links on 127.0.0.1 only. --models is
the folder `wear` names models in (the wearer's own without it). --tag is for tests: the
sockets' names end in it, so two runs don't meet. It runs where the programs were built (on
the Frame, in their container), and never ships. Nothing here reads what a room sends: the
programs it starts do.
"""
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from . import peeracts, peerline, peershare

REPO = Path(__file__).resolve().parents[2]
PEERS = (1, 2, 3)
POSES = ('stand', 'wave', 'reach', 'type', 'sit', 'lean', 'look')   # avatar/view/demo.h
FIRST_PORT = 47193   # ft-together's own; peer N takes calls on FIRST_PORT + N
PROGRAMS = {'ft-together': REPO / 'together/build/ft-together', 'ft-avatars': REPO / 'screens/build/ft-avatars',
            'ft-roomvoice': REPO / 'together/build/ft-roomvoice', 'ft-sharepanel': REPO / 'together/build/ft-sharepanel'}
HOST = re.compile(r'[A-Za-z0-9][A-Za-z0-9.:-]{0,252}')
CODE = re.compile(r'[A-Za-z0-9]{1,32}')
MODEL = re.compile(r'[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}')
TAG = re.compile(r'[A-Za-z0-9_]{0,32}')
WEAR_EVERY_S = 2.0   # as ft-avatars --room tells it
PANEL_FPS = 30       # its test panel's pictures a second
QUESTIONS = ('state | join HOST PORT CODE | leave | pose NAME | walk RIGHT BACK DEGREES | wear [NAME] | speak HZ|off | away game|off|back | '
             + peeracts.QUESTIONS + ' | ' + peershare.QUESTIONS + ' | quit')


def sim_socket(n, tag=''):
    return f'ft_peer_sim_{n}{tag}'


def together_socket(n, tag=''):
    return f'ft_peer_{n}{tag}'


class Sim:
    """One fake peer: its programs, and what it is asked to do."""

    def __init__(self, n, name, port, programs, loopback=False, models=None, tag='', spawn=subprocess.Popen, clock=time.monotonic):
        self.n, self.name, self.port, self.programs, self.socket = n, name, port, programs, together_socket(n, tag)
        self.loopback, self.models, self.spawn, self.clock = loopback, models, spawn, clock
        self.together = self.body = self.voice = None
        self.pose, self.walked, self.hz, self.wears = 'stand', (0.0, 0.0, 0.0), 0.0, ''
        self.wear_reply, self.told_wear, self.away = '', -1e9, False
        self.line = peerline.Line(self.socket)
        self.acts = peeracts.Acts(lambda text: self.ask(text), lambda data: self.send(data), clock)
        self.share = peershare.Share(lambda text: self.ask(text), self.line.heard, self.picture, self.stop, lambda: not self.away,
                                     ((n - 2) * 2.4, 1.5, -2.5), clock)

    def start(self):
        env = dict(os.environ, FT_AVATARS_DIR=str(self.models)) if self.models else None
        argv = [str(self.programs['ft-together']), '--port', str(self.port), '--socket', self.socket, '--name', self.name]
        self.together = self.spawn(argv + (['--loopback'] if self.loopback else []), env=env)
        self.stand()

    def stand(self):
        """The wearer the room is told about: one pose, that far from the peer's spot. None while
        the peer is away: ft-avatars --room tells the room nothing of a wearer who is."""
        self.stop(self.body)
        self.body = None
        if self.away:
            return
        self.body = self.spawn([str(self.programs['ft-avatars']), '--pretend', self.pose, '--together', self.socket,
                                '--walked', '%g,%g,%g' % self.walked])

    def hum(self):
        self.stop(self.voice)
        self.voice = None
        if self.hz:
            self.voice = self.spawn([str(self.programs['ft-roomvoice']), '--socket', self.socket + '_voice',
                                     '--together', self.socket, '--screens', self.socket + '_screens',
                                     '--hum', '%g' % self.hz, '--probe'])

    def picture(self, panel, size):
        """The program that makes its panel's picture: a test picture that counts, encoded only
        while someone watches (together/README.md, "The sharer's side")."""
        return self.spawn([str(self.programs['ft-sharepanel']), '--panel', str(panel), '--size', '%dx%d' % size,
                           '--fps', str(PANEL_FPS), '--socket', self.socket, '--pattern'])

    @staticmethod
    def stop(child):
        if child is None or child.poll() is not None:
            return
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()

    def ask(self, text):
        """A question to the peer's own ft-together: its answer, or `error` and why there is none."""
        return self.line.ask(text)

    def send(self, data):
        """A datagram for the peer's own ft-together that has no answer: its pointer."""
        self.line.send(data)

    def tick(self):
        self.acts.tick()
        self.share.tick()
        if self.wears and self.clock() - self.told_wear >= WEAR_EVERY_S:
            self.told_wear, self.wear_reply = self.clock(), self.ask('wear ' + self.wears)

    def state(self):
        together = peeracts.told(self.ask('state'))
        running = {name: child is not None and child.poll() is None
                   for name, child in (('ft-together', self.together), ('ft-avatars', self.body), ('ft-roomvoice', self.voice),
                                       ('ft-sharepanel', self.share.child))}
        return {'peer': self.n, 'name': self.name, 'port': self.port, 'socket': self.socket, 'pose': self.pose,
                'walked': list(self.walked), 'away': self.away, 'speaks': self.hz, 'wears': self.wears, 'wear': self.wear_reply,
                'running': running, 'together': together, **self.acts.state(), **self.share.state()}

    def answer(self, text):
        words = text.split()
        try:
            return self.act(words[0] if words else '', words[1:])
        except (ValueError, IndexError):
            return 'error ' + QUESTIONS

    def act(self, what, rest):
        if what == 'state' and not rest:
            return 'ok ' + json.dumps(self.state())
        if what == 'join' and len(rest) == 3 and HOST.fullmatch(rest[0]) and 0 < int(rest[1]) < 65536 and CODE.fullmatch(rest[2]):
            self.rest()   # what it pointed at and shared was another room's
            return self.ask('join %s %d %s' % (rest[0], int(rest[1]), rest[2]))
        if what == 'leave' and not rest:
            self.rest()
            return self.ask('leave')
        if what == 'away' and rest in (['game'], ['off'], ['back']):
            if rest[0] != 'back':
                self.acts.rest()   # someone away points at nothing and looks nowhere
            reply = self.ask('away ' + rest[0])
            if reply.startswith('ok') and self.away != (rest[0] != 'back'):
                self.away = rest[0] != 'back'
                self.stand()
            return reply
        if what == 'pose' and len(rest) == 1 and rest[0] in POSES:
            self.pose = rest[0]
            self.stand()
            return 'ok'
        if what == 'walk' and len(rest) == 3:
            right, back, degrees = (float(word) for word in rest)
            if not (abs(right) <= 20 and abs(back) <= 20 and abs(degrees) <= 360):
                raise ValueError
            self.walked = (right, back, degrees)
            self.stand()
            return 'ok'
        if what == 'wear' and (not rest or MODEL.fullmatch(' '.join(rest))):
            self.wears, self.told_wear = ' '.join(rest), self.clock()
            self.wear_reply = self.ask(('wear ' + self.wears).strip())
            return self.wear_reply
        if what == 'speak' and len(rest) == 1 and (rest[0] == 'off' or 50 <= float(rest[0]) <= 2000):
            self.hz = 0.0 if rest[0] == 'off' else float(rest[0])
            self.hum()
            return 'ok'
        for theirs in (self.acts, self.share):
            reply = theirs.act(what, rest)
            if reply is not None:
                return reply
        raise ValueError

    def rest(self):
        """Out of the room it is in with its hand, its eyes and its panel."""
        self.acts.rest()
        self.share.rest()

    def end(self):
        if self.together is not None and self.together.poll() is None:
            self.ask('leave')   # ft-together leaves on SIGTERM too; this tells the room first
        for child in (self.share.child, self.voice, self.body, self.together):
            self.stop(child)
        self.line.close()


def arguments(argv):
    """(n, the options) of a command line, or None."""
    options = {'--name': None, '--port': None, '--models': None, '--loopback': False, '--tag': '',
               '--ft-together': None, '--ft-avatars': None, '--ft-roomvoice': None, '--ft-sharepanel': None}
    words = list(argv)
    if not words or not words[0].isdigit() or int(words[0]) not in PEERS:
        return None
    n, words = int(words[0]), words[1:]
    while words:
        if words[0] == '--loopback':
            options['--loopback'], words = True, words[1:]
        elif words[0] in options and len(words) >= 2:
            options[words[0]], words = words[1], words[2:]
        else:
            return None
    port = options['--port'] or str(FIRST_PORT + n)
    if not port.isdigit() or not 0 < int(port) < 65536 or not TAG.fullmatch(options['--tag']):
        return None
    options['--port'] = int(port)
    return n, options


def main(argv=None):
    parsed = arguments(sys.argv[1:] if argv is None else argv)
    if parsed is None:
        sys.exit(__doc__.strip())
    n, options = parsed
    programs = {name: Path(options['--' + name] or path) for name, path in PROGRAMS.items()}
    missing = [str(path) for path in programs.values() if not path.is_file()]
    if missing:
        sys.exit('ft-peer-sim: not built: ' + ', '.join(missing) + ' (together/build.sh, screens/build.sh)')
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
    try:
        sock.bind('\0' + sim_socket(n, options['--tag']))
    except OSError:
        sys.exit(f'ft-peer-sim: peer {n} is running already')
    sim = Sim(n, options['--name'] or f'Peer {n}', options['--port'], programs, options['--loopback'], options['--models'], options['--tag'])

    def ended(*_):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, ended)
    signal.signal(signal.SIGINT, ended)
    try:
        sim.start()
        print(f'ft-peer-sim: peer {n} ({sim.name}) takes calls on port {sim.port}', flush=True)
        while True:
            sim.tick()
            if not select.select([sock], [], [], 0.5)[0]:
                continue
            try:
                data, who = sock.recvfrom(1024)
                text = data.decode('utf-8', 'replace')
                sock.sendto(('ok' if text.strip() == 'quit' else sim.answer(text)).encode(), who)
                if text.strip() == 'quit':
                    return 0
            except OSError:
                pass   # an asker that went before its answer
    finally:
        sim.end()


if __name__ == '__main__':
    raise SystemExit(main())
