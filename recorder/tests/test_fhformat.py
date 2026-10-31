#!/usr/bin/python3
"""recorder/fhformat.py against the C headers it mirrors (hands/camd/fhring.h,
hands/track/record.h): a small C++ program built here prints every struct's size and each
field's offset, and the Python layouts must agree. Skipped without a C++ compiler."""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)
from recorder import fhformat  # noqa: E402

# (C type, Python struct, its fields in order: the padding at the end isn't one)
LAYOUTS = {
    "fh_ring_cam_t": (fhformat.RING_CAM, ["sensor", "name", "node", "format", "width", "height", "stride",
                                          "nslots", "slot_offset", "slot_bytes", "latest", "published",
                                          "dropped", "flags", "dark_mean"]),
    "fh_ring_slot_t": (fhformat.RING_SLOT, ["seq", "frame", "capture_ns", "dqbuf_ns", "publish_ns",
                                            "v4l2_seq", "mean"]),
    "fh_ring_hdr_t": (fhformat.RING_HDR, ["magic", "version", "header_bytes", "ncams", "reserved0",
                                          "file_bytes", "writer_pid", "heartbeat_ns"]),
    "fh_set_hdr_t": (fhformat.SET_HDR, ["magic", "ncams", "bytes"]),
    "fh_set_cam_t": (fhformat.SET_CAM, ["name", "width", "height", "capture_ns", "dqbuf_ns"]),
}


def offsets(st):
    """Each item's offset in a little-endian struct format ("<8sII16x": 0, 8, 12)."""
    out, fmt = [], st.format.lstrip("<")
    for m in re.finditer(r"(\d*)([a-zA-Z?])", fmt):
        count, code = m.groups()
        if code == "x":
            continue
        start = struct.calcsize("<" + fmt[:m.start()])
        n = 1 if code == "s" or not count else int(count)
        for i in range(n):
            out.append(start + i * struct.calcsize("<" + code))
    return out


def c_program():
    lines = ['#include <cstdio>', '#include <cstddef>', '#include "camd/fhring.h"', '#include "track/record.h"',
             'int main() {']
    for ctype, (_, fields) in LAYOUTS.items():
        lines.append(f'    std::printf("{ctype} size %zu\\n", sizeof({ctype}));')
        for f in fields:
            lines.append(f'    std::printf("{ctype} {f} %zu\\n", offsetof({ctype}, {f}));')
    lines.append('    std::printf("FH_RING_MAGIC %s\\nFH_SET_MAGIC %s\\n", FH_RING_MAGIC, FH_SET_MAGIC);')
    lines.append('    std::printf("FH_RING_VERSION %d\\nFH_RING_MAX_CAMS %d\\n", FH_RING_VERSION, FH_RING_MAX_CAMS);')
    lines.append('    std::printf("FH_CAM_DARK %u\\nFH_CAM_COLOR %u\\nFH_FMT_GREY8 %d\\n", '
                 '(unsigned)FH_CAM_DARK, (unsigned)FH_CAM_COLOR, (int)FH_FMT_GREY8);')
    lines.append('    std::printf("FH_RING_NAME %s\\n", FH_RING_NAME);')
    lines.append('    return 0;\n}')
    return "\n".join(lines) + "\n"


CXX = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")


@unittest.skipUnless(CXX, "no C++ compiler")
class HeadersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.mkdtemp(prefix="fhformat-")
        try:
            src, exe = os.path.join(tmp, "layout.cpp"), os.path.join(tmp, "layout")
            with open(src, "w") as f:
                f.write(c_program())
            subprocess.run([CXX, "-std=c++17", "-I", os.path.join(REPO, "hands"), "-o", exe, src],
                           check=True, capture_output=True, text=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        cls.c = {}
        for line in out.splitlines():
            key, value = line.rsplit(" ", 1)
            cls.c[key] = value

    def test_sizes_and_offsets(self):
        for ctype, (st, fields) in LAYOUTS.items():
            with self.subTest(ctype):
                size = int(self.c[f"{ctype} size"])
                if ctype == "fh_ring_hdr_t":   # the cams follow its 64 bytes
                    self.assertEqual(size, st.size + fhformat.RING_MAX_CAMS * fhformat.RING_CAM.size)
                else:
                    self.assertEqual(size, st.size)
                self.assertEqual(offsets(st), [int(self.c[f"{ctype} {f}"]) for f in fields])

    def test_named_offsets(self):
        cam = "fh_ring_cam_t"
        self.assertEqual(fhformat.RING_HEARTBEAT_OFF, int(self.c["fh_ring_hdr_t heartbeat_ns"]))
        self.assertEqual(fhformat.RING_CAM_LATEST_OFF, int(self.c[f"{cam} latest"]))
        self.assertEqual(fhformat.RING_CAM_PUBLISHED_OFF, int(self.c[f"{cam} published"]))
        self.assertEqual(fhformat.RING_CAM_DROPPED_OFF, int(self.c[f"{cam} dropped"]))
        self.assertEqual(fhformat.RING_CAM_FLAGS_OFF, int(self.c[f"{cam} flags"]))
        self.assertEqual(fhformat.RING_CAM_DARK_MEAN_OFF, int(self.c[f"{cam} dark_mean"]))

    def test_constants(self):
        self.assertEqual(fhformat.RING_MAGIC.decode(), self.c["FH_RING_MAGIC"])
        self.assertEqual(fhformat.SET_MAGIC.decode(), self.c["FH_SET_MAGIC"])
        self.assertEqual(fhformat.RING_VERSION, int(self.c["FH_RING_VERSION"]))
        self.assertEqual(fhformat.RING_MAX_CAMS, int(self.c["FH_RING_MAX_CAMS"]))
        self.assertEqual(fhformat.CAM_DARK, int(self.c["FH_CAM_DARK"]))
        self.assertEqual(fhformat.CAM_COLOR, int(self.c["FH_CAM_COLOR"]))
        self.assertEqual(fhformat.FMT_GREY8, int(self.c["FH_FMT_GREY8"]))
        self.assertEqual(fhformat.RING_NAME, self.c["FH_RING_NAME"])


class PythonTest(unittest.TestCase):
    def test_offsets_helper(self):
        self.assertEqual(offsets(struct.Struct("<8sII16x")), [0, 8, 12])
        self.assertEqual(offsets(struct.Struct("<QIf")), [0, 8, 12])

    def test_ring_cam_offset(self):
        self.assertEqual(fhformat.ring_cam_offset(0), 64)
        self.assertEqual(fhformat.ring_cam_offset(2), 64 + 320)


if __name__ == "__main__":
    unittest.main()
