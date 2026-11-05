"""Display Settings' stereo wallpaper (DEE-317, display-settings/stereo_wallpaper.py): pictures to
the PPMs ft-screens reads, the Plasma scripts (run in a QJSEngine against fake desktops), the
host CLI's order of steps, and the backend's slots. HOME is a temp folder; ft-screens and
Plasma are recorded, not asked. Needs PySide6 (the image's python3).

  python3 display-settings/tests/test_stereo_wallpaper.py
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-settings-wall-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import ft_display_settings as fds  # noqa: E402
import stereo_wallpaper as sw  # noqa: E402
from PySide6.QtCore import QUrl  # noqa: E402
from PySide6.QtGui import QColor, QGuiApplication, QImage  # noqa: E402
from PySide6.QtQml import QJSEngine, QQmlApplicationEngine  # noqa: E402


def picture(path, w, h, left, right=None):
    """A w x h picture, `left` colour in its left half and `right` in its right."""
    im = QImage(w, h, QImage.Format_RGB32)
    for x in range(w):
        for y in range(h):
            im.setPixelColor(x, y, QColor(*(left if right is None or x < w // 2 else right)))
    assert im.save(path)
    return path


def ppm_parts(path):
    data = Path(path).read_bytes()
    header, _, rest = data.partition(b"\n255\n")
    magic, size = header.split(b"\n")
    w, h = map(int, size.split())
    return magic, w, h, rest


class Pictures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.tmp = tempfile.mkdtemp()

    def test_side_by_side_splits_left_eye_left(self):
        src = picture(os.path.join(self.tmp, "sbs.png"), 8, 4, (255, 0, 0), (0, 0, 255))
        self.assertEqual(sw.prepare(src), "")
        self.assertTrue(sw.ready())
        lp, rp = sw.paths()
        magic, w, h, px = ppm_parts(lp)
        self.assertEqual((magic, w, h, len(px)), (b"P6", 4, 4, 4 * 4 * 3))
        self.assertEqual(px[:3], b"\xff\x00\x00")
        self.assertEqual(ppm_parts(rp)[3][:3], b"\x00\x00\xff")

    def test_pair_right_scaled_to_left(self):
        a = picture(os.path.join(self.tmp, "l.png"), 6, 4, (0, 255, 0))
        b = picture(os.path.join(self.tmp, "r.png"), 3, 2, (0, 0, 255))
        self.assertEqual(sw.prepare(a, b), "")
        lp, rp = sw.paths()
        self.assertEqual(ppm_parts(rp)[1:3], (6, 4))
        self.assertEqual(ppm_parts(lp)[3][:3], b"\x00\xff\x00")

    def test_rows_without_padding(self):
        # 3 pixels = 9 bytes a row; QImage pads its rows to 4 bytes
        src = picture(os.path.join(self.tmp, "odd.png"), 3, 3, (10, 20, 30))
        data = sw.ppm(QImage(src))
        self.assertTrue(data.startswith(b"P6\n3 3\n255\n"))
        self.assertEqual(data[len(b"P6\n3 3\n255\n"):], b"\x0a\x14\x1e" * 9)

    def test_big_pictures_scaled_down(self):
        im = QImage(sw.MAX_SIDE * 2, 100, QImage.Format_RGB32)
        im.fill(QColor(1, 2, 3))
        data = sw.ppm(im)
        self.assertTrue(data.startswith(b"P6\n%d 50\n255\n" % sw.MAX_SIDE))

    def test_bad_files_write_nothing(self):
        lp, _ = sw.paths()
        before = Path(lp).read_bytes() if os.path.exists(lp) else None
        bad = os.path.join(self.tmp, "not-a-picture.png")
        Path(bad).write_text("hello")
        self.assertIn("Couldn't read", sw.prepare(bad))
        good = picture(os.path.join(self.tmp, "g.png"), 4, 4, (1, 1, 1))
        self.assertIn("Couldn't read", sw.prepare(good, bad))
        self.assertEqual(Path(lp).read_bytes() if os.path.exists(lp) else None, before)


# Plasma's desktop scripting, enough of it: desktops() with ids, wallpaperPlugin, config groups.
FAKE = """
function Desk(id, plugin, image, color) {
    this.id = id; this.wallpaperPlugin = plugin; this.cfg = {}; this.currentConfigGroup = [];
    this.cfg["Wallpaper/org.kde.image/General/Image"] = image;
    this.cfg["Wallpaper/org.kde.color/General/Color"] = color;
}
Desk.prototype.readConfig = function (k, d) { var v = this.cfg[this.currentConfigGroup.join("/") + "/" + k]; return v === undefined ? d : v; };
Desk.prototype.writeConfig = function (k, v) { this.cfg[this.currentConfigGroup.join("/") + "/" + k] = v; };
var printed = [];
function print(s) { printed.push(s); }
"""


def run_js(desks, script):
    """Runs `script` against fake desktops [(id, plugin, image, color)]; (desktops after, printed)."""
    e = QJSEngine()
    setup = FAKE + "var D = [" + ",".join(f"new Desk({i}, {json.dumps(p)}, {json.dumps(im)}, {json.dumps(c)})"
                                          for i, p, im, c in desks) + "]; function desktops() { return D; }"
    for part in (setup, script):
        r = e.evaluate(part)
        if r.isError():
            raise AssertionError(r.toString())
    after = json.loads(e.evaluate("JSON.stringify(D.map(function (d) { return [d.id, d.wallpaperPlugin, "
                                  "d.cfg['Wallpaper/org.kde.image/General/Image'], "
                                  "d.cfg['Wallpaper/org.kde.color/General/Color']]; }))").toString())
    return after, json.loads(e.evaluate("JSON.stringify(printed)").toString())


class PlasmaScripts(unittest.TestCase):
    def setUp(self):
        if os.path.exists(sw.saved_path()):
            sw.ft_store.write_json(sw.saved_path(), {})

    def test_on_sets_key_and_prints_before(self):
        after, printed = run_js([(1, "org.kde.image", "file:///a.jpg", ""), (2, "org.kde.color", "", "0,0,0")],
                                sw.on_script())
        self.assertEqual([a[1] for a in after], ["org.kde.color", "org.kde.color"])
        self.assertEqual([a[3] for a in after], ["255,0,255", "255,0,255"])
        before = json.loads(printed[-1])
        self.assertEqual(before["1"], {"plugin": "org.kde.image", "image": "file:///a.jpg", "color": ""})
        self.assertEqual(before["2"]["color"], "0,0,0")

    def test_round_trip(self):
        desks = [(1, "org.kde.image", "file:///a.jpg", ""), (2, "org.kde.color", "", "0,0,0")]
        _, printed = run_js(desks, sw.on_script())
        self.assertTrue(sw.remember(printed[-1]))
        keyed = [(1, "org.kde.color", "file:///a.jpg", "255,0,255"), (2, "org.kde.color", "", "255,0,255"),
                 (3, "org.kde.color", "", "#ff00ff"), (4, "org.kde.image", "file:///b.jpg", "")]
        after, printed = run_js(keyed, sw.off_script(sw.saved()))
        self.assertEqual(printed, ["ok"])
        self.assertEqual(after[0][:3], [1, "org.kde.image", "file:///a.jpg"])
        self.assertEqual(after[1][1:], ["org.kde.color", "", "0,0,0"])
        # a desktop made since, with the key colour (read back as a colour): Plasma's default picture
        self.assertEqual(after[2][1], "org.kde.image")
        # one it doesn't know and isn't keyed: left alone
        self.assertEqual(after[3][:3], [4, "org.kde.image", "file:///b.jpg"])

    def test_on_twice_keeps_the_first_wallpaper(self):
        self.assertTrue(sw.remember(json.dumps({"1": {"plugin": "org.kde.image", "image": "file:///a.jpg", "color": ""}})))
        self.assertTrue(sw.remember(json.dumps({"1": {"plugin": "org.kde.color", "image": "", "color": "#FF00FF"}})))
        self.assertEqual(sw.saved()["1"]["image"], "file:///a.jpg")

    def test_remember_refuses_other_output(self):
        self.assertFalse(sw.remember("Error: ReferenceError"))
        self.assertFalse(sw.remember("[1, 2]"))


class HostSteps(unittest.TestCase):
    def setUp(self):
        os.makedirs(os.path.dirname(sw.ft_layout.CONF_PATH), exist_ok=True)
        with open(sw.ft_layout.CONF_PATH, "w") as f:
            f.write("# mine\n")
        self.asked, self.scripts = [], []
        self.answer, self.plasma_ok = "ok wallpaper 4x4", (True, '{"1": {"plugin": "org.kde.image", "image": "x", "color": ""}}')
        for name, fn in (("ask_screens", lambda t: self.asked.append(t) or self.answer),
                         ("plasma", lambda js: self.scripts.append(js) or self.plasma_ok)):
            old = getattr(sw, name)
            setattr(sw, name, fn)
            self.addCleanup(setattr, sw, name, old)
        old = sw.ready
        sw.ready = lambda: True
        self.addCleanup(setattr, sw, "ready", old)

    def conf(self):
        return sw.ft_layout.read_conf().get("STEREO_WALLPAPER")

    def test_on_screens_then_plasma_then_conf(self):
        self.assertEqual(sw.main(["on"]), 0)
        self.assertEqual(self.asked, ["wallpaper on"])
        self.assertIn("org.kde.color", self.scripts[0])
        self.assertEqual(self.conf(), "1")
        self.assertIn("# mine", Path(sw.ft_layout.CONF_PATH).read_text())

    def test_on_screens_refuse(self):
        self.answer = "error can't open left.ppm"
        self.assertEqual(sw.main(["on"]), 1)
        self.assertEqual(self.scripts, [])   # Plasma keeps its wallpaper: no magenta
        self.assertIsNone(self.conf())

    def test_on_plasma_fails_put_back_then_screens_stop(self):
        # a timeout after Plasma took it, or output that isn't on_script's: undo Plasma first
        for answer in [(False, "timeout"), (True, "Error: TypeError")]:
            self.asked.clear(), self.scripts.clear()
            answers = [answer, (True, "ok")]
            sw.plasma = lambda js: self.scripts.append(js) or answers.pop(0)
            self.assertEqual(sw.main(["on"]), 1)
            self.assertEqual(len(self.scripts), 2)
            self.assertIn("var before = ", self.scripts[1])   # off_script
            self.assertEqual(self.asked, ["wallpaper on", "wallpaper off"])
            self.assertIsNone(self.conf())

    def test_on_plasma_and_put_back_fail_stays_on(self):
        answers = [(False, "timeout"), (False, "timeout")]
        sw.plasma = lambda js: answers.pop(0)
        self.assertEqual(sw.main(["on"]), 1)
        self.assertEqual(self.asked, ["wallpaper on"])   # ft-screens keeps drawing over the key
        self.assertEqual(self.conf(), "1")               # the switch shows on: off tries again

    def test_off_plasma_then_screens(self):
        self.plasma_ok = (True, "ok")
        self.assertEqual(sw.main(["off"]), 0)
        self.assertEqual(self.asked, ["wallpaper off"])
        self.assertEqual(self.conf(), "0")

    def test_off_plasma_fails_keeps_it_on(self):
        sw.ft_layout.ft_conf.set_value("STEREO_WALLPAPER", "1", sw.ft_layout.CONF_PATH)
        self.plasma_ok = (False, "timeout")
        self.assertEqual(sw.main(["off"]), 1)
        self.assertEqual(self.asked, [])   # the key colour would show bare otherwise
        self.assertEqual(self.conf(), "1")

    def test_reload_only_when_on(self):
        self.assertEqual(sw.main(["reload"]), 0)
        self.assertEqual(self.asked, [])
        sw.ft_layout.ft_conf.set_value("STEREO_WALLPAPER", "1", sw.ft_layout.CONF_PATH)
        self.assertEqual(sw.main(["reload"]), 0)
        self.assertEqual(self.asked, ["wallpaper on"])

    def test_usage(self):
        self.assertEqual(sw.main([]), 2)
        self.assertEqual(sw.main(["sideways"]), 2)


class BackendSlots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        os.makedirs(os.path.dirname(fds.CONF_PATH), exist_ok=True)
        with open(fds.CONF_PATH, "w") as f:
            f.write("# mine\n")
        self.b = fds.Backend()
        self.runs, self.messages = [], []
        self.b._run = lambda label, *args, argv=None: self.runs.append((label, argv))
        self.b.message.connect(lambda text, err: self.messages.append((text, err)))
        self.b._running = True
        self.tmp = tempfile.mkdtemp()

    def test_off_by_default(self):
        self.assertFalse(self.b.stereoWallpaper)

    def test_on_needs_pictures_and_the_desktop(self):
        old = sw.ready
        self.addCleanup(setattr, sw, "ready", old)
        sw.ready = lambda: False
        self.b.setStereoWallpaper(True)
        self.assertEqual(self.runs, [])
        self.assertTrue(self.messages[-1][1])
        sw.ready = lambda: True
        self.b._running = False
        self.b.setStereoWallpaper(True)
        self.assertEqual(self.runs, [])
        self.b._running = True
        self.b.setStereoWallpaper(True)
        self.assertEqual(self.runs[-1][0], "Stereo wallpaper on")
        self.assertEqual(self.runs[-1][1][-2:], [os.path.abspath(sw.__file__), "on"])
        self.b.setStereoWallpaper(False)
        self.assertEqual(self.runs[-1][1][-1], "off")

    def test_choose_reloads_when_on(self):
        src = picture(os.path.join(self.tmp, "sbs.png"), 8, 4, (255, 0, 0), (0, 0, 255))
        self.b.chooseStereoWallpaper(QUrl.fromLocalFile(src).toString(), "")
        self.assertEqual(self.runs, [])
        self.assertFalse(self.messages[-1][1])
        fds.write_conf_value("STEREO_WALLPAPER", "1")
        self.b.chooseStereoWallpaper(src, "")
        self.assertEqual(self.runs[-1][1][-1], "reload")
        self.b.chooseStereoWallpaper(os.path.join(self.tmp, "missing.png"), "")
        self.assertTrue(self.messages[-1][1])

    def test_screens_page_loads(self):
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        engine.rootContext().setContextProperty("backend", self.b)
        engine.rootContext().setContextProperty("startPage", "screens")
        qml = os.path.join(HERE, "..", "main.qml")
        engine.load(QUrl.fromLocalFile(qml))
        if not engine.rootObjects():
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        for _ in range(20):
            self.app.processEvents()
        lines = Path(qml).read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "DEE-317" in t) + 1
        last = next(i for i in range(first, len(lines)) if "id: rightDialog" in lines[i]) + 6
        mine = [e for e in errors if any(f"main.qml:{n}:" in e for n in range(first, last + 1))]
        self.assertEqual(mine, [])


if __name__ == "__main__":
    unittest.main()
