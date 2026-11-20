"""keyboard/fetch.py, offline: a made-up release (GitHub's JSON, SHA256SUMS, a package built
here) served through a replaced get(), and the checks that refuse a bad one."""
import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fetch  # noqa: E402

TAG = "0.20.0"
NAME = f"framekeyboard-{TAG}-aarch64.tar.gz"
BASE = f"https://github.com/{fetch.REPO}/releases/download/{TAG}"


def package(tag=TAG, extra=None):
    """A .tar.gz like a release's: bin/framekeyboard and share/framekeyboard under its folder."""
    top = f"framekeyboard-{tag}-aarch64"
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w:gz") as archive:
        def add(name, data=b"", kind=tarfile.REGTYPE, link=""):
            info = tarfile.TarInfo(name)
            info.type, info.linkname, info.size, info.mode = kind, link, len(data), 0o755
            archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        add(f"{top}", kind=tarfile.DIRTYPE)
        add(f"{top}/bin/framekeyboard", b"#!/bin/sh\n")
        add(f"{top}/share/framekeyboard", kind=tarfile.DIRTYPE)
        add(f"{top}/share/framekeyboard/layouts.json", b"{}")
        for entry in extra or []:
            add(*entry)
    return raw.getvalue()


class Release:
    """What GitHub serves for one release, by URL."""

    def __init__(self, tag=TAG, data=None, digest=True, **fields):
        self.data = data if data is not None else package(tag)
        sha = hashlib.sha256(self.data).hexdigest()
        name = f"framekeyboard-{tag}-aarch64.tar.gz"
        base = f"https://github.com/{fetch.REPO}/releases/download/{tag}"
        self.json = {"tag_name": tag, "draft": False, "prerelease": False, **fields,
                     "assets": [{"name": name, "browser_download_url": f"{base}/{name}",
                                 **({"digest": f"sha256:{sha}"} if digest else {})},
                                {"name": "SHA256SUMS", "browser_download_url": f"{base}/SHA256SUMS"}]}
        self.urls = {f"{base}/{name}": self.data, f"{base}/SHA256SUMS": f"{sha}  {name}\n".encode()}
        self.asked = []

    def get(self, url, limit, accept=None):
        self.asked.append(url)
        if url.startswith(fetch.API):
            return json.dumps(self.json).encode()
        return self.urls[url]


class FetchTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.dest = Path(self.dir.name) / "build" / "release"
        patcher = mock.patch.object(fetch, "check_program", lambda folder, tag: None)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self.dir.cleanup()

    def fetch(self, release, **kw):
        with mock.patch.object(fetch, "get", release.get):
            return fetch.fetch(self.dest, **kw)

    def test_the_newest_release_goes_in_with_what_it_is(self):
        release = Release()
        self.assertEqual(self.fetch(release), TAG)
        self.assertEqual(release.asked[0], f"{fetch.API}/latest")
        self.assertTrue((self.dest / "bin" / "framekeyboard").is_file())
        written = json.loads((self.dest / "RELEASE").read_text())
        self.assertEqual(written["version"], TAG)
        self.assertEqual(written["sha256"], hashlib.sha256(release.data).hexdigest())

    def test_the_same_release_again_is_kept_and_a_new_one_replaces_it(self):
        self.fetch(Release())
        (self.dest / "marker").write_text("x")
        again = Release()
        self.fetch(again)
        self.assertNotIn(f"{BASE}/{NAME}", again.asked)   # not downloaded again
        self.assertTrue((self.dest / "marker").exists())
        self.fetch(Release("0.21.0"))
        self.assertFalse((self.dest / "marker").exists())
        self.assertEqual(json.loads((self.dest / "RELEASE").read_text())["version"], "0.21.0")
        self.assertEqual(sorted(p.name for p in self.dest.parent.iterdir()), ["release"])   # no leftovers

    def test_an_exact_version(self):
        release = Release("0.19.1")
        self.fetch(release, exact="0.19.1")
        self.assertEqual(release.asked[0], f"{fetch.API}/tags/0.19.1")
        with self.assertRaises(fetch.FetchError):
            self.fetch(Release(), exact="../latest")

    def test_refused_releases_leave_what_was_there(self):
        self.fetch(Release())
        bad = [
            Release(draft=True),
            Release(prerelease=True),
            Release("0.18.9"),               # older than the first one tested
            Release("v0.20.0"),              # not a plain version
            Release(data=b"not a package"),  # (its sums match: unpacking refuses it)
            Release(data=package(extra=[("../evil", b"x")])),
            Release(data=package(extra=[(f"framekeyboard-{TAG}-aarch64/lib/x", b"", tarfile.SYMTYPE, "/etc/passwd")])),
        ]
        for release in bad:
            with self.subTest(release.json["tag_name"]), self.assertRaises(fetch.FetchError):
                self.fetch(release, force=True)
        self.assertEqual(json.loads((self.dest / "RELEASE").read_text())["version"], TAG)
        self.assertEqual(sorted(p.name for p in self.dest.parent.iterdir()), ["release"])

    def test_a_package_that_does_not_match_its_sum(self):
        release = Release()
        release.urls[f"{BASE}/{NAME}"] = package(extra=[(f"framekeyboard-{TAG}-aarch64/more", b"y")])
        with self.assertRaisesRegex(fetch.FetchError, "sha256"):
            self.fetch(release)
        self.assertFalse(self.dest.exists())

    def test_sums_and_digest_must_agree(self):
        sha = "a" * 64
        self.assertEqual(fetch.expected_sha(f"{sha}  {NAME}\n{'b' * 64}  other\n", NAME, f"sha256:{sha}"), sha)
        self.assertEqual(fetch.expected_sha(f"{sha} *{NAME}\n", NAME, None), sha)
        with self.assertRaises(fetch.FetchError):
            fetch.expected_sha(f"{sha}  {NAME}\n", NAME, "sha256:" + "c" * 64)
        with self.assertRaises(fetch.FetchError):
            fetch.expected_sha(f"{sha}  other\n", NAME, None)

    def test_only_github_over_https(self):
        fetch.check_url("https://api.github.com/repos/x")
        fetch.check_url("https://release-assets.githubusercontent.com/x")
        for url in ("http://github.com/x", "https://example.com/x", "file:///etc/passwd",
                    "https://github.com.example.com/x"):
            with self.subTest(url), self.assertRaises(fetch.FetchError):
                fetch.check_url(url)

    def test_a_token_goes_to_the_api_only(self):
        seen = []

        class Opener:
            def open(self, request, timeout):
                seen.append((request.full_url, request.get_header("Authorization")))
                return io.BytesIO(b"{}")
        with mock.patch.dict("os.environ", {"GITHUB_TOKEN": "t0ken"}), \
                mock.patch.object(fetch.urllib.request, "build_opener", lambda *a: Opener()):
            fetch.get(f"{fetch.API}/latest", 100)
            fetch.get(f"{BASE}/{NAME}", 100)
        self.assertEqual(seen, [(f"{fetch.API}/latest", "Bearer t0ken"), (f"{BASE}/{NAME}", None)])

    def test_redirects_stay_on_github_and_the_token_stays_on_the_api(self):
        handler = fetch._OnlyGitHub()
        request = fetch.urllib.request.Request(f"{fetch.API}/latest", headers={"Authorization": "Bearer t0ken"})
        asset = "https://release-assets.githubusercontent.com/x"
        new = handler.redirect_request(request, None, 302, "Found", {}, asset)
        self.assertEqual(new.full_url, asset)
        self.assertIsNone(new.get_header("Authorization"))
        new = handler.redirect_request(request, None, 302, "Found", {}, f"{fetch.API}/other")
        self.assertEqual(new.get_header("Authorization"), "Bearer t0ken")
        for elsewhere in ("https://example.com/x", "http://github.com/x"):
            with self.subTest(elsewhere), self.assertRaises(fetch.FetchError):
                handler.redirect_request(request, None, 302, "Found", {}, elsewhere)

    def test_a_download_cut_short_is_a_fetch_error(self):
        class Opener:
            def open(self, request, timeout):
                raise fetch.http.client.IncompleteRead(b"part", 10)
        with mock.patch.object(fetch.urllib.request, "build_opener", lambda *a: Opener()), \
                self.assertRaises(fetch.FetchError):
            fetch.get(f"{BASE}/{NAME}", 100)

    def test_a_swap_that_fails_keeps_the_release_that_was_there(self):
        self.fetch(Release())
        real = Path.rename

        def rename(path, target):
            if Path(target) == self.dest and path.name != "old":   # the new one going in
                raise OSError("no room")
            return real(path, target)
        with mock.patch.object(Path, "rename", rename), self.assertRaises(OSError):
            self.fetch(Release("0.21.0"))
        self.assertEqual(json.loads((self.dest / "RELEASE").read_text())["version"], TAG)

    def test_main_says_why_and_exits_1(self):
        with mock.patch.object(fetch, "get", Release(draft=True).get), \
                mock.patch("sys.stderr", io.StringIO()) as err:
            self.assertEqual(fetch.main(["--dest", str(self.dest)]), 1)
        self.assertIn("draft", err.getvalue())


class CheckProgram(unittest.TestCase):
    """What the release's program says when it's run, on the Frame."""

    def check(self, version_out, protocols_out):
        def run(command, **kw):
            out = version_out if command[1] == "--version" else protocols_out
            return mock.Mock(stdout=out)
        with mock.patch.object(fetch.os, "uname", lambda: mock.Mock(machine="aarch64")), \
                mock.patch.object(fetch, "OPENVR", mock.Mock(is_file=lambda: True)), \
                mock.patch.object(fetch.subprocess, "run", run):
            fetch.check_program(Path("/x"), "0.19.1")

    def test_the_right_version_with_protocol_1(self):
        self.check("framekeyboard 0.19.1\n", "1\n")
        self.check("0.19.1", "0 1 2")

    def test_another_version_or_no_protocol_1(self):
        for version_out, protocols_out in (("framekeyboard 0.19.0", "1"), ("", "1"),
                                           ("framekeyboard 0.19.1", "2"), ("framekeyboard 0.19.1", "")):
            with self.subTest(f"{version_out} / {protocols_out}"), self.assertRaises(fetch.FetchError):
                self.check(version_out, protocols_out)

    def test_a_program_that_wont_run(self):
        with mock.patch.object(fetch.os, "uname", lambda: mock.Mock(machine="aarch64")), \
                mock.patch.object(fetch, "OPENVR", mock.Mock(is_file=lambda: True)), \
                mock.patch.object(fetch.subprocess, "run", side_effect=OSError("exec format error")), \
                self.assertRaises(fetch.FetchError):
            fetch.check_program(Path("/x"), "0.19.1")


if __name__ == "__main__":
    unittest.main()
