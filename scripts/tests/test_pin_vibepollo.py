#!/usr/bin/env python3
"""scripts/pin-vibepollo.py: the kits and READMEs match host/vibepollo-pin.json, and a new pin
is written into each of them from a release's assets, offline (a fake fetch stands in for
GitHub; the files are copies in a temp folder).

  python3 scripts/tests/test_pin_vibepollo.py
"""
import hashlib
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load():
    spec = importlib.util.spec_from_file_location("pin_vibepollo", ROOT / "scripts/pin-vibepollo.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pv = load()
NEW_TAG = "frametop-2.0.5"
NEW_PKG = "vibepollo-2.0.5-1-x86_64.pkg.tar.zst"
INSTALLER = b"#!/bin/bash\necho installer 2.0.5\n"


def sha(name):
    return hashlib.sha256(name.encode()).hexdigest()


def fake_release(tag=NEW_TAG, prerelease=False, drop=(), extra=(), setup_exe=True):
    names = [NEW_PKG, "vibepollo-debug-2.0.5-1-x86_64.pkg.tar.zst", "sunshine.exe",
             "sunshine_wgc_capture.exe", "frametop_clipboard.exe", *extra]
    if setup_exe:
        names.append(pv.SETUP_EXE)
    assets = {}
    for n in names:
        if n not in drop:
            assets[n] = f"https://dl/{n}"
            assets[f"{n}.sha256"] = f"https://dl/{n}.sha256"
    urls = {
        f"https://api.github.com/repos/{pv.REPO}/releases/latest": json.dumps({
            "tag_name": tag, "draft": False, "prerelease": prerelease,
            "assets": [{"name": n, "browser_download_url": u} for n, u in assets.items()],
        }).encode(),
        f"https://raw.githubusercontent.com/{pv.REPO}/{tag}/scripts/linux_install.sh": INSTALLER,
    }
    for n in names:
        urls[f"https://dl/{n}.sha256"] = f"{sha(n)}  {n}\n".encode()

    def fetch(url):
        if url not in urls:
            raise AssertionError(f"unexpected fetch {url}")
        return urls[url]
    return fetch


class Copy(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for p in (pv.PIN, pv.GET_SH, pv.KIT) + pv.DOCS:
            (self.root / p).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / p, self.root / p)
        self.before = {p: (self.root / p).read_text() for p in (pv.PIN, pv.GET_SH, pv.KIT) + pv.DOCS}

    def tearDown(self):
        self.tmp.cleanup()

    def text(self, p):
        return (self.root / p).read_text()


class TestRepo(unittest.TestCase):
    def test_files_match_the_pin(self):
        self.assertEqual(pv.check(ROOT), [])


class TestPin(Copy):
    def test_new_release_moves_every_file(self):
        old = pv.read_pin(self.root)
        new, changed, warnings = pv.pin(self.root, fetch=fake_release())
        self.assertEqual(warnings, [])
        self.assertEqual(new["tag"], NEW_TAG)
        self.assertEqual(new["linux"]["package"], NEW_PKG)  # not the -debug- one
        self.assertEqual(new["linux"]["installer_sha256"], hashlib.sha256(INSTALLER).hexdigest())
        self.assertEqual(new["windows"]["base"], old["windows"]["base"])
        self.assertEqual(pv.read_pin(self.root), new)
        self.assertEqual(sorted(changed), sorted((pv.GET_SH, pv.KIT) + pv.DOCS))

        sh = self.text(pv.GET_SH)
        self.assertIn(f"\nTAG={NEW_TAG}\n", sh)
        self.assertIn(f"\nPACKAGE={NEW_PKG}\n", sh)
        self.assertIn(f"\nPACKAGE_SHA256={sha(NEW_PKG)}\n", sh)
        kit = self.text(pv.KIT)
        self.assertIn(f'releases/download/{NEW_TAG}"', kit)
        for path in pv.WINDOWS_FILES:
            self.assertIn(f'Path = "{path}"; Sha = "{sha(path.split(chr(92))[-1]).upper()}"', kit)
        # The build it replaces: the kit still updates over it, named for its tag.
        self.assertIn(f'"{old["windows"]["files"]["sunshine.exe"].upper()}"  # {old["tag"]}\n', kit)
        linux = self.text("host/linux/README.md")
        self.assertNotIn(old["tag"], linux)
        self.assertIn(f"sudo bash linux_install.sh --package ./{NEW_PKG}", linux)
        self.assertIn("Builds before frametop-2.0.3", linux)  # an earlier build, left alone
        for doc in ("README.md", "host/windows/README.md"):
            self.assertIn(f"releases/download/{NEW_TAG}/{pv.SETUP_EXE}", self.text(doc))
        self.assertEqual(pv.check(self.root), [])

    def test_again_changes_nothing(self):
        pv.pin(self.root, fetch=fake_release())
        kit = self.text(pv.KIT)
        _, changed, _ = pv.pin(self.root, fetch=fake_release())
        self.assertEqual(changed, [])
        self.assertEqual(self.text(pv.KIT), kit)
        self.assertEqual(kit.count("  # frametop-2.0.4\n"), 1)

    def test_two_releases_keep_both_older_builds(self):
        pv.pin(self.root, fetch=fake_release())
        pin = pv.read_pin(self.root)
        pin["tag"] = "frametop-2.0.6"
        pin["windows"]["files"]["sunshine.exe"] = sha("2.0.6")
        pv.write_pin(self.root, pin)
        pv.apply(self.root)
        kit = self.text(pv.KIT)
        self.assertIn("  # frametop-2.0.4\n", kit)
        self.assertIn(f'"{sha("sunshine.exe").upper()}"  # {NEW_TAG}\n', kit)
        self.assertEqual(pv.current(self.root)["older"][-1], sha("sunshine.exe"))

    def test_no_setup_exe_warns(self):
        _, _, warnings = pv.pin(self.root, fetch=fake_release(setup_exe=False))
        self.assertEqual(len(warnings), 1)
        self.assertIn(pv.SETUP_EXE, warnings[0])

    def test_refusals_change_nothing(self):
        for name, fetch in [
            ("prerelease", fake_release(prerelease=True)),
            ("not ours", fake_release(tag="2.18.9")),
            ("missing file", fake_release(drop=("frametop_clipboard.exe",))),
            ("two packages", fake_release(extra=("vibepollo-2.0.5-2-x86_64.pkg.tar.zst",))),
        ]:
            with self.subTest(name):
                with self.assertRaises(pv.PinError):
                    pv.pin(self.root, fetch=fetch)
                for p, t in self.before.items():
                    self.assertEqual(self.text(p), t, p)

    def test_other_windows_base_refused(self):
        pin = pv.read_pin(self.root)
        pin["windows"]["base"] = "2.0.6"
        pv.write_pin(self.root, pin)
        with self.assertRaisesRegex(pv.PinError, "base by hand"):
            pv.apply(self.root)
        for p in (pv.GET_SH, pv.KIT) + pv.DOCS:
            self.assertEqual(self.text(p), self.before[p], p)

    def test_check_names_drift(self):
        (self.root / pv.GET_SH).write_text(self.text(pv.GET_SH).replace("\nTAG=frametop-2.0.4\n", "\nTAG=frametop-2.0.3\n"))
        problems = pv.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("frametop-2.0.3", problems[0])

    def test_bad_sha_asset_refused(self):
        fetch = fake_release()

        def bad(url):
            if url.endswith("/sunshine.exe.sha256"):
                return b"nothex  sunshine.exe\n"
            return fetch(url)
        with self.assertRaisesRegex(pv.PinError, "sunshine.exe.sha256"):
            pv.pin(self.root, fetch=bad)


if __name__ == "__main__":
    unittest.main()
