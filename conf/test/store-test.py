#!/usr/bin/env python3
"""conf/ft_store.py: a file that doesn't read is kept, writers take turns, a crash mid-write
leaves the old file (DEE-300). Everything happens in a temp folder."""
import contextlib
import io
import json
import multiprocessing
import os
import stat
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "conf"))
import ft_store  # noqa: E402


def bump(path, key, times):
    """In a process of its own: add one to data[key], `times` times, each under the lock."""
    for _ in range(times):
        ft_store.update_json(path, lambda d: d.update({key: d.get(key, 0) + 1}), {})


def crash_before_rename(path):
    """In a process of its own: die (no cleanup at all) after the temp file is written."""
    os.replace = lambda *a: os._exit(9)
    ft_store.write_json(path, {"new": True})


def hold_lock(path, ready, done):
    with ft_store.locked(path):
        ready.set()
        done.wait(30)


class Store(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ft-store-test.")
        self.path = os.path.join(self.dir, "state.json")
        self.ctx = multiprocessing.get_context("fork")

    def write(self, text):
        with open(self.path, "w") as f:
            f.write(text)

    def read(self):
        with open(self.path) as f:
            return f.read()

    def text_of(self, name):
        with open(os.path.join(self.dir, name)) as f:
            return f.read()

    def copies(self):
        return sorted(p for p in os.listdir(self.dir) if p.startswith("state.json.bad-"))

    def test_missing_file_is_the_default(self):
        self.assertEqual(ft_store.load_json_strict(self.path, {"a": 1}), {"a": 1})
        self.assertEqual(ft_store.update_json(self.path, lambda d: d.update(b=2), {}), {"b": 2})
        self.assertEqual(json.loads(self.read()), {"b": 2})

    def test_bad_file_is_kept_with_one_copy(self):
        for text in ('{"a": 1,}', "", "[1, 2]"):
            with self.subTest(text=text):
                self.write(text)
                for _ in range(3):
                    with self.assertRaisesRegex(ft_store.StoreError, "won't save over it .a copy: "):
                        ft_store.update_json(self.path, lambda d: d.update(a=2), {}, ft_store.is_object)
                self.assertEqual(self.read(), text)
                kept = [c for c in self.copies() if self.text_of(c) == text]
                self.assertEqual(len(kept), 1)

    def test_lenient_reader_warns_once_and_gives_the_default(self):
        self.write("not json")
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            for _ in range(3):
                self.assertEqual(ft_store.load_json(self.path, {"d": 1}, who="tester"), {"d": 1})
        self.assertEqual(err.getvalue().count("tester: "), 1)
        self.assertEqual(self.read(), "not json")

    def test_check_gives_the_reason(self):
        self.write('{"a": 1}')
        with self.assertRaisesRegex(ft_store.StoreError, "no b"):
            ft_store.load_json_strict(self.path, {}, lambda d: None if "b" in d else "no b")

    def test_two_writers_both_land(self):
        procs = [self.ctx.Process(target=bump, args=(self.path, key, 40)) for key in ("a", "b", "a")]
        for p in procs:
            p.start()
        for p in procs:
            p.join(60)
            self.assertEqual(p.exitcode, 0)
        self.assertEqual(json.loads(self.read()), {"a": 80, "b": 40})

    def test_crash_mid_write_keeps_the_old_file(self):
        ft_store.write_json(self.path, {"old": True})
        p = self.ctx.Process(target=crash_before_rename, args=(self.path,))
        p.start()
        p.join(30)
        self.assertEqual(p.exitcode, 9)
        self.assertEqual(json.loads(self.read()), {"old": True})
        # the dead writer's temp file is left, under a name no reader opens
        self.assertTrue(all(n == "state.json" or (n.startswith(".state.json.") and n.endswith(".tmp"))
                            for n in os.listdir(self.dir)))

    def test_failed_write_cleans_up_and_keeps_the_old_file(self):
        ft_store.write_json(self.path, {"old": True})
        real = os.replace
        os.replace = lambda *a: (_ for _ in ()).throw(OSError("disk gone"))
        try:
            with self.assertRaises(OSError):
                ft_store.update_json(self.path, lambda d: {"new": True}, {})
        finally:
            os.replace = real
        self.assertEqual(json.loads(self.read()), {"old": True})
        self.assertEqual(sorted(os.listdir(self.dir)), [".state.json.lock", "state.json"])

    def test_a_held_lock_refuses_after_the_timeout(self):
        ft_store.write_json(self.path, {"old": True})
        ready, done = self.ctx.Event(), self.ctx.Event()
        p = self.ctx.Process(target=hold_lock, args=(self.path, ready, done))
        p.start()
        try:
            self.assertTrue(ready.wait(30))
            with self.assertRaisesRegex(ft_store.StoreError, "locked for 0.2 s; not saved"):
                ft_store.update_json(self.path, lambda d: {"new": True}, {}, timeout=0.2)
        finally:
            done.set()
            p.join(30)
        self.assertEqual(json.loads(self.read()), {"old": True})

    def test_lock_is_not_taken_twice(self):
        with ft_store.locked(self.path):
            with self.assertRaisesRegex(ft_store.StoreError, "already being saved"):
                ft_store.update_json(self.path, lambda d: d, {})

    def test_format_mode_and_symlink(self):
        target = os.path.join(self.dir, "real.json")
        ft_store.write_json(target, {"a": "é"}, indent=1, newline=False, ensure_ascii=False)
        with open(target, encoding="utf-8") as f:
            self.assertEqual(f.read(), '{\n "a": "é"\n}')
        os.chmod(target, 0o600)
        os.symlink(target, self.path)
        ft_store.write_json(self.path, {"b": 1})
        self.assertTrue(os.path.islink(self.path))  # followed, not replaced
        self.assertEqual(json.loads(self.read()), {"b": 1})
        self.assertEqual(stat.S_IMODE(os.stat(target).st_mode), 0o600)


if __name__ == "__main__":
    unittest.main()
