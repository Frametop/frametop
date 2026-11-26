"""ft-npud's client interface: the shared memory, the doorbell socket and the request bytes. No DSP here, so the
trust boundary can be tested on the CPU (test_transport.py).

The memory is a sealed memfd (F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_SEAL): the daemon maps it and hands the client the
same fd over the socket (SCM_RIGHTS) once SO_PEERCRED shows the client's UID. A client can't shrink it under the
daemon's mapping (a SIGBUS in the root daemon in the middle of a DSP job), and a client from an earlier daemon run
holds a different file.

  memory    SHM_BYTES: header (HDR), then 4 crop slots of 224*224 u8 at CROPS (a smaller graph reads each slot's
            first rows), then from OUTS a request's n crops' outputs, dense, one after another (heat s8 [21, h, w] |
            kp_feat s8 [kpd, h, w] | pool u8 [g, ph, pw] a crop, and for a two-view graph | pool_pre u8 [g, ph, pw],
            its features before the partner fusion; room for four of the 224 graph's, OUT_BYTES each),
            then 4 detector output slots at DET_OUTS: obj s8 [1, 28, 28] | reg s8 [4, 28, 28] (value = q * the
            header's scale).
  socket    SOCK_SEQPACKET, mode 600, owned by UID. On connect the daemon sends one byte: HELLO with the memory's
            fd, or BUSY (no fd) while it serves another client, and closes. Request: 1 byte n (1..4: landmarks on n
            crops; DET | n: the detector on n whole camera images in the crop slots). A two-view landmark graph
            (two_view.py, mode f) reads crops in pairs (0, 1) and (2, 3): PAIR0 | n and PAIR1 | n say that the pair's
            two crops are one hand's views (each gets its partner's features); without the bit each is read
            alone. PAIR0 needs n >= 2 and PAIR1 n == 4; a one-view graph or the detector takes neither. Reply: 1
            byte, OK once the outputs and the header are written, BAD for a malformed request. Replies never
            block: a client that stops reading is dropped.
"""
import array, fcntl, os, select, socket, struct, mmap

# fcntl's names for the seals, or Linux's own numbers for them (include/uapi/linux/fcntl.h: fixed ABI) where this
# Python was built without them: the tree's CPython (python-build-standalone, npu/build.sh) has none of them
F_ADD_SEALS = getattr(fcntl, 'F_ADD_SEALS', 1033)
F_SEAL_SEAL = getattr(fcntl, 'F_SEAL_SEAL', 0x1)
F_SEAL_SHRINK = getattr(fcntl, 'F_SEAL_SHRINK', 0x2)
F_SEAL_GROW = getattr(fcntl, 'F_SEAL_GROW', 0x4)

MAGIC, VERSION = 0x4450504E, 2                    # 'NPPD'; version 2: the memory comes over the socket
# u32 magic, version, last request byte, frames, features, frames, bad requests, graph id |
# f32 DSP ms, daemon ms (request received to outputs published), obj scale, reg scale, 4 x pad
HDR = struct.Struct('<8I8f')
CROP = 224 * 224
CROPS = 4096
OUT_SHAPES = (('heat', 21, 28, 28), ('kp_feat', 32, 28, 28), ('pool', 128, 7, 7))
DET_SHAPES = (('obj', 1, 28, 28), ('reg', 4, 28, 28))
OUT_BYTES = sum(c * h * w for _, c, h, w in OUT_SHAPES)
DET_BYTES = sum(c * h * w for _, c, h, w in DET_SHAPES)
OUTS = CROPS + 4 * CROP
DET_OUTS = OUTS + 4 * OUT_BYTES
SHM_BYTES = (DET_OUTS + 4 * DET_BYTES + 4095) // 4096 * 4096
DET, FEAT_DET = 0x10, 1
PAIR0, PAIR1 = 0x20, 0x40
HELLO, BUSY, OK, BAD = b'\x00', b'\x02', b'\x00', b'\x01'


def fits_slots(shapes):
    """a landmark graph's (name, ch, h, w) outputs fit the slots: exactly 21 keypoints, no more channels or rows
    than the 224 graph's, and a two-view graph's pool_pre the shape of its pool, all within one slot's bytes"""
    if len(shapes) not in (3, 4) or (len(shapes) == 4 and tuple(shapes[3][1:]) != tuple(shapes[2][1:])):
        return False
    return shapes[0][1] == OUT_SHAPES[0][1] and all(
        x[1] <= y[1] and x[2] <= y[2] and x[3] <= y[3] for x, y in zip(shapes, OUT_SHAPES)) and \
        sum(c * h * w for _, c, h, w in shapes) <= OUT_BYTES


def parse(b, served, paired=()):
    """a request's bytes -> (key, n, pairs), or None for a malformed one. served: the graph keys (0, and DET with a
    detector); paired: those whose graph is two-view. pairs: bit 0 for PAIR0, bit 1 for PAIR1."""
    if len(b) != 1 or b[0] & 0x80:
        return None
    key, n, pairs = b[0] & DET, b[0] & 0x0F, (b[0] >> 5) & 3
    if not 1 <= n <= 4 or key not in served:
        return None
    if pairs and (key not in paired or (pairs & 1 and n < 2) or (pairs & 2 and n < 4)):
        return None
    return key, n, pairs


