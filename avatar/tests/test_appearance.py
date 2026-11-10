#!/usr/bin/env python3
"""Frametop Settings' Appearance page (avatar/ft_avatar_settings.py, hand_models.py,
avatar_models.py), offline: the models ft-handview lists are shown with their pictures, a choice
is saved and sent, an import runs the importer under a name that isn't taken and switches to the
model, and a model of yours can be removed; your models that are whole avatars can be worn, and
the mirror started with the one you wear and told when it changes. ft-handview, ft-model-import
and ft-avatars are a made-up program here (FAKE below), the settings file and the folders are
temp ones, and nothing is sent to the real ft-handview or the real mirror, or started.

  python3 avatar/tests/test_appearance.py
"""
import os
import struct
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import avatar_models  # noqa: E402
import fixtures  # noqa: E402
import hand_models  # noqa: E402

try:
    from PySide6.QtCore import QCoreApplication
    import ft_avatar_settings
except ImportError as e:  # (a PC without PySide6)
    print(f"no PySide6 here: {e}")
    sys.exit(77)

# Plays the programs: `--models` lists the bundled models and the folder's files, `--thumb F`
# writes a file, and anything else is an import that writes NAME.ftmodel (or refuses "bad" files).
FAKE = textwrap.dedent('''\
    import os, sys
    args = sys.argv[1:]
    folder = os.environ.get("FT_AVATARS_DIR", "")
    if "--models" in args:
        print("asset\\tbundled\\nbabylon\\tbundled")
        for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if f.endswith(".ftmodel"):
                print(f[:-8] + "\\town")
        print("robot\\tbuilt\\ncapsules\\tbuilt")
    elif "--thumb" in args:
        open(args[args.index("--thumb") + 1], "wb").write(b"png " + os.environ.get("FT_HANDS_VIEW_MODEL", "an avatar").encode())
    else:
        source, name, to = args[0], args[args.index("--name") + 1], args[args.index("--to") + 1]
        if "bad" in os.path.basename(source):
            print("error no hands were found: the ring finger has 2 bones: 3 to 5 are read")
            sys.exit(1)
        os.makedirs(to, exist_ok=True)
        open(os.path.join(to, name + ".ftmodel"), "wb").write(open(source, "rb").read())
        print("ok " + name + "\\nfile " + os.path.join(to, name + ".ftmodel") + "\\nname My Avatar\\nshare no")
        print("left 100 triangles\\nright 100 triangles, mirrored from the left")
''')


def model_file(title, body=True):
    """A model file's bytes as far as a list of models reads them: what it says of itself, and
    whether it has a body."""
    def chunk(tag, data):
        return tag + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)

    info = b"".join(struct.pack("<BH", key, len(text)) + text for key, text in ((1, title.encode()), (3, b"Someone"), (4, b"vrm")))
    return b"FTMD" + struct.pack("<I", 1) + chunk(b"INFO", info) + (chunk(b"BODY", bytes(40)) if body else b"")


