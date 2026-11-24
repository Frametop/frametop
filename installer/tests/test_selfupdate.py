"""The Frametop Installer's own update, offline: which release counts as a newer installer, the
download and its checksum, the flatpak command that installs it, and opening the new one."""
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "installer"))
from frametop_installer import releases, selfupdate  # noqa: E402

API = releases.API
DL = "https://github.com/Frametop/frametop/releases/download/"   # the one place its files may come from
BUNDLE = b"a flatpak bundle"
SUM = hashlib.sha256(BUNDLE).hexdigest()


def gh_release(tag, draft=False, bundle=True, sums=True, base=DL):
    assets = [{"name": "Frametop.zip", "browser_download_url": f"{base}{tag}/Frametop.zip"}]
    if bundle:
        assets.append({"name": "FrametopInstaller.flatpak", "browser_download_url": f"{base}{tag}/FrametopInstaller.flatpak"})
    if sums:
        assets.append({"name": "SHA256SUMS", "browser_download_url": f"{base}{tag}/SHA256SUMS"})
    return {"tag_name": tag, "draft": draft, "prerelease": "-" in tag, "assets": assets}


def github(latest, listed):
    answers = {f"{API}/releases/latest": latest, f"{API}/releases?per_page=30": listed}

    def get(url):
        if url not in answers:
            raise OSError(f"404 {url}")
        return json.dumps(answers[url]).encode()
    return get


class Check(unittest.TestCase):
    def test_an_experimental_installer_follows_the_newest_release_of_any_kind(self):
        gh = github(gh_release("v0.3.0", bundle=False),
                    [gh_release("v0.3.0-exp.28", draft=True), gh_release("windows-host-1", bundle=False),
                     gh_release("v0.3.0-exp.27"), gh_release("v0.3.0-exp.26"), gh_release("v0.3.0", bundle=False)])
        update = selfupdate.check("0.3.0-exp.26", gh)
        self.assertEqual(update, selfupdate.Update("0.3.0-exp.27", f"{DL}v0.3.0-exp.27/FrametopInstaller.flatpak",
                                                   f"{DL}v0.3.0-exp.27/SHA256SUMS"))   # not the draft
        gh = github(gh_release("v0.3.1"), [gh_release("v0.3.1"), gh_release("v0.3.0-exp.27")])
        self.assertEqual(selfupdate.check("0.3.0-exp.27", gh).version, "0.3.1")   # the stable after it

    def test_a_stable_installer_follows_them_too_with_an_experimental_frametop(self):
        # An experimental installer that took a stable release's bundle is a stable one from then
        # on. It goes on getting the experimental releases' installers as long as the Frametop
        # installed is experimental. (Experimental builds after a stable release keep its number,
        # as 0.3.0-exp.24 came after 0.3.0: the newest release listed is the newer.)
        gh = github(gh_release("v0.3.1"), [gh_release("v0.3.1-exp.28"), gh_release("v0.3.1")])
        self.assertEqual(selfupdate.check("0.3.1", gh, experimental=True).version, "0.3.1-exp.28")
        self.assertIsNone(selfupdate.check("0.3.1", gh))
        self.assertIsNone(selfupdate.check("0.3.1", github(gh_release("v0.3.1"), [gh_release("v0.3.1")]), True))

    def test_a_stable_installer_stays_on_stable_releases(self):
        gh = github(gh_release("v0.3.1"), [gh_release("v0.4.0-exp.1"), gh_release("v0.3.1")])
        self.assertEqual(selfupdate.check("0.3.0", gh).version, "0.3.1")
        gh = github(gh_release("v0.3.0"), [gh_release("v0.4.0-exp.1"), gh_release("v0.3.0")])
        self.assertIsNone(selfupdate.check("0.3.0", gh))

    def test_the_same_or_an_older_release_is_no_update(self):
        listed = [gh_release("v0.3.0-exp.26"), gh_release("v0.3.0-exp.25")]
        self.assertIsNone(selfupdate.check("0.3.0-exp.26", github(gh_release("v0.3.0"), listed)))
        self.assertIsNone(selfupdate.check("0.3.0-exp.30", github(gh_release("v0.3.0"), listed)))   # no downgrade

    def test_a_local_build_never_updates(self):
        asked = []

        def get(url):
            asked.append(url)
            raise OSError("offline")
        for own in ("", "v0.3.0-exp.26-3-g37558dea", "0.3.0-local.077e678", "dev"):
            self.assertIsNone(selfupdate.check(own, get))
        self.assertEqual(asked, [])

    def test_a_release_it_cant_check_is_no_update(self):
        for odd in (gh_release("v0.3.0-exp.27", sums=False),                         # nothing to check it against
                    gh_release("0.3.0-exp.27"), gh_release("v../../x"),              # not a release's tag
                    gh_release("v0.3.0-exp.27/../../../../evil/x/releases/download/v1"),
                    gh_release("v\u0660.\u0664.\u0660")):                            # (digits, but not 0-9)
            self.assertIsNone(selfupdate.check("0.3.0-exp.26", github(gh_release("v0.3.0"), [odd])), odd["tag_name"])
        self.assertIsNone(selfupdate.check("0.3.0-exp.26", github(gh_release("v0.3.0"), [])))   # no release has one
        self.assertIsNone(selfupdate.check("0.3.0", github(gh_release("v0.3.1", bundle=False), [])))

    def test_its_files_come_from_the_tagged_release_whatever_the_answer_says(self):
        for base in ("https://example.com/dl/", "https://github.com/evil/x/releases/download/",
                     f"{DL}v0.3.0-exp.27/../../../../evil/x/releases/download/", "http://github.com/Frametop/frametop/releases/download/"):
            rel = gh_release("v0.3.0-exp.27", base=base)
            update = selfupdate.check("0.3.0-exp.26", github(gh_release("v0.3.0"), [rel]))
            self.assertEqual((update.url, update.sums_url), (f"{DL}v0.3.0-exp.27/FrametopInstaller.flatpak",
                                                             f"{DL}v0.3.0-exp.27/SHA256SUMS"), base)

    def test_a_pulled_build_is_offered_the_stable_release_of_its_number(self):
        # 0.3.0-exp.28 was pulled, and the newest release left with an installer is 0.3.0. The
        # numbers can't say 0.3.0 is the older one, so it's offered: back to the last good one.
        gh = github(gh_release("v0.3.0"), [gh_release("v0.3.0-exp.28", draft=True), gh_release("v0.3.0")])
        self.assertEqual(selfupdate.check("0.3.0-exp.28", gh).version, "0.3.0")
        # ...but never the build before it, which the numbers do show is older
        gh = github(gh_release("v0.3.0"), [gh_release("v0.3.0-exp.28", draft=True), gh_release("v0.3.0-exp.27")])
        self.assertIsNone(selfupdate.check("0.3.0-exp.28", gh))

    def test_github_out_of_reach_raises(self):
        def get(url):
            raise OSError("offline")
        with self.assertRaises(OSError):
            selfupdate.check("0.3.0-exp.26", get)


