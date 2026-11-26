"""ctypes bindings for the FrameNPU host kernels (host/fnpu_host.c).

The bundled library (data/host_aarch64.so) is loaded only on aarch64, and only after
its size and SHA-256 match data/host_aarch64.json; anything else returns None and
callers use the NumPy path. Tests load a local build of the same source with
``Host(path, record)``.

Every wrapper checks dtype, contiguity and size before passing a pointer: the C
side trusts its arguments.
"""

import ctypes
import hashlib
import json
import platform
from pathlib import Path

DATA = Path(__file__).with_name("data")
ABI = 4

_i32, _f32, _p = ctypes.c_int32, ctypes.c_float, ctypes.c_void_p
_SIGNATURES = {
    "fnpu_abi": (_i32, ()),
    "fnpu_read32": (None, (_p, _p, _i32, _p)),
    "fnpu_nonfinite32": (_i32, (_p, _i32)),
    "fnpu_to_f16": (_i32, (_p, _p, _i32)),
    "fnpu_from_f16": (None, (_p, _p, _i32)),
    "fnpu_stem_columns": (None, (_p, _p)),
    "fnpu_grid_pack": (None, (_p, _i32, _i32, _p, _i32, _p)),
    "fnpu_grid_unpack": (None, (_p, _i32, _i32, _p, _i32, _p, _p)),
    "fnpu_dense": (None, (_p, _p, _p, _i32, _i32, _i32, _i32, _p, _p)),
    "fnpu_conv1x1": (None, (_p, _p, _p, _i32, _i32, _i32, _i32, _p, _p)),
    "fnpu_avgpool": (None, (_p, _i32, _i32, _i32, _i32, _i32, _i32, _i32, _p)),
    "fnpu_hint": (_i32, (_p, _p, _p, _p, _i32, _i32, _i32, _i32, _p, _p, _p)),
    "fnpu_conv1x1_f16": (None, (_p, _p, _p, _i32, _i32, _i32, _i32, _p, _p)),
}


def _arr(a, dtype, n=None, writable=False):
    """Pointer to a C-contiguous array of exactly ``dtype`` (and ``n`` elements)."""
    import numpy as np

    if not isinstance(a, np.ndarray) or a.dtype != np.dtype(dtype) or not a.flags.c_contiguous:
        raise TypeError(f"native kernels need a C-contiguous {np.dtype(dtype)} array")
    if n is not None and a.size != n:
        raise ValueError(f"native kernel expected {n} elements, got {a.size}")
    if writable and not a.flags.writeable:
        raise ValueError("native kernel output must be writable")
    return a.ctypes.data


