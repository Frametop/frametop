"""Fake peers for Frametop Together (docs/peers.md): starting and stopping an ft-peer-sim
(peersim.py), the questions to it, and letting one into this Frame's own room the way its
wearer would: with an invite, and a yes to its ask."""
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

from . import peersim

ROOT = Path(__file__).resolve().parents[1]
UNIT = 'frame-testbench-peer-%d'
OWNER = 'ft_together'   # this Frame's own ft-together
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9 ._-]{0,31}')
SOCKET = re.compile(r'[A-Za-z0-9_]{1,64}')
START_S, STOP_S, JOIN_S = 40.0, 15.0, 20.0


def exchange(name, text, timeout=5.0):
    """One question to a Frametop datagram socket: its answer, or None when nothing listens there."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC) as sock:
        sock.bind('')
        sock.settimeout(timeout)
        try:
            sock.sendto(text.encode(), '\0' + name)
        except (ConnectionRefusedError, FileNotFoundError):
            return None
        try:
            return sock.recv(1 << 20).decode(errors='replace')
        except socket.timeout:
            raise RuntimeError(f'@{name} did not answer {text.split()[0]!r}') from None


def ask(n, text, tag=''):
    """A question to peer n, whose answer starts with ok."""
    reply = exchange(peersim.sim_socket(n, tag), text)
    if reply is None:
        raise RuntimeError(f"peer {n} isn't running: frame-testbench peer {n} start")
    if not reply.startswith('ok'):
        raise RuntimeError(f'peer {n}: {reply}')
    return reply


def state(n, tag=''):
    return json.loads(ask(n, 'state', tag)[3:])


def tell(n, text, tag=''):
    return {'ok': True, 'peer': n, 'reply': ask(n, text, tag)}


def in_box():
    """Whether this runs in the container Frametop's programs were built in."""
    return os.path.exists('/run/.containerenv')


def start(n, name=None, loopback=False, models=None, tag='', run=subprocess.run, boxed=None):
    """Runs peer n: on the Frame as a transient user unit, in the programs' container."""
    if name is not None and not NAME.fullmatch(name):
        raise ValueError('a name is up to 32 letters, digits, spaces, dots, dashes and underscores')
    if models is not None and not Path(models).is_dir():
        raise ValueError(f'{models}: not a folder of models')
    if exchange(peersim.sim_socket(n, tag), 'state') is not None:
        raise RuntimeError(f'peer {n} is running already')
    argv = [str(ROOT / 'ft-peer-sim'), str(n)] + (['--name', name] if name else []) + (['--loopback'] if loopback else [])
    argv += (['--models', str(Path(models).resolve())] if models else []) + (['--tag', tag] if tag else [])
    if in_box() if boxed is None else boxed:   # a test in the container: on its own, and nobody's child
        run(['setsid', '--fork', sys.executable] + argv, check=True, timeout=20,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        run(['systemd-run', '--user', '--quiet', '--collect', '--unit=' + UNIT % n, '--working-directory=' + str(ROOT),
             str(ROOT.parent / 'scripts/in-box'), 'python3'] + argv, check=True, timeout=20)
    deadline = time.monotonic() + START_S
    while time.monotonic() < deadline:
        try:
            if exchange(peersim.sim_socket(n, tag), 'state', timeout=1.0) is not None:
                return dict(state(n, tag), ok=True)
        except RuntimeError:
            pass   # there, and its ft-together isn't yet
        time.sleep(0.2)
    raise RuntimeError(f'peer {n} did not start: journalctl --user -u {UNIT % n}')


def stop(n, tag='', run=subprocess.run):
    """Peer n leaves, and its programs end. Whether one was running."""
    name = peersim.sim_socket(n, tag)
    if exchange(name, 'quit') is None:
        return False
    deadline = time.monotonic() + STOP_S
    while time.monotonic() < deadline:
        try:
            if exchange(name, 'state', timeout=0.5) is None:
                return True
        except RuntimeError:
            pass   # busy ending
        time.sleep(0.1)
    # It leaves and ends its programs on SIGTERM too. A signal to the unit wouldn't reach it in the container.
    run(['pkill', '-TERM', '-f', f'ft-peer-sim {n}( |$)'], timeout=10)
    raise RuntimeError(f'peer {n} did not end when asked, and was told to with SIGTERM')


def stop_all(tag='', run=subprocess.run):
    """Every fake peer goes. Who was running, and who wouldn't go."""
    stopped, stuck = [], {}
    for n in peersim.PEERS:
        try:
            if stop(n, tag, run):
                stopped.append(n)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            stuck[str(n)] = str(error)
    return dict({'stopped': stopped}, **({'stuck': stuck} if stuck else {}))


def owner_state(owner):
    reply = exchange(owner, 'state')
    if reply is None:
        raise RuntimeError("Frametop Together isn't on on this Frame: together/run.sh on, or Frametop Settings")
    if not reply.startswith('ok {'):
        raise RuntimeError(f'@{owner}: {reply}')
    return json.loads(reply[3:])


def owner_ask(owner, text):
    reply = exchange(owner, text) or 'nothing'
    if not reply.startswith('ok'):
        raise RuntimeError(f'@{owner} answered {text.split()[0]!r} with: {reply}')
    return reply[3:]


def join_here(n, owner=OWNER, tag='', seconds=JOIN_S):
    """Peer n comes into the room this Frame's wearer has open: the testbench makes the invite
    and says yes to the ask, as the wearer would on the Frametop Together page."""
    if not SOCKET.fullmatch(owner):
        raise ValueError('not a socket name')
    if owner_state(owner)['mode'] != 'hosting':
        raise RuntimeError('this Frame has no room of its own open: open one on the Frametop Together page, or with together/ft-room open')
    peer = state(n, tag)
    if peer['together'].get('mode') != 'idle':
        raise RuntimeError(f"peer {n} can't join now: its session is {peer['together'].get('mode', peer['together'])}")
    address, port = owner_ask(owner, 'here').split()[:2]
    ask(n, f"join {address} {int(port)} {owner_ask(owner, 'invite')}", tag)
    deadline, accepted = time.monotonic() + seconds, False
    while time.monotonic() < deadline:
        if not accepted:
            for waiting in owner_state(owner)['asks']:
                if waiting['name'] == peer['name'] and waiting['address'] == address:
                    owner_ask(owner, 'accept %d' % waiting['id'])
                    accepted = True
        together = state(n, tag)['together']
        if together.get('mode') == 'joined':
            return {'ok': True, 'peer': n, 'place': together['me'], 'people': len(together['people'])}
        if together.get('mode') == 'idle':
            raise RuntimeError(f"peer {n} was not let in: {together.get('note') or 'no reason given'}")
        time.sleep(0.1)
    raise RuntimeError(f'peer {n} was not in the room after {seconds:g} s')
