#!/usr/bin/env python3
"""ft-avatars (avatar/view) on a made-up whole person, from the file to the pixels: the importer
writes the person's model file, ft-avatars poses it as a made-up wearer and draws it on the GPU,
and the picture has the person in it, in every view, moved by the pose and by the face shapes.
The mirror's picture has them as each eye sees them in the glass, and the picture of the body
below has it as its own wearer sees it looking down. What it can't draw (no file, a hand model, a
pose that isn't one) ends in an error.

It runs the built programs (avatar/build.sh, screens/build.sh) and is skipped where they aren't
built or there is no GPU to draw on.
Run: python3 avatar/tests/test_avatars_picture.py"""
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bodyfixtures  # noqa: E402
import fixtures  # noqa: E402
from avatarsprogram import TOOL, built  # noqa: E402
from importcase import REPO, ImportCase  # noqa: E402
from pngread import read_png  # noqa: E402

GREY = 82   # what a sheet is drawn over
GLASS, FRAME = (23, 26, 31), (140, 148, 158)   # the mirror's glass with nothing in it, and its edge


def lopsided(there):
    """How many more of a standing figure's pixels by its head are left of its feet than right of them."""
    top, bottom = min(y for _, y in there), max(y for _, y in there)
    feet = [x for x, y in there if y > bottom - 6]
    middle = sum(feet) / len(feet)
    by_head = [x for x, y in there if y < top + 0.2 * (bottom - top)]
    return sum(x < middle for x in by_head) - sum(x > middle for x in by_head)


