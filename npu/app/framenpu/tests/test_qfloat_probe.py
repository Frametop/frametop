"""Independent dyadic arithmetic oracle and hardware opt-in checks."""

from pathlib import Path
import subprocess
import sys
import unittest

from hardware_qfloat_probe import check_values, expected, operands


class QFloatProbeTests(unittest.TestCase):
    def test_closed_form_oracle_matches_repeated_exact_dyadic_operations(self):
        for seed in range(3):
            a, b = operands(seed)
            for kind in range(5):
                for n in (1, 2, 63, 64):
                    actual = list(a)
                    for i in range(n):
                        for lane, (x, y) in enumerate(zip(a, b)):
                            if kind == 1:
                                actual[lane] *= y
                            elif kind in (2, 3):
                                actual[lane] += y
                            elif kind == 4:
                                actual[lane] += (-x if i % 2 else x) * y
                    self.assertEqual(actual, expected(kind, n, a, b))

    def test_observed_multiply_identity_rounding_is_within_fixture_bound(self):
        a, b = operands(1)
        nominal = expected(1, 63, a, b)
        observed = [x * (1 + 2**-23) for x in nominal]
        check_values(1, 63, a, b, observed)
        corrupted = list(observed)
        corrupted[0] += 0.1
        with self.assertRaises(ValueError):
            check_values(1, 63, a, b, corrupted)
        observed[0] = float("nan")
        with self.assertRaises(ValueError):
            check_values(1, 63, a, b, observed)

    def test_probe_requires_explicit_hardware_opt_in_before_artifacts(self):
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("hardware_qfloat_probe.py")),
                "--probe",
                "unused",
                "--output",
                "unused",
            ],
            capture_output=True,
            text=True,
            cwd="/tmp",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device and Frame root are required", result.stderr)


if __name__ == "__main__":
    unittest.main()
