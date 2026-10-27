import math
import unittest


class PoseTests(unittest.TestCase):
    def module(self):
        try:
            from frame_testbench import pose
        except ImportError:
            self.fail('pose math implementation missing')
        return pose

    def test_identity(self):
        p = self.module()
        self.assertEqual(p.from_euler([1, 2, 3], [0, 0, 0]),
                         {'position': [1., 2., 3.], 'quaternion': [1., 0., 0., 0.]})

    def test_yaw_about_up(self):
        p = self.module()
        q = p.from_euler([0, 0, 0], [90, 0, 0])['quaternion']
        self.assertAlmostEqual(q[0], math.sqrt(.5))
        self.assertAlmostEqual(q[2], math.sqrt(.5))
        v = p.rotate(q, [0, 0, -1])
        for actual, expected in zip(v, [-1, 0, 0]):
            self.assertAlmostEqual(actual, expected)

    def test_pitch_about_right(self):
        p = self.module()
        q = p.from_euler([0, 0, 0], [0, 90, 0])['quaternion']
        for actual, expected in zip(p.rotate(q, [0, 0, -1]), [0, 1, 0]):
            self.assertAlmostEqual(actual, expected)

    def test_world_translation_and_rotation(self):
        p = self.module()
        original = p.from_euler([1, 2, 3], [0, 0, 0])
        result = p.offset(original, [2, 0, -1], [90, 0, 0])
        self.assertEqual(result['position'], [3., 2., 2.])
        for a, b in zip(result['quaternion'], p.from_euler([0, 0, 0], [90, 0, 0])['quaternion']):
            self.assertAlmostEqual(a, b)
        self.assertEqual(original['position'], [1., 2., 3.])

    def test_matrix_roundtrip(self):
        p = self.module()
        original = p.from_euler([.25, 1.6, -.75], [120, -30, 40])
        matrix = p.to_matrix(original)
        restored = p.from_matrix(matrix)
        for a, b in zip(p.to_matrix(restored), matrix):
            self.assertAlmostEqual(a, b, places=6)

    def test_reference_space_conversion(self):
        p = self.module()
        raw_to_standing = p.from_euler([0, 1.6, 0], [30, 0, 0])
        desired = p.from_euler([.3, 1.7, -.2], [90, -20, 0])
        raw = p.compose(p.inverse(raw_to_standing), desired)
        actual = p.compose(raw_to_standing, raw)
        for a, b in zip(p.to_matrix(actual), p.to_matrix(desired)):
            self.assertAlmostEqual(a, b, places=7)

    def test_invalid_values(self):
        p = self.module()
        for pos, rot in [([0, 0], [0, 0, 0]), ([0, float('nan'), 0], [0, 0, 0]),
                         ([0, 0, 0], [float('inf'), 0, 0]), ([True, 0, 0], [0, 0, 0])]:
            with self.assertRaises(ValueError):
                p.from_euler(pos, rot)
        with self.assertRaises(ValueError):
            p.normalize([0, 0, 0, 0])
        with self.assertRaises(ValueError):
            p.from_matrix([0] * 12)


if __name__ == '__main__':
    unittest.main()