class Download(unittest.TestCase):
    UPDATE = selfupdate.Update("0.3.0-exp.27", f"{DL}v0.3.0-exp.27/FrametopInstaller.flatpak",
                               f"{DL}v0.3.0-exp.27/SHA256SUMS")

    def get(self, sums, bundle=BUNDLE):
        files = {self.UPDATE.sums_url: sums.encode(), self.UPDATE.url: bundle}
        self.limits = {}

        def get(url, limit):
            self.limits[url] = limit
            return files[url]
        return get

    def test_a_bundle_that_matches_its_checksum_is_saved(self):
        sums = f"{'0' * 64}  Frametop.zip\n{SUM}  FrametopInstaller.flatpak\n"
        with tempfile.TemporaryDirectory() as d:
            path = selfupdate.download(self.UPDATE, pathlib.Path(d, "update"), self.get(sums))
            self.assertEqual((path.name, path.read_bytes()), ("FrametopInstaller.flatpak", BUNDLE))
            self.assertEqual([p.name for p in path.parent.iterdir()], ["FrametopInstaller.flatpak"])
        self.assertEqual(self.limits[self.UPDATE.url], selfupdate.MAX_BUNDLE)

    def test_one_that_doesnt_match_is_refused_and_not_saved(self):
        sums = f"{SUM}  FrametopInstaller.flatpak\n"
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(selfupdate.UpdateError, "doesn't match its checksum"):
                selfupdate.download(self.UPDATE, pathlib.Path(d, "update"), self.get(sums, b"something else"))
            self.assertFalse(pathlib.Path(d, "update", "FrametopInstaller.flatpak").exists())

    def test_no_checksum_listed_is_refused(self):
        for sums in ("", f"{SUM}  Frametop.zip\n", f"{SUM[:10]}  FrametopInstaller.flatpak\n",
                     f"{SUM}  other/FrametopInstaller.flatpak\n"):
            with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(selfupdate.UpdateError, "doesn't list"):
                selfupdate.download(self.UPDATE, d, self.get(sums))

    def test_a_checksum_in_capitals_is_the_same_checksum(self):
        with tempfile.TemporaryDirectory() as d:
            path = selfupdate.download(self.UPDATE, d, self.get(f"{SUM.upper()}  FrametopInstaller.flatpak\n"))
            self.assertEqual(path.read_bytes(), BUNDLE)

    def test_one_it_cant_save_says_so_and_leaves_nothing_behind(self):
        sums = f"{SUM}  FrametopInstaller.flatpak\n"
        with tempfile.TemporaryDirectory() as d, mock.patch.object(selfupdate.os, "replace", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(selfupdate.UpdateError, "couldn't save it in .* \\(disk full\\)"):
                selfupdate.download(self.UPDATE, d, self.get(sums))
            self.assertEqual(list(pathlib.Path(d).iterdir()), [])

    def test_a_download_far_too_big_is_given_up(self):
        class Answer:
            def __init__(self, data):
                self.data, self.asked = data, []

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self, n):
                self.asked.append(n)
                return self.data[:n]
        for data, fits in ((b"x" * 10, True), (b"x" * 11, False), (b"x" * 5000, False)):
            answer = Answer(data)
            with mock.patch.object(selfupdate.urllib.request, "urlopen", return_value=answer) as urlopen:
                if fits:
                    self.assertEqual(selfupdate.fetch_file(f"{DL}v1/x", 10), data)
                else:
                    with self.assertRaisesRegex(selfupdate.UpdateError, "far bigger"):
                        selfupdate.fetch_file(f"{DL}v1/x", 10)
            self.assertEqual(answer.asked, [11])                      # never more than the limit is read
            self.assertEqual(urlopen.call_args[0][0].full_url, f"{DL}v1/x")

    def test_a_download_that_fails_says_so(self):
        def get(url, limit):
            raise OSError("connection reset")
        with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(selfupdate.UpdateError, "connection reset"):
            selfupdate.download(self.UPDATE, d, get)

    def test_the_real_sums_file_s_shape(self):
        # release.yml appends the bundle's line with sha256sum, as the other files' lines
        self.assertIn("sha256sum FrametopInstaller.flatpak >>SHA256SUMS", (REPO / ".github/workflows/release.yml").read_text())
        self.assertEqual(selfupdate.listed_sum(f"{SUM} *FrametopInstaller.flatpak\n"), SUM)   # (binary mode's star)


