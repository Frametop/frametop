#!/usr/bin/env python3
"""The avatar's row on Frametop Settings' Together page (together/roomavatar.py,
roompage.avatar, ft_together_settings.py, TogetherPages.qml), offline: what the worn avatar's
file and ft-together's `looks` say read into what the row says, and the ask before a model is
first sent. The file is marked only after its person said yes, by the command the page runs: a
made-up one here, and the built ft-model-import where there is one. The rest of the page is
test_roompage.py's.

    python3 together/tests/test_roomavatar.py
"""
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "avatar" / "tests"))
import roomavatar  # noqa: E402
import roompage  # noqa: E402
from pagekit import ADA, PageCase, page  # noqa: E402

if page is not None:
    from PySide6.QtCore import QMetaObject

import bodyfixtures  # noqa: E402  (avatar/tests: a made-up person)
import fixtures  # noqa: E402
from importcase import ImportCase, ftmodel  # noqa: E402

# What ft-model-import --allow-share does, as far as the page can tell: the file's mark is set
# and the lines are printed.
MARK = """import os
import sys
path = sys.argv[1]
data = open(path, "rb").read()
at = data.index(bytes([5, 1, 0, 0]))
open(path + ".part", "wb").write(data[:at + 3] + bytes([1]) + data[at + 4:])
os.replace(path + ".part", path)
print("ok\\nname Fox\\nformat gltf\\nshare yes")
"""


def model_file(path, name="Fox", form="gltf", may_share=False, body=True):
    """A model file as far as Settings reads one: what it is, and whether it has a body."""
    def chunk(tag, data):
        return tag + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)
    info = b"".join(struct.pack("<BH", key, len(value)) + value
                    for key, value in ((1, name.encode()), (4, form.encode()), (5, b"\x01" if may_share else b"\x00")))
    path.write_bytes(b"FTMD" + struct.pack("<I", 1) + chunk(b"INFO", info) + (chunk(b"BODY", b"\0" * 8) if body else b""))


def worn(**facts):
    return dict({"name": "fox", "title": "Fox", "path": "/x/fox.ftmodel", "body": True, "may_share": False, "vrm": False}, **facts)


class Rules(unittest.TestCase):
    def test_what_the_row_says(self):
        abstract = "The others see an abstract figure"
        row = roompage.avatar
        self.assertEqual(row(None, None), {"text": abstract + ": you have no avatar on (Appearance)", "allow": False})
        self.assertEqual(row(worn(body=False), None), {"text": abstract + ": Fox isn't a whole avatar", "allow": False})
        # a model that can't say whether it may be passed on: its person can
        self.assertEqual(row(worn(), None), {"text": abstract + ": Fox stays on this headset", "allow": True})
        self.assertEqual(row(worn(), {"wear": "fox", "sent": False, "why": "that avatar isn't marked as one the others may have"}),
                         {"text": abstract + ": Fox stays on this headset", "allow": True})
        # a VRM said so itself: nobody is asked
        self.assertEqual(row(worn(vrm=True), None), {"text": abstract + ": Fox's licence doesn't let it be passed on", "allow": False})
        marked = worn(may_share=True)
        self.assertEqual(row(marked, None), {"text": "The others will see you as Fox", "allow": False})
        self.assertEqual(row(marked, {"wear": "wolf", "sent": True, "why": ""})["text"], "The others will see you as Fox")
        self.assertEqual(row(marked, {"wear": "fox", "sent": True, "why": ""}), {"text": "The others see you as Fox", "allow": False})
        self.assertEqual(row(marked, {"wear": "fox", "sent": False, "why": "that avatar's file is too large to send"}),
                         {"text": abstract + ": that avatar's file is too large to send", "allow": False})
        self.assertEqual(row(marked, {"wear": "fox", "sent": False})["text"], abstract + ": your avatar can not be sent")
        self.assertEqual(row(marked, {"wear": "fox", "sent": False, "why": 7})["text"], abstract + ": 7")

    def test_what_the_command_said(self):
        self.assertEqual(roomavatar.allowed(0, "ok\nname Fox\nformat gltf\nshare yes\n"), (True, "The people in your rooms may have Fox now"))
        self.assertEqual(roomavatar.allowed(1, "error it came from a VRM whose licence doesn't let it be passed on\n"),
                         (False, "Your avatar stays on this headset: it came from a VRM whose licence doesn't let it be passed on"))
        for code, out in ((1, ""), (0, ""), (0, "ok\nshare no\n"), (1, "ok\nshare yes\n"), (0, "junk")):
            self.assertEqual(roomavatar.allowed(code, out), (False, "Your avatar stays on this headset: the importer stopped"), (code, out))
        self.assertEqual(roomavatar.allow_command("/x/fox.ftmodel")[1:], ["--allow-share", "/x/fox.ftmodel"])


