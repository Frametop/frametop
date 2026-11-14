#!/usr/bin/env python3
"""ft-avatars' side of a shared room (avatar/room), from a pose to the pixels: someone is told
over the wire, fitted to the body of their place and drawn on their sheet to each eye, in their
place's colour, facing the way they face. Someone who sent their avatar is drawn in it, if its
file says they may pass it on; any other file is refused. And the room's modes say what they
can't do: a pose that isn't one, options that aren't theirs, and a pretended person with no
ft-together to talk to stays out of any room.

It runs the built program (screens/build.sh) and is skipped where it isn't built; the pictures
are skipped where there is no GPU to draw on, and the sent avatars where the importer isn't
built (avatar/build.sh). Nothing here starts SteamVR or opens a link.
Run: python3 avatar/tests/test_room_picture.py"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bodyfixtures  # noqa: E402
import fixtures  # noqa: E402
from avatarsprogram import TOOL, built  # noqa: E402
from importcase import TOOL as IMPORTER  # noqa: E402
from importcase import ImportCase  # noqa: E402
from pngread import read_png  # noqa: E402

# which of red, green and blue a place's colour has most of, and least (avatar/abstract)
MOST_LEAST = [(2, 0), (0, 2), (2, 1), (0, 1)]


def run(*args):
    return subprocess.run([str(TOOL), *args], capture_output=True, text=True, timeout=120)


class RoomPictureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        built(gpu=True)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def draw(self, *args, size=192):
        out = self.dir / 'person.png'
        done = run('--room-snapshot', str(out), '--size', str(size), *args)
        self.assertEqual(done.returncode, 0, done.stderr)
        return read_png(out), done.stdout

    @staticmethod
    def halves(picture):
        """What is drawn to each eye: [(x, y, (r, g, b))] of the solid pixels, x from the eye's own left edge."""
        width, height, _, rows = picture
        each = width // 2
        return [[(x, y, tuple(rows[y][(half * each + x) * 4:][:3])) for y in range(height) for x in range(each) if rows[y][(half * each + x) * 4 + 3] > 200]
                for half in range(2)]

    @staticmethod
    def visor(there):
        """How many of the pixels are the visor's: dark, and no colour to speak of."""
        return sum(max(c) < 70 and max(c) - min(c) < 25 for _, _, c in there)

    def test_someone_is_drawn_to_each_eye(self):
        picture, said = self.draw('--place', '0', '--pose', 'wave')
        width, height, channels, rows = picture
        self.assertEqual((width, height, channels), (2 * 192, 192, 4))
        self.assertIn('place 0, wave', said)
        middles = []
        for half, there in enumerate(self.halves(picture)):
            x0 = half * 192
            for x, y in ((x0, 0), (x0 + 191, 0), (x0, 191), (x0 + 191, 191)):
                self.assertEqual(rows[y][x * 4 + 3], 0, (x, y))   # clear where they aren't
            self.assertGreater(len(there), 300, half)
            self.assertLess(len(there), 0.5 * 192 * 192, half)   # a head and two hands, not a slab
            xs, ys = [x for x, _, _ in there], [y for _, y, _ in there]
            # the sheet is fitted to them: they reach most of the way across it one way or the other, and stay inside it
            self.assertGreater(max(max(xs) - min(xs), max(ys) - min(ys)), 0.45 * 192, half)
            self.assertGreater(min(xs), 0), self.assertLess(max(xs), 191)
            self.assertGreater(min(ys), 0), self.assertLess(max(ys), 191)
            self.assertGreater(self.visor(there), 30, half)   # they face the one who looks
            middles.append(sum(xs) / len(xs))
        # they have depth of their own about the sheet: the two eyes don't see the same picture
        each = 192 * 4
        self.assertNotEqual([row[:each] for row in rows], [row[each:] for row in rows])
        self.assertLess(abs(middles[0] - middles[1]), 12)   # ...but nearly: the sheet stands where they do

    def test_each_place_has_its_colour(self):
        pictures = []
        for place, (most, least) in enumerate(MOST_LEAST):
            picture, _ = self.draw('--place', str(place), '--pose', 'stand')
            coloured = [c for _, _, c in self.halves(picture)[0] if max(c) - min(c) >= 25]
            self.assertGreater(len(coloured), 200, place)
            mean = [sum(c[i] for c in coloured) / len(coloured) for i in range(3)]
            self.assertEqual((mean.index(max(mean)), mean.index(min(mean))), (most, least), (place, mean))
            pictures.append(picture[3])
        self.assertEqual(len({tuple(rows) for rows in pictures}), 4)
        self.assertEqual(self.draw('--place', '4', '--pose', 'stand')[0][3], pictures[0])   # the colours go round

    def test_they_face_the_way_they_face(self):
        front, behind = (self.halves(self.draw('--pose', 'stand', '--round', turn)[0])[0] for turn in ('0', '180'))
        self.assertGreater(self.visor(front), 30)
        self.assertLess(self.visor(behind), 0.1 * self.visor(front))   # the back of a head has no visor
        self.assertGreater(len(behind), 300)

    def test_a_pose_and_its_moment_move_them(self):
        stand, wave, later = (self.draw('--pose', pose, '--at', at)[0][3] for pose, at in (('stand', '0'), ('wave', '0'), ('wave', '0.4')))
        self.assertNotEqual(stand, wave)
        self.assertNotEqual(wave, later)
        self.assertEqual(wave, self.draw('--pose', 'wave', '--at', '0')[0][3])   # the same asked, the same drawn

    def test_from_further_away_the_sheet_is_the_same_and_they_are_too(self):
        near, far = (self.draw('--pose', 'stand', '--away', away)[0] for away in ('1.5', '6'))
        self.assertNotEqual(near[3], far[3])
        # the sheet is sized to them, not to how far off they are: about as much of it is theirs
        counts = [len(self.halves(picture)[0]) for picture in (near, far)]
        self.assertLess(abs(counts[0] - counts[1]), 0.35 * counts[0])

    def test_what_it_cannot_draw_ends_in_an_error(self):
        out = str(self.dir / 'none.png')
        done = run('--room-snapshot', out, '--pose', 'fly')
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn('no pose called fly', done.stderr)
        self.assertFalse(Path(out).exists())
        done = run('--room-snapshot', str(self.dir / 'no/such/folder.png'))
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("can't write", done.stderr)


