"""Cache entry opt-in and independent compiled-entry verifier regressions."""

from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_resident_cache import verify_entry, verify_relocations


def entry_fixture():
    symbols = {"vectors": 0xF0A92300, "frame_job": 0xF0A92C00}
    lines = []
    for index in range(64):
        name = "fault" if index in (2, 4, 6, 8) else f"forward{index}"
        lines.append(f"{symbols['vectors'] + index * 4:x}: jump target <{name}>")
        if name != "fault":
            symbols[name] = 0xF0A92400 + index * 4
            lines.append(
                f"{symbols[name]:x}: r0 = #0x{index:x} ; jump target <resident_forward_common>"
            )
    lines += [
        "<resident_forward_common>:",
        "r0 = asl(r0,#0x2)",
        "immext(#0x87000000)",
        "r0 = add(r0,##-0x79000000)",
        "jumpr r0",
        "<fault>:",
        "<resident_dispatch>:",
        "r1 = ##-0x7856d400",
        "<frame_job>:",
    ]
    return "\n".join(lines), symbols


class ResidentCacheTests(unittest.TestCase):
    def test_compiler_code_table_and_function_addresses_require_review(self):
        verify_relocations("R_HEX_B22_PCREL fnpu_hmx_load 0x0")
        verify_relocations("")
        for row in ("R_HEX_32 .text 0x70", "R_HEX_32_6_X fnpu_hmx_load 0x0"):
            with self.subTest(row=row), self.assertRaises(ValueError):
                verify_relocations(row)

    def test_forward_table_and_cached_function_alias_match_firmware_contract(self):
        text, symbols = entry_fixture()
        self.assertEqual(verify_entry(text, symbols), 0x87A92C00)

    def test_misdirected_vector_register_clobber_and_wrong_alias_are_rejected(self):
        text, symbols = entry_fixture()
        for changed in (
            text.replace("<forward63>", "<forward62>"),
            text.replace("r0 = asl(r0,#0x2)", "r1 = asl(r0,#0x2)"),
            text.replace("r1 = ##-0x7856d400", "r1 = ##-0xf56d400"),
        ):
            with self.assertRaises(ValueError):
                verify_entry(changed, symbols)

    def test_cache_hardware_controls_require_opt_in_before_loading_jobs(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests/hardware_resident_cache.py"),
                "--probe-a",
                "unused",
                "--probe-b",
                "unused",
                "--output",
                "unused",
            ],
            cwd="/tmp",
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device and Frame root are required", result.stderr)


if __name__ == "__main__":
    unittest.main()
