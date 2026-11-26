"""Offline lane and address model of the small-map depthwise row tiling.

Mirrors the data movement of tests/resident_dw_tile.h with symbolic values and
compares every consumed lane with direct convolution indexing.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Pinned depthwise geometries: (input size, output size, kernel, stride, top/left pad).
LAYERS = {
    "convdwclip_0": (112, 112, 3, 1, 1),
    "convdwclip_1": (112, 56, 3, 2, 0),
    "convdwclip_2": (56, 56, 3, 1, 1),
    "convdwclip_3": (56, 28, 5, 2, 1),
    "convdwclip_4": (28, 28, 5, 1, 2),
    "convdwclip_5": (28, 14, 3, 2, 0),
    "convdwclip_6": (14, 14, 3, 1, 1),
    "convdwclip_8": (14, 14, 5, 1, 2),
    "convdwclip_11": (14, 7, 5, 2, 1),
    "convdwclip_12": (7, 7, 5, 1, 2),
    "convdwclip_15": (7, 7, 3, 1, 1),
}
TILED = {n for n, g in LAYERS.items() if g[1] in (7, 14)}
ROWS, BROADCASTS = 0x000, 0xC00


def fits(size, out, kernel, stride, pad, in_place=False):
    """dwt_fits: the record fields the native operator relies on."""
    return out in (7, 14) and not in_place and size + pad <= 32 and stride in (1, 2)


def staged_rows(tensor, size, channel, y, slots, kernel, stride, pad):
    """dwt_pass staging: 64-lane rows, the input row written at its left pad."""
    rows = []
    iy = y * stride - pad
    for j in range((slots - 1) * stride + kernel):
        lanes = [0] * 64
        if 0 <= iy + j < size:
            start = (channel * size + iy + j) * size
            # One 32-lane load from the row start, masked to the row width.
            loaded = [tensor[start + i] if i < size else 0 for i in range(32)]
            lanes[pad : pad + 32] = loaded
        rows.append(lanes)
    return rows


def gather(rows, slots, lanes_per_slot, ky, kx, stride):
    """One tap for all slots: descending rows joined by rotate and mux."""
    result = [0] * 32
    for row in reversed(range(slots)):
        v = rows[row * stride + ky][kx : kx + 32]
        if stride == 2:
            v = v[0::2] + [0] * 16
        result = v[:lanes_per_slot] + result[: 32 - lanes_per_slot]
    return result


class RowTileModelTests(unittest.TestCase):
    def test_only_the_seven_and_fourteen_wide_layers_are_tiled(self):
        self.assertEqual({n for n, g in LAYERS.items() if fits(*g)}, TILED)
        self.assertFalse(fits(14, 14, 3, 1, 1, in_place=True))
        self.assertFalse(fits(31, 14, 3, 1, 2))

    def test_every_consumed_lane_is_the_ordinary_tap(self):
        channels = 3  # first, interior and last channel of the tensor
        for name in sorted(TILED):
            size, out, kernel, stride, pad = LAYERS[name]
            slots, lanes = (4, 8) if out == 7 else (2, 16)
            # Unique value per input element; reads past the tensor are tagged.
            tensor = [("in", i) for i in range(channels * size * size)]
            tensor += [("past", i) for i in range(64)]
            for channel in range(channels):
                # A request may resume at any row, so every row starts a pass.
                for y in range(out):
                    rows = staged_rows(tensor, size, channel, y, slots, kernel, stride, pad)
                    self.assertLessEqual(len(rows), 11)
                    for ky in range(kernel):
                        for kx in range(kernel):
                            # The tap load stays inside its staged row pair.
                            self.assertLessEqual(kx * 4 + 128, 256)
                            self.assertLess((slots - 1) * stride + ky, len(rows))
                            got = gather(rows, slots, lanes, ky, kx, stride)
                            for row in range(min(slots, out - y)):
                                # Unit `row` of the pass sees its slot rotated to lane 0.
                                unit = got[row * lanes :] + got[: row * lanes]
                                for x in range(out):
                                    iy = (y + row) * stride - pad + ky
                                    ix = x * stride - pad + kx
                                    want = (
                                        tensor[(channel * size + iy) * size + ix]
                                        if 0 <= iy < size and 0 <= ix < size
                                        else 0
                                    )
                                    self.assertEqual(
                                        unit[x], want, (name, channel, y, row, ky, kx, x)
                                    )

    def test_staged_rows_and_waiting_results_fit_saved_vtcm(self):
        for name in TILED:
            size, out, kernel, stride, pad = LAYERS[name]
            slots = 4 if out == 7 else 2
            rows = (slots - 1) * stride + kernel
            self.assertLessEqual(ROWS + rows * 256, BROADCASTS, name)
            # The padded row is one unaligned 128-byte store inside its pair.
            self.assertLessEqual(pad * 4 + 128, 256)
            self.assertLessEqual(size + pad, 32)
        # Broadcasts end where the waiting sum and flags begin; both end below
        # the unaligned-load pair at 0x1a00.
        self.assertEqual(BROADCASTS + 25 * 128, 0x1880)
        self.assertLessEqual(0x1900 + 128, 0x1A00)

    def test_a_pass_never_spans_two_channels(self):
        for out, slots in ((7, 4), (14, 2)):
            ahead, passes = 0, []
            for unit in range(3 * out):
                y = unit % out
                if not ahead:
                    passes.append(y)
                    ahead = slots
                ahead -= 1
                # Native: rows computed ahead are dropped when the next row is 0.
                if (unit + 1) % out == 0:
                    ahead = 0
            self.assertEqual(passes, [y for _ in range(3) for y in range(0, out, slots)])


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("clang", "ld.lld", "llvm-objcopy", "llvm-objdump")),
    "LLVM tools are not installed",
)
class RowTileBuildTests(unittest.TestCase):
    def test_tiled_build_uses_the_wide_layout_and_spills_no_vector(self):
        with tempfile.TemporaryDirectory() as folder:
            for flags in (["--dw-reuse", "6", "--dw-row-tile"], ["--dw-reuse", "6"]):
                out = Path(folder) / ("tile" if len(flags) == 3 else "plain")
                subprocess.run(
                    [sys.executable, str(ROOT / "tools/build_resident_landmark.py"), "--linked"]
                    + flags
                    + ["--output", str(out)],
                    check=True,
                    capture_output=True,
                    cwd=ROOT,
                )
                subprocess.run(
                    [
                        sys.executable,
                        str(ROOT / "tools/check_resident_link.py"),
                        "--jobs",
                        str(out),
                        "--output",
                        str(out / "verify.json"),
                    ],
                    check=True,
                    capture_output=True,
                    cwd=ROOT,
                )
            sys.path.insert(0, str(ROOT / "src"))
            from framenpu.resident_landmark import CODE_LAYOUTS, load_linked

            tiled, plain = load_linked(Path(folder) / "tile"), load_linked(Path(folder) / "plain")
            self.assertEqual(CODE_LAYOUTS[tiled.loader.name], 16384)
            self.assertEqual(CODE_LAYOUTS[plain.loader.name], 12288)
            self.assertLessEqual(len(plain.image), 12288)
            self.assertLess(12288, len(tiled.image))
            self.assertLessEqual(len(tiled.image), 16384)
            text = (Path(folder) / "tile/linked/disassembly.txt").read_text()
            for function in ("rg_op_dw", "rg_op_dwt", "dwt_pass", "dwt_row"):
                body = text.split(f"<{function}>:", 1)[1].split(">:\n", 1)[0]
                self.assertNotRegex(body, r"vmemu?\((r29|r30)", function)


if __name__ == "__main__":
    unittest.main()
