#!/usr/bin/env python3
"""ft-model-import (avatar/import) on made-up models (fixtures.py) and on the bundled hands'
GLBs: the hand's bones are found whatever the rig calls them, the joints land where they are,
only the hand and its cuff are kept, a hand of another size is scaled, a dense one reduced, a
VRM's licence is kept, and what can't be told is refused with the reason. Damaged files end in
an error, never a crash.

It runs the built program (avatar/build.sh), and is skipped where that isn't built.
Run: python3 avatar/tests/test_import.py"""
import copy
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import fixtures  # noqa: E402
import ftmodel  # noqa: E402

# FT_MODEL_IMPORT: another build of the program to test (one with sanitizers, say)
TOOL = Path(os.environ.get('FT_MODEL_IMPORT') or REPO / 'avatar/build/ft-model-import')
FUZZ = int(os.environ.get('FT_MODEL_IMPORT_FUZZ') or 120)   # damaged copies tried of each file
SOURCES = [*(REPO / 'avatar/import').glob('*'), REPO / 'avatar/modelfile.cpp', REPO / 'avatar/modelfile.h']
HANDS = REPO / 'screens/assets/hands'


def dist(a, b):
    return math.dist(a, b)


class ImportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            raise unittest.SkipTest('avatar/build/ft-model-import is not built (avatar/build.sh)')
        if any(src.stat().st_mtime > TOOL.stat().st_mtime for src in SOURCES):
            raise unittest.SkipTest('avatar/build/ft-model-import is older than its sources (avatar/build.sh)')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def source(self, name, data):
        path = self.dir / name
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        return path

    def run_tool(self, path, *args):
        """(exit status, {key: value} of what it printed)."""
        done = subprocess.run([str(TOOL), str(path), '--to', str(self.dir / 'out'), *args],
                              capture_output=True, text=True, timeout=120)
        said = dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line)
        return done.returncode, said

    def imported(self, name, data, *args):
        status, said = self.run_tool(self.source(name, data), *args)
        self.assertEqual(status, 0, said)
        return ftmodel.read(said['file']), said

    def refused(self, name, data, *args):
        status, said = self.run_tool(self.source(name, data), *args)
        self.assertEqual(status, 1, said)
        self.assertIn('error', said)
        return said['error']

    def check_joints(self, model, rig, scale=1.0, tip=0.0015):
        """Each joint is where the rig has it (from the wrist), the tips within `tip`."""
        for side, joints in rig.joints.items():
            hand, wrist = model.hands[side], joints[0]
            for j, at in joints.items():
                want = tuple((a - w) * scale for a, w in zip(at, wrist))
                self.assertLess(dist(hand.bind[j], want), tip if j % 5 == 4 else 2e-4, (side, j, hand.bind[j], want))

    def check_hand(self, hand):
        """Only the hand and its cuff: nothing of the body's, a little behind the wrist, and
        each finger's far half moved by that finger's last bone."""
        along = [c / math.hypot(*hand.bind[11]) for c in hand.bind[11]]
        behind = [sum(p[k] * along[k] for k in range(3)) for p in hand.pos]
        self.assertGreaterEqual(min(behind), -0.0405)
        self.assertLess(min(behind), -0.015)
        self.assertLess(max(math.hypot(*p) for p in hand.pos), 0.30)
        for weights in hand.weights:
            self.assertEqual(sum(weights), 255)
        for tip in (4, 9, 14, 19, 24):
            near = [i for i, p in enumerate(hand.pos) if dist(p, hand.bind[tip]) < 0.012]
            self.assertTrue(near, tip)
            for i in near:
                self.assertEqual(hand.joints[i][0], tip - 1)
                self.assertEqual(hand.weights[i][0], 255)

    def test_naming_schemes(self):
        for scheme in ('mixamo', 'unity', 'blender', 'unreal'):
            for extra in ({}, {'metacarpals': True}, {'tips': True}, {'metacarpals': True, 'tips': True}):
                with self.subTest(scheme=scheme, **extra):
                    rig = fixtures.make(scheme, **extra)
                    model, said = self.imported(f'{scheme}.glb', fixtures.glb(rig))
                    self.assertEqual(sorted(model.hands), ['left', 'right'])
                    self.check_joints(model, rig)
                    for hand in model.hands.values():
                        self.check_hand(hand)
                    self.assertNotIn('mirrored', said['left'] + said['right'])
                    self.assertNotIn('scaled', said['left'])

    def test_palm_bones_give_their_weights_to_the_wrist(self):
        rig = fixtures.make('blender', palm_bones=True)
        model, _ = self.imported('rigify.glb', fixtures.glb(rig))
        self.check_joints(model, rig)

    def test_vrm_humanoid_names_the_bones(self):
        for version in (0, 1):
            with self.subTest(vrm=version):
                rig = fixtures.make('opaque')
                meta = ({'title': 'Test Avatar', 'author': 'Someone', 'licenseName': 'CC_BY'} if version == 0 else
                        {'name': 'Test Avatar', 'authors': ['Someone'], 'licenseUrl': 'https://vrm.dev/licenses/1.0/',
                         'allowRedistribution': True})
                model, said = self.imported(f'avatar{version}.vrm', fixtures.glb(rig, vrm=version, meta=meta))
                self.check_joints(model, rig)
                self.assertEqual((model.name, model.author, model.format), ('Test Avatar', 'Someone', f'vrm {version}'))
                self.assertTrue(model.may_share)
                self.assertEqual(said['share'], 'yes')
        # the same bones without the humanoid: nothing says which is a hand
        self.assertIn('is named for a hand', self.refused('opaque.glb', fixtures.glb(fixtures.make('opaque'))))

    def test_a_vrm_licence_that_forbids_passing_on_is_kept(self):
        for version, meta in ((0, {'title': 'Mine', 'licenseName': 'Redistribution_Prohibited'}),
                              (0, {'title': 'Mine', 'licenseName': 'Other'}), (0, {'title': 'Mine'}),
                              (1, {'name': 'Mine', 'allowRedistribution': False}), (1, {'name': 'Mine'})):
            with self.subTest(vrm=version, meta=meta):
                data = fixtures.glb(fixtures.make('opaque'), vrm=version, meta=meta)
                model, said = self.imported('private.vrm', data, '--may-share')   # the option can't override it
                self.assertFalse(model.may_share)
                self.assertEqual(said['share'], 'licence')

    def test_a_gltf_may_be_passed_on_when_the_person_says_so(self):
        data = fixtures.glb(fixtures.make())
        model, said = self.imported('plain.glb', data)
        self.assertFalse(model.may_share)
        self.assertEqual((said['share'], model.format, model.name), ('no', 'gltf', 'plain'))
        model, said = self.imported('plain.glb', data, '--may-share')
        self.assertTrue(model.may_share)
        self.assertEqual(said['share'], 'yes')

    def test_fbx(self):
        for unit_cm in (True, False):
            for scheme in ('mixamo', 'unreal'):
                with self.subTest(scheme=scheme, unit_cm=unit_cm):
                    rig = fixtures.make(scheme, tips=scheme == 'mixamo', metacarpals=scheme == 'unreal')
                    model, said = self.imported('rig.fbx', fixtures.fbx(rig, unit_cm))
                    self.assertEqual(model.format, 'fbx')
                    self.check_joints(model, rig)
                    for hand in model.hands.values():
                        self.check_hand(hand)
                    self.assertNotIn('scaled', said['left'])
        self.assertIn('ring finger', self.refused('noring.fbx', fixtures.fbx(fixtures.make('mixamo', without=('ring',)))))

    def test_no_normals_in_the_file(self):
        rig = fixtures.make()
        model, _ = self.imported('bare.glb', fixtures.glb(rig, normals=False))
        self.check_joints(model, rig)

    def test_one_hand_is_mirrored(self):
        rig = fixtures.make(sides=('left',))
        model, said = self.imported('lefty.glb', fixtures.glb(rig))
        self.assertIn('mirrored from the left', said['right'])
        left, right = model.hands['left'], model.hands['right']
        self.assertEqual(len(left.tris), len(right.tris))
        for a, b in zip(left.bind, right.bind):
            self.assertLess(dist((-a[0], a[1], a[2]), b), 1e-6)
        # turned to face out again: a triangle's normal is the mirrored one's, mirrored
        def normal(hand, t):
            p, q, r = (hand.pos[i] for i in hand.tris[t])
            u, v = [q[k] - p[k] for k in range(3)], [r[k] - p[k] for k in range(3)]
            return (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for t in range(0, len(left.tris), 7):
            a, b = normal(left, t), normal(right, t)
            self.assertLess(dist((-a[0], a[1], a[2]), b), 1e-9)

    def test_another_size_is_scaled_to_a_persons(self):
        for scale in (0.4, 100.0):
            with self.subTest(scale=scale):
                rig = fixtures.make(scale=scale)
                model, said = self.imported('sized.glb', fixtures.glb(rig))
                self.assertIn('scaled', said['left'])
                palm = sum(math.hypot(*model.hands['left'].bind[j]) for j in (6, 11, 16, 21))
                self.assertAlmostEqual(palm, 0.36, places=3)
                wrist = rig.joints['left'][0]
                rig_palm = sum(dist(rig.joints['left'][j], wrist) for j in (6, 11, 16, 21))
                self.check_joints(model, rig, 0.36 / rig_palm)
                self.check_hand(model.hands['left'])

    def test_a_dense_hand_is_reduced(self):
        rig = fixtures.make(dense=True)
        model, said = self.imported('dense.glb', fixtures.glb(rig))
        for side in ('left', 'right'):
            self.assertIn('reduced from', said[side])
            self.assertLessEqual(len(model.hands[side].tris), 7500)
            self.assertGreater(len(model.hands[side].tris), 2000)
        self.check_joints(model, rig)
        for tip in (9, 24):   # the fingers are still there, to their ends
            self.assertLess(min(dist(p, model.hands['left'].bind[tip]) for p in model.hands['left'].pos), 0.012)

    def test_what_cannot_be_told_is_refused(self):
        self.assertIn('no bones named for the ring finger', self.refused('noring.glb', fixtures.glb(fixtures.make(without=('ring',)))))
        self.assertIn('has 2 bones: 3 to 5 are read', self.refused('short.glb', fixtures.glb(fixtures.make(finger_bones=2))))
        self.assertIn('not a glTF', self.refused('junk.glb', b'glTF' + bytes(range(200))))
        self.assertIn('by its name', self.refused('model.obj', b'v 0 0 0\n'))
        self.assertIn('not an FBX', self.refused('junk.fbx', b'Kaydara FBX Binary  \0' + bytes(range(200))))
        elsewhere = '{"asset": {"version": "2.0"}, "buffers": [{"uri": "../other.bin", "byteLength": 4}]}'
        self.assertIn('somewhere else', self.refused('elsewhere.gltf', elsewhere))
        status, said = self.run_tool(self.dir / 'missing.glb')
        self.assertEqual((status, said.get('error')), (1, "the file can't be read"))

    def test_names(self):
        data = fixtures.glb(fixtures.make())
        _, said = self.imported('My Hands (v2).glb', data)
        self.assertEqual((said['ok'], Path(said['file']).name, said['name']), ('my-hands-v2', 'my-hands-v2.ftmodel', 'My Hands (v2)'))
        _, said = self.imported('x.glb', data, '--name', 'other_one')
        self.assertEqual(Path(said['file']).name, 'other_one.ftmodel')
        self.assertIn('lower-case', self.refused('x.glb', data, '--name', 'Not Allowed'))
        self.assertIn('lower-case', self.refused('x.glb', data, '--name', '../up'))
        # --check writes nothing
        status, said = self.run_tool(self.source('x.glb', data), '--name', 'unwritten', '--check')
        self.assertEqual(status, 0)
        self.assertNotIn('file', said)
        self.assertFalse((self.dir / 'out/unwritten.ftmodel').exists())

    def test_bundled_hands(self):
        """The GLBs the bundled models are made from: flat rigs (every joint beside the wrist) and
        rigs with their own names, and files of one hand that the name tells the side of."""
        for folder, tris in (('', 2314), ('babylon', 1808), ('godot', 3052), ('mrtk2', 2664)):
            for side, other in (('left', 'right'), ('right', 'left')):
                with self.subTest(model=folder or 'asset', side=side):
                    status, said = self.run_tool(HANDS / folder / f'{side}.glb', '--check')
                    self.assertEqual(status, 0, said)
                    self.assertEqual(said[side], f'{tris} triangles')
                    self.assertEqual(said[other], f'{tris} triangles, mirrored from the {side}')
        # a file of one hand that says its side nowhere
        data = (HANDS / 'left.glb').read_bytes()
        self.assertIn('--side', self.refused('generic.glb', data))
        _, said = self.imported('generic.glb', data, '--side', 'right')
        self.assertNotIn('mirrored', said['right'])
        self.assertIn('mirrored from the right', said['left'])
        # mrtk3's skin has no wrist bone (hand-asset.py has a table for its joints): refused, with why
        self.assertIn('is named for a hand', self.refused('mrtk3.glb', (HANDS / 'mrtk3/right.glb').read_bytes()))

    def test_damaged_files_end_in_an_error(self):
        rng = random.Random(1)
        rig = fixtures.make(tips=True)
        for name, good in (('damaged.glb', fixtures.glb(rig)), ('damaged.vrm', fixtures.glb(rig, vrm=1, meta={'name': 'x'})),
                           ('damaged.fbx', fixtures.fbx(rig).encode())):
            for n in range(FUZZ):
                data = bytearray(good)
                if n % 3 == 0:
                    del data[rng.randrange(len(data)):]
                else:
                    for _ in range(rng.choice((1, 1, 4, 32))):
                        data[rng.randrange(len(data))] = rng.randrange(256)
                status, _ = self.run_tool(self.source(name, bytes(data)), '--check')
                self.assertIn(status, (0, 1), f'{name} #{n}: ended with {status}')

    def test_a_gltf_whose_numbers_lie_ends_in_an_error(self):
        """Counts, offsets, node and accessor numbers changed one at a time: the file still parses,
        and what it says is wrong."""
        rng = random.Random(2)
        good = fixtures.glb(fixtures.make(tips=True), vrm=1, meta={'name': 'x'})
        size = struct.unpack_from('<I', good, 12)[0]
        doc, rest = json.loads(good[20:20 + size]), good[20 + size:]

        def numbers(value, found):
            """Every (holder, key) in the document whose value is a number."""
            for key, inner in (value.items() if isinstance(value, dict) else enumerate(value)):
                if isinstance(inner, (dict, list)):
                    numbers(inner, found)
                elif isinstance(inner, (int, float)) and not isinstance(inner, bool):
                    found.append((value, key))
            return found

        for n in range(FUZZ * 2):
            changed = copy.deepcopy(doc)
            holder, key = rng.choice(numbers(changed, []))
            holder[key] = rng.choice((0, -1, 1, holder[key] + 1, holder[key] * 1000, 65535, 2 ** 31, 2 ** 32 - 1, 1e30, 0.5))
            text = json.dumps(changed).encode()
            text += b' ' * (-len(text) % 4)
            data = struct.pack('<4sII', b'glTF', 2, 20 + len(text) + len(rest)) + struct.pack('<I4s', len(text), b'JSON') + text + rest
            status, _ = self.run_tool(self.source('lies.glb', data), '--check')
            self.assertIn(status, (0, 1), f'#{n} ({key}): ended with {status}')


if __name__ == '__main__':
    unittest.main()