def pair_mask(pairs):
    """the FUSE has2 mask over the crops for a request's pair bits: PAIR0 -> crops 0 and 1, PAIR1 -> 2 and 3"""
    return (3 if pairs & 1 else 0) | (12 if pairs & 2 else 0)


class Memory:
    """the sealed memfd and the daemon's mapping of it (kept for the daemon's life)"""

    def __init__(self, size=SHM_BYTES):
        self.fd = os.memfd_create('ft-npud', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        os.ftruncate(self.fd, size)
        fcntl.fcntl(self.fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)
        self.map = mmap.mmap(self.fd, size)


class Server:
    """the doorbell: one client at a time, the memory's fd for it, BUSY for any other. Signals: wait() is the only
    place the daemon blocks, and the caller marks itself idle around it."""

    def __init__(self, path, uid, memory):
        self.path, self.uid, self.memory, self.conn = path, uid, memory, None
        if path.exists() or path.is_symlink():
            path.unlink()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC)
        old = os.umask(0o177)       # born 0600: no window where another user can connect, and no chmod to follow a
        try:                        # symlink swapped in after bind
            self.srv.bind(str(path))
        finally:
            os.umask(old)
        os.chown(path, uid, -1, follow_symlinks=False)
        self.rejected = 0

    def listen(self):
        self.srv.listen(4)

    def _peer_ok(self, c):
        return struct.unpack('3i', c.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1] == self.uid

    def _send(self, c, b, fds=()):
        try:
            if fds:
                c.sendmsg([b], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack('%di' % len(fds), *fds))],
                           socket.MSG_DONTWAIT)
            else:
                c.send(b, socket.MSG_DONTWAIT)
            return True
        except OSError:
            return False

    def _accept(self):
        c, _ = self.srv.accept()
        if not self._peer_ok(c) or self.conn is not None:
            if self._peer_ok(c):      # another user's process learns nothing, not even that one is served
                self._send(c, BUSY)
            self.rejected += 1
            c.close()
            return False
        if not self._send(c, HELLO, (self.memory.fd,)):
            c.close()
            return False
        self.conn = c
        return True

    def wait(self):
        """blocks until something happens: ('joined', None) for a new client, ('request', bytes) for the client's
        next request, ('left', None) when it has gone. Connections while one is served are answered BUSY and
        closed without an event."""
        while True:
            watch = [self.srv] + ([self.conn] if self.conn is not None else [])
            ready, _, _ = select.select(watch, [], [])
            # the client first: one that just left must be gone before the next is accepted, or a client that
            # connects right after it would be turned away BUSY
            if self.conn is not None and self.conn in ready:
                try:
                    b = self.conn.recv(16)
                except OSError:
                    b = b''
                if b:
                    # a connection waiting meanwhile is turned away now: a client sending back to back keeps conn
                    # ready on every select, and the accept below would never run
                    if self.srv in ready:
                        self._accept()
                    return 'request', b
                self.drop()
                return 'left', None
            if self.srv in ready and self._accept():
                return 'joined', None

    def reply(self, b):
        """False (and the client dropped) when it can't take the reply now: it left, or it stopped reading. The
        caller then ends the client's session itself: wait() reports no 'left' for it"""
        if self.conn is not None and self._send(self.conn, b):
            return True
        self.drop()
        return False

    def drop(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None

    def close(self):
        self.drop()
        self.srv.close()
        if self.path.exists():
            self.path.unlink()


class Busy(ConnectionError):
    """the daemon serves another client"""


def connect(sock_path, timeout=2.0):
    """the client's side: (socket, mapping of the memory). Busy while another client is served; ConnectionError
    when the daemon closes without a HELLO (another UID) or doesn't answer in time"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC)
    try:
        s.settimeout(timeout)
        s.connect(str(sock_path))
        fds = array.array('i')
        msg, anc, _, _ = s.recvmsg(1, socket.CMSG_SPACE(fds.itemsize))
        for level, kind, data in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds.frombytes(data[:len(data) - len(data) % fds.itemsize])
        if msg == BUSY:
            raise Busy('ft-npud serves another client')
        if msg != HELLO or len(fds) != 1:
            raise ConnectionError('ft-npud closed the connection without the memory (status %r)' % msg)
        try:
            if os.fstat(fds[0]).st_size < SHM_BYTES:
                raise ConnectionError('ft-npud memory is smaller than this client expects')
            m = mmap.mmap(fds[0], SHM_BYTES)
        finally:
            os.close(fds[0])
        magic, version = struct.unpack_from('<2I', m, 0)
        if (magic, version) != (MAGIC, VERSION):
            m.close()
            raise ConnectionError('ft-npud protocol %08x/%d, this client speaks %d' % (magic, version, VERSION))
        s.settimeout(None)
        return s, m
    except BaseException:
        s.close()
        raise
