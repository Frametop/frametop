"""The MCP server's fake-peer tools (frame_testbench/peer_tools.py). Like test_mcp.py they need
the mcp package, and nothing else."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

from frame_testbench import cli


@unittest.skipUnless(importlib.util.find_spec('mcp') is not None, 'needs the mcp package (testbench/.venv)')
class ToolsTest(unittest.IsolatedAsyncioTestCase):
    async def test_each_tool_is_one_command_and_takes_nothing_else(self):
        from frame_testbench import mcp_server
        calls = []
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        server = mcp_server.create_server(mcp_server.Config(artifacts_dir=Path(tmp.name)), runner=lambda argv: calls.append(argv) or {'ok': True})
        cases = [('peer_start', {'peer': 1}, ['peer', '1', 'start']), ('peer_start', {'peer': 2, 'name': 'Bo B'}, ['peer', '2', 'start', '--name', 'Bo B']),
                 ('peer_stop', {'peer': 3}, ['peer', '3', 'stop']), ('peer_state', {'peer': 1}, ['peer', '1', 'state']),
                 ('peer_join', {'peer': 1}, ['peer', '1', 'join']), ('peer_leave', {'peer': 1}, ['peer', '1', 'leave']),
                 ('peer_pose', {'peer': 1, 'pose': 'type'}, ['peer', '1', 'pose', 'type']),
                 ('peer_walk', {'peer': 1, 'right': -1.5, 'back': 0.25}, ['peer', '1', 'walk', '--', '-1.5', '0.25', '0']),
                 ('peer_walk', {'peer': 1, 'right': 0.0, 'back': 2.0, 'degrees': -90.0}, ['peer', '1', 'walk', '--', '0.0', '2.0', '-90.0']),
                 ('peer_avatar', {'peer': 1, 'model': 'Robot one'}, ['peer', '1', 'wear', 'Robot one']), ('peer_avatar', {'peer': 1}, ['peer', '1', 'wear']),
                 ('peer_speak', {'peer': 1, 'hz': 220.0}, ['peer', '1', 'speak', '220.0']), ('peer_speak', {'peer': 1}, ['peer', '1', 'speak', 'off']),
                 ('peer_away', {'peer': 2, 'state': 'off'}, ['peer', '2', 'away', 'off'])]
        for name, args, expected in cases:
            with self.subTest(name=name, args=args):
                reply = await server.call_tool(name, args)
                self.assertFalse(reply.isError, reply)
                self.assertEqual(calls[-1], expected)
                cli.parser().parse_args(expected)   # the CLI takes what the tool makes
        before = len(calls)
        for name, args in [('peer_start', {'peer': 4}), ('peer_start', {'peer': 1, 'name': '--loopback'}), ('peer_start', {'peer': 1, 'loopback': True}),
                           ('peer_join', {'peer': 1, 'host': 'frame'}), ('peer_pose', {'peer': 1, 'pose': 'fly'}),
                           ('peer_walk', {'peer': 1, 'right': 21.0, 'back': 0.0}), ('peer_walk', {'peer': 1, 'right': '1', 'back': 0.0}),
                           ('peer_avatar', {'peer': 1, 'model': '../x'}), ('peer_avatar', {'peer': 1, 'model': '-x'}),
                           ('peer_speak', {'peer': 1, 'hz': 10.0}), ('peer_away', {'peer': 1, 'state': 'gone'}), ('peer_state', {'peer': '1'})]:
            with self.subTest(name=name, args=args):
                self.assertTrue((await server.call_tool(name, args)).isError)
        self.assertEqual(len(calls), before)


if __name__ == '__main__':
    unittest.main()
