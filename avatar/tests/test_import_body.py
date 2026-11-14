#!/usr/bin/env python3
"""ft-model-import (avatar/import) on a made-up whole person (bodyfixtures.py): the body's bones
are found by a VRM's humanoid or by the rig's names, it stands at the origin facing -z however
the file has it, a bone a humanoid doesn't have gives its mesh to the one it hangs from, the
materials, pictures and face shapes come along, a dense body is reduced and its pictures kept
to a budget, and a skeleton that can't be told is no body, with the reason. Damaged files end
in an error, never a crash.

It runs the built program (avatar/build.sh), and is skipped where that isn't built.
Run: python3 avatar/tests/test_import_body.py"""
import copy
import json
import math
import random
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bodyfixtures  # noqa: E402
import fixtures  # noqa: E402
from importcase import FUZZ, ImportCase, ftmodel  # noqa: E402

ROLE = {name: n for n, name in enumerate(ftmodel.ROLES)}
OPAQUE, MASK, BLEND = 0, 1, 2
DOUBLE_SIDED, UNLIT, TOON = 1, 2, 4
SHAPES = ['aa', 'oh', 'blink', 'blinkLeft', 'blinkRight']
# what programs that aren't VRM's call the face shapes
OTHER_NAMES = {'aa': 'vrc.v_aa', 'oh': 'Fcl_MTH_O', 'blink': 'まばたき', 'blinkLeft': 'Fcl_EYE_Close_L', 'blinkRight': 'eye_close_R',
               'smile': 'mouth_smile'}


