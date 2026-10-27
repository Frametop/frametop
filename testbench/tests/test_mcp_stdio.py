"""Real SDK stdio round trips against the real CLI and a CPU-only observer fixture."""
import base64
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK_AVAILABLE = importlib.util.find_spec('mcp') is not None


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0')) + chunk(b'IEND', b''))


PNG = png()


@unittest.skipUnless(SDK_AVAILABLE, 'install .[mcp] for MCP tests')
class StdioTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        observer = self.directory / 'observer'
        observer.write_text(f'''#!{sys.executable}
import base64, json, sys
from pathlib import Path
print('fixture observer diagnostic', file=sys.stderr)
command = sys.argv[1]
if command == 'status':
    print(json.dumps({{'ok': True, 'frame_index': 42}}))
elif command == 'inputs':
    assert sys.argv[2].endswith('/resources/input-actions.json')
    print(json.dumps({{'ok': True, 'fixture_input_readback': True}}))
elif command == 'capture':
    out = Path(sys.argv[2])
    out.mkdir()
    for name in ('preview.png', 'stereo.png'):
        (out / name).write_bytes(base64.b64decode({base64.b64encode(PNG).decode()!r}))
    print(json.dumps({{'ok': True, 'screenshot_handle': 77}}))
else:
    print(json.dumps({{'ok': False, 'error': 'fixture backend refused setting'}}))
    raise SystemExit(1)
''')
        observer.chmod(0o700)
        self.env = dict(os.environ, FRAME_TESTBENCH_OBSERVER=str(observer),
                        FRAME_TESTBENCH_SOCKET=str(self.directory / 'absent.sock'),
                        PYTHONPATH=str(ROOT))

    async def test_initialize_list_call_validation_failure_images_and_stderr(self):
        import anyio
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        params = StdioServerParameters(command=sys.executable, args=[
            '-m', 'frame_testbench.mcp_server', '--artifacts-dir', str(self.directory / 'artifacts')],
            env=self.env, cwd=str(self.directory))
        log = self.directory / 'stderr.log'
        with anyio.fail_after(20), log.open('w') as errlog:
            async with stdio_client(params, errlog=errlog) as (read, write):
                async with ClientSession(read, write) as client:
                    initialized = await client.initialize()
                    self.assertEqual(initialized.serverInfo.name, 'frame-testbench')
                    tools = await client.list_tools()
                    self.assertEqual(len(tools.tools), 18)
                    inputs = await client.call_tool('controller_input_status', {})
                    self.assertFalse(inputs.isError, inputs)
                    self.assertTrue(inputs.structuredContent['fixture_input_readback'])
                    invalid_input = await client.call_tool('controller_trigger', {'side': 'left', 'value': True})
                    self.assertTrue(invalid_input.isError)
                    failed_input = await client.call_tool('controller_button', {'side': 'right', 'button': 'a', 'pressed': True})
                    self.assertTrue(failed_input.isError)
                    self.assertNotIn('unknown tool', failed_input.content[0].text)
                    status = await client.call_tool('status', {})
                    self.assertFalse(status.isError, status)
                    self.assertEqual(status.structuredContent['runtime']['frame_index'], 42)
                    invalid = await client.call_tool('worn', {'state': 'invalid'})
                    self.assertTrue(invalid.isError)
                    invalid = await client.call_tool('status', {'host': 'elsewhere'})
                    self.assertTrue(invalid.isError)
                    failed = await client.call_tool('compositor', {'mode': 'awake'})
                    self.assertTrue(failed.isError)
                    self.assertIn('fixture backend refused', failed.content[0].text)
                    capture = await client.call_tool('capture', {})
                    self.assertFalse(capture.isError, capture)
                    self.assertEqual(capture.structuredContent['screenshot_handle'], 77)
                    self.assertEqual([base64.b64decode(c.data) for c in capture.content if c.type == 'image'], [PNG, PNG])
        self.assertIn('fixture observer diagnostic', log.read_text())
        self.assertNotIn('Failed to parse JSONRPC', log.read_text())

    async def test_cli_failure_does_not_leak_to_stdout(self):
        try:
            from frame_testbench import mcp_server
        except ImportError:
            self.fail('MCP server module missing')
        self.assertTrue(hasattr(mcp_server, 'run_cli'), 'CLI subprocess bridge missing')
        import contextlib
        import io
        from unittest.mock import patch
        for response in (
            subprocess.CompletedProcess([], 0, 'not JSON', 'diagnostic'),
            subprocess.CompletedProcess([], 1, '{"ok":true}', 'diagnostic'),
            subprocess.CompletedProcess([], 0, '{"ok":false,"error":"refused"}', 'diagnostic'),
        ):
            stdout, stderr = io.StringIO(), io.StringIO()
            with (patch.object(mcp_server.subprocess, 'run', return_value=response) as run,
                  contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr),
                  self.assertRaises(RuntimeError)):
                mcp_server.run_cli(['status'])
            self.assertEqual(stdout.getvalue(), '')
            self.assertIn('diagnostic', stderr.getvalue())
            argv = run.call_args.args[0]
            self.assertEqual(argv[0], sys.executable)
            self.assertEqual(argv[-1], 'status')
            self.assertNotIn('shell', run.call_args.kwargs)
            self.assertGreater(run.call_args.kwargs['timeout'], 0)


if __name__ == '__main__':
    unittest.main()
