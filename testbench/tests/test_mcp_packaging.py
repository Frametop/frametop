"""The checkout MCP launcher's contract."""
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    def test_checkout_mcp_launcher(self):
        import importlib.util
        if importlib.util.find_spec('mcp') is None:
            self.skipTest('needs the mcp package (testbench/.venv)')
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