class BodyImportTest(ImportCase):
    def body_of(self, name, data, *args):
        model, said = self.imported(name, data, *args, body=True)
        self.assertTrue(model.has_body, said)
        self.assertIn('body', said)
        return model.body, said, model

    def no_body(self, name, data):
        """Why the file's model has no body (its hands were still imported)."""
        model, said = self.imported(name, data, body=True)
        self.assertFalse(model.has_body)
        self.assertNotIn('body', said)
        return said['nobody']

    def at(self, body, rig, block):
        """The body's vertices that are a block's, by where they are."""
        want = [rig.placed(rig.built[v]) for v in rig.blocks[block][0]]
        found = [i for i, p in enumerate(body.pos) if any(math.dist(p, w) < 2e-4 for w in want)]
        self.assertTrue(found, block)
        return found

    def check_bones(self, body, rig):
        self.assertEqual(sorted(body.bones), sorted(rig.bones))
        for role, at in rig.bones.items():
            self.assertLess(math.dist(body.bones[role], rig.placed(at)), 1e-5, role)

    def check_mesh(self, body, rig):
        """The mesh is whole: every weight adds up, every triangle is in a part, the feet are on
        the floor, and each block moves with the bone it should."""
        for weights in body.weights:
            self.assertEqual(sum(weights), 255)
        at = 0
        for first, count, material in body.parts:
            self.assertEqual(first, at)
            self.assertLess(material, len(body.materials))
            at += count
        self.assertEqual(at, len(body.tris))
        alphas = [body.materials[material]['alpha'] for _, _, material in body.parts]
        self.assertEqual(alphas, sorted(alphas))   # what is see-through is drawn last

        def moved_by(block, *roles):
            for i in self.at(body, rig, block):
                got = {ftmodel.ROLES[j]: w for j, w in zip(body.joints[i], body.weights[i]) if w}
                self.assertEqual(sorted(got), sorted(roles), (block, got))
                for w in got.values():
                    self.assertAlmostEqual(w, 255 / len(roles), delta=1)

        moved_by('pelvis', 'hips'), moved_by('head', 'head'), moved_by('rightFoot', 'rightFoot')
        self.assertAlmostEqual(min(body.pos[i][1] for i in self.at(body, rig, 'leftFoot')), 0, delta=1e-4)
        knee = [i for i in self.at(body, rig, 'leftThigh') if abs(body.pos[i][1] - 0.5) < 1e-3]
        self.assertTrue(knee)
        for i in knee:
            self.assertEqual(sorted(zip(body.joints[i], body.weights[i]), reverse=True)[:2],
                             sorted([(ROLE['leftUpperLeg'], 128), (ROLE['leftLowerLeg'], 127)], reverse=True))
        if 'hair' in rig.blocks:   # what a humanoid has no bone for moves with the bone it hangs from
            moved_by('hair', 'head'), moved_by('leftBand', 'leftLowerArm'), moved_by('stand', 'hips')
            self.assertAlmostEqual(min(p[1] for p in body.pos), -0.06, delta=1e-4)   # the stand is under the floor

    def check_look(self, body, rig, toon):
        skin, hair, face, dark = body.materials   # the fifth is the first again: one is kept
        self.assertEqual([m['alpha'] for m in body.materials], [OPAQUE, BLEND, MASK, OPAQUE])
        self.assertEqual([m['flags'] for m in body.materials], [TOON if toon else 0, DOUBLE_SIDED, 0, 0])
        for got, want in zip(skin['color'] + dark['color'] + hair['color'], (1, 0.9, 0.8, 1, 0.2, 0.2, 0.25, 1, 1, 1, 1, 0.5)):
            self.assertAlmostEqual(got, want, places=6)
        self.assertEqual(dark['texture'], -1)
        self.assertEqual([body.textures[m['texture']][:2] for m in (skin, hair, face)], [(4, 4), (2, 2), (8, 8)])
        pixels = body.textures[skin['texture']][2]
        self.assertEqual(pixels[4 * (4 * 2 + 3):][:4], bytes((203, 152, 120, 255)))   # rows top first
        self.assertEqual(body.textures[face['texture']][2][3], 0)
        material_of = {}
        for first, count, material in body.parts:
            for t in range(first, first + count):
                for i in body.tris[t]:
                    material_of.setdefault(i, set()).add(material)
        for block, material in (('head', face), ('hair', hair), ('leftToes', dark), ('rightShin', skin), ('pelvis', skin)):
            for i in self.at(body, rig, block):
                self.assertEqual(material_of[i], {body.materials.index(material)}, block)
        head = rig.blocks['head'][0]
        for i in self.at(body, rig, 'head'):   # each vertex kept its place in the picture
            v = min(head, key=lambda v: math.dist(rig.placed(rig.built[v]), body.pos[i]))
            self.assertLess(math.dist(body.uv[i], rig.uv[v]), 1e-6)

    def check_shapes(self, body, rig, said, names=None, split=False):
        """Each of Frametop's shapes moves the vertices the rig's does, as far. split: the file
        has a vertex for each corner of a face (an FBX without normals), so a place may be several."""
        self.assertEqual(said['shapes'], ' '.join(SHAPES))
        self.assertEqual(sorted(body.shapes), sorted(SHAPES))   # the smile isn't one of Frametop's
        for shape in SHAPES:
            moves = rig.shapes[(names or {}).get(shape, shape)]
            found = set()
            for i, delta in body.shapes[shape].items():
                v = min(moves, key=lambda v: math.dist(rig.placed(rig.built[v]), body.pos[i]))
                self.assertLess(math.dist(rig.placed(rig.built[v]), body.pos[i]), 2e-4, shape)
                self.assertLess(math.dist(delta, rig.placed(rig.moved[shape][v])), 1e-4, shape)
                found.add(v)
            self.assertEqual(found, set(moves), shape)
            if not split:
                self.assertEqual(len(body.shapes[shape]), len(moves), shape)

    def test_a_vrm_avatar(self):
        for version in (0, 1):
            with self.subTest(vrm=version):
                rig = bodyfixtures.person('opaque', upper_chest=True)
                body, said, model = self.body_of(f'person{version}.vrm', fixtures.glb(rig, vrm=version, meta={'name': 'x', 'title': 'x'}))
                self.check_bones(body, rig)
                self.check_mesh(body, rig)
                self.check_look(body, rig, toon=True)
                self.check_shapes(body, rig, said)
                self.assertLess(math.dist(body.eyes, (0, 1.64, -0.08)), 1e-5)   # between the eye bones
                self.assertRegex(said['body'], r'^\d+ triangles, \d+ vertices, 1\.64 m to the eyes$')
                self.assertEqual(said['look'], '4 materials, 3 pictures (0.0 MB)')
                self.assertEqual(sorted(model.hands), ['left', 'right'])   # the avatar's hands are the wearer's
                self.assertNotIn('mirrored', said['left'] + said['right'])

    def test_a_bone_that_turns_with_one_it_does_not_hang_from(self):
        """VRM 1.0's constraints: a sleeve's bone hangs from the chest and aims along the arm, so
        the sleeve is the upper arm's. A bone that copies a turn from somewhere else can't be a
        bone of the body's (they turn about their own places): it stays with what it hangs from."""
        rig = bodyfixtures.person('opaque')
        chest, upper, fore = (rig.humanoid[name] for name in ('chest', 'leftUpperArm', 'leftLowerArm'))
        shoulder, elbow = rig.nodes[upper][2], rig.nodes[fore][2]

        def helper(name, at, kind, source, weight=1.0):
            node = rig.node(name, chest, at)
            rig.constraints[node] = (kind, source, weight)
            corner = (0.1 * len(rig.constraints), 1.9, 0.4)   # a block of its own, away from the rest of the person
            rig.block(name, [(node, 1.0)], corner, tuple(c + 0.02 for c in corner))
            return node

        sleeve = helper('sleeve', shoulder, 'aim', fore)
        cuff = rig.node('cuff', sleeve, tuple((a + b) / 2 for a, b in zip(shoulder, elbow)))   # what hangs from it goes with it
        rig.block('cuff', [(cuff, 1.0)], (0.0, 1.9, 0.4), (0.02, 1.92, 0.42))
        helper('copy', shoulder, 'rotation', upper)
        helper('faint', shoulder, 'rotation', upper, 0.2)
        helper('roll', shoulder, 'roll', upper)
        helper('robot', (0.0, 1.9, 0.2), 'rotation', upper)
        helper('aside', (0.0, 1.9, 0.2), 'aim', fore)
        helper('lost', shoulder, 'aim', 10 ** 6)
        rig.built = [v[0] for v in rig.verts]
        body, _, _ = self.body_of('sleeves.vrm', fixtures.glb(rig, vrm=1, meta={'name': 'x'}))
        self.check_bones(body, rig)
        self.check_mesh(body, rig)
        for block, role in (('sleeve', 'leftUpperArm'), ('cuff', 'leftUpperArm'), ('copy', 'leftUpperArm'), ('faint', 'chest'),
                            ('roll', 'chest'), ('robot', 'chest'), ('aside', 'chest'), ('lost', 'chest')):
            for i in self.at(body, rig, block):
                self.assertEqual([(ftmodel.ROLES[j], w) for j, w in zip(body.joints[i], body.weights[i]) if w], [(role, 255)], block)
        # the same file as a VRM 0.x, which has no constraints: every helper is the chest's
        body, _, _ = self.body_of('sleeves0.vrm', fixtures.glb(rig, vrm=0, meta={'title': 'x'}))
        for i in self.at(body, rig, 'sleeve'):
            self.assertEqual([ftmodel.ROLES[j] for j, w in zip(body.joints[i], body.weights[i]) if w], ['chest'])

    def test_a_skeleton_told_by_its_names(self):
        for scheme in ('unity', 'mixamo', 'blender', 'unreal'):
            for extra in ({}, {'upper_chest': True}, {'shoulders': False, 'toes': False, 'eyes': False}):
                with self.subTest(scheme=scheme, **extra):
                    rig = bodyfixtures.named_shapes(bodyfixtures.person(scheme, **extra), OTHER_NAMES)
                    body, said, _ = self.body_of(f'{scheme}.glb', fixtures.glb(rig))
                    self.check_bones(body, rig)
                    self.check_mesh(body, rig)
                    self.check_look(body, rig, toon=False)
                    self.check_shapes(body, rig, said, OTHER_NAMES)
                    if not extra.get('eyes', True):   # no eye bones: where a head of this size has them
                        self.assertLess(math.dist(body.eyes, (0, 1.58 + 0.11 * 0.58, -0.13 * 0.58)), 1e-4)

    def test_it_stands_at_the_origin_however_the_file_has_it(self):
        for turn in (90, 180, -135):
            with self.subTest(turn=turn):
                rig = bodyfixtures.person().move(lift=0.4, shift=(2.0, -3.0), turn=turn)
                body, said, _ = self.body_of('moved.glb', fixtures.glb(rig))
                self.check_bones(body, rig)
                self.check_mesh(body, rig)
                self.check_shapes(body, rig, said)

    def test_fbx(self):
        for scheme, unit_cm in (('mixamo', True), ('unreal', False)):
            with self.subTest(scheme=scheme):
                rig = bodyfixtures.named_shapes(bodyfixtures.person(scheme), OTHER_NAMES)
                for name, data in rig.pictures.items():
                    self.source(name, data)   # an FBX's pictures are files beside it
                body, said, model = self.body_of('person.fbx', fixtures.fbx(rig, unit_cm))
                self.assertEqual(model.format, 'fbx')
                self.check_bones(body, rig)
                self.check_mesh(body, rig)
                self.check_shapes(body, rig, said, OTHER_NAMES, split=True)
                # an FBX says less of a material: its colour or its picture, cut out where the picture is see-through
                skin, hair, face, dark = body.materials
                self.assertEqual([m['alpha'] for m in body.materials], [OPAQUE, OPAQUE, MASK, OPAQUE])
                self.assertEqual([body.textures[m['texture']][:2] for m in (skin, hair, face)], [(4, 4), (2, 2), (8, 8)])
                self.assertEqual((dark['texture'], said['look']), (-1, '4 materials, 3 pictures (0.0 MB)'))
                for i in self.at(body, rig, 'head'):
                    v = min(rig.blocks['head'][0], key=lambda v: math.dist(rig.placed(rig.built[v]), body.pos[i]))
                    self.assertLess(math.dist(body.uv[i], rig.uv[v]), 1e-6)
        # a picture that isn't beside the file: the material is its colour
        rig = bodyfixtures.person('mixamo')
        rig.materials[2]['file'] = 'elsewhere.png'
        body, said, _ = self.body_of('person.fbx', fixtures.fbx(rig))
        self.assertEqual(said['look'], "4 materials, 2 pictures (0.0 MB), 1 couldn't be read")
        # ...and one named as a way out of the file's folder isn't looked for there
        rig.materials[2]['file'] = '../face.png'
        (self.dir / 'inner').mkdir(exist_ok=True)
        for name in ('skin.png', 'hair.png'):
            self.source('inner/' + name, rig.pictures[name])
        self.assertIn("1 couldn't be read", self.body_of('inner/person.fbx', fixtures.fbx(rig))[1]['look'])

    def test_a_dense_body_is_reduced(self):
        rig = bodyfixtures.person(dense=True)
        body, said, _ = self.body_of('dense.vrm', fixtures.glb(rig, vrm=1, meta={'name': 'x'}))
        self.assertGreater(len(rig.tris), 24000)
        self.assertRegex(said['body'], rf'^\d+ triangles, reduced from {len(rig.tris)}, ')
        self.assertLessEqual(len(body.tris), 20000)
        self.assertGreater(len(body.tris), 10000)
        self.check_bones(body, rig)
        self.check_shapes(body, rig, said)   # the vertices a shape moves are kept as they are
        for side in ('left', 'right'):       # the fingers are still there, to their ends
            tip = rig.placed(rig.joints[side][9])
            self.assertLess(min(math.dist(p, tip) for p in body.pos), 0.012)

    def test_pictures_are_kept_to_a_budget(self):
        rig = bodyfixtures.person()
        blocks = ('pelvis', 'belly', 'neck', 'leftThigh', 'rightThigh')
        rig.materials = [{'image': bodyfixtures.png(1024, 1024, (40 * n, 10, 10, 255))} for n in range(len(blocks))]
        rig.materials.append({'image': bodyfixtures.png(4096, 2048, (9, 9, 9, 255))})   # larger than any is kept
        rig.tri_material = {}
        for n, block in enumerate(blocks):
            rig.paint(block, n + 1 if n + 1 < len(blocks) else 0)
        rig.paint('head', len(blocks))
        body, said, _ = self.body_of('pictures.glb', fixtures.glb(rig))
        sizes = sorted(t[:2] for t in body.textures)
        self.assertEqual(sizes, [(512, 512)] * 2 + [(1024, 512)] + [(1024, 1024)] * 3)
        self.assertLessEqual(sum(len(t[2]) for t in body.textures), 16 << 20)
        self.assertEqual(said['look'], '6 materials, 6 pictures (16.0 MB)')
        for width, height, pixels in body.textures:   # made smaller, a picture is still the picture
            self.assertEqual(len(pixels), width * height * 4)
            self.assertEqual(pixels[:4], pixels[-4:])
            self.assertEqual(pixels[1:4], bytes((10, 10, 255)) if width == height else bytes((9, 9, 255)))

    def test_a_picture_is_no_larger_than_2048_a_side(self):
        rig = bodyfixtures.person(look=False)
        rig.materials = [{'image': bodyfixtures.png(4096, 2048, (9, 9, 9, 255))}]
        body, said, _ = self.body_of('large.glb', fixtures.glb(rig))
        self.assertEqual([t[:2] for t in body.textures], [(2048, 1024)])
        self.assertEqual(said['look'], '1 materials, 1 pictures (8.0 MB)')

    def test_a_picture_that_cannot_be_read_leaves_the_colour(self):
        rig = bodyfixtures.person()
        rig.materials[2]['image'] = b'\x89PNG\r\n\x1a\n' + bytes(40)
        body, said, _ = self.body_of('nopicture.glb', fixtures.glb(rig))
        self.assertEqual(said['look'], "4 materials, 2 pictures (0.0 MB), 1 couldn't be read")
        self.assertEqual([m['texture'] >= 0 for m in body.materials], [True, True, False, False])

    def test_an_avatar_without_fingers_keeps_the_hands_frametop_has(self):
        for vrm in (None, 1):
            with self.subTest(vrm=vrm):
                rig = bodyfixtures.person('unity', fingers=False)
                body, said, model = self.body_of('mittens.glb', fixtures.glb(rig, vrm=vrm, meta={'name': 'x'}))
                self.check_bones(body, rig)
                self.assertEqual(model.hands, {})
                self.assertIn('noleft', said)
                self.assertIn('noright', said)

    def test_a_skeleton_that_cannot_be_told_is_no_body(self):
        # a VRM whose humanoid lacks bones a body needs: the hands still come
        rig = bodyfixtures.person('opaque', without=('head', 'leftLowerLeg'))
        why = self.no_body('headless.vrm', fixtures.glb(rig, vrm=1, meta={'name': 'x'}))
        self.assertEqual(why, 'its skeleton has no bone that can be told as: head, leftLowerLeg')
        # the same by names: a leg that ends at the knee
        why = self.no_body('nofoot.glb', fixtures.glb(bodyfixtures.person('unity', without=('rightFoot', 'rightToes'))))
        self.assertIn('rightFoot', why)
        self.assertNotIn('leftFoot', why)
        # arms and hands alone (what a hand model is)
        self.assertIn('head', self.no_body('arms.glb', fixtures.glb(fixtures.make())))
        # lying down: which way it faces can't be told from its arms
        rig = bodyfixtures.person()
        rig.nodes = [(name, parent, (at[0], at[2], at[1])) for name, parent, at in rig.nodes]
        rig.verts = [((p[0], p[2], p[1]), w, n) for p, w, n in rig.verts]
        self.assertEqual(self.no_body('lying.glb', fixtures.glb(rig)), "it isn't standing upright")
        # more materials than a body may have
        rig = bodyfixtures.person(look=False)
        rig.materials = [{'color': (n / 40, 0.5, 0.5, 1)} for n in range(33)]
        for n in range(33):
            rig.block(f'patch{n}', [(rig.humanoid['hips'], 1.0)], (0.01 * n, 1.0, 0.1), (0.01 * n + 0.005, 1.01, 0.11))
            rig.paint(f'patch{n}', n)
        self.assertEqual(self.no_body('patches.glb', fixtures.glb(rig)), 'it has more than 32 materials')

    def test_damaged_files_end_in_an_error(self):
        rng = random.Random(3)
        rig = bodyfixtures.person('mixamo')
        for name, data in rig.pictures.items():
            self.source(name, data)
        for name, good in (('damaged.vrm', fixtures.glb(rig, vrm=1, meta={'name': 'x'})), ('damaged0.vrm', fixtures.glb(rig, vrm=0)),
                           ('damaged.fbx', fixtures.fbx(bodyfixtures.named_shapes(rig, OTHER_NAMES)).encode())):
            for n in range(FUZZ):
                data = bytearray(good)
                if n % 3 == 0:
                    del data[rng.randrange(len(data)):]
                else:
                    for _ in range(rng.choice((1, 1, 4, 32))):
                        data[rng.randrange(len(data))] = rng.randrange(256)
                status, _ = self.run_tool(self.source(name, bytes(data)), '--check')
                self.assertIn(status, (0, 1), f'{name} #{n}: ended with {status}')

    def test_a_vrm_whose_numbers_lie_ends_in_an_error(self):
        """A material's picture, a shape's mesh, a bone's node, a count: changed one at a time."""
        rng = random.Random(4)
        for version in (0, 1):
            good = fixtures.glb(bodyfixtures.person('opaque'), vrm=version, meta={'name': 'x'})
            size = struct.unpack_from('<I', good, 12)[0]
            doc, rest = json.loads(good[20:20 + size]), good[20 + size:]

            def numbers(value, found):
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
                data = (struct.pack('<4sII', b'glTF', 2, 20 + len(text) + len(rest)) + struct.pack('<I4s', len(text), b'JSON') +
                        text + rest)
                status, _ = self.run_tool(self.source('lies.vrm', data), '--check')
                self.assertIn(status, (0, 1), f'vrm {version} #{n} ({key}): ended with {status}')


if __name__ == '__main__':
    unittest.main()
