"""ftipc: questions to Frametop's datagram sockets from Python, and their answers.

Frametop's programs listen on abstract AF_UNIX datagram sockets ("@ft_screens", "@ft_floatd",
...; conf/ftsock.h names the C side's). A question is one datagram from a socket of the asker's
own, and the answer one datagram back to it. The servers don't number their answers, so the
client makes sure a late answer can't be taken for the next one:

- Before each question it drops whatever is already waiting on its socket.
- After a question gets no answer in time, it moves to a new socket: an answer that comes
  later goes to the old one, which is closed.

    c = ftipc.Client()
    reply = c.ask("ft_screens", "remotes", timeout=1.0)   # str, or None
    if reply is None: print(c.why)                       # "nobody", "timeout" or an error

    ftipc.ask("ft_pointer_helper", "gaze on")            # one question on a socket of its own

Answers are decoded as UTF-8 (bad bytes replaced), up to MAX_REPLY bytes. Standard library only.
A Client is for one thread at a time.
"""

import socket

MAX_REPLY = 1 << 20   # the largest answer read (a longer datagram is cut there)
NOBODY, TIMEOUT = "nobody", "timeout"


def address(name):
    """The abstract address of `name` ("ft_screens" or "\\0ft_screens")."""
    return name if name.startswith("\0") else "\0" + name


class Client:
    """A socket of its own (autobound, or bound to `bind`) for questions to any server."""

    def __init__(self, timeout=1.0, bind=""):
        self.timeout = timeout
        self.why = ""   # why the last ask() returned None: NOBODY, TIMEOUT, or the error
        self._bind = bind
        self._sock = None

    def _socket(self):
        if self._sock is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
            try:
                s.bind(address(self._bind) if self._bind else "")
            except OSError:
                s.close()
                raise
            self._sock = s
        return self._sock

    def _drain(self, s):
        s.setblocking(False)
        try:
            while True:
                s.recv(MAX_REPLY)
        except OSError:   # (BlockingIOError: nothing more waiting)
            pass

    def ask(self, to, text, timeout=None):
        """Sends `text` to @to and waits up to `timeout` seconds (the client's own by default)
        for the answer: it, or None (and `why`)."""
        self.why = ""
        try:
            s = self._socket()
            self._drain(s)
            s.settimeout(self.timeout if timeout is None else timeout)
            s.sendto(text.encode() if isinstance(text, str) else text, address(to))
            return s.recv(MAX_REPLY).decode("utf-8", "replace")
        except (ConnectionRefusedError, FileNotFoundError):
            self.why = NOBODY
        except (TimeoutError, socket.timeout):
            self.why = TIMEOUT
            self.close()   # its answer, if it comes, goes to a socket that's gone
        except OSError as e:
            self.why = str(e) or type(e).__name__
        return None

    def send(self, to, text):
        """`text` to @to without waiting for an answer (one it sends is dropped before the next
        question). True if it went."""
        self.why = ""
        try:
            s = self._socket()
            s.setblocking(False)
            s.sendto(text.encode() if isinstance(text, str) else text, address(to))
            return True
        except (ConnectionRefusedError, FileNotFoundError):
            self.why = NOBODY
        except OSError as e:
            self.why = str(e) or type(e).__name__
        return False

    def close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def ask(to, text, timeout=1.0, bind=""):
    """One question on a socket of its own: the answer, or None."""
    with Client(timeout, bind) as c:
        return c.ask(to, text)


def exchange(to, text, timeout=1.0, bind=""):
    """Like ask(), with why there's no answer: (answer or None, "" or NOBODY, TIMEOUT, the error)."""
    with Client(timeout, bind) as c:
        reply = c.ask(to, text)
        return reply, c.why
