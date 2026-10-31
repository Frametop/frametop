"""frametop-layout.json: one that doesn't read is kept, never saved over, and writers take turns
without losing each other's changes (DEE-296)."""
import contextlib
import importlib.util
import io
import json
import multiprocessing
import os
import pathlib
import tempfile
import unittest


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("layout_file_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


def bump(folder, key, times):
    """In a process of its own: load, add one to layout[key], save; `times` times."""
    m = load_layout_module(pathlib.Path(folder))
    for _ in range(times):
        layout = m.load_layout()
        layout[key] = layout.get(key, 0) + 1
        m.save_layout(layout)


class LayoutFile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.m = load_layout_module(self.dir)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text):
        pathlib.Path(self.m.LAYOUT_PATH).write_text(text)

    def saved(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)

    def backups(self):
        return sorted(p for p in os.listdir(self.dir) if p.startswith("layout.json.bad-"))

    def check_kept(self, text):
        self.write(text)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            layout = self.m.load_layout()
            layout2 = self.m.load_layout()
        self.assertEqual(layout, layout2)
        self.assertEqual(layout["preset"], self.m.DEFAULTS["preset"])  # the defaults, said once
        self.assertEqual(err.getvalue().count("won't save over it"), 1)
        layout["mode"] = "flat"
        with self.assertRaisesRegex(self.m.LayoutFileError, "won't save over it"):
            self.m.save_layout(layout)
        with self.assertRaises(self.m.LayoutFileError):
            self.m.save_layout({"mode": "flat"})  # a plain dict is refused too
        self.assertEqual(pathlib.Path(self.m.LAYOUT_PATH).read_text(), text)
        self.assertEqual(len(self.backups()), 1)  # one copy, however often it's tried
        self.assertEqual((self.dir / self.backups()[0]).read_text(), text)

    def test_bad_json_is_kept(self):
        self.check_kept('{"mode": "flat",}')

    def test_top_level_list_is_kept(self):
        self.check_kept('[{"mode": "flat"}]')

    def test_preset_not_an_object_is_kept(self):
        self.check_kept('{"preset": [1, 2]}')

    def test_fixed_file_saves_again(self):
        self.write("not json")
        with contextlib.redirect_stderr(io.StringIO()):
            self.m.load_layout()
        self.write('{"mode": "flat"}')
        layout = self.m.load_layout()
        layout["primary"] = 2
        self.m.save_layout(layout)
        self.assertEqual((self.saved()["mode"], self.saved()["primary"]), ("flat", 2))

    def test_two_editors_keep_each_others_changes(self):
        self.m.save_layout({"mode": "flat"})
        a, b = self.m.load_layout(), self.m.load_layout()
        a["primary"] = 2
        b["auto"] = True
        self.m.save_layout(a)
        self.m.save_layout(b)  # b loaded before a saved: a's primary stays
        self.assertEqual((self.saved()["primary"], self.saved()["auto"], self.saved()["mode"]), (2, True, "flat"))
        b.pop("auto")
        self.m.save_layout(b)  # a key b removes goes; a's stays
        self.assertNotIn("auto", self.saved())
        self.assertEqual(self.saved()["primary"], 2)

    def test_same_key_last_save_wins(self):
        a, b = self.m.load_layout(), self.m.load_layout()
        a["primary"], b["primary"] = 2, 3
        self.m.save_layout(a)
        self.m.save_layout(b)
        self.assertEqual(self.saved()["primary"], 3)

    def test_writers_in_two_processes_both_land(self):
        self.m.save_layout({"mode": "flat"})
        ctx = multiprocessing.get_context("fork")
        procs = [ctx.Process(target=bump, args=(self.tmp.name, key, 25)) for key in ("a", "b")]
        for p in procs:
            p.start()
        for p in procs:
            p.join(60)
            self.assertEqual(p.exitcode, 0)
        self.assertEqual((self.saved()["a"], self.saved()["b"], self.saved()["mode"]), (25, 25, "flat"))

    def test_no_temp_file_left_and_a_failed_write_keeps_the_old_file(self):
        self.m.save_layout({"mode": "flat"})
        layout = self.m.load_layout()
        layout["mode"] = "orbit"
        real = os.replace

        def crash(*args):
            raise OSError("disk gone")
        self.m.ft_conf.os.replace = crash
        try:
            with self.assertRaises(OSError):
                self.m.save_layout(layout)
        finally:
            self.m.ft_conf.os.replace = real
        self.assertEqual(self.saved()["mode"], "flat")
        self.assertEqual(sorted(os.listdir(self.dir)), [".layout.json.lock", "layout.json"])


if __name__ == "__main__":
    unittest.main()
