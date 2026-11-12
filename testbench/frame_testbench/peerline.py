"""A fake peer's line to its own ft-together (peersim.py): a datagram socket of its own, for
questions and their answers, for datagrams that have no answer, and for what ft-together sends
unasked to a program that asked for it (together/local/datagrams.h: those begin 0x81 or above,
an answer is text)."""
import collections
import select
import socket
import time

ANSWER_S = 2.0
MOST_KEPT = 256   # of what came unasked and nobody took yet; older ones are dropped


class Line:
    def __init__(self, name):
        self.to = '\0' + name
        self.kept = collections.deque(maxlen=MOST_KEPT)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('')

    def take(self, seconds):
        """What comes within that time: kept if it came unasked, given back if it is text."""
        if not select.select([self.sock], [], [], max(0.0, seconds))[0]:
            return None
        data = self.sock.recv(1 << 20)
        if data[:1] < b'\x80':
            return data.decode(errors='replace')
        self.kept.append(data)
        return None

    def ask(self, text):
        """A question: its answer, or `error` and why there is none."""
        try:
            while select.select([self.sock], [], [], 0)[0]:
                self.take(0)   # an answer that came too late for its question is dropped
            self.sock.sendto(text.encode(), self.to)
            deadline = time.monotonic() + ANSWER_S
            while (left := deadline - time.monotonic()) > 0:
                answer = self.take(left)
                if answer is not None:
                    return answer
            return 'error its ft-together gave no answer (timeout)'
        except OSError as error:
            return 'error its ft-together gave no answer (%s)' % (error.strerror or 'timeout')

    def send(self, data):
        """A datagram that has no answer."""
        try:
            self.sock.sendto(data, self.to)
        except OSError:
            pass   # nothing listens there now, as `state` says

    def heard(self):
        """What came unasked since this was last asked, the oldest first."""
        try:
            while select.select([self.sock], [], [], 0)[0]:
                self.take(0)
        except OSError:
            pass
        got = list(self.kept)
        self.kept.clear()
        return got

    def close(self):
        self.sock.close()
