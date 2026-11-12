"""The CLI's `peer` commands: a fake peer for Frametop Together (peers.py, docs/peers.md)."""
from . import peeracts, peers, peershare, peersim, peerwords


def hz(text):
    if text != 'off' and not 50 <= float(text) <= 2000:
        raise ValueError('a note is 50 to 2000 Hz, or off')
    return text


def panel(text):
    if not 0 <= int(text) < peeracts.PANELS:
        raise ValueError("a panel's id is 0 to %d" % (peeracts.PANELS - 1))
    return int(text)


def size(text):
    width, height = (int(word) for word in text.split('x'))
    if width not in peershare.WIDTHS or not peershare.HEIGHTS[0] <= height <= peershare.HEIGHTS[1] or height % 8:
        raise ValueError('a size is WxH: W one of %s, H a multiple of 8 from %d to %d' % (', '.join(map(str, peershare.WIDTHS)), *peershare.HEIGHTS))
    return width, height


def person(text):
    if not 0 <= int(text) < peerwords.PEOPLE:
        raise ValueError("someone's place in a room is 0 to %d" % (peerwords.PEOPLE - 1))
    return int(text)


def add_share(does):
    """A peer's own test panel in a room (peershare.py)."""
    share = does.add_parser('share', help='puts a test panel of its own into the room, two metres wide, or moves the one it has')
    share.add_argument('--size', type=size, default=peershare.SIZE, metavar='WxH', help="its picture's size in pixels (1280x720)")
    share.add_argument('--at', type=float, nargs=3, metavar=('X', 'Y', 'Z'), help='its middle in the room, in metres (where it was, or a spot of its own)')
    share.add_argument('--turn', type=float, default=0.0, metavar='DEGREES', help='with --at: turned that much to its left')
    does.add_parser('unshare', help='takes its panel back')
    does.add_parser('give', help='gives its own panel to the person in that place of the room').add_argument('person', type=person)
    does.add_parser('take', help='takes its panel back from them, or from everyone').add_argument('person', type=person, nargs='?')


def add_acts(does):
    """What a peer does in a room with its hand and its eyes (peeracts.py)."""
    point = does.add_parser('point', help='its pointer on a panel of the room (its id), across and down the picture, each 0 to 1; or off')
    point.add_argument('where', nargs='+', metavar='PANEL X Y | off')
    point.add_argument('--down', action='store_true', help='with a button held')
    look = does.add_parser('look', help='it shares where it looks: degrees to its left and up from the way its head faces; or off')
    look.add_argument('where', nargs='+', metavar='YAW PITCH | off')
    does.add_parser('ask', help="asks a panel's owner for it").add_argument('panel', type=panel)
    act = does.add_parser('act', help='on a panel it was given: move X Y (each 0 to 1), button B 0|1, scroll RIGHT DOWN, key CODE 0|1')
    act.add_argument('panel', type=panel)
    act.add_argument('act', choices=('move',) + tuple(peeracts.ACTS))
    act.add_argument('numbers', nargs=2, metavar='N')
    put = does.add_parser('put', help="puts someone's panel there in the room: its middle in metres, and a turn to its left in degrees")
    put.add_argument('panel', type=panel)
    for name in ('x', 'y', 'z'):
        put.add_argument(name, type=float)
    put.add_argument('degrees', type=float, nargs='?', default=0.0)


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
    add_acts(does)
    add_share(does)


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
    if what in ('point', 'look'):
        return peers.tell(n, ' '.join([what] + args.where + (['down'] if getattr(args, 'down', False) else [])))
    if what == 'ask':
        return peers.tell(n, 'ask %d' % args.panel)
    if what == 'act':
        return peers.tell(n, 'act %d %s %s %s' % (args.panel, args.act, *args.numbers))
    if what == 'put':
        return peers.tell(n, 'put %d %r %r %r %r' % (args.panel, args.x, args.y, args.z, args.degrees))
    if what == 'share':
        if args.turn and not args.at:
            raise ValueError('--turn goes with --at')
        return peers.tell(n, 'share %d %d' % args.size + (' %r %r %r %r' % (*args.at, args.turn) if args.at else ''))
    if what in ('give', 'take'):
        return peers.tell(n, what if args.person is None else '%s %d' % (what, args.person))
    words = {'leave': '', 'unshare': '', 'pose': getattr(args, 'pose', ''), 'wear': getattr(args, 'model', None) or '',
             'speak': getattr(args, 'hz', ''), 'away': getattr(args, 'away', '')}
    return peers.tell(n, (what + ' ' + words[what]).strip())
