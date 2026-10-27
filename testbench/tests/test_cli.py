import json
import tempfile
import unittest
from pathlib import Path


class CliTests(unittest.TestCase):
    def module(self):
        try:
            from frame_testbench import cli
        except ImportError:
            self.fail('CLI missing')
        return cli

    def bench(self):
        cli = self.module()
        from frame_testbench import pose
        class Fake(cli.Bench):
            def __init__(self):
                self.commands = []
                self.current = pose.from_euler([0, 0, 0], [0, 0, 0])
                self.on = False
                self.controllers = {side: {'device_index': index, 'pose_override': False,
                                          'pose': pose.from_euler([0, 0, 0], [0, 0, 0]),
                                          'synthetic': True}
                                    for side, index in [('left', 4), ('right', 7)]}
                self.transform = [[1, 0, 0, 0], [0, 1, 0, 1.6], [0, 0, 1, 0]]
            def native(self, *args):
                self.commands.append(('native', args))
                return {'transforms': {'raw_to_standing': self.transform}}
            def control(self, text):
                self.commands.append(('control', text))
                bits = text.split()
                if bits[0] == 'pose':
                    values = list(map(float, bits[1:]))
                    self.current = {'position': values[:3], 'quaternion': values[3:]}
                    self.on = True
                if bits[0] == 'controller-pose':
                    values = list(map(float, bits[2:]))
                    self.controllers[bits[1]].update(pose_override=True,
                        pose={'position': values[:3], 'quaternion': values[3:]})
                if bits[0] == 'controller-release':
                    self.controllers[bits[1]]['pose_override'] = False
                return {'ok': True, 'pose_override': self.on, 'pose': self.current,
                        'controllers': self.controllers}
        return Fake()

    def test_controller_input_commands_are_single_atomic_batches(self):
        cli = self.module()
        common = ['system', 'bumper', 'trigger', 'grip', 'thumbstick']
        for side, names in [('left', ['view', 'dpad_up', 'dpad_right', 'dpad_down', 'dpad_left']),
                            ('right', ['menu', 'a', 'b', 'x', 'y'])]:
            cases = [
                (['button', name, 'on', '--touch', 'off'],
                 f'/input/{name}/click 1 /input/{name}/touch 0') for name in common + names]
            cases += [(['touch', name, 'on'], f'/input/{name}/touch 1')
                      for name in common + names + ['thumbrest']]
            cases += [
                (['button', 'system', 'off'], '/input/system/click 0'),
                (['trigger', '0.5'], '/input/trigger/value 0.5'),
                (['grip', '1', '--click', 'off', '--touch', 'on'],
                 '/input/grip/value 1 /input/grip/click 0 /input/grip/touch 1'),
                (['trigger', '0', '--click', 'on'], '/input/trigger/value 0 /input/trigger/click 1'),
                (['thumbstick', '-1', '1', '--touch', 'off', '--click', 'on'],
                 '/input/thumbstick/x -1 /input/thumbstick/y 1 /input/thumbstick/click 1 /input/thumbstick/touch 0'),
                (['thumbstick', '0', '0'], '/input/thumbstick/x 0 /input/thumbstick/y 0'),
            ]
            for argv, expected in cases:
                with self.subTest(side=side, argv=argv):
                    bench = self.bench()
                    cli.dispatch(cli.parser().parse_args(['controller', side, *argv]), bench)
                    self.assertEqual(bench.commands, [('control', f'controller-inputs {side} {expected}')])
            bench = self.bench()
            cli.dispatch(cli.parser().parse_args(['controller', side, 'inputs-reset']), bench)
            self.assertEqual(bench.commands, [('control', 'controller-input-release ' + side)])

    def test_invalid_input_batches_fail_before_local_or_remote_io(self):
        import contextlib
        import io
        from unittest.mock import patch
        cli = self.module()
        cases = [
            ['left', 'button', name, 'on'] for name in ['a', 'b', 'x', 'y', 'menu', 'thumbrest', 'unknown']]
        cases += [['right', 'touch', name, 'off'] for name in
                  ['view', 'dpad_up', 'dpad_right', 'dpad_down', 'dpad_left', 'unknown']]
        cases += [[side, op, value] for side in ['left', 'right']
                  for op in ['trigger', 'grip'] for value in ['nan', 'inf', '-0.01', '1.01']]
        cases += [['left', 'thumbstick', x, y] for x, y in
                  [('nan', '0'), ('0', 'inf'), ('-1.01', '0'), ('0', '1.01')]]
        for argv in cases:
            with self.subTest(argv=argv):
                bench = self.bench()
                args = cli.parser().parse_args(['controller', *argv])
                with self.assertRaises(ValueError):
                    cli.dispatch(args, bench)
                self.assertEqual(bench.commands, [])
                with patch.object(cli.transport, 'run_json') as run, contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(cli.main(['--host', 'frame', 'controller', *argv]), 1)
                run.assert_not_called()

    def test_bool_analog_values_rejected_before_io(self):
        cli, bench = self.module(), self.bench()
        args = cli.parser().parse_args(['controller', 'left', 'trigger', '0'])
        args.value = True
        with self.assertRaises(ValueError):
            cli.dispatch(args, bench)
        self.assertEqual(bench.commands, [])

    def test_inputs_reads_observer_without_control_socket(self):
        cli, bench = self.module(), self.bench()
        result = cli.dispatch(cli.parser().parse_args(['inputs']), bench)
        self.assertEqual(bench.commands, [('native', ('inputs', str(cli.ROOT / 'resources/input-actions.json')))])
        self.assertIn('transforms', result)

    def test_compositor_mode(self):
        cli, bench = self.module(), self.bench()
        cli.dispatch(cli.parser().parse_args(['compositor', 'awake']), bench)
        self.assertEqual(bench.commands[-1], ('native', ('setting', 'false')))
        cli.dispatch(cli.parser().parse_args(['compositor', 'auto']), bench)
        self.assertEqual(bench.commands[-1], ('native', ('setting', 'true')))

    def test_worn_and_release(self):
        cli, bench = self.module(), self.bench()
        for command, expected in [(['worn', 'on'], 'worn 1'), (['worn', 'off'], 'worn 0'),
                                  (['worn', 'physical'], 'worn-release'), (['release'], 'release')]:
            cli.dispatch(cli.parser().parse_args(command), bench)
            self.assertEqual(bench.commands[-1], ('control', expected))

    def test_absolute_standing_pose_is_converted_to_raw(self):
        cli, bench = self.module(), self.bench()
        result = cli.dispatch(cli.parser().parse_args(['pose', 'set', '--position', '1', '1.8', '-2',
                                                      '--rotation', '90', '0', '0']), bench)
        for a, b in zip(result['pose']['position'], [1, .2, -2]):
            self.assertAlmostEqual(a, b)
        self.assertTrue(result['pose_override'])

    def test_move_world_space_and_reset(self):
        cli, bench = self.module(), self.bench()
        args = cli.parser().parse_args(['pose', 'move', '--translation', '.25', '0', '0'])
        with self.assertRaisesRegex(ValueError, 'set'):
            cli.dispatch(args, bench)
        cli.dispatch(cli.parser().parse_args(['pose', 'set', '--position', '0', '1.6', '0']), bench)
        result = cli.dispatch(args, bench)
        self.assertEqual(result['pose']['position'], [.25, 0, 0])
        cli.dispatch(cli.parser().parse_args(['pose', 'reset']), bench)
        self.assertEqual(bench.commands[-1], ('control', 'pose-release'))

    def test_invalid_pose_never_calls_backend(self):
        cli, bench = self.module(), self.bench()
        args = cli.parser().parse_args(['pose', 'set', '--position', 'nan', '0', '0'])
        with self.assertRaises(ValueError):
            cli.dispatch(args, bench)
        self.assertEqual(bench.commands, [])

    def test_controller_set_raw_targets_only_selected_side(self):
        from frame_testbench import pose
        for side, other in [('left', 'right'), ('right', 'left')]:
            with self.subTest(side=side):
                cli, bench = self.module(), self.bench()
                result = cli.dispatch(cli.parser().parse_args([
                    'controller', side, 'set', '--position', '1', '2', '-3',
                    '--rotation', '90', '30', '15', '--space', 'raw']), bench)
                expected = pose.from_euler([1, 2, -3], [90, 30, 15])
                self.assertEqual(result['controllers'][side]['pose'], expected)
                self.assertTrue(result['controllers'][side]['pose_override'])
                self.assertFalse(result['controllers'][other]['pose_override'])
                self.assertFalse(result['pose_override'])
                self.assertEqual(len(bench.commands), 1)
                self.assertTrue(bench.commands[0][1].startswith('controller-pose ' + side + ' '))

    def test_controller_set_standing_uses_runtime_rotation_and_translation(self):
        cli, bench = self.module(), self.bench()
        # Raw -> standing is yaw +90 degrees and translation [10, 2, 20].
        bench.transform = [[0, 0, 1, 10], [0, 1, 0, 2], [-1, 0, 0, 20]]
        result = cli.dispatch(cli.parser().parse_args([
            'controller', 'left', 'set', '--position', '11', '3', '18',
            '--rotation', '90', '0', '0']), bench)
        actual = result['controllers']['left']['pose']
        for a, b in zip(actual['position'] + actual['quaternion'], [2, 1, 1, 1, 0, 0, 0]):
            self.assertAlmostEqual(a, b)
        self.assertEqual(bench.commands[0], ('native', ('status',)))

    def test_controller_move_requires_selected_side_override(self):
        for space in ('raw', 'standing'):
            with self.subTest(space=space):
                cli, bench = self.module(), self.bench()
                bench.on = True
                bench.controllers['left']['pose_override'] = True
                args = cli.parser().parse_args(['controller', 'right', 'move', '--space', space])
                with self.assertRaisesRegex(ValueError, 'controller right.*set'):
                    cli.dispatch(args, bench)
                self.assertEqual(bench.commands, [('control', 'status')])

    def test_controller_move_uses_world_axes_in_selected_space(self):
        from frame_testbench import pose
        for space in ('raw', 'standing'):
            with self.subTest(space=space):
                cli, bench = self.module(), self.bench()
                bench.transform = [[0, 0, 1, 10], [0, 1, 0, 2], [-1, 0, 0, 20]]
                cli.dispatch(cli.parser().parse_args([
                    'controller', 'right', 'set', '--position', '1', '2', '3',
                    '--rotation', '0', '90', '0', '--space', 'raw']), bench)
                bench.commands.clear()
                result = cli.dispatch(cli.parser().parse_args([
                    'controller', 'right', 'move', '--translation', '1', '0', '0',
                    '--rotation', '0', '0', '90', '--space', space]), bench)
                actual = result['controllers']['right']['pose']
                # Standing +X is raw +Z. Rotation is pre-multiplied in world space.
                expected_position = [2, 2, 3] if space == 'raw' else [1, 2, 4]
                for a, b in zip(actual['position'], expected_position):
                    self.assertAlmostEqual(a, b)
                delta = pose.from_euler([0, 0, 0], [0, 0, 90] if space == 'raw' else [0, -90, 0])
                expected_q = pose.multiply(delta['quaternion'], pose.from_euler([0, 0, 0], [0, 90, 0])['quaternion'])
                for a, b in zip(actual['quaternion'], expected_q):
                    self.assertAlmostEqual(a, b)
                self.assertEqual(bench.commands[0], ('control', 'status'))
                self.assertEqual(sum(kind == 'native' for kind, _ in bench.commands), int(space == 'standing'))
                self.assertFalse(result['controllers']['left']['pose_override'])
                self.assertFalse(result['pose_override'])

    def test_controller_reset_sends_only_selected_release(self):
        for side in ('left', 'right'):
            with self.subTest(side=side):
                cli, bench = self.module(), self.bench()
                cli.dispatch(cli.parser().parse_args(['controller', side, 'reset']), bench)
                self.assertEqual(bench.commands, [('control', 'controller-release ' + side)])

    def test_invalid_controller_numbers_never_call_backend(self):
        for command, option in [('set', '--position'), ('set', '--rotation'),
                                ('move', '--translation'), ('move', '--rotation')]:
            for value in ('nan', 'inf', '-inf'):
                with self.subTest(command=command, option=option, value=value):
                    cli, bench = self.module(), self.bench()
                    args = ['controller', 'left', command]
                    if command == 'set' and option != '--position':
                        args += ['--position', '0', '0', '0']
                    # argparse recognizes negative numeric literals, but not -inf.
                    args += [option, value if value != '-inf' else '-' + '9' * 400, '0', '0']
                    parsed = cli.parser().parse_args(args)
                    with self.assertRaises(ValueError):
                        cli.dispatch(parsed, bench)
                    self.assertEqual(bench.commands, [])

    def test_capture_native_owns_directory_creation(self):
        cli = self.module()
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'fresh'
            bench = cli.Bench()
            def native(*args):
                if args[0] == 'status':
                    return {'compositor': {'frame_index': 1}, 'hmd': {'poses': {}}}
                self.assertEqual(args[0], 'capture')
                directory = Path(args[1])
                self.assertFalse(directory.exists())
                directory.mkdir()
                for name in ('preview.png', 'stereo.png'):
                    (directory / name).write_bytes(b'fixture')
                return {'ok': True}
            with patch.object(bench, 'native', side_effect=native):
                result = bench.capture(str(out))
            self.assertEqual(result['files']['stereo.png'], str(out / 'stereo.png'))
            self.assertIn('runtime_before', result)
            self.assertIn('runtime_after', result)

    def test_remote_capture_fetch_writes_local_artifacts(self):
        cli = self.module()
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'capture'
            reply = {'ok': True, 'output': '/remote/artifacts/capture-123',
                     'files': {'stereo.png': '/remote/artifacts/capture-123/stereo.png',
                               'preview.png': '/remote/artifacts/capture-123/preview.png'}}
            calls = []
            def copy(argv, **kwargs):
                calls.append(argv)
                Path(argv[-1]).write_bytes(b'\x89PNG\r\n\x1a\nfixture')
            with patch.object(cli.subprocess, 'run', side_effect=copy):
                result = cli.fetch_capture('frame', reply, out)
            self.assertEqual(len(calls), 2)
            self.assertEqual(Path(result['local_files']['stereo.png']).parent, out)
            self.assertTrue((out / 'result.json').is_file())
            with self.assertRaises(FileExistsError):
                cli.fetch_capture('frame', reply, out)

    def test_status_without_proxy_still_observes(self):
        cli, bench = self.module(), self.bench()
        def refused(text):
            raise FileNotFoundError('not installed')
        bench.control = refused
        result = cli.dispatch(cli.parser().parse_args(['status']), bench)
        self.assertIn('raw_to_standing', result['runtime']['transforms'])
        self.assertFalse(result['input']['available'])


if __name__ == '__main__':
    unittest.main()
