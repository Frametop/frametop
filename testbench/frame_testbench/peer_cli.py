"""The CLI's `peer` commands: a fake peer for Frametop Together (peers.py, docs/peers.md)."""
from . import peers, peersim


def hz(text):
    if text != 'off' and not 50 <= float(text) <= 2000:
        raise ValueError('a note is 50 to 2000 Hz, or off')
    return text


def add_parser(commands):
    peer = commands.add_parser('peer', help='a fake peer for Frametop Together: its real programs with nobody wearing them')
    peer.add_argument('n', type=int, choices=peersim.PEERS, help='which of the three')
    does = peer.add_subparsers(dest='peer_command', required=True)
    start = does.add_parser('start', help='run it: an ft-together of its own on port 47193 + N, and someone standing')
    start.add_argument('--name', help='what the others call it (Peer N)')
    start.add_argument('--loopback', action='store_true', help="as ft-together's: no Tailscale, links on 127.0.0.1 only")
    start.add_argument('--models', help='the folder `wear` names models in (your own avatars folder)')
    does.add_parser('stop', help='it leaves and its programs end')
    does.add_parser('state', help="what it does, and its ft-together's own state")
    join = does.add_parser('join', help="into this Frame's open room (invited and let in for you), or the room at HOST PORT with CODE")
    join.add_argument('invite', nargs='*', metavar='HOST PORT CODE')
    join.add_argument('--together', default=peers.OWNER, metavar='SOCKET', help="this Frame's ft-together, when it isn't @ft_together")
    does.add_parser('leave', help='out of the room')
    does.add_parser('pose', help='how it stands').add_argument('pose', choices=peersim.POSES)
    walk = does.add_parser('walk', help='where it stands, from the spot the room gave it: metres to its right and back, and a turn in degrees')
    for name in ('right', 'back', 'degrees'):
        walk.add_argument(name, type=float)
    does.add_parser('wear', help='the model it has on, or none').add_argument('model', nargs='?')
    does.add_parser('speak', help='it hums a note of that many Hz, or is quiet').add_argument('hz', type=hz, metavar='HZ|off')
    does.add_parser('away', help='in a game, the headset off, or back').add_argument('away', choices=('game', 'off', 'back'))


def dispatch(args):
    n, what = args.n, args.peer_command
    if what == 'start':
        return peers.start(n, args.name, args.loopback, args.models)
    if what == 'stop':
        return {'ok': True, 'peer': n, 'was_running': peers.stop(n)}
    if what == 'state':
        return dict(peers.state(n), ok=True)
    if what == 'join' and not args.invite:
        return peers.join_here(n, args.together)
    if what == 'join':
        if len(args.invite) != 3:
            raise ValueError('join takes HOST PORT CODE, or nothing for the room open on this Frame')
        return peers.tell(n, 'join ' + ' '.join(args.invite))
    if what == 'walk':
        return peers.tell(n, 'walk %r %r %r' % (args.right, args.back, args.degrees))
    words = {'leave': '', 'pose': getattr(args, 'pose', ''), 'wear': getattr(args, 'model', None) or '',
             'speak': getattr(args, 'hz', ''), 'away': getattr(args, 'away', '')}
    return peers.tell(n, (what + ' ' + words[what]).strip())
