"""CDSP ABI 9 descriptor transport and atomic operation ownership."""

import array
import ctypes
import hashlib
import mmap
import os
import socket
import struct
import time
from .errors import DeviceBusy, UnsupportedFirmware, RecoveryRequired
from .profile import PROFILE


class Transport:
    def __init__(self, profile=PROFILE):
        self.profile = profile
        self.sock = None
        self.memory = None
        self.fds = []
        self.poisoned = False
        self.owned = False
        self.transient_waits = 0  # claims that waited out a transient non-idle master
        try:
            with open(profile.firmware_path, "rb") as stream:
                digest = hashlib.sha256(stream.read()).hexdigest()
            if digest != profile.firmware_sha256:
                raise UnsupportedFirmware(
                    "Valve CDSP firmware hash differs from verified ABI 9 image"
                )
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(3)
            self.sock.connect("\0dsp_service")
            greeting = bytearray()
            while len(greeting) < 4:
                part, controls, flags, _ = self.sock.recvmsg(
                    4 - len(greeting), socket.CMSG_SPACE(16 * 4)
                )
                for level, kind, raw in controls:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        received = array.array("i")
                        received.frombytes(raw[: len(raw) // received.itemsize * received.itemsize])
                        self.fds.extend(received)
                if flags & socket.MSG_CTRUNC:
                    raise UnsupportedFirmware("truncated DSP descriptor handshake")
                if not part:
                    raise ConnectionError("DSP service closed during handshake")
                greeting.extend(part)
            if (
                greeting != b"ADSP"
                or len(self.fds) != 2
                or os.fstat(self.fds[1]).st_size != profile.size
            ):
                raise UnsupportedFirmware("unexpected DSP descriptor handshake")
            for fd in self.fds:
                os.set_inheritable(fd, False)
            self.sock.sendall(struct.pack("<I", 4))
            self.memory = mmap.mmap(
                self.fds[1],
                profile.size,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
                offset=0x80000000,
            )
            self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.memory))
            self.atomic = ctypes.CDLL("libatomic.so.1")
            self._load = getattr(self.atomic, "__atomic_load_4")
            self._load.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self._load.restype = ctypes.c_uint32
            self._store = getattr(self.atomic, "__atomic_store_4")
            self._store.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
            self._cas = getattr(self.atomic, "__atomic_compare_exchange_4")
            self._cas.argtypes = [
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.c_bool,
                ctypes.c_int,
                ctypes.c_int,
            ]
            self._cas.restype = ctypes.c_bool
            if struct.unpack_from("<QQ", self.memory, 0xA90000) != (profile.magic, profile.abi):
                raise UnsupportedFirmware("shared CDSP identity/ABI mismatch")
        except BaseException:
            self.close()
            raise

    def get(self, offset):
        return self._load(self.base + offset, 5)

    def put(self, offset, value):
        self._store(self.base + offset, value, 5)

    def wait(self, predicate, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return True
            time.sleep(0.0001)
        return bool(predicate())

    def claim(self, timeout):
        if self.poisoned or self.owned:
            raise RecoveryRequired("device session needs recovery or already owns an operation")
        p = self.profile
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            expected = ctypes.c_uint32(1)
            if self.get(p.control) == 1 and self._cas(
                self.base + p.control + 8, ctypes.byref(expected), 4, False, 5, 5
            ):
                self.owned = True
                seen = (self.get(p.callback), self.get(p.control + 4), self.get(p.control))
                if seen[0] or seen[1]:
                    self.release_unsubmitted()
                    raise DeviceBusy(
                        "another callback or command is installed (callback %#x, command %d, state %d)" % seen
                    )
                if seen[2] != 1:
                    # Nothing installed, but the master left idle between the two reads (it does
                    # so briefly right after a completed job): give the gate back unchanged and
                    # wait for idle, as for a busy gate.
                    self.release_unsubmitted()
                    self.transient_waits += 1
                    time.sleep(0.0001)
                    continue
                return
            time.sleep(0.001)
        raise DeviceBusy("CDSP operation gate is busy")

    def release_unsubmitted(self):
        if self.owned and not self.poisoned:
            self.put(self.profile.control + 8, 1)
            self.owned = False

    def publish(self, *, direct=False):
        self.put(self.profile.callback, self.profile.dsp_base + self.profile.code + int(direct))
        self.put(self.profile.control + 4, 1)
        self.sock.sendall(b"client_notification\0")

    def wait_complete(self, seq, timeout):
        p = self.profile

        def complete():
            done = all(self.get(p.status + i * 64) == seq for i in range(3))
            if not done:
                return False
            state = self.get(p.control)
            if state not in (2, 3) or self.get(p.control + 8) != 4:
                self.poisoned = True
                raise RecoveryRequired("stock completion window expired after DSP completion")
            # Direct mode joins worker returns before the master publishes 3.
            # Completion while the master still owns state 2 is not an expiry.
            return state == 3

        if not self.wait(complete, timeout):
            self.poisoned = True
            raise RecoveryRequired(
                "DSP callback timed out; code, data and HMX power are retained. The stock gate may expire; reset the DSP before reuse."
            )

    def acknowledge(self, timeout):
        self.put(self.profile.control + 4, 0)
        if not self.wait(
            lambda: self.get(self.profile.control) == 1 and self.get(self.profile.control + 8) == 1,
            timeout,
        ):
            self.poisoned = True
            raise RecoveryRequired("CDSP did not acknowledge completion")
        self.owned = False

    def status(self):
        p = self.profile
        return {
            "abi": p.abi,
            "control": list(struct.unpack_from("<3I", self.memory, p.control)),
            "callback": self.get(p.callback),
            "poisoned": self.poisoned,
        }

    def close(self):
        # Closing host descriptors does not alter a potentially running DSP job.
        if self.memory is not None:
            self.memory.close()
            self.memory = None
        for fd in self.fds:
            os.close(fd)
        self.fds = []
        if self.sock is not None:
            self.sock.close()
            self.sock = None
