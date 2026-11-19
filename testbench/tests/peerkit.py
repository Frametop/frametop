"""What the tests that run a fake peer for real share (test_peersim.py, test_peeracts.py): an
`ft-together --loopback` whose room it comes into, a real ft-peer-sim, and a socket for
questions to each."""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from frame_testbench import peersim

TESTBENCH = Path(__file__).resolve().parents[1]
sys.path.append(str(TESTBENCH.parent / 'together' / 'tests'))   # after testbench's own
from roomkit import free_port  # noqa: E402  (ports no other test running beside this one gets)
POSE_HEAD = '<BH3f'   # together/proto/posewire.h: kind, count, the eyes' place
BUILT = all(path.is_file() for path in peersim.PROGRAMS.values())
NOT_BUILT = 'Frametop Together is not built (together/build.sh, screens/build.sh)'


class Asker:
    """A socket of its own for questions to one program, which also gets what the program sends it."""

    def __init__(self, name):
        self.to = '\0' + name
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('')

    def ask(self, text, seconds=3.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                self.sock.sendto(text.encode(), self.to)
                while True:
                    data = self.sock.recv(1 << 20)
                    if data[:1] < b'\x80':   # what comes unasked begins 0x81 or above (together/local/datagrams.h)
                        return data.decode()
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.05)   # not listening yet
            except OSError:
                break
        return 'none'

    def state(self):
        reply = self.ask('state')
        return json.loads(reply[3:]) if reply.startswith('ok {') else {}

    def until(self, what, seconds=10.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(self.state()):
                return True
            time.sleep(0.05)
        return False

    def got(self, kind, question, seconds):
        """What the program passed on in that time, once asked for it: [(person, datagram)]."""
        self.ask(question)
        return self.heard(kind, seconds)

    def heard(self, kind, seconds):
        """What the program passed on since it last answered, and in that time."""
        got, deadline = [], time.monotonic() + seconds
        while (left := deadline - time.monotonic()) > 0:
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except OSError:
                break
            if data[:1] == kind:
                got.append((data[1], data[2:]))
        return got


class RealRoom(unittest.TestCase):
    """An owner's ft-together and fake peer 1, both running, the peer in no room yet."""

    def setUp(self):
        self.tag = '_%d' % os.getpid()
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.addCleanup(self.log.close)
        self.owner_socket = 'ft_peer_test_owner' + self.tag
        self.owner_proc = self.run_program([str(peersim.PROGRAMS['ft-together']), '--loopback', '--name', 'Owner',
                                            '--port', str(self.port), '--socket', self.owner_socket])
        self.sim_proc = self.run_program([sys.executable, str(TESTBENCH / 'ft-peer-sim'), '1', '--loopback', '--name', 'Sim',
                                          '--port', str(free_port()), '--tag', self.tag, '--models', tempfile.gettempdir()])
        self.owner, self.sim = Asker(self.owner_socket), Asker(peersim.sim_socket(1, self.tag))
        self.addCleanup(self.owner.sock.close)
        self.addCleanup(self.sim.sock.close)

    def run_program(self, argv):
        proc = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT, cwd=TESTBENCH)
        self.addCleanup(self.end, proc)
        return proc

    @staticmethod
    def end(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def said(self):
        self.log.seek(0)
        return self.log.read().decode(errors='replace')[-3000:]

    def eyes(self, seconds=0.6):
        """Where the owner is told the peer's eyes are: the newest (x, z)."""
        poses = [pose for person, pose in self.owner.got(b'\x81', 'watch', seconds) if person == 1]
        self.assertTrue(poses, 'the owner got no pose of the peer\n' + self.said())
        _, _, x, _, z = struct.unpack_from(POSE_HEAD, poses[-1])
        return x, z, poses

    def join(self):
        """The owner opens a room, and the peer is invited, calls and is let in."""
        self.assertEqual(self.owner.ask('open'), 'ok', self.said())
        code = self.owner.ask('invite')
        self.assertTrue(code.startswith('ok '), code)
        self.assertTrue(self.sim.until(lambda s: s.get('together', {}).get('mode') == 'idle'), self.said())
        self.assertEqual(self.sim.ask('join 127.0.0.1 %d %s' % (self.port, code[3:])), 'ok')
        self.assertTrue(self.owner.until(lambda s: s.get('asks')), self.said())
        ask = self.owner.state()['asks'][0]
        self.assertEqual(ask['name'], 'Sim')
        self.assertEqual(self.owner.ask('accept %d' % ask['id']), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['together'].get('mode') == 'joined' and s['together'].get('me') == 1), self.said())
