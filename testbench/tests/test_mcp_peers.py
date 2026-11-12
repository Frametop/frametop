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
                 ('peer_away', {'peer': 2, 'state': 'off'}, ['peer', '2', 'away', 'off']),
                 ('peer_point', {'peer': 1, 'panel': 8, 'x': 0.25, 'y': 1.0}, ['peer', '1', 'point', '8', '0.25', '1.0']),
                 ('peer_point', {'peer': 1, 'panel': 0, 'down': True}, ['peer', '1', 'point', '0', '0.5', '0.5', '--down']),
                 ('peer_point', {'peer': 1}, ['peer', '1', 'point', 'off']),
                 ('peer_look', {'peer': 1, 'yaw': -20.0, 'pitch': 5.5}, ['peer', '1', 'look', '--', '-20.0', '5.5']),
                 ('peer_look', {'peer': 1, 'yaw': 0.0}, ['peer', '1', 'look', '--', '0.0', '0']), ('peer_look', {'peer': 1}, ['peer', '1', 'look', 'off']),
                 ('peer_control_ask', {'peer': 2, 'panel': 31}, ['peer', '2', 'ask', '31']),
                 ('peer_act_move', {'peer': 1, 'panel': 8, 'x': 0.0, 'y': 1.0}, ['peer', '1', 'act', '8', 'move', '0.0', '1.0']),
                 ('peer_act_button', {'peer': 1, 'panel': 8, 'button': 3, 'down': True}, ['peer', '1', 'act', '8', 'button', '3', '1']),
                 ('peer_act_button', {'peer': 1, 'panel': 8, 'button': 1, 'down': False}, ['peer', '1', 'act', '8', 'button', '1', '0']),
                 ('peer_act_scroll', {'peer': 1, 'panel': 8, 'down': -120}, ['peer', '1', 'act', '8', 'scroll', '--', '0', '-120']),
                 ('peer_act_scroll', {'peer': 1, 'panel': 8, 'right': 2400, 'down': 240}, ['peer', '1', 'act', '8', 'scroll', '--', '2400', '240']),
                 ('peer_act_key', {'peer': 1, 'panel': 8, 'code': 30, 'down': True}, ['peer', '1', 'act', '8', 'key', '30', '1']),
                 ('peer_put', {'peer': 1, 'panel': 8, 'x': -1.0, 'y': 1.5, 'z': -2.0}, ['peer', '1', 'put', '8', '--', '-1.0', '1.5', '-2.0', '0']),
                 ('peer_put', {'peer': 1, 'panel': 8, 'x': 0.0, 'y': 0.0, 'z': 0.0, 'degrees': -90.0}, ['peer', '1', 'put', '8', '--', '0.0', '0.0', '0.0', '-90.0']),
                 ('peer_share', {'peer': 1}, ['peer', '1', 'share', '--size', '1280x720']),
                 ('peer_share', {'peer': 2, 'width': 1920, 'height': 1080}, ['peer', '2', 'share', '--size', '1920x1080']),
                 ('peer_share', {'peer': 1, 'width': 640, 'height': 360, 'x': -1.0}, ['peer', '1', 'share', '--size', '640x360', '--at', '-1.0', '1.5', '-2.5', '--turn', '0']),
                 ('peer_share', {'peer': 1, 'x': 0.0, 'y': -1.0, 'z': 2.0, 'degrees': -90.0},
                  ['peer', '1', 'share', '--size', '1280x720', '--at', '0.0', '-1.0', '2.0', '--turn', '-90.0']),
                 ('peer_unshare', {'peer': 3}, ['peer', '3', 'unshare']), ('peer_control_give', {'peer': 1, 'person': 0}, ['peer', '1', 'give', '0']),
                 ('peer_control_give', {'peer': 1, 'person': 3}, ['peer', '1', 'give', '3']), ('peer_control_take', {'peer': 1}, ['peer', '1', 'take']),
                 ('peer_control_take', {'peer': 1, 'person': 0}, ['peer', '1', 'take', '0'])]
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
                           ('peer_speak', {'peer': 1, 'hz': 10.0}), ('peer_away', {'peer': 1, 'state': 'gone'}), ('peer_state', {'peer': '1'}),
                           ('peer_point', {'peer': 1, 'panel': 32}), ('peer_point', {'peer': 1, 'panel': 8, 'x': 1.5}), ('peer_point', {'peer': 1, 'panel': 8, 'down': 1}),
                           ('peer_point', {'peer': 1, 'panel': 8, 'socket': 'ft_together'}), ('peer_look', {'peer': 1, 'yaw': 64.0}),
                           ('peer_look', {'peer': 1, 'yaw': 0.0, 'pitch': -70.0}), ('peer_control_ask', {'peer': 1}), ('peer_control_ask', {'peer': 1, 'panel': -1}),
                           ('peer_control_ask', {'peer': 1, 'panel': 8, 'give': True}), ('peer_act_move', {'peer': 1, 'panel': 8, 'x': 0.5}),
                           ('peer_act_move', {'peer': 1, 'panel': 8, 'x': -0.1, 'y': 0.0}), ('peer_act_button', {'peer': 1, 'panel': 8, 'button': 4, 'down': True}),
                           ('peer_act_button', {'peer': 1, 'panel': 8, 'button': 1, 'down': 1}), ('peer_act_scroll', {'peer': 1, 'panel': 8, 'down': 2401}),
                           ('peer_act_scroll', {'peer': 1, 'panel': 8, 'down': 1.5}), ('peer_act_key', {'peer': 1, 'panel': 8, 'code': 125, 'down': True}),
                           ('peer_act_key', {'peer': 1, 'panel': 8, 'code': 0, 'down': True}), ('peer_put', {'peer': 1, 'panel': 8, 'x': 101.0, 'y': 0.0, 'z': 0.0}),
                           ('peer_put', {'peer': 1, 'panel': 8, 'x': 0.0, 'y': 0.0}), ('peer_put', {'peer': 1, 'panel': 8, 'x': 0.0, 'y': 0.0, 'z': 0.0, 'degrees': 400.0}),
                           ('peer_share', {'peer': 1, 'width': 1366, 'height': 768}), ('peer_share', {'peer': 1, 'width': 1280, 'height': 724}),
                           ('peer_share', {'peer': 1, 'height': 352}), ('peer_share', {'peer': 1, 'height': 1208}), ('peer_share', {'peer': 1, 'width': '1280'}),
                           ('peer_share', {'peer': 1, 'x': 101.0}), ('peer_share', {'peer': 1, 'x': 0.0, 'degrees': 361.0}), ('peer_share', {'peer': 1, 'panel': 1}),
                           ('peer_share', {'peer': 4}), ('peer_unshare', {'peer': 1, 'panel': 0}), ('peer_control_give', {'peer': 1}),
                           ('peer_control_give', {'peer': 1, 'person': 4}), ('peer_control_give', {'peer': 1, 'person': -1}),
                           ('peer_control_give', {'peer': 1, 'person': 0, 'panel': 8}), ('peer_control_take', {'peer': 1, 'person': 4}),
                           ('peer_control_take', {'peer': 1, 'person': '0'})]:
            with self.subTest(name=name, args=args):
                self.assertTrue((await server.call_tool(name, args)).isError)
        self.assertEqual(len(calls), before)


if __name__ == '__main__':
    unittest.main()
