#!/usr/bin/env python3
"""ft-model-import --allow-share (avatar/sharemark.h): a model imported before is marked as one
the people in a room may have, in its own file, and is otherwise the file it was. A VRM's
licence still decides, and a file that isn't a model is left alone.

    python3 avatar/tests/test_allow_share.py
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bodyfixtures  # noqa: E402
import fixtures  # noqa: E402
from importcase import TOOL, ImportCase, ftmodel  # noqa: E402


def chunks(path):
    return list(ftmodel._chunks(path))


class AllowShareTest(ImportCase):
    def allow(self, path):
        """(exit status, {key: value} of what it printed)."""
        done = subprocess.run([str(TOOL), '--allow-share', str(path)], capture_output=True, text=True, timeout=120)
        return done.returncode, dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line), done.stdout

    def made(self, name, data, *args):
        """A model imported from a made-up file: its path."""
        model, said = self.imported(name, data, '--name', Path(name).stem, *args)
        return Path(said['file'])

    def test_an_avatar_is_marked_and_is_otherwise_the_file_it_was(self):
        for what, data in (('person', fixtures.glb(bodyfixtures.person('unity', extras=False))), ('hands', fixtures.glb(fixtures.make())),
                           ('boxed', fixtures.fbx(fixtures.make('mixamo')))):
            with self.subTest(what):
                path = self.made(f'{what}.{"fbx" if what == "boxed" else "glb"}', data)
                before, was = ftmodel.read(path), chunks(path)
                self.assertFalse(before.may_share)
                status, said, out = self.allow(path)
                self.assertEqual(status, 0, out)
                self.assertEqual((said['name'], said['format'], said['share']), (before.name, before.format, 'yes'))
                self.assertTrue(out.startswith('ok\n'))
                after, now = ftmodel.read(path), chunks(path)
                self.assertTrue(after.may_share)
                self.assertEqual((after.name, after.licence, after.author, after.format, after.has_body),
                                 (before.name, before.licence, before.author, before.format, before.has_body))
                self.assertEqual([tag for tag, _ in now], [tag for tag, _ in was])
                self.assertEqual([c for c in now if c[0] != b'INFO'], [c for c in was if c[0] != b'INFO'])   # hands, body, pictures, shapes
                self.assertEqual(sorted(p.name for p in path.parent.iterdir() if p.name.startswith(what)), [path.name])   # nothing left beside it
                # again: it is marked already, and the file isn't written
                marked = path.read_bytes()
                status, said, out = self.allow(path)
                self.assertEqual((status, said['share']), (0, 'yes'), out)
                self.assertEqual(path.read_bytes(), marked)

    def test_a_vrm_licence_that_forbids_passing_on_still_decides(self):
        for version, meta in ((0, {'title': 'Mine', 'licenseName': 'Redistribution_Prohibited'}), (1, {'name': 'Mine', 'allowRedistribution': False})):
            with self.subTest(vrm=version):
                path = self.made(f'private{version}.vrm', fixtures.glb(fixtures.make('opaque'), vrm=version, meta=meta))
                was = path.read_bytes()
                status, said, out = self.allow(path)
                self.assertEqual(status, 1, out)
                self.assertIn('licence', said['error'])
                self.assertEqual(path.read_bytes(), was)
                self.assertFalse(ftmodel.read(path).may_share)

    def test_a_vrm_that_may_be_passed_on_is_marked_already(self):
        path = self.made('free.vrm', fixtures.glb(fixtures.make('opaque'), vrm=0, meta={'title': 'Free', 'licenseName': 'CC0'}))
        was = path.read_bytes()
        status, said, out = self.allow(path)
        self.assertEqual((status, said['share']), (0, 'yes'), out)
        self.assertEqual(path.read_bytes(), was)

    def test_a_file_that_is_not_a_model_is_refused_and_left(self):
        good = self.made('whole.glb', fixtures.glb(bodyfixtures.person('unity', extras=False))).read_bytes()
        for what, data in (('empty', b''), ('junk', b'not a model at all'), ('source', fixtures.glb(fixtures.make())), ('cut', good[:len(good) // 2]),
                           ('longer', good + b'\0' * 3), ('header', good[:8])):
            with self.subTest(what):
                path = self.source(f'{what}.ftmodel', data)
                status, said, out = self.allow(path)
                self.assertEqual(status, 1, out)
                self.assertTrue(said['error'])
                self.assertEqual(path.read_bytes(), data)
        status, said, out = self.allow(self.dir / 'none.ftmodel')
        self.assertEqual(status, 1, out)
        self.assertFalse((self.dir / 'none.ftmodel').exists())

    def test_usage(self):
        for args in (['--allow-share'], ['--allow-share', 'a.ftmodel', 'b.ftmodel'], ['--allow-share', 'a.ftmodel', '--check']):
            with self.subTest(args=args):
                done = subprocess.run([str(TOOL), *args], capture_output=True, text=True, timeout=120)
                self.assertNotEqual(done.returncode, 0)
                self.assertNotIn('ok', done.stdout.split())


if __name__ == '__main__':
    unittest.main()