class Host:
    """The host kernels of one verified library file."""

    def __init__(self, path, record):
        path = Path(path)
        data = path.read_bytes()
        if (
            record.get("format") != "framenpu-host-native"
            or record.get("abi") != ABI
            or record.get("bytes") != len(data)
            or record.get("sha256") != hashlib.sha256(data).hexdigest()
        ):
            raise ValueError(f"{path.name} does not match its manifest")
        self.path, self.sha256 = path, record["sha256"]
        self.lib = ctypes.CDLL(str(path))
        for name, (res, args) in _SIGNATURES.items():
            f = getattr(self.lib, name)
            f.restype, f.argtypes = res, args
        if self.lib.fnpu_abi() != ABI:
            raise ValueError("host library ABI differs")

    # -------------------------------------------------------------- elementwise
    def nonfinite(self, x):
        return self.lib.fnpu_nonfinite32(_arr(x, "f4"), x.size)

    def to_f16(self, x, out):
        """out (uint16) = FP16 bits of x (float32); returns the non-finite count."""
        return self.lib.fnpu_to_f16(_arr(x, "f4"), _arr(out, "u2", x.size, True), x.size)

    def from_f16(self, x, out):
        self.lib.fnpu_from_f16(_arr(x, "u2"), _arr(out, "f4", x.size, True), x.size)

    # -------------------------------------------------------------- layout
    def stem_columns(self, img16, out):
        self.lib.fnpu_stem_columns(_arr(img16, "u2", 128 * 128), _arr(out, "u2", 10 * 4096, True))

    def grid_pack(self, t16, channels, idx, nwords, out):
        hw = idx.size
        if channels % 2 or t16.size != channels * hw:
            raise ValueError("grid pack needs an even channel count and full planes")
        if idx.size and (int(idx.min()) < 0 or int(idx.max()) >= nwords):
            raise ValueError("grid index outside the tensor")
        self.lib.fnpu_grid_pack(
            _arr(t16, "u2"), channels, hw, _arr(idx, "i4"), nwords,
            _arr(out, "u2", channels * nwords, True),
        )

    def grid_unpack(self, raw16, channels, idx, nwords, out, stats):
        """out (float32 channels x hw); returns (stray nonzero halves, non-finite values)."""
        hw = idx.size
        if channels % 2:
            raise ValueError("grid unpack needs an even channel count")
        if idx.size and (int(idx.min()) < 0 or int(idx.max()) >= nwords):
            raise ValueError("grid index outside the tensor")
        self.lib.fnpu_grid_unpack(
            _arr(raw16, "u2", channels * nwords), channels, hw, _arr(idx, "i4"), nwords,
            _arr(out, "f4", channels * hw, True), _arr(stats, "i4", 2, True),
        )
        return int(stats[0]), int(stats[1])

    # -------------------------------------------------------------- arithmetic
    @staticmethod
    def _batch(x, n_in):
        nb = x.size // n_in if n_in else 0
        if not 1 <= nb <= 3 or x.size != nb * n_in:
            raise ValueError("native layers take one to three inputs of n_in values")
        return nb

    def dense(self, wt32, b, x, relu6, acc, out):
        """wt32: the FP32 weight transposed (n_in x n_out); x: nb x n_in (1..3 inputs);
        acc: nb x n_out FP64 scratch; out: nb x n_out."""
        n_in, n_out = wt32.shape
        nb = self._batch(x, n_in)
        self.lib.fnpu_dense(
            _arr(wt32, "f4"), _arr(b, "f4", n_out), _arr(x, "f4"), nb, n_out, n_in,
            int(bool(relu6)), _arr(acc, "f8", nb * n_out, True),
            _arr(out, "f4", nb * n_out, True),
        )

    def conv1x1(self, wt32, b, x, relu6, acc, out):
        """Same shapes as dense, for the FP32 1x1 convolution of a 1x1 map."""
        n_in, n_out = wt32.shape
        nb = self._batch(x, n_in)
        self.lib.fnpu_conv1x1(
            _arr(wt32, "f4"), _arr(b, "f4", n_out), _arr(x, "f4"), nb, n_out, n_in,
            int(bool(relu6)), _arr(acc, "f8", nb * n_out, True),
            _arr(out, "f4", nb * n_out, True),
        )

    def avgpool(self, x, kh, kw, oh, ow, out):
        c, h, w = x.shape
        if oh * kh > h or ow * kw > w:
            raise ValueError("pool window outside the map")
        self.lib.fnpu_avgpool(
            _arr(x, "f4"), c, h, w, kh, kw, oh, ow, _arr(out, "f4", c * oh * ow, True)
        )

    def hint(self, wt32, b, kp, flags, relu6, acc, tmp, out):
        """FP16 bits of FP32(relu6 dense(kp[b])) * flags[b] for nb = 1..3 crops (kp: nb x
        n_in); returns the non-finite count."""
        n_in, n_out = wt32.shape
        nb = self._batch(kp, n_in)
        return self.lib.fnpu_hint(
            _arr(wt32, "f4"), _arr(b, "f4", n_out), _arr(kp, "f4"), _arr(flags, "f4", nb), nb,
            n_out, n_in, int(bool(relu6)), _arr(acc, "f8", nb * n_out, True),
            _arr(tmp, "f4", nb * n_out, True), _arr(out, "u2", nb * n_out, True),
        )


_BUNDLED = {}


def bundled():
    """The verified bundled library on aarch64, else None (callers use NumPy)."""
    if "host" not in _BUNDLED:
        host = None
        if platform.machine() == "aarch64":
            lib, rec = DATA / "host_aarch64.so", DATA / "host_aarch64.json"
            if lib.is_file() and rec.is_file():
                host = Host(lib, json.loads(rec.read_text()))
        _BUNDLED["host"] = host
    return _BUNDLED["host"]
