"""The Frametop Installer's parts table, offline: every option it passes is one get.sh,
install-release.sh and install.sh take (so the table can't drift from them), what a release can
leave out comes from its install_options or its install-release.sh, and the picks turn into the
right get.sh options."""
import pathlib
import re
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "installer"))
from frametop_installer import options  # noqa: E402

# 0.3.0's install-release.sh, the line that passes options on (before INSTALL_OPTIONS)
OLD_SCRIPT = """
    case $1 in
      --yes) yes=1; pass+=("$1") ;;
      --no-voice|--no-hands|--no-hand-recorder|--no-eye-tracker|--no-eye-recorder|--no-bluetooth|--bluetooth) pass+=("$1") ;;
"""
EXP24 = ("--no-voice", "--no-hands", "--no-hand-recorder", "--no-eye-tracker", "--no-eye-recorder",
         "--no-keyboard", "--no-together", "--together", "--no-bluetooth", "--bluetooth")


def case_options(path):
    """Every --option a script's case statements name."""
    return set(re.findall(r"(?:^|\|)\s*(--[a-z][a-z-]*)(?=[|)])", path.read_text(), re.M))


def all_flags():
    return {f for p in options.PARTS for f in (p.on, p.off) if f}


class Table(unittest.TestCase):
    def test_every_option_reaches_install_sh(self):
        for script in ("install.sh", "get.sh"):
            missing = all_flags() - case_options(REPO / script)
            self.assertEqual(missing, set(), f"{script} doesn't take {missing}")
        listed = subprocess.run(["bash", str(REPO / "pack/install-release.sh"), "--list-options"],
                                capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(all_flags() - set(listed), set())
        self.assertEqual(set(listed) - all_flags(), set(), "install-release.sh takes an option the window doesn't offer")

    def test_install_release_sh_reads_back_from_its_source(self):
        listed = subprocess.run(["bash", str(REPO / "pack/install-release.sh"), "--list-options"],
                                capture_output=True, text=True, check=True).stdout.split()
        text = (REPO / "pack/install-release.sh").read_text()
        self.assertEqual(options.options_from_script(text), tuple(listed))

    def test_an_old_release_is_read_from_its_case_line(self):
        self.assertEqual(options.options_from_script(OLD_SCRIPT), options.BASELINE)
        self.assertIsNone(options.options_from_script("echo nothing here"))

    def test_the_release_json_wins_then_the_script_then_the_baseline(self):
        self.assertEqual(options.supported_options({"install_options": ["--no-voice"]}, OLD_SCRIPT), ("--no-voice",))
        self.assertEqual(options.supported_options({}, OLD_SCRIPT), options.BASELINE)
        self.assertEqual(options.supported_options({}, None), options.BASELINE)
        self.assertEqual(options.supported_options({"install_options": "x"}, None), options.BASELINE)

    def test_needs_point_at_parts(self):
        keys = [p.key for p in options.PARTS]
        self.assertEqual(len(keys), len(set(keys)))
        for p in options.PARTS:
            if p.needs:
                self.assertLess(keys.index(p.needs), keys.index(p.key), "a part comes after the one it needs")


class Picks(unittest.TestCase):
    ALL = {p.key: True for p in options.PARTS}

    def test_what_each_release_offers(self):
        kinds = {p.key: options.availability(p, options.BASELINE) for p in options.PARTS}
        self.assertEqual(kinds["gaze"], "always")
        self.assertEqual(kinds["together"], "absent")
        self.assertEqual(kinds["keyboard"], "always")
        self.assertEqual(kinds["bluetooth"], "choice")
        now = subprocess.run(["bash", str(REPO / "pack/install-release.sh"), "--list-options"],
                             capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual({options.availability(p, now) for p in options.PARTS}, {"choice"})

    def test_everything_ticked(self):
        self.assertEqual(options.install_flags(self.ALL, EXP24), ["--yes", "--together", "--bluetooth"])
        self.assertEqual(options.install_flags(self.ALL, options.BASELINE), ["--yes", "--bluetooth"])
        self.assertTrue(options.needs_password(self.ALL, EXP24))

    def test_nothing_ticked(self):
        none = {k: False for k in self.ALL}
        flags = options.install_flags(none, EXP24)
        self.assertEqual(flags[0], "--yes")
        self.assertEqual(set(flags[1:]), {"--no-voice", "--no-hands", "--no-hand-recorder", "--no-eye-tracker",
                                          "--no-eye-recorder", "--no-keyboard", "--no-together", "--no-bluetooth"})
        self.assertFalse(options.needs_password(none, EXP24))

    def test_a_part_goes_with_the_one_it_needs(self):
        picks = dict(self.ALL, eye_tracker=False)
        on = options.effective(picks, EXP24)
        self.assertFalse(on["eye_recorder"])
        self.assertIn("--no-eye-recorder", options.install_flags(picks, EXP24))
        picks = dict(self.ALL, hands=False)
        self.assertFalse(options.effective(picks, EXP24)["hand_recorder"])

    def test_gaze_off_where_a_release_takes_it(self):
        now = options.BASELINE + ("--no-gaze",)
        flags = options.install_flags(dict(self.ALL, gaze=False), now)
        self.assertIn("--no-gaze", flags)
        self.assertIn("--no-eye-tracker", flags)
        # a release that always installs gaze mode keeps it, and our eye tracker with it
        on = options.effective(dict(self.ALL, gaze=False), options.BASELINE)
        self.assertTrue(on["gaze"])
        self.assertTrue(on["eye_tracker"])

    def test_a_box_locks_while_the_part_it_needs_is_off(self):
        picks = dict(self.ALL, hand_recorder=False)
        self.assertEqual(options.boxes(picks, EXP24)["hand_recorder"], (False, True))
        off = options.boxes(dict(picks, hands=False), EXP24)
        self.assertEqual(off["hand_recorder"], (False, False))
        # ticked again, a box shows what was picked for it, not what it showed while locked
        self.assertEqual(options.boxes(dict(self.ALL, hands=False), EXP24)["hand_recorder"], (False, False))
        self.assertEqual(options.boxes(self.ALL, EXP24)["hand_recorder"], (True, True))
        # two levels: gaze off locks our eye tracker and, through it, the Eye Recorder
        now = options.BASELINE + ("--no-gaze",)
        off = options.boxes(dict(self.ALL, gaze=False), now)
        self.assertEqual((off["eye_tracker"], off["eye_recorder"]), ((False, False), (False, False)))
        self.assertNotIn("voice", off)   # a part that needs nothing isn't in it

    def test_npu_hand_tracking_only_where_a_release_has_it(self):
        # releases before it don't have it, so it's absent there, not "always installed"
        self.assertEqual(options.availability(options.BY_KEY["npu"], EXP24), "absent")
        self.assertNotIn("--no-npu", options.install_flags({k: False for k in self.ALL}, EXP24))
        now = EXP24 + ("--no-npu",)
        self.assertEqual(options.availability(options.BY_KEY["npu"], now), "choice")
        self.assertEqual(options.install_flags(self.ALL, now), ["--yes", "--together", "--bluetooth"])
        self.assertIn("--no-npu", options.install_flags(dict(self.ALL, npu=False), now))
        # it goes with hand tracking, and its box locks while that's off
        self.assertIn("--no-npu", options.install_flags(dict(self.ALL, hands=False), now))
        self.assertEqual(options.boxes(dict(self.ALL, hands=False), now)["npu"], (False, False))
        self.assertTrue(options.needs_password({k: k == "npu" or k == "hands" for k in self.ALL}, now))

    def test_only_sudo_parts_need_the_password(self):
        picks = {k: False for k in self.ALL}
        picks.update(voice=True, gaze=True, eye_recorder=True, keyboard=True, together=True)
        self.assertFalse(options.needs_password(picks, EXP24))
        self.assertTrue(options.needs_password(dict(picks, bluetooth=True), EXP24))


if __name__ == "__main__":
    unittest.main()