class Worn(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.conf = self.dir / "frametop.conf"
        self.worn = roomavatar.Worn(conf=str(self.conf), folder=str(self.dir))

    def wear(self, name):
        self.conf.write_text(f"HANDS_VIEW_MODEL=robot\nAVATAR_MODEL={name}\n")

    def test_no_avatar(self):
        self.assertIsNone(self.worn.look())   # no frametop.conf
        for name in ("", "''", "Fox", "../fox", "/etc/passwd", "a" * 32, "fox.ftmodel", "'a b'"):
            self.wear(name)
            self.assertIsNone(self.worn.look(), name)

    def test_what_its_file_says(self):
        self.wear("fox")
        path = self.dir / "fox.ftmodel"
        gone = self.worn.look()
        self.assertEqual(gone, {"name": "fox", "title": "fox", "path": str(path), "body": False, "may_share": False, "vrm": False})
        model_file(path)
        self.assertEqual(self.worn.look(), {"name": "fox", "title": "Fox", "path": str(path), "body": True, "may_share": False, "vrm": False})
        for form, vrm in (("vrm 0", True), ("vrm 1", True), ("fbx", False)):
            model_file(self.dir / "next", form=form, may_share=True)
            os.replace(self.dir / "next", path)
            got = self.worn.look()
            self.assertEqual((got["may_share"], got["vrm"]), (True, vrm), form)
        model_file(self.dir / "next", body=False)
        os.replace(self.dir / "next", path)
        self.assertFalse(self.worn.look()["body"])   # hands
        path.write_bytes(b"FTMD junk that is no model")
        self.assertEqual(self.worn.look(), dict(gone))
        model_file(self.dir / "wolf.ftmodel", name="")
        self.wear("wolf")
        self.assertEqual(self.worn.look()["title"], "wolf")

    def test_the_file_is_read_once_until_it_changes(self):
        self.wear("fox")
        model_file(self.dir / "fox.ftmodel")
        reads, real = [], ftmodel.read
        roomavatar.ftmodel.read = lambda *a, **k: (reads.append(a), real(*a, **k))[1]
        self.addCleanup(setattr, roomavatar.ftmodel, "read", real)
        first = self.worn.look()
        first["title"] = "changed by the caller"
        for _ in range(3):
            self.assertEqual(self.worn.look()["title"], "Fox")
        self.assertEqual(len(reads), 1)
        model_file(self.dir / "next", may_share=True)
        os.replace(self.dir / "next", self.dir / "fox.ftmodel")
        self.assertTrue(self.worn.look()["may_share"])
        self.assertEqual(len(reads), 2)


class Row(PageCase):
    def room(self, name="fox", **file):
        """In a room, with an avatar on. The command that marks its file is a made-up one."""
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA]
        self.wear(name, **file)
        self.ran = []
        old = roomavatar.allow_command
        roomavatar.allow_command = lambda path: (self.ran.append(path), self.command)[1]
        self.addCleanup(setattr, roomavatar, "allow_command", old)

    def wear(self, name, **file):
        self.conf.write_text(f"AVATAR_MODEL={name}\n")
        self.path = self.avatars / f"{name}.ftmodel"
        model_file(self.path, **file)
        self.command = [sys.executable, "-c", MARK, str(self.path)]

    def test_the_ask_before_a_model_is_first_sent(self):
        self.room()
        self.assertTrue(self.fresh(lambda: self.b.avatarAllow))
        self.assertEqual(self.b.avatarText, "The others see an abstract figure: Fox stays on this headset")
        self.assertFalse(ftmodel.read(self.path, hands=False).may_share, "nothing is marked until its person says so")
        self.b.allowAvatar()
        self.assertFalse(self.b.avatarAllow, "not twice at once")
        self.b.allowAvatar()
        self.assertTrue(self.until(lambda: self.said == [("The people in your rooms may have Fox now", False)]))
        self.assertEqual(self.ran, [str(self.path)])
        self.assertTrue(self.fresh(lambda: self.b.avatarText == "The others will see you as Fox"))
        self.assertFalse(self.b.avatarAllow)
        self.together.looks = {"wear": "fox", "sent": True, "why": "", "people": []}   # ft-together read the file again
        self.assertTrue(self.fresh(lambda: self.b.avatarText == "The others see you as Fox"))
        self.b.allowAvatar()
        self.assertEqual(self.ran, [str(self.path)], "a marked model isn't marked again")
        self.assertEqual(self.together.asked, [], "the page asks ft-together about the avatar, and tells it nothing")

    def test_a_command_that_fails(self):
        self.room()
        self.assertTrue(self.fresh(lambda: self.b.avatarAllow))
        self.command = [sys.executable, "-c", "print('error the file is damaged'); raise SystemExit(1)"]
        self.b.allowAvatar()
        self.assertTrue(self.until(lambda: self.said == [("Your avatar stays on this headset: the file is damaged", True)]))
        self.command = [str(self.avatars / "no-such-program")]
        self.b.allowAvatar()
        self.assertTrue(self.until(lambda: len(self.said) == 2))
        self.assertEqual(self.said[1], ("Your avatar stays on this headset: the importer stopped", True))
        self.assertTrue(self.fresh(lambda: self.b.avatarAllow), "it can be asked again")

    def test_nothing_to_ask(self):
        self.room()
        for name, file, text in (("vrm", {"form": "vrm 1"}, "The others see an abstract figure: Fox's licence doesn't let it be passed on"),
                                 ("hands", {"body": False}, "The others see an abstract figure: Fox isn't a whole avatar"),
                                 ("free", {"form": "vrm 0", "may_share": True}, "The others will see you as Fox")):
            with self.subTest(name):
                self.wear(name, **file)
                self.assertTrue(self.fresh(lambda: self.b.avatarText == text), self.b.avatarText)
                self.assertFalse(self.b.avatarAllow)
                self.b.allowAvatar()
                self.assertEqual(self.ran, [])
        self.conf.write_text("AVATAR_MODEL=\n")
        self.assertTrue(self.fresh(lambda: self.b.avatarText == "The others see an abstract figure: you have no avatar on (Appearance)"))
        self.together.looks = {"wear": "free", "sent": False, "why": "that avatar's file is too large to send", "people": []}
        self.conf.write_text("AVATAR_MODEL=free\n")
        self.assertTrue(self.fresh(lambda: self.b.avatarText == "The others see an abstract figure: that avatar's file is too large to send"))

    def test_the_row_on_the_page(self):
        """The row and its dialog: the file is marked after a yes, and not after a cancel."""
        self.room()
        _, found, warnings = self.window()
        self.assertTrue(self.until(lambda: found("allowAvatarButton") is not None and found("allowAvatarButton").property("visible")))
        self.assertEqual(found("avatarLabel").property("text"), "The others see an abstract figure: Fox stays on this headset")
        dialog = found("allowDialog")
        self.assertFalse(dialog.property("visible"))
        found("allowAvatarButton").clicked.emit()
        self.assertTrue(self.until(lambda: dialog.property("visible")))
        self.assertEqual(self.ran, [], "opening the ask marks nothing")
        QMetaObject.invokeMethod(dialog, "close")
        self.assertTrue(self.until(lambda: not dialog.property("visible")))
        self.assertEqual(self.ran, [])
        found("allowAvatarButton").clicked.emit()
        self.assertTrue(self.until(lambda: dialog.property("visible")))
        QMetaObject.invokeMethod(found("allowAction"), "trigger")
        self.assertTrue(self.until(lambda: self.ran == [str(self.path)] and not dialog.property("visible")))
        self.assertTrue(self.until(lambda: found("avatarLabel").property("text") == "The others will see you as Fox"
                                   and not found("allowAvatarButton").property("visible")))
        self.assertTrue(ftmodel.read(self.path, hands=False).may_share)
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])


@unittest.skipIf(page is None, "no PySide6 here")
class Imported(ImportCase, PageCase):
    """The real command on a model the importer made."""

    def test_an_imported_avatar_is_marked_by_the_page(self):
        made, said = self.imported("person.glb", fixtures.glb(bodyfixtures.person("unity", extras=False)), "--name", "person")
        self.assertFalse(made.may_share)
        path = self.avatars / "person.ftmodel"
        shutil.copy(said["file"], path)
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA]
        self.conf.write_text("AVATAR_MODEL=person\n")
        self.assertTrue(self.fresh(lambda: self.b.avatarAllow))
        self.b.allowAvatar()
        self.assertTrue(self.until(lambda: len(self.said) == 1, 20), self.said)
        self.assertFalse(self.said[0][1], self.said)
        after = ftmodel.read(path, body=True)
        self.assertTrue(after.may_share and after.has_body)
        self.assertTrue(self.fresh(lambda: self.b.avatarText.startswith("The others will see you as ")))
        self.assertFalse(self.b.avatarAllow)


if __name__ == "__main__":
    unittest.main()