class AvatarsPictureTest(ImportCase):
    @classmethod
    def setUpClass(cls):
        built(gpu=True)
        super().setUpClass()
        rig = bodyfixtures.person('opaque', extras=False)   # no stand under the feet: the views are fitted to the person
        path = cls.dir / 'person.vrm'
        path.write_bytes(fixtures.glb(rig, vrm=1, meta={'name': 'x'}))
        done = subprocess.run([str(REPO / 'avatar/build/ft-model-import'), str(path), '--to', str(cls.dir / 'out')],
                              capture_output=True, text=True, timeout=120)
        cls.model = dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line)['file']
        hands = cls.dir / 'hands.glb'
        hands.write_bytes(fixtures.glb(fixtures.make()))
        done = subprocess.run([str(REPO / 'avatar/build/ft-model-import'), str(hands), '--to', str(cls.dir / 'out')],
                              capture_output=True, text=True, timeout=120)
        cls.hands = dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line)['file']

    def draw(self, kind, *args, model=None, status=0):
        out = self.dir / f'{kind.strip("-")}.png'
        done = subprocess.run([str(TOOL), kind, str(out), model or self.model, *args], capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, status, done.stderr)
        return read_png(out) if status == 0 else done.stderr

    def drawn(self, picture, cell=None):
        """The pixels that aren't the background's, as (x, y), in one of a sheet's four views or all."""
        width, height, channels, rows = picture
        x0, x1 = (cell * width // 4, (cell + 1) * width // 4) if cell is not None else (0, width)
        return {(x, y) for y in range(height) for x in range(x0, x1)
                if any(abs(rows[y][x * channels + c] - GREY) > 6 for c in range(3))}

    def test_a_sheet_has_the_person_from_four_sides(self):
        picture = self.draw('--snapshot', '--pose', 'rest', '--size', '160')
        width, height, channels, rows = picture
        self.assertEqual((width, height, channels), (4 * 96, 160, 3))
        for corner in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
            self.assertEqual(rows[corner[1]][corner[0] * 3:][:3], bytes((GREY,) * 3))
        spans = []
        for cell in range(4):
            there = self.drawn(picture, cell)
            self.assertGreater(len(there), 300, cell)
            ys, xs = [y for _, y in there], [x for x, _ in there]
            self.assertGreater(max(ys) - min(ys), 0.4 * height, cell)   # as large as a view that fits its arms lets it be
            self.assertGreater(min(ys), 2), self.assertLess(max(ys), height - 3)   # ...and all of it is in the view
            spans.append(max(xs) - min(xs))
        # arms out at rest: wide from in front and behind, narrow from the side
        self.assertGreater(spans[0], 2.5 * spans[2])
        self.assertAlmostEqual(spans[0], spans[3], delta=3)

    def test_a_pose_moves_it(self):
        rest, stand, wave = (self.drawn(self.draw('--snapshot', '--pose', pose, '--size', '160'), 0) for pose in ('rest', 'stand', 'wave'))

        def span(there):
            return max(x for x, _ in there) - min(x for x, _ in there)

        self.assertLess(span(stand), 0.6 * span(rest))   # the arms hang
        self.assertLess(abs(lopsided(stand)), 12)
        # the waving hand is the right one, which a view from in front has on its left, by the head
        self.assertGreater(lopsided(wave), 25)

    def test_a_face_shape_moves_the_face(self):
        plain = self.draw('--snapshot', '--pose', 'stand', '--size', '640')
        shaped = self.draw('--snapshot', '--pose', 'stand', '--size', '640', '--shape', 'aa=1', '--shape', 'blink=1')
        self.assertNotEqual(plain[3], shaped[3])
        changed = [y for y in range(plain[1]) if plain[3][y] != shaped[3][y]]
        top = min(y for _, y in self.drawn(plain, 0))
        self.assertLess(max(changed), top + 0.25 * plain[1])   # only up at the head

    def test_a_thumb_is_the_person_on_nothing(self):
        width, height, channels, rows = self.draw('--thumb', '--size', '96')
        self.assertEqual((width, height, channels), (96, 96, 4))
        alpha = [[rows[y][x * 4 + 3] for x in range(width)] for y in range(height)]
        self.assertEqual({alpha[0][0], alpha[0][-1], alpha[-1][0], alpha[-1][-1]}, {0})
        solid = sum(a == 255 for line in alpha for a in line)
        self.assertGreater(solid, 150)
        self.assertLess(solid, 0.5 * width * height)

    def test_the_mirror_shows_each_eye_the_wearer(self):
        picture = self.draw('--mirror-snapshot', '--pose', 'wave', '--size', '480')
        width, height, channels, rows = picture
        each = width // 2
        self.assertEqual((width, height, channels), (2 * 264, 480, 3))   # a glass 1.1 m by 2 m, to each eye

        def near(x, y, colour):
            return all(abs(rows[y][x * 3 + c] - colour[c]) <= 2 for c in range(3))

        figures = []
        for half in range(2):
            x0 = half * each
            for x, y in ((x0, 0), (x0 + each - 1, 0), (x0, height - 1), (x0 + each - 1, height - 1), (x0 + each // 2, 1)):
                self.assertTrue(near(x, y, FRAME), (x, y))
            for x, y in ((x0 + 8, 8), (x0 + each - 9, height - 9)):
                self.assertTrue(near(x, y, GLASS), (x, y))
            there = {(x - x0, y) for y in range(8, height - 8) for x in range(x0 + 8, x0 + each - 8) if not near(x, y, GLASS)}
            self.assertGreater(len(there), 300, half)
            ys = [y for _, y in there]
            # a mirror shows you at half your size, your eyes level with your eyes
            self.assertGreater(max(ys) - min(ys), 0.3 * height, half)
            self.assertLess(max(ys) - min(ys), 0.5 * height, half)   # the hand is up over the head
            self.assertGreater(min(ys), 0.1 * height), self.assertLess(max(ys), 0.8 * height)
            # the waving hand is the right one, and in a mirror your right is on the right
            self.assertLess(lopsided(there), -25, half)
            figures.append(there)

        def feet(there):
            low = [x for x, y in there if y > max(y for _, y in there) - 6]
            return sum(low) / len(low)

        # the wearer is as far behind the glass as before it: the left eye sees them 32 mm left of where the right eye does
        apart = (feet(figures[1]) - feet(figures[0])) * 1.1 / each
        self.assertAlmostEqual(apart, 0.032, delta=0.012)

        rest = self.draw('--mirror-snapshot', '--pose', 'rest', '--size', '240')
        self.assertEqual(rest[:3], (2 * 132, 240, 3))
        self.assertNotEqual(set(rest[3][100]), set(rest[3][3]))   # someone is in it

    def test_the_body_below_is_the_wearer_seen_from_their_own_eyes(self):
        width, height, channels, rows = self.draw('--below-snapshot', '--pose', 'stand', '--size', '320')
        self.assertEqual((width, height, channels), (2 * 320, 320, 4))   # the square under the wearer, to each eye
        middles = []
        for half in range(2):
            x0 = half * 320
            for x, y in ((x0, 0), (x0 + 319, 0), (x0, 319), (x0 + 319, 319), (x0 + 160, 2)):
                self.assertEqual(rows[y][x * 4 + 3], 0, (x, y))   # clear where no body is
            there = [(x - x0, y) for y in range(320) for x in range(x0, x0 + 320) if rows[y][x * 4 + 3] > 128]
            self.assertGreater(len(there), 300, half)
            self.assertLess(len(there), 0.5 * 320 * 320, half)
            middle = (sum(x for x, _ in there) / len(there), sum(y for _, y in there) / len(there))
            # your body is under you: about the middle of the square
            self.assertLess(abs(middle[0] - 160), 50, half), self.assertLess(abs(middle[1] - 160), 70, half)
            middles.append(middle)
        # most of what a standing wearer sees of themselves is nearer than the square, so it crosses their eyes
        self.assertGreater(middles[0][0] - middles[1][0], 2)
        typing = self.draw('--below-snapshot', '--pose', 'type', '--size', '320')
        ahead = [y for y in range(320) for x in range(320) if typing[3][y][x * 4 + 3] > 128]
        self.assertLess(min(ahead), min(y for _, y in there) - 10)   # arms held out ahead are drawn toward the top

    def test_what_it_cannot_draw_ends_in_an_error(self):
        self.assertIn('no pose called fly', self.draw('--below-snapshot', '--pose', 'fly', status=1))
        self.assertIn('no body', self.draw('--below-snapshot', model=self.hands, status=1))
        self.assertIn('no pose called fly', self.draw('--snapshot', '--pose', 'fly', status=1))
        self.assertIn('no body', self.draw('--snapshot', model=self.hands, status=1))
        self.assertIn('no pose called fly', self.draw('--mirror-snapshot', '--pose', 'fly', status=1))
        self.assertIn('no body', self.draw('--mirror-snapshot', model=self.hands, status=1))
        # the mirror itself reads its model before it starts anything
        for model, why in ((self.hands, 'no body'), (str(self.dir / 'nothing.ftmodel'), 'avatars:')):
            done = subprocess.run([str(TOOL), '--mirror', model], capture_output=True, text=True, timeout=30)
            self.assertEqual(done.returncode, 1, done.stderr)
            self.assertIn(why, done.stderr)
        for args in (['--mirror'], ['--mirror', self.model, '--height'], ['--mirror', self.model, '--walk', '1']):
            done = subprocess.run([str(TOOL), *args], capture_output=True, text=True, timeout=30)
            self.assertEqual(done.returncode, 2, done.stderr)
            self.assertIn('usage:', done.stderr)
        self.assertIn('avatars:', self.draw('--snapshot', model=str(self.dir / 'nothing.ftmodel'), status=1))
        damaged = self.dir / 'damaged.ftmodel'
        damaged.write_bytes(Path(self.model).read_bytes()[:-40])
        self.assertIn('avatars:', self.draw('--thumb', model=str(damaged), status=1))
        for args in (['--shape', 'nod=1'], ['--shape', 'aa=2'], ['--size'], ['--walk']):
            self.assertIn('usage:', self.draw('--snapshot', *args, status=2))
        done = subprocess.run([str(TOOL), '--poses'], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.stdout.split(), ['rest', 'stand', 'wave', 'reach', 'type', 'sit', 'lean', 'look'])


if __name__ == '__main__':
    unittest.main()
