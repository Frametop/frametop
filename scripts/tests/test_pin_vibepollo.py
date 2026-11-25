#!/usr/bin/env python3
"""scripts/pin-vibepollo.py: the kits and READMEs match host/vibepollo-pin.json, and a new pin
is written into each of them from a release's assets, offline (a fake fetch stands in for
GitHub; the files are copies in a temp folder).

  python3 scripts/tests/test_pin_vibepollo.py
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import re
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
# The pin in the tree is whatever it is now; the fake releases are far past any real one.
OLD_TAG = pv.read_pin(ROOT)["tag"]
NEW_TAG = "frametop-99.0.0"
NEWER_TAG = "frametop-99.0.1"
NEW_PKG = "vibepollo-99.0.0-1-x86_64.pkg.tar.zst"
DRIFTED_TAG = "frametop-0.0.1"
INSTALLER = b"#!/bin/bash\necho installer 99.0.0\n"


def sha(name):
    return hashlib.sha256(name.encode()).hexdigest()


NOTES = "- Windows: `sunshine.exe` ... go into a Vibepollo 2.0.3 install (Nonary's setup).\n"


def fake_release(tag=NEW_TAG, prerelease=False, draft=False, drop=(), extra=(), setup_exe=True, notes=NOTES):
    names = [NEW_PKG, "vibepollo-debug-99.0.0-1-x86_64.pkg.tar.zst", "sunshine.exe",
             "sunshine_wgc_capture.exe", "frametop_clipboard.exe", *extra]
    if setup_exe:
        names.append(pv.SETUP_EXE)
    assets = {}
    for n in names:
        if n not in drop:
            assets[n] = f"https://dl/{n}"
            assets[f"{n}.sha256"] = f"https://dl/{n}.sha256"
    release = json.dumps({
        "tag_name": tag, "draft": draft, "prerelease": prerelease, "body": notes,
        "assets": [{"name": n, "browser_download_url": u} for n, u in assets.items()],
    }).encode()
    urls = {
        f"https://api.github.com/repos/{pv.REPO}/releases/latest": release,
        f"https://api.github.com/repos/{pv.REPO}/releases/tags/{tag}": release,
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
        self.assertIn(f"sha256sum -c {NEW_PKG}.sha256", linux)  # the package's .sha256 too
        self.assertNotIn(old["linux"]["package"], linux)
        self.assertIn("Builds before frametop-2.0.3", linux)  # an earlier build, left alone
        for doc in ("README.md", "host/windows/README.md"):
            self.assertIn(pv.SETUP_EXE_URL, self.text(doc))  # no pins in it: the latest copy
            self.assertIn(f"releases/tag/{NEW_TAG}", self.text(doc))
        self.assertEqual(pv.check(self.root), [])

    def test_pin_a_named_tag(self):
        new, _, _ = pv.pin(self.root, NEW_TAG, fetch=fake_release())
        self.assertEqual(new["tag"], NEW_TAG)

    def test_again_changes_nothing(self):
        pv.pin(self.root, fetch=fake_release())
        kit = self.text(pv.KIT)
        _, changed, _ = pv.pin(self.root, fetch=fake_release())
        self.assertEqual(changed, [])
        self.assertEqual(self.text(pv.KIT), kit)
        self.assertEqual(kit.count(f"  # {OLD_TAG}\n"), 1)

    def test_two_releases_keep_both_older_builds(self):
        pv.pin(self.root, fetch=fake_release())
        pin = pv.read_pin(self.root)
        pin["tag"] = NEWER_TAG
        pin["windows"]["files"]["sunshine.exe"] = sha(NEWER_TAG)
        pv.write_pin(self.root, pin)
        pv.apply(self.root)
        kit = self.text(pv.KIT)
        self.assertIn(f"  # {OLD_TAG}\n", kit)
        self.assertIn(f'"{sha("sunshine.exe").upper()}"  # {NEW_TAG}\n', kit)
        self.assertEqual(pv.current(pv.read_texts(self.root))["older"][-1], sha("sunshine.exe"))

    def test_no_setup_exe_warns(self):
        _, _, warnings = pv.pin(self.root, fetch=fake_release(setup_exe=False))
        self.assertEqual(len(warnings), 1)
        self.assertIn(pv.SETUP_EXE, warnings[0])

    def test_setup_exe_warning_is_about_the_latest_release(self):
        named = fake_release(setup_exe=False)  # the pinned tag lacks it
        latest = fake_release(tag=NEWER_TAG)  # the latest has it

        def fetch(url):
            return latest(url) if url.endswith("/latest") else named(url)
        _, _, warnings = pv.pin(self.root, NEW_TAG, fetch=fetch)
        self.assertEqual(warnings, [])

    def test_refusals_change_nothing(self):
        for name, fetch in [
            ("prerelease", fake_release(prerelease=True)),
            ("draft", fake_release(draft=True)),
            ("no base in the notes", fake_release(notes="Built on upstream 2.0.2.")),
            # Another base: the kit needs changing by hand, and the pin stays as it was.
            ("other base", fake_release(notes=NOTES.replace("2.0.3", "2.0.6"))),
            ("not ours", fake_release(tag="2.18.9")),
            ("a tag the kits can't take", fake_release(tag="frametop-$(id)")),
            ("missing file", fake_release(drop=("frametop_clipboard.exe",))),
            ("two packages", fake_release(extra=("vibepollo-99.0.0-2-x86_64.pkg.tar.zst",))),
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

    def test_base_given_by_hand(self):
        new, _, _ = pv.pin(self.root, base="2.0.3", fetch=fake_release(notes=""))
        self.assertEqual(new["windows"]["base"], "2.0.3")

    def test_older_list_closed_by_its_own_line(self):
        # A hand-written entry whose comment ends in ")" stays inside the list.
        by_hand = '$OlderBuildShas = @(\n    "' + "A" * 64 + '"  # a test build (by hand)\n)'
        kit = re.sub(pv.OLDER, lambda m: by_hand, self.text(pv.KIT), flags=re.M)
        (self.root / pv.KIT).write_text(kit)
        replaced = pv.read_pin(self.root)["windows"]["files"]["sunshine.exe"].upper()
        pv.pin(self.root, fetch=fake_release())
        kit = self.text(pv.KIT)
        self.assertIn(f'  # a test build (by hand)\n    "{replaced}"', kit)
        self.assertEqual(pv.current(pv.read_texts(self.root))["older"][0], "a" * 64)
        self.assertEqual(len(pv.current(pv.read_texts(self.root))["older"]), 2)

    def test_suffixed_tag_left_alone(self):
        doc = self.root / "host/linux/README.md"
        doc.write_text(doc.read_text() + f"\n{OLD_TAG}-1 was a test build.\n")
        pv.pin(self.root, fetch=fake_release())
        self.assertIn(f"{OLD_TAG}-1 was a test build.", doc.read_text())

    def test_check_names_drift(self):
        (self.root / pv.GET_SH).write_text(self.text(pv.GET_SH).replace(f"\nTAG={OLD_TAG}\n", f"\nTAG={DRIFTED_TAG}\n"))
        problems = pv.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn(DRIFTED_TAG, problems[0])

    def test_check_names_windows_drift(self):
        kit = self.text(pv.KIT)
        sha = pv.read_pin(self.root)["windows"]["files"]["tools\\frametop_clipboard.exe"].upper()
        (self.root / pv.KIT).write_text(kit.replace(sha, "0" * 64))
        self.assertEqual(len(pv.check(self.root)), 1)
        (self.root / "README.md").write_text(self.text("README.md").replace(pv.SETUP_EXE_URL, "x"))
        self.assertEqual(len(pv.check(self.root)), 2)

    def test_bad_sha_asset_refused(self):
        fetch = fake_release()

        def bad(url):
            if url.endswith("/sunshine.exe.sha256"):
                return b"nothex  sunshine.exe\n"
            return fetch(url)
        with self.assertRaisesRegex(pv.PinError, "sunshine.exe.sha256"):
            pv.pin(self.root, fetch=bad)


class TestMain(unittest.TestCase):
    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = pv.main(["pin-vibepollo.py", *args])
        return code, out.getvalue() + err.getvalue()

    def test_check_on_the_repo(self):
        code, out = self.run_main("check")
        self.assertEqual(code, 0, out)
        self.assertIn("every file pins", out)

    def test_usage(self):
        for args in [(), ("nope",), ("check", "x"), ("apply", "--base", "2.0.3"), ("pin", "--base"), ("pin", "a", "b")]:
            with self.subTest(args):
                self.assertEqual(self.run_main(*args)[0], 2)

    def test_check_drift_exits_1(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for p in (pv.PIN, pv.GET_SH, pv.KIT) + pv.DOCS:
                (root / p).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(ROOT / p, root / p)
            (root / pv.GET_SH).write_text((root / pv.GET_SH).read_text().replace(f"TAG={OLD_TAG}", f"TAG={DRIFTED_TAG}"))
            saved = pv.ROOT
            pv.ROOT = root
            try:
                code, out = self.run_main("check")
            finally:
                pv.ROOT = saved
            self.assertEqual(code, 1)
            self.assertIn(DRIFTED_TAG, out)


if __name__ == "__main__":
    unittest.main()