class Logic(unittest.TestCase):
    def test_names(self):
        self.assertEqual(hand_models.slug("My Hands (v2)"), "my-hands-v2")   # as ft-model-import names it (test_import.py)
        self.assertEqual(hand_models.slug("ÅÄÖ"), "model")
        self.assertEqual(hand_models.slug("x" * 40), "x" * 31)
        self.assertEqual(hand_models.slug("a" * 30 + " b"), "a" * 30)
        self.assertEqual(hand_models.free_name("glove", {"asset", "robot"}), "glove")
        self.assertEqual(hand_models.free_name("robot", {"asset", "robot"}), "robot-2")
        self.assertEqual(hand_models.free_name("robot", {"robot", "robot-2"}), "robot-3")
        self.assertEqual(len(hand_models.free_name("r" * 31, {"r" * 31})), 31)

    def test_listing(self):
        self.assertEqual(hand_models.parse_models("asset\tbundled\nmine\town\nrobot\tbuilt\nnoise\n\tx\nq\tother\n"),
                         [("asset", "bundled"), ("mine", "own"), ("robot", "built")])

    def test_folder(self):
        self.assertEqual(hand_models.folder({"FT_AVATARS_DIR": "/a", "HOME": "/h"}), "/a")
        self.assertEqual(hand_models.folder({"XDG_DATA_HOME": "/d", "HOME": "/h"}), "/d/frametop/avatars")
        self.assertEqual(hand_models.folder({"HOME": "/h"}), "/h/.local/share/frametop/avatars")

    def test_import_messages(self):
        said = hand_models.parse_import("ok glove\nfile /x/glove.ftmodel\nname My Glove\nshare no\nleft 10 triangles\n"
                                        "right 10 triangles, mirrored from the left\n")
        self.assertEqual(hand_models.import_message(said, "/dl/Glove.glb"),
                         ("Imported My Glove (right hand 10 triangles, mirrored from the left)", False))
        self.assertEqual(hand_models.import_message(hand_models.parse_import("error no hands were found\n"), "/dl/Cube.glb"),
                         ("Couldn't import Cube.glb: no hands were found", True))
        self.assertEqual(hand_models.import_message({}, "/dl/Cube.glb"), ("Couldn't import Cube.glb: the importer stopped", True))

    def test_a_models_own_words(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(hand_models.describe("robot", "built", tmp)["title"], "Robot")
            self.assertEqual(hand_models.describe("gone", "own", tmp)["title"], "gone")   # no file: its name
            self.assertTrue(hand_models.describe("gone", "own", tmp)["own"])

    def test_avatars_are_the_models_with_a_body(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(avatar_models.listed(os.path.join(tmp, "nothere")), [])
            for name, data in (("fox", model_file("Fox")), ("glove", model_file("Glove", body=False)), ("junk", b"x"), ("owl", model_file(""))):
                Path(tmp, name + ".ftmodel").write_bytes(data)
            Path(tmp, "notes.txt").write_text("x")
            self.assertEqual(avatar_models.listed(tmp), [{"name": "fox", "title": "Fox", "detail": "by Someone, VRM", "own": True},
                                                         {"name": "owl", "title": "owl", "detail": "by Someone, VRM", "own": True}])
            self.assertEqual(avatar_models.mirror_command(tmp, "fox"), [avatar_models.AVATARS, "--mirror", os.path.join(tmp, "fox.ftmodel")])


class Page(unittest.TestCase):
    def setUp(self):
        self.app = QCoreApplication.instance() or QCoreApplication([])
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        tmp = Path(self.tmp.name)
        self.conf, self.folder, self.cache = tmp / "frametop.conf", tmp / "avatars", tmp / "thumbs"
        fake = tmp / "fake.py"
        fake.write_text(FAKE)
        self.ran = []
        old = ft_avatar_settings.tool_command
        ft_avatar_settings.tool_command = lambda *cmd: (self.ran.append(cmd), [sys.executable, str(fake), *cmd[1:]])[1]
        self.addCleanup(setattr, ft_avatar_settings, "tool_command", old)
        self.started = []   # what would go on after Settings closes: not started here
        for module, name, stand_in in ((ft_avatar_settings, "start_detached", lambda argv: (self.started.append(argv), self.starts)[1]),
                                       (ft_avatar_settings, "MIRROR_UP_MS", 3600 * 1000),   # the tests ask when they want to
                                       (avatar_models, "SOCKET", "ft_avatars_test_%d" % os.getpid())):   # nobody listens there
            self.addCleanup(setattr, module, name, getattr(module, name))
            setattr(module, name, stand_in)
        self.starts = True
        self.b = ft_avatar_settings.Backend(conf=str(self.conf), folder=str(self.folder), cache=str(self.cache), avatar_cache=str(tmp / "avatar-thumbs"))
        self.sent, self.said, self.told = [], [], []
        self.b._send = self.sent.append
        self.b._tell_mirror = self.told.append
        self.b.message.connect(lambda text, error: self.said.append((text, error)))

    def settle(self, seconds=5):
        """Until the programs have run and the folder has been read (which may queue pictures to draw)."""
        end, quiet = time.monotonic() + seconds, 0
        while quiet < 3 and time.monotonic() < end:
            self.app.processEvents()
            quiet = 0 if self.b._running is not None or self.b._jobs or self.b._ipc.pending() else quiet + 1
            time.sleep(0.01)
        self.app.processEvents()
        self.assertIsNone(self.b._running)
        self.assertEqual(self.b._ipc.pending(), 0)

    def avatar(self, name, title):
        self.folder.mkdir(exist_ok=True)
        (self.folder / (name + ".ftmodel")).write_bytes(model_file(title))
        return str(self.folder / (name + ".ftmodel"))

    def names(self):
        return [m["name"] for m in self.b.models]

    def test_models_and_their_pictures(self):
        self.assertEqual(self.b.model, "asset")
        self.assertIn("robot", self.names())   # before ft-handview has answered: the ones every build has
        self.b.refresh()
        self.settle()
        self.assertEqual(self.names(), ["asset", "babylon", "robot", "capsules"])
        for m in self.b.models:
            self.assertTrue(m["thumb"].startswith("file://"), m)
            self.assertEqual(m["current"], m["name"] == "asset")
        drawn = len([c for c in self.ran if "--thumb" in c])
        self.assertEqual(drawn, 4)
        self.b.refresh()   # pictures are kept: none drawn again
        self.settle()
        self.assertEqual(len([c for c in self.ran if "--thumb" in c]), drawn)

    def test_a_choice_is_saved_and_sent(self):
        self.b.refresh()
        self.settle()
        self.b.choose("robot")
        self.assertEqual((self.b.model, self.sent), ("robot", ["model robot"]))
        self.assertIn("HANDS_VIEW_MODEL=robot", self.conf.read_text())
        self.b.choose("robot")    # no change: nothing sent
        self.b.choose("nothere")  # not a model: nothing saved
        self.assertEqual((self.b.model, self.sent), ("robot", ["model robot"]))
        self.b.setStyle("solid")
        self.b.setStyle("shiny")
        self.b.setInRoom(False)
        self.assertEqual(self.sent[1:], ["style solid", "inroom 0"])
        self.assertEqual((self.b.style, self.b.inRoom), ("solid", False))
        self.assertIn("HANDS_VIEW_ROOMVIEW=0", self.conf.read_text())

    def test_a_setting_that_names_no_model_shows_the_generic_hand(self):
        self.conf.write_text("HANDS_VIEW_MODEL=gone\nHANDS_VIEW_STYLE=odd\n")
        self.assertEqual((self.b.model, self.b.style, self.b.inRoom), ("asset", "ghost", True))

    def test_import(self):
        self.b.refresh()
        self.settle()
        source = Path(self.tmp.name) / "My Avatar.vrm"
        source.write_bytes(fixtures.glb(fixtures.make()))
        self.b.importFile(source.as_uri(), False)
        self.assertEqual(self.b.busy, "Importing My Avatar.vrm…")
        self.settle()
        self.assertEqual(self.b.busy, "")
        self.assertEqual(self.said, [("Imported My Avatar (right hand 100 triangles, mirrored from the left)", False)])
        self.assertTrue((self.folder / "my-avatar.ftmodel").exists())
        self.assertEqual(self.names(), ["asset", "babylon", "my-avatar", "robot", "capsules"])
        self.assertEqual((self.b.model, self.sent), ("my-avatar", ["model my-avatar"]))
        mine = next(m for m in self.b.models if m["name"] == "my-avatar")
        self.assertTrue(mine["own"] and mine["current"] and mine["thumb"])
        # a name one of Frametop's models has is passed over; "--may-share" only when said
        robot = Path(self.tmp.name) / "Robot.glb"
        robot.write_bytes(b"x")
        self.b.importFile(str(robot), True)
        self.settle()
        last = [c for c in self.ran if c[0] == hand_models.IMPORTER][-1]
        self.assertEqual(last[last.index("--name") + 1], "robot-2")
        self.assertIn("--may-share", last)
        self.assertNotIn("--may-share", [c for c in self.ran if c[0] == hand_models.IMPORTER][0])

    def test_an_import_that_is_refused_says_why(self):
        bad = Path(self.tmp.name) / "bad.glb"
        bad.write_bytes(b"x")
        self.b.importFile(str(bad), False)
        self.settle()
        self.assertEqual(self.said, [("Couldn't import bad.glb: no hands were found: the ring finger has 2 bones: 3 to 5 are read", True)])
        self.assertEqual((self.b.model, self.sent, self.b.busy), ("asset", [], ""))

    def test_remove(self):
        source = Path(self.tmp.name) / "glove.glb"
        source.write_bytes(b"x")
        self.b.importFile(str(source), False)
        self.settle()
        self.assertEqual(self.b.model, "glove")
        self.b.remove("asset")   # not yours: stays
        self.b.remove("glove")
        self.settle()
        self.assertFalse((self.folder / "glove.ftmodel").exists())
        self.assertEqual(self.names(), ["asset", "babylon", "robot", "capsules"])
        self.assertEqual((self.b.model, self.sent[-1]), ("asset", "model asset"))
        self.assertEqual([f for f in os.listdir(self.cache) if f.startswith("glove-")], [])

    def test_your_avatars_and_their_pictures(self):
        self.assertEqual((self.b.avatars, self.b.avatar, self.b.mirror), ([], "", False))
        fox = self.avatar("fox", "Fox")
        (self.folder / "glove.ftmodel").write_bytes(model_file("Glove", body=False))
        self.b.refresh()
        self.settle()
        self.assertEqual([(a["name"], a["title"], a["current"]) for a in self.b.avatars], [("fox", "Fox", False)])
        self.assertTrue(self.b.avatars[0]["thumb"].startswith("file://"))
        drawn = [c for c in self.ran if c[0] == avatar_models.AVATARS]
        self.assertEqual([(c[1], c[3:]) for c in drawn], [("--thumb", (fox, "--size", "256"))])
        self.b.refresh()   # its picture is kept
        self.settle()
        self.assertEqual(len([c for c in self.ran if c[0] == avatar_models.AVATARS]), 1)
        self.assertFalse(self.b.mirror)   # nobody answered on the mirror's socket

    def test_wearing_an_avatar_and_the_mirror(self):
        self.avatar("fox", "Fox")
        owl = self.avatar("owl", "Owl")
        self.b.refresh()
        self.settle()
        self.b.setMirror(True)   # nobody to show yet
        self.assertEqual((self.started, self.b.mirror), ([], False))
        self.b.wear("fox")
        self.b.wear("nobody")
        self.assertEqual((self.b.avatar, self.told), ("fox", []))
        self.assertIn("AVATAR_MODEL=fox", self.conf.read_text())
        self.assertEqual((self.b.model, self.sent), ("fox", ["model fox"]))   # its hands are yours
        self.assertEqual([a["name"] for a in self.b.avatars if a["current"]], ["fox"])
        self.b.setMirror(True)
        self.b.setMirror(True)
        self.assertEqual(len(self.started), 1)
        self.assertEqual(self.ran[-1], (avatar_models.AVATARS, "--mirror", str(self.folder / "fox.ftmodel")))
        self.assertTrue(self.b.mirror)
        self.b.wear("owl")   # the mirror is told
        self.assertEqual(self.told, ["model " + owl])
        self.b.choose("robot")   # other hands on the same body
        self.assertEqual((self.b.avatar, self.b.model), ("owl", "robot"))
        self.b.setMirror(False)
        self.assertEqual((self.told[-1], self.b.mirror, len(self.started)), ("quit", False, 1))

    def test_a_mirror_that_does_not_start_says_so(self):
        self.avatar("fox", "Fox")
        self.b.refresh()
        self.settle()
        self.b.wear("fox")
        self.starts = False
        self.b.setMirror(True)
        self.assertEqual((self.b.mirror, self.said), (False, [("Couldn't start the mirror", True)]))

    def test_a_mirror_that_does_not_come_up_says_so(self):
        self.avatar("fox", "Fox")
        self.b.refresh()
        self.settle()
        self.b.wear("fox")
        self.b.setMirror(True)
        self.assertEqual((self.b.mirror, self.said), (True, []))
        self.b._mirror_up()   # nobody answers on its socket
        self.settle()
        self.assertEqual((self.b.mirror, self.said), (False, [("The mirror didn't come up. Is SteamVR running?", True)]))
        self.b._mirror_up()   # ...and it is only said once
        self.settle()
        self.assertEqual(len(self.said), 1)

    def test_a_mirror_already_standing_shows_as_on(self):
        self.b._scanned(([], True))
        self.assertTrue(self.b.mirror)
        self.b.setMirror(False)
        self.assertEqual((self.told, self.b.mirror), (["quit"], False))

    def test_an_imported_avatar_is_worn(self):
        source = Path(self.tmp.name) / "Fox.vrm"
        source.write_bytes(model_file("Fox"))
        self.b.importFile(str(source), False)
        self.settle()
        self.assertEqual((self.b.avatar, self.b.model), ("fox", "fox"))
        glove = Path(self.tmp.name) / "glove.glb"   # hands alone: the avatar stays
        glove.write_bytes(model_file("Glove", body=False))
        self.b.importFile(str(glove), False)
        self.settle()
        self.assertEqual((self.b.avatar, self.b.model), ("fox", "glove"))

    def test_removing_the_avatar_you_wear(self):
        self.avatar("fox", "Fox")
        self.b.refresh()
        self.settle()
        self.b.wear("fox")
        self.b.setMirror(True)
        self.b.remove("fox")
        self.settle()
        self.assertEqual((self.b.avatars, self.b.avatar, self.b.mirror, self.told), ([], "", False, ["quit"]))
        self.assertEqual((self.b.model, self.sent[-1]), ("asset", "model asset"))
        self.assertFalse((self.folder / "fox.ftmodel").exists())
        self.assertNotIn("AVATAR_MODEL=fox", self.conf.read_text())


if __name__ == "__main__":
    unittest.main()
