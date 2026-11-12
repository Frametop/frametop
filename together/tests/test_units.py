"""Frametop Together's user units (together/*.service), as files: the room's programs each
start and stop with ft-together's unit and no other way, the lists of them in together/run.sh
and install.sh are the units there are, and each unit starts a program this repo has or builds.

    python3 together/tests/test_units.py
"""
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
REPO = HERE.parent
MAIN = "frametop-together.service"
UNITS = sorted(p.name for p in HERE.glob("*.service"))
ROOMS = [u for u in UNITS if u != MAIN]


def keys(unit, key):
    return [line.split("=", 1)[1] for line in (HERE / unit).read_text().splitlines() if line.startswith(key + "=")]


class Units(unittest.TestCase):
    def test_the_rooms_programs_go_with_ft_together(self):
        self.assertIn(MAIN, UNITS)
        self.assertEqual(len(ROOMS), 8)
        self.assertEqual(sorted(" ".join(keys(MAIN, "Wants")).split()), ROOMS)
        self.assertEqual(keys(MAIN, "WantedBy"), ["default.target"])
        for unit in ROOMS:
            with self.subTest(unit):
                self.assertEqual(keys(unit, "PartOf"), [MAIN])
                self.assertIn(MAIN, " ".join(keys(unit, "After")).split())
                self.assertEqual(keys(unit, "WantedBy"), [], "it starts with ft-together's unit only")

    def test_each_starts_a_program_there_is(self):
        built = (HERE / "build.sh").read_text() + (REPO / "screens/build.sh").read_text()
        for unit in UNITS:
            with self.subTest(unit):
                (start,) = keys(unit, "ExecStart")
                words = [w for w in start.split() if w.startswith("@REPO@/") and not w.endswith("/scripts/in-box")]
                self.assertEqual(len(words), 1, start)
                rel = words[0][len("@REPO@/"):]
                if "/build/" in rel:
                    self.assertEqual(start.split()[0], "@REPO@/scripts/in-box", "a built program runs in the container")
                    self.assertIn("build/" + rel.rsplit("/", 1)[1], built)
                else:
                    self.assertEqual(start.split()[0], "/usr/bin/python3")
                    self.assertTrue((REPO / rel).is_file(), rel)

    def test_what_is_killed_is_only_its_own(self):
        """A program in the container isn't in its unit's cgroup, so each cleans its own up by
        name: the whole name (-x), or for ft-avatars, which the mirror runs too, its --room."""
        for unit in UNITS:
            for line in keys(unit, "ExecStartPre") + keys(unit, "ExecStop") + keys(unit, "ExecStopPost"):
                with self.subTest(unit=unit, line=line):
                    (start,) = keys(unit, "ExecStart")
                    program = [w for w in start.split() if "/build/" in w][0].rsplit("/", 1)[1]
                    self.assertRegex(line, r"^-/usr/bin/pkill (-x %s|-f \"%s --room\( \|\$\$\)\")$" % (re.escape(program), re.escape(program)))
                    self.assertEqual("-f" in line.split(), program == "ft-avatars")

    def test_the_installers_name_them_all(self):
        run = (HERE / "run.sh").read_text()
        named = re.search(r'^main=(\S+)\nrooms="([^"]+)"', run, re.M)
        self.assertEqual(sorted([named.group(1)] + named.group(2).split()), UNITS)
        install = re.search(r'^together_units="([^"]+)"', (REPO / "install.sh").read_text(), re.M)
        self.assertEqual(sorted(install.group(1).split()), UNITS)


if __name__ == "__main__":
    unittest.main()