class Install(unittest.TestCase):
    def test_it_installs_where_the_app_is(self):
        ran = []

        def run(argv, timeout=60):
            ran.append((argv, timeout))
            return 0, b"Installation complete.\n", b""
        selfupdate.install("/c/FrametopInstaller.flatpak", "system", run)
        selfupdate.install("/c/FrametopInstaller.flatpak", "user", run)
        self.assertEqual([a[2] for a, _ in ran], ["--system", "--user"])
        for argv, timeout in ran:
            self.assertEqual(argv[:2] + argv[3:], ["flatpak", "install", "--bundle", "--or-update", "--assumeyes",
                                                   "/c/FrametopInstaller.flatpak"])
            self.assertNotIn("--noninteractive", argv)   # a system-wide install has to ask for the password
            self.assertGreaterEqual(timeout, 300)        # ...and wait while it's typed

    def test_a_refused_install_says_what_flatpak_said(self):
        def run(argv, timeout=60):
            return 1, b"Installing...\n", b"\nerror: Failed to install bundle: Not authorized\n"
        with self.assertRaisesRegex(selfupdate.UpdateError, "Not authorized"):
            selfupdate.install("/c/x.flatpak", "system", run)
        with self.assertRaisesRegex(selfupdate.UpdateError, r"flatpak install failed \(124\)"):
            selfupdate.install("/c/x.flatpak", "system", lambda argv, timeout=60: (124, b"", b""))

    def test_where_the_app_is(self):
        with tempfile.TemporaryDirectory() as d:
            home, info = pathlib.Path(d, "home"), pathlib.Path(d, "flatpak-info")
            app = "app/io.github.frametop.Installer/aarch64/stable/abc/files"
            info.write_text(f"[Application]\nname=x\n\n[Instance]\napp-path={home}/.local/share/flatpak/{app}\n")
            self.assertEqual(selfupdate.installation(info, home), "user")
            info.write_text(f"[Instance]\napp-path=/var/lib/flatpak/{app}\n")
            self.assertEqual(selfupdate.installation(info, home), "system")
            self.assertEqual(selfupdate.installation(pathlib.Path(d, "none"), home), "")   # run from the repo


class Reopen(unittest.TestCase):
    def test_it_starts_the_installed_version_after_this_one_quit(self):
        started = []
        self.assertTrue(selfupdate.reopen(popen=lambda argv, **kw: started.append((argv, kw))))
        argv, kw = started[0]
        self.assertEqual(argv[:2], ["flatpak-spawn", "--latest-version"])   # not this window's own files
        self.assertRegex(argv[4], r"^sleep \d+; exec frametop-installer")
        self.assertTrue(kw["start_new_session"])
        # the command is the one the Flatpak has
        self.assertIn("--command=frametop-installer", (REPO / "installer/flatpak/build.sh").read_text())

    def test_no_flatpak_spawn_says_it_couldnt(self):
        def popen(argv, **kw):
            raise FileNotFoundError("flatpak-spawn")
        self.assertFalse(selfupdate.reopen(popen=popen))


class OwnVersion(unittest.TestCase):
    def test_it_reads_what_the_build_wrote(self):
        self.assertIn("printf 'VERSION=%s\\nCOMMIT=%s\\n'", (REPO / "installer/flatpak/build.sh").read_text())
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d, "VERSION")
            p.write_text("VERSION=0.3.0-exp.26\nCOMMIT=71e88840a4c82787ed6ea0a46f029e9c9a4f9beb\n")
            self.assertEqual(selfupdate.own_version(p), "0.3.0-exp.26")
            self.assertEqual(selfupdate.own_version(pathlib.Path(d, "none")), "")
        self.assertEqual(selfupdate.own_version(), "")   # the repo has none: no update from a checkout


if __name__ == "__main__":
    unittest.main()
