"""The Frametop Installer's release lookup, offline: GitHub is a dict of canned answers. It picks
releases as get.sh --release does, reads what each can leave out, and decides tested, untested
or broken as install-release.sh's check_release does (the real one runs on the same files)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "installer"))
from frametop_installer import options, releases  # noqa: E402

API, RAW = releases.API, releases.RAW
CHECK = (REPO / "pack/install-release.sh").read_text().rsplit('\nmain "$@"', 1)[0]
SHA = "a" * 64


def rel_json(version, commit="abc1234", **extra):
    return dict({"schema": "frametop.release/v1", "version": version, "commit": commit,
                 "channel": "experimental" if "-" in version else "stable",
                 "image": {"file": "frametop-image.tar", "sha256": SHA, "id": "b" * 64},
                 "steamos": {"tested": [{"build": "B1", "version": "0.3.0"}], "broken": []}}, **extra)


def gh_release(tag, prerelease=False, draft=False, zip=True, info=True):
    assets = []
    if zip:
        assets.append({"name": "Frametop.zip", "size": 1_200_000_000,
                       "browser_download_url": f"https://dl/{tag}/Frametop.zip"})
    if info:
        assets.append({"name": "frametop-release.json", "browser_download_url": f"https://dl/{tag}/frametop-release.json"})
    return {"tag_name": tag, "prerelease": prerelease, "draft": draft, "assets": assets}


class FakeGitHub:
    def __init__(self, answers):
        self.answers = answers
        self.asked = []

    def __call__(self, url):
        self.asked.append(url)
        if url not in self.answers:
            raise OSError(f"404 {url}")
        a = self.answers[url]
        return a if isinstance(a, bytes) else json.dumps(a).encode()


def github(stable="0.3.0", exp="0.3.0-exp.24", draft="0.3.0-exp.25", extra=None):
    answers = {
        f"{API}/releases/latest": gh_release(f"v{stable}"),
        f"{API}/releases?per_page=30": [gh_release(f"v{draft}", prerelease=True, draft=True),
                                        gh_release(f"v{exp}", prerelease=True), gh_release(f"v{stable}")],
        f"https://dl/v{stable}/frametop-release.json": rel_json(stable),
        f"https://dl/v{exp}/frametop-release.json": rel_json(exp, install_options=["--no-voice", "--no-gaze"]),
        f"{RAW}/abc1234/pack/install-release.sh": b'      --no-voice|--no-bluetooth|--bluetooth) pass+=("$1") ;;\n',
    }
    answers.update(extra or {})
    return FakeGitHub(answers)


class Pick(unittest.TestCase):
    def test_stable_is_githubs_latest_and_experimental_the_newest_public_one(self):
        found = releases.lookup_both("B1", get=github())
        self.assertEqual(found["stable"].version, "0.3.0")
        self.assertEqual(found["experimental"].version, "0.3.0-exp.24")   # not the draft
        self.assertTrue(found["experimental"].prerelease)
        self.assertEqual(found["stable"].zip_size, 1_200_000_000)

    def test_what_each_can_leave_out(self):
        gh = github()
        found = releases.lookup_both("B1", get=gh)
        self.assertEqual(found["experimental"].supported, ("--no-voice", "--no-gaze"))
        # no install_options: read from that commit's install-release.sh
        self.assertEqual(found["stable"].supported, ("--no-voice", "--no-bluetooth", "--bluetooth"))
        self.assertIn(f"{RAW}/abc1234/pack/install-release.sh", gh.asked)

    def test_the_script_unreachable_falls_back_to_the_baseline(self):
        gh = github()
        del gh.answers[f"{RAW}/abc1234/pack/install-release.sh"]
        self.assertEqual(releases.lookup_both("B1", get=gh)["stable"].supported, options.BASELINE)

    def test_github_unreachable_says_so_for_each(self):
        found = releases.lookup_both("B1", get=FakeGitHub({}))
        for channel in ("stable", "experimental"):
            self.assertIsInstance(found[channel], LookupError)
            self.assertIn("couldn't reach GitHub", str(found[channel]))

    def test_a_release_without_its_json_or_with_the_wrong_one(self):
        gh = github(extra={f"{API}/releases/latest": gh_release("v0.3.0", info=False)})
        self.assertIn("no frametop-release.json", str(releases.lookup_both("B1", get=gh)["stable"]))
        gh = github(extra={"https://dl/v0.3.0/frametop-release.json": rel_json("0.2.9")})
        self.assertIn("names version 0.2.9", str(releases.lookup_both("B1", get=gh)["stable"]))
        gh = github(extra={"https://dl/v0.3.0/frametop-release.json": rel_json("0.3.0", commit="nothex!")})
        self.assertIn("isn't usable", str(releases.lookup_both("B1", get=gh)["stable"]))

    def test_an_odd_answer_is_a_lookup_error(self):
        gh = github(extra={f"{API}/releases?per_page=30": [{"draft": False, "assets": [{"name": "Frametop.zip"}]}]})
        self.assertIsInstance(releases.lookup_both("B1", get=gh)["experimental"], LookupError)

    def test_main_s_table_wins_over_the_release_s(self):
        gh = github(extra={releases.TABLE_URL: {"tested": [{"build": "B2", "version": "0.5.9"}], "broken": []}})
        found = releases.lookup_both("B2", get=gh)
        self.assertEqual(found["stable"].status, "tested")
        found = releases.lookup_both("B1", get=gh)
        self.assertEqual((found["stable"].status, found["stable"].note), ("untested", "tested on SteamOS 0.5.9"))


class SteamOS(unittest.TestCase):
    """steamos_status against the real check_release, on the same files."""

    def check_release(self, rj, table, build):
        with tempfile.TemporaryDirectory() as d:
            rp, tp = os.path.join(d, "r.json"), os.path.join(d, "t.json")
            pathlib.Path(rp).write_text(json.dumps(rj))
            if table is not None:
                pathlib.Path(tp).write_text(json.dumps(table))
            out = subprocess.run(["bash", "-c", CHECK + '\ncheck_release "$@"', "x", rp,
                                  tp if table is not None else "", build],
                                 capture_output=True, text=True, check=True).stdout.splitlines()
        return out[0]

    def test_same_status_as_install_release_sh(self):
        broken = {"tested": [{"build": "B1", "version": "0.3.0"}],
                  "broken": [{"build": "B1", "reason": "no laser", "fixed_in": "0.3.1"},
                             {"build": "B3", "reason": "later break", "from": "0.4.0"}]}
        cases = [(rel_json("0.3.0"), None, "B1"), (rel_json("0.3.0"), None, "B9"),
                 (rel_json("0.3.0"), broken, "B1"), (rel_json("0.3.1"), broken, "B1"),
                 (rel_json("0.3.0"), broken, "B3"), (rel_json("0.4.0"), broken, "B3"),
                 (rel_json("0.3.0-exp.2"), broken, "B1")]
        for rj, table, build in cases:
            with self.subTest(version=rj["version"], build=build, table=bool(table)):
                self.assertEqual(releases.steamos_status(rj, table, build)[0], self.check_release(rj, table, build))

    def test_the_note_names_the_fix(self):
        table = {"tested": [], "broken": [{"build": "B1", "reason": "no\nlaser", "fixed_in": "0.3.1"}]}
        self.assertEqual(releases.steamos_status(rel_json("0.3.0"), table, "B1"),
                         ("broken", "no laser (fixed in Frametop 0.3.1)"))


class OsRelease(unittest.TestCase):
    def test_reads_the_hosts_first(self):
        with tempfile.TemporaryDirectory() as d:
            host_, runtime = pathlib.Path(d, "host"), pathlib.Path(d, "runtime")
            host_.write_text('ID=steamos\nVARIANT_ID="vr"\nBUILD_ID=20261113.6134720\n')
            runtime.write_text("ID=org.gnome.os\n")
            osr = releases.os_release((str(host_), str(runtime)))
            self.assertTrue(releases.is_frame(osr))
            self.assertEqual(osr["BUILD_ID"], "20261113.6134720")
            self.assertFalse(releases.is_frame(releases.os_release((str(pathlib.Path(d, "none")), str(runtime)))))


if __name__ == "__main__":
    unittest.main()
