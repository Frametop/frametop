"""SM8650 HMX power sequencing recovered from the installed Qualcomm image."""

import hashlib
import mmap
import os
import struct
import time
from .errors import DeviceBusy, UnsupportedFirmware, RecoveryRequired
from .profile import PROFILE


# The held configuration verify() re-reads before every request: (page, byte offsets), in
# read order. Pages: PLL, clock controller, RSC power sequencer (its status words, then the
# uploaded 76-word program).
_PAGES = {"p": 0x32341000, "c": 0x32348000, "s": 0x323C0000}
_HELD = (
    ("p", (0, 4, 0x10, 0x14, 0x18)),
    ("c", (0x180, 0x184, 0x1A0, 0x200)),
    ("s", (0x404, 0x410, 0x460, 0x464, 0x4BC, 0x4C0)),
    ("s", tuple(range(0x550, 0x680, 4))),
)


class _NativeVerify:
    """verify()'s register reads through the host library's fnpu_read32: the same
    addresses, one 32-bit load each, in the same order, on read-only mappings (a write
    through them faults in this process instead of reaching the hardware). Built only if
    its first read equals the Python read of the held configuration."""

    def __init__(self, host, pages, held):
        import numpy as np

        self.lib = host.lib
        self._views = {}
        for key, buf in pages.items():
            view = np.frombuffer(buf, np.uint8)
            if view.size != 4096:
                raise ValueError("register page views must be 4 KiB")
            self._views[key] = view
        n = sum(len(offs) for _, offs in _HELD)
        self.out = np.zeros(n, np.uint32)
        self._words, self.calls, at = [], [], 0
        for key, offs in _HELD:
            if any(o % 4 or not 0 <= o < 4096 for o in offs):
                raise ValueError("register offset outside its page")
            words = np.array([o // 4 for o in offs], np.int32)
            self._words.append(words)
            self.calls.append(
                (self._views[key].ctypes.data, words.ctypes.data, len(offs),
                 self.out[at:].ctypes.data)
            )
            at += len(offs)
        self.expected = np.array([v for part in held for v in part], np.uint32).tobytes()
        if self.read() != self.expected:
            raise ValueError("native register reads differ from the Python reads")

    def read(self):
        for args in self.calls:
            self.lib.fnpu_read32(*args)
        return self.out.tobytes()

    def close(self):
        # drop the array views first: they hold buffer exports on the mappings
        self.calls, self._views, self._words = [], {}, []


class HmxPower:
    def __init__(self, profile=PROFILE, mmio_factory=None, *, native=None, ro_factory=None):
        """native: a _native.Host for the read-only verify path (default: the bundled
        library when this maps /dev/mem itself; False: always Python). ro_factory(address)
        gives read-only page buffers instead of /dev/mem (tests)."""
        self.profile = profile
        self._factory = mmio_factory
        self._native, self._ro_factory = native, ro_factory
        self._fast = None
        self._ro_maps, self._ro_fd = [], None
        self.fast_verify_error = None
        self.maps = []
        self.fd = None
        self.changed = False
        self.triggered = False
        self.abandoned = False

    def _program(self):
        with open(self.profile.qcom_path, "rb") as stream:
            image = stream.read()
        if hashlib.sha256(image).hexdigest() != self.profile.qcom_sha256:
            raise UnsupportedFirmware("Qualcomm firmware hash differs from the verified profile")
        po = struct.unpack_from("<I", image, 28)[0]
        pn = struct.unpack_from("<H", image, 44)[0]
        for i in range(pn):
            typ, off, va, _, size, _, _, _ = struct.unpack_from("<8I", image, po + i * 32)
            start = 0xF02411F0
            if typ == 1 and va <= start and start + 304 <= va + size:
                code = image[off + start - va : off + start - va + 304]
                if (
                    hashlib.sha256(code).hexdigest()
                    == "b32772aa3effca0ebc32a41dd3b28b47df9fcc2a01dfd9818294d35e2d36187c"
                ):
                    return struct.unpack("<76I", code)
        raise UnsupportedFirmware("verified HMX sequencer program not found")

    @staticmethod
    def _r(m, off):
        return struct.unpack_from("<I", m, off)[0]

    @staticmethod
    def _w(m, off, value):
        struct.pack_into("<I", m, off, value)
        return struct.unpack_from("<I", m, off)[0]

    def _poll(self, m, off, mask, value, timeout=0.05):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self._r(m, off) & mask == value:
                return True
        return False

    def _sequence(self, start):
        s = self.s
        if self._r(s, 0x404) & 1:
            raise RecoveryRequired("HMX sequencer is busy")
        self._w(s, 0x410, 0x80000000 | (self._r(s, 0x410) & 1))
        if self._w(s, 0x464, start) != start:
            raise RecoveryRequired("HMX sequencer start address did not stick")
        self._w(s, 0x460, 0)
        self._w(s, 0x460, 1)
        if not self._poll(s, 0x460, 1, 1) or not self._poll(s, 0x410, 1, 1):
            raise RecoveryRequired("HMX power sequence did not complete")
        self._w(s, 0x460, 0)
        self._w(s, 0x410, 0x80000000 | (self._r(s, 0x410) & 1))

    def start(self):
        """Caller must hold the CDSP operation gate before calling this."""
        program = self._program()
        try:
            if self._factory:
                self.maps = [self._factory(a) for a in (0x32341000, 0x32348000, 0x323C0000)]
            else:
                self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
                for address in (0x32341000, 0x32348000, 0x323C0000):
                    self.maps.append(
                        mmap.mmap(
                            self.fd,
                            4096,
                            flags=mmap.MAP_SHARED,
                            prot=mmap.PROT_READ | mmap.PROT_WRITE,
                            offset=address,
                        )
                    )
            self.p, self.c, self.s = self.maps
            r = self._r
            self.bp = {o: r(self.p, o) for o in (0, 4, 0x10, 0x14, 0x18)}
            self.bc = {o: r(self.c, o) for o in (0x180, 0x184, 0x1A0, 0x200)}
            self.bs = {o: r(self.s, o) for o in (0x464, 0x4BC, 0x4C0, *range(0x550, 0x680, 4))}
            if r(self.s, 0) != 0x30200 or r(self.s, 8) != 0x1600214:
                raise UnsupportedFirmware("RSC identity differs from the verified SM8650")
            if any(r(self.s, o) for o in (0x404, 0x410, 0x460)) or any(self.bs.values()):
                raise DeviceBusy("HMX RSC is in use or has an existing program")
            if self.bp != {0: 0x100, 4: 0, 0x10: 0, 0x14: 0, 0x18: 1}:
                raise DeviceBusy("HMX PLL is not in the verified inactive state")
            if self.bc != {0x180: 0x80000000, 0x184: 0, 0x1A0: 0x80000000, 0x200: 0x80000000}:
                raise DeviceBusy("HMX clocks are already in use")
            self.changed = True
            for i, value in enumerate(program):
                if self._w(self.s, 0x550 + i * 4, value) != value:
                    raise RuntimeError("HMX program upload failed")
            self._w(self.s, 0x4BC, 4)
            self._w(self.s, 0x4C0, 40)
            self._w(self.p, 0x10, 20)
            self._w(self.p, 0x14, 0)
            self._w(self.p, 4, 1)
            self._w(self.p, 0, self.bp[0] | 4)
            if not self._poll(self.p, 0, 1 << 31, 1 << 31):
                raise RuntimeError("HMX PLL did not lock")
            self._w(self.p, 0, self._r(self.p, 0) | 1)
            self._w(self.c, 0x184, 0x203)
            self._w(self.c, 0x180, self._r(self.c, 0x180) | 1)
            if not self._poll(self.c, 0x180, 1, 0):
                raise RuntimeError("HMX clock root update did not complete")
            self._w(self.c, 0x1A0, self.bc[0x1A0] | 1)
            self.triggered = True
            self._sequence(2)
            if not self._poll(self.c, 0x1A0, 1 << 31, 0):
                raise RecoveryRequired("HMX core clock is halted after power-on")
            return self
        except BaseException as exc:
            if isinstance(exc, RecoveryRequired):
                self.abandon()
            else:
                self.close()
            raise

    def _held(self):
        r, pages = self._r, {"p": self.p, "c": self.c, "s": self.s}
        return tuple(tuple(r(pages[key], o) for o in offs) for key, offs in _HELD)

    def _open_native_verify(self):
        """Read-only mappings plus the native reader, or None (Python verify)."""
        host = self._native
        if host is False:
            return None
        if host is None:
            if self._factory is not None:
                return None  # simulated registers: keep the Python path
            from . import _native

            host = _native.bundled()
            if host is None:
                return None
        try:
            if self._ro_factory is not None:
                pages = {k: self._ro_factory(a) for k, a in _PAGES.items()}
            else:
                self._ro_fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
                pages = {}
                for key, address in _PAGES.items():
                    mm = mmap.mmap(
                        self._ro_fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
                        offset=address,
                    )
                    self._ro_maps.append(mm)
                    pages[key] = mm
            return _NativeVerify(host, pages, self.held)
        except (OSError, ValueError) as exc:
            self.fast_verify_error = f"{type(exc).__name__}: {exc}"
            self._close_native_verify()
            return None

    def _close_native_verify(self):
        if self._fast is not None:
            self._fast.close()
            self._fast = None
        for mm in self._ro_maps:
            mm.close()
        self._ro_maps = []
        if self._ro_fd is not None:
            os.close(self._ro_fd)
            self._ro_fd = None

    def hold(self):
        """Record the powered configuration this owner established.

        A session that keeps HMX powered between requests calls this once
        after start(), still under the operation gate, and verify() under the
        gate before every later request.
        """
        if not self.triggered or self.abandoned:
            raise RecoveryRequired("HMX power is not held by this owner")
        self.held = self._held()
        self._fast = self._open_native_verify()

    def verify(self):
        """Caller must hold the CDSP operation gate. Read-only."""
        if not self.maps or self.abandoned or not self.triggered:
            raise RecoveryRequired("HMX power is not held by this owner")
        if self._fast is not None:
            if self._fast.read() != self._fast.expected:
                raise RecoveryRequired("held HMX power configuration changed")
        elif self._held() != self.held:
            raise RecoveryRequired("held HMX power configuration changed")

    def abandon(self):
        """Unmap host views only. Never gate clocks under a possibly running DSP."""
        self.abandoned = True
        self._unmap()

    def close(self):
        try:
            if self.abandoned or not self.changed:
                return
            if self.triggered:
                self._sequence(0)
                self.triggered = False
            self._w(self.c, 0x1A0, self.bc[0x1A0])
            self._w(self.c, 0x184, self.bc[0x184])
            self._w(self.c, 0x180, self.bc[0x180] | 1)
            if not self._poll(self.c, 0x180, 1, 0):
                raise RecoveryRequired("HMX clock restore update timed out")
            self._w(self.c, 0x180, self.bc[0x180])
            self._w(self.p, 0, self._r(self.p, 0) & ~1)
            time.sleep(0.001)
            self._w(self.p, 4, self.bp[4])
            self._w(self.p, 0, self.bp[0])
            for off in (0x18, 0x14, 0x10):
                self._w(self.p, off, self.bp[off])
            for off, value in self.bs.items():
                self._w(self.s, off, value)
            for mm, original in ((self.p, self.bp), (self.c, self.bc), (self.s, self.bs)):
                if any(self._r(mm, off) != value for off, value in original.items()):
                    raise RecoveryRequired("HMX register restoration did not verify")
            self.changed = False
        finally:
            self._unmap()

    def _unmap(self):
        self._close_native_verify()
        for mm in self.maps:
            mm.close()
        self.maps = []
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
