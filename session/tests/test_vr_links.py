#!/usr/bin/env python3
"""session/vr-links.sh (issue 78): OpenXR and OpenVR programs started in the Frametop
desktop find SteamVR. They look it up under the config folder, which in the desktop is
~/.config/frametop, so the session links openxr and openvr there to the user's folders
one level up. Whatever is already under those names stays as it is.

  python3 session/tests/test_vr_links.py
"""
import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
LINKS = os.path.join(HERE, "..", "vr-links.sh")
SESSION = os.path.join(HERE, "..", "frametop-session.sh")
RUNTIME = os.path.join("openxr", "1", "active_runtime.json")
REGISTRY = os.path.join("openvr", "openvrpaths.vrpath")


class VrLinks(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.user = os.path.join(self.dir.name, ".config")
        self.desktop = os.path.join(self.user, "frametop")
        os.makedirs(self.desktop)

    def write(self, config, path, text):
        os.makedirs(os.path.dirname(os.path.join(config, path)), exist_ok=True)
        with open(os.path.join(config, path), "w") as f:
            f.write(text)

    def read(self, path):
        with open(os.path.join(self.desktop, path)) as f:
            return f.read()

    def link(self):
        p = subprocess.run(["sh", LINKS, self.desktop], capture_output=True, text=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stderr

    def test_the_desktop_sees_the_users_runtime_and_registry(self):
        self.write(self.user, RUNTIME, "steamvr")
        self.write(self.user, REGISTRY, "paths")
        self.assertEqual(self.link(), "")
        self.assertEqual((self.read(RUNTIME), self.read(REGISTRY)), ("steamvr", "paths"))

    def test_the_links_are_relative_so_a_moved_home_keeps_them(self):
        self.link()
        self.assertEqual([os.readlink(os.path.join(self.desktop, n)) for n in ("openxr", "openvr")],
                         ["../openxr", "../openvr"])

    def test_a_folder_steamvr_makes_later_shows_up(self):
        self.link()
        self.assertFalse(os.path.exists(os.path.join(self.desktop, REGISTRY)))
        self.write(self.user, REGISTRY, "paths")
        self.assertEqual(self.read(REGISTRY), "paths")

    def test_a_second_start_changes_nothing(self):
        self.write(self.user, REGISTRY, "paths")
        self.link()
        self.assertEqual(self.link(), "")
        self.assertEqual(self.read(REGISTRY), "paths")
        self.assertEqual(sorted(os.listdir(self.user)), ["frametop", "openvr"])

    def test_a_folder_already_there_stays_and_is_named(self):
        self.write(self.user, REGISTRY, "paths")
        self.write(self.desktop, REGISTRY, "theirs")
        said = self.link()
        self.assertEqual(self.read(REGISTRY), "theirs")
        self.assertIn(os.path.join(self.desktop, "openvr"), said)
        self.assertNotIn(os.path.join(self.desktop, "openxr"), said)
        self.assertTrue(os.path.islink(os.path.join(self.desktop, "openxr")))

    def test_a_link_to_somewhere_else_stays(self):
        other = os.path.join(self.dir.name, "monado")
        os.mkdir(other)
        os.symlink(other, os.path.join(self.desktop, "openxr"))
        self.assertEqual(self.link(), "")
        self.assertEqual(os.readlink(os.path.join(self.desktop, "openxr")), other)

    def test_the_session_links_its_own_config_folder(self):
        with open(SESSION) as f:
            session = f.read()
        made = session.index('mkdir -p "$XDG_CONFIG_HOME"')
        linked = session.index('"$here/vr-links.sh" "$XDG_CONFIG_HOME"')
        self.assertLess(session.index("export XDG_CONFIG_HOME=$HOME/.config/frametop"), made)
        self.assertLess(made, linked)


if __name__ == "__main__":
    unittest.main()