class SentAvatarTest(ImportCase):
    """Someone in the avatar they sent: a file as ft-together keeps it for the room."""

    @classmethod
    def setUpClass(cls):
        built(gpu=True)
        super().setUpClass()
        person = cls.dir / 'person.glb'
        person.write_bytes(fixtures.glb(bodyfixtures.person('unity', extras=False)))
        hands = cls.dir / 'hands.glb'
        hands.write_bytes(fixtures.glb(fixtures.make()))
        rig = bodyfixtures.person('unity', extras=False)
        rig.shapes['aa'] = {v: (0.0, -0.06, 0.03) for v in rig.shapes['aa']}   # a mouth that opens wide enough to show from across a room
        talker = cls.dir / 'talker.glb'
        talker.write_bytes(fixtures.glb(rig))
        cls.given, cls.kept, cls.hands, cls.talker = (cls.model(source, name, *more) for source, name, more in (
            (person, 'given', ['--may-share']), (person, 'kept', []), (hands, 'hands', ['--may-share']), (talker, 'talker', ['--may-share'])))

    @classmethod
    def model(cls, source, name, *more):
        done = subprocess.run([str(IMPORTER), str(source), '--to', str(cls.dir / 'out'), '--name', name, *more], capture_output=True, text=True, timeout=120)
        said = dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line)
        assert done.returncode == 0 and said['share'] == ('yes' if more else 'no'), (done.stdout, done.stderr)
        return said['file']

    def draw(self, *args):
        out = self.dir / 'sent.png'
        done = run('--room-snapshot', str(out), '--size', '192', '--pose', 'stand', *args)
        self.assertEqual(done.returncode, 0, done.stderr)
        return read_png(out), done.stdout

    def refused(self, model, why):
        out = self.dir / 'refused.png'
        done = run('--room-snapshot', str(out), '--model', str(model))
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("isn't drawn", done.stderr)
        self.assertIn(why, done.stderr)
        self.assertFalse(out.exists())

    def test_someone_is_drawn_in_the_avatar_they_sent(self):
        abstract, said = self.draw()
        self.assertIn('in the abstract body', said)
        sent, said = self.draw('--model', self.given)
        self.assertIn('in the avatar they sent', said)
        self.assertNotEqual(sent[3], abstract[3])
        for half, there in enumerate(RoomPictureTest.halves(sent)):
            self.assertGreater(len(there), 300, half)
            xs, ys = [x for x, _, _ in there], [y for _, y, _ in there]
            self.assertGreater(min(xs), 0), self.assertLess(max(xs), 191)   # all of them is on their sheet
            self.assertGreater(min(ys), 0), self.assertLess(max(ys), 191)
            self.assertGreater(max(ys) - min(ys), 1.5 * (max(xs) - min(xs)), half)   # a person standing, from head to foot
        self.assertEqual(sent[3], self.draw('--model', self.given, '--place', '2')[0][3])   # their own colours, not their place's
        self.assertNotEqual(sent[3], self.draw('--model', self.given, '--round', '90')[0][3])

    def test_their_mouth_moves_as_their_pose_tells_it(self):
        def rows(*args):
            return self.draw('--model', self.talker, '--size', '640', *args)[0][3]

        shut, wide, half = rows(), rows('--mouth', 'aa=1'), rows('--mouth', 'aa=0.5')
        self.assertTrue(wide != shut and half != shut and half != wide)
        changed = [y for y in range(640) if shut[y] != wide[y]]
        top = min(y for there in RoomPictureTest.halves(self.draw('--model', self.talker, '--size', '640')[0]) for _, y, _ in there)
        self.assertLess(max(changed), top + 0.25 * 640)   # only up at the head
        self.assertTrue(rows('--mouth', 'aa=0') == shut)
        # a shape their avatar hasn't got moves nothing, and neither does any on the abstract body
        self.assertTrue(rows('--mouth', 'ee=1') == shut)
        self.assertTrue(self.draw('--mouth', 'aa=1')[0][3] == self.draw()[0][3])

    def test_a_file_that_was_not_theirs_to_send_is_not_drawn(self):
        self.refused(self.kept, "isn't marked")

    def test_what_is_not_an_avatar_is_not_drawn(self):
        self.refused(self.hands, 'no body')
        self.refused(self.dir / 'none.ftmodel', 'none.ftmodel')
        whole = Path(self.given).read_bytes()
        for name, data in (('cut', whole[:len(whole) // 2]), ('junk', b'FTMD' + bytes(range(256)) * 8), ('empty', b''), ('long', whole + b'more')):
            path = self.dir / f'{name}.ftmodel'
            path.write_bytes(data)
            self.refused(path, '')


class RoomModesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        built(gpu=False)

    def test_options_that_are_not_theirs_are_a_usage_error(self):
        for args in (['--room', '--size'], ['--room', '--height', '400'], ['--room', 'now'], ['--room-snapshot'], ['--room-snapshot', 'x.png', '--place'], ['--room-snapshot', 'x.png', '--model'],
                     ['--room-snapshot', 'x.png', '--mouth'], ['--room-snapshot', 'x.png', '--mouth', 'blink=1'], ['--room-snapshot', 'x.png', '--mouth', 'aa=2'],
                     ['--room-snapshot', 'x.png', '--mouth', 'aa'],
                     ['--room-snapshot', 'x.png', '--walked', '0,0,0'], ['--pretend'], ['--pretend', 'wave', '--walked'], ['--pretend', 'wave', '--walked', '1,2'],
                     ['--pretend', 'wave', '--walked', '1,2,3,4'], ['--pretend', 'wave', '--walked', '99,0,0'], ['--pretend', 'wave', '--walked', '0,0,400'],
                     ['--pretend', 'wave', '--walked', 'nan,0,0'], ['--pretend', 'wave', '--size', '64']):
            done = run(*args)
            self.assertEqual(done.returncode, 2, (args, done.stderr))
            self.assertIn('usage:', done.stderr, args)

    def test_a_pretended_pose_has_to_be_one(self):
        done = run('--pretend', 'fly')
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn('no pose called fly', done.stderr)

    def test_pretending_with_no_ft_together_stays_out_of_any_room(self):
        nobody = f'ft_test_nobody_{os.getpid()}'
        done = run('--pretend', 'wave', '--together', nobody, '--seconds', '0.7', '--status', '0.2')
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        self.assertGreaterEqual(len(lines), 2)
        self.assertEqual(set(lines), {'pretending to wave: not in a room'})


if __name__ == '__main__':
    unittest.main()
