"""Packaging metadata and checkout launcher contract, independent of the optional SDK."""
import subprocess
import sys
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    def test_explicit_package_discovery_optional_sdk_and_mit_metadata(self):
        path = ROOT / 'pyproject.toml'
        self.assertTrue(path.is_file(), 'packaging metadata missing')
        config = tomllib.loads(path.read_text())
        project = config['project']
        self.assertEqual(project['license'], 'MIT')
        self.assertEqual(project['requires-python'], '>=3.11')
        self.assertEqual(project.get('dependencies', []), [])
        self.assertIn('mcp', project['optional-dependencies'])
        self.assertEqual(project['scripts']['frame-testbench'], 'frame_testbench.cli:main')
        self.assertEqual(project['scripts']['frame-testbench-mcp'], 'frame_testbench.mcp_server:main')
        self.assertEqual(config['tool']['setuptools']['packages']['find']['include'], ['frame_testbench'])

    def test_checkout_mcp_launcher(self):
        import importlib.util
        if importlib.util.find_spec('mcp') is None:
            self.skipTest('install .[mcp] for launcher test')
        script = ROOT / 'frame-testbench-mcp'
        self.assertTrue(script.is_file(), 'checkout MCP launcher missing')
        self.assertTrue(script.stat().st_mode & 0o111)
        p = subprocess.run([sys.executable, str(script), '--help'], capture_output=True, text=True, timeout=10, check=False)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('--host', p.stdout)
        self.assertIn('--remote-root', p.stdout)
        self.assertIn('--artifacts-dir', p.stdout)
        for args in (['--socket', '/other'], ['--transport', 'http'], ['--host=-oProxyCommand=bad']):
            p = subprocess.run([sys.executable, str(script), *args], capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(p.returncode, 2, p)
            self.assertEqual(p.stdout, '')
            self.assertIn('error:', p.stderr)


if __name__ == '__main__':
    unittest.main()
