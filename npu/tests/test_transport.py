"""npu/app/npud/transport.py, offline: the daemon's memory is sealed (it can't shrink or grow under the root
daemon's mapping) also with a Python whose fcntl has no seal names, as the tree's own CPython has none
(python-build-standalone), and the numbers it falls back to are Linux's."""
import fcntl
import importlib
import os
import pathlib
import sys
import types
import unittest

NPUD = pathlib.Path(__file__).resolve().parents[1] / "app/npud"
SEALS = ("F_ADD_SEALS", "F_GET_SEALS", "F_SEAL_SEAL", "F_SEAL_SHRINK", "F_SEAL_GROW", "F_SEAL_WRITE",
         "F_SEAL_FUTURE_WRITE")
F_GET_SEALS = 1034   # Linux's (include/uapi/linux/fcntl.h)


def load_transport(fcntl_module):
    """transport.py imported afresh with fcntl_module as its fcntl"""
    saved = sys.modules.get("fcntl")
    sys.modules["fcntl"] = fcntl_module
    sys.path.insert(0, str(NPUD))
    try:
        sys.modules.pop("transport", None)
        return importlib.import_module("transport")
    finally:
        sys.path.remove(str(NPUD))
        sys.modules.pop("transport", None)
        sys.modules["fcntl"] = saved


def fcntl_without_seals():
    m = types.ModuleType("fcntl")
    m.__dict__.update({k: v for k, v in vars(fcntl).items() if k not in SEALS and not k.startswith("__")})
    return m


class Memory(unittest.TestCase):
    def check_sealed(self, transport):
        memory = transport.Memory()
        try:
            self.assertEqual(fcntl.fcntl(memory.fd, F_GET_SEALS), 0x1 | 0x2 | 0x4)   # SEAL, SHRINK, GROW
            with self.assertRaises(PermissionError):
                os.ftruncate(memory.fd, transport.SHM_BYTES // 2)
            with self.assertRaises(PermissionError):
                os.ftruncate(memory.fd, transport.SHM_BYTES * 2)
        finally:
            memory.map.close()
            os.close(memory.fd)

    def test_sealed_with_fcntls_names(self):
        self.check_sealed(load_transport(fcntl))

    def test_sealed_with_no_seal_names_in_fcntl(self):
        bare = fcntl_without_seals()
        self.assertFalse(hasattr(bare, "F_ADD_SEALS"))
        self.check_sealed(load_transport(bare))

    def test_the_numbers_it_falls_back_to_are_linuxs(self):
        bare = load_transport(fcntl_without_seals())
        for name in ("F_ADD_SEALS", "F_SEAL_SEAL", "F_SEAL_SHRINK", "F_SEAL_GROW"):
            if hasattr(fcntl, name):   # a Python built with them (the host's, the image's)
                self.assertEqual(getattr(bare, name), getattr(fcntl, name), name)


if __name__ == "__main__":
    unittest.main()
