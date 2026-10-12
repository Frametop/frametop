#!/usr/bin/env python3
"""Filesystem/ownership lifecycle checks; never contact systemd or a headset."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('passthrough_run', HERE / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


@unittest.skipUnless(sys.platform == "linux", "systemd environment paths are Linux-specific")
class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'build'
        self.output.mkdir()
        for name in ('libft_passthrough.so', 'libft_passthrough_gate.so', 'VkLayer_FT_passthrough.json'):
            (self.output / name).touch()
        self.dropin = self.root / 'config/steamvr.service.d/99-ft-passthrough.conf'
        self.bridge = self.root / 'config/frametop-passthrough.service'
        self.state = self.root / 'config/state.json'
        self.legacy_dropin = self.root / 'runtime/99-ft-passthrough.conf'
        self.legacy_state = self.output / 'launch-state.private.json'
        self.calls = []
        self.env = 'LD_PRELOAD=/other.so VK_ADD_LAYER_PATH=/other-layer VK_INSTANCE_LAYERS=VK_OTHER'
        def command(*args, **kwargs):
            self.calls.append(args)
            return subprocess.CompletedProcess(args, 0, '', '')
        def prop(unit, name):
            if name == 'Environment':
                return self.env
            return 'not-found' if name == 'LoadState' else 'inactive'
        for target, value in (('command', command), ('property_value', prop),
                              ('snapshot_desktop', lambda: {'environment': {'SECRET_SESSION': 'old'}}),
                              ('restore_session', lambda value: None)):
            context = patch.object(run, target, value)
            context.start()
            self.addCleanup(context.stop)

    def install(self, minutes=0):
        run.install(self.output, self.dropin, self.bridge, self.state,
                    self.legacy_dropin, self.legacy_state, minutes)

    def disable(self):
        run.disable(self.dropin, self.bridge, self.state, self.legacy_dropin, self.legacy_state)

    def test_persistent_install_preserves_other_layers_not_desktop_environment(self):
        self.install()
        self.assertIn('/other.so', self.dropin.read_text())
        self.assertIn('VK_OTHER', self.dropin.read_text())
        unit = self.bridge.read_text()
        for field in ('WantedBy=steamvr.service', 'PartOf=steamvr.service', 'Restart=on-failure'):
            self.assertIn(field, unit)
        self.assertNotIn('SECRET_SESSION', self.state.read_text())
        self.assertIn(('systemctl', '--user', 'enable', run.BRIDGE), self.calls)

    def test_migrates_exact_legacy_override_without_duplicating_own_environment(self):
        content = run.launch_dropin(self.output)
        self.legacy_dropin.parent.mkdir()
        self.legacy_dropin.write_text(content)
        run.save_state(self.legacy_state, {'dropin_path': str(self.legacy_dropin), 'dropin': content,
                                         'desktop': {'environment': {'SECRET_SESSION': 'stale'}}})
        self.env = ('LD_PRELOAD=' + str(self.output / 'libft_passthrough_gate.so') + ':/other.so '
                    'VK_ADD_LAYER_PATH=' + str(self.output) + ':/other-layer '
                    'VK_INSTANCE_LAYERS=VK_LAYER_FT_passthrough:VK_OTHER')
        self.install()
        self.assertFalse(self.legacy_dropin.exists())
        self.assertFalse(self.legacy_state.exists())
        self.assertEqual(self.dropin.read_text().count('libft_passthrough_gate.so'), 1)
        self.assertEqual(self.dropin.read_text().count('VK_LAYER_FT_passthrough'), 1)
        self.assertNotIn('SECRET_SESSION', self.state.read_text())

    def test_unknown_override_is_not_overwritten(self):
        self.dropin.parent.mkdir(parents=True)
        self.dropin.write_text('unrelated')
        with self.assertRaises(RuntimeError):
            self.install()
        self.assertEqual(self.dropin.read_text(), 'unrelated')
        self.assertFalse(self.calls)

    def test_discovers_verified_old_output_when_switching_builds(self):
        content = run.launch_dropin(self.output)
        self.legacy_dropin.parent.mkdir()
        self.legacy_dropin.write_text(content)
        run.save_state(self.legacy_state, {'dropin_path': str(self.legacy_dropin), 'dropin': content})
        fallback = self.root / 'new-build/launch-state.private.json'
        self.assertEqual(run.legacy_state_file(self.legacy_dropin, fallback), self.legacy_state)
        self.legacy_dropin.write_text(content + '# changed externally\n')
        with self.assertRaises(RuntimeError):
            run.legacy_state_file(self.legacy_dropin, fallback)

    def test_changed_owned_unit_is_not_removed(self):
        self.install()
        self.bridge.write_text(self.bridge.read_text() + '# external edit\n')
        self.calls.clear()
        with self.assertRaises(RuntimeError):
            self.disable()
        self.assertTrue(self.dropin.exists())
        self.assertFalse(self.calls)

    def test_disable_after_boot_does_not_need_legacy_state(self):
        self.install()
        self.disable()
        for path in (self.dropin, self.bridge, self.state):
            self.assertFalse(path.exists())
        self.assertIn(('systemctl', '--user', 'disable', run.BRIDGE), self.calls)

    def test_trial_rollback_uses_global_install_not_output_specific_state(self):
        self.install(minutes=10)
        timer = next(c for c in self.calls if c[0] == 'systemd-run')
        self.assertIn('--on-active=10m', timer)
        self.assertEqual(timer[-1], 'disable')
        self.assertNotIn('--output', timer)

    def test_bridge_paths_escape_systemd_specifiers_and_variables(self):
        content = run.bridge_unit(Path('/tmp/mask %n $HOME'))
        self.assertIn('%%n $$HOME', content)


if __name__ == '__main__':
    unittest.main()
