import json
import os
import socket
import tempfile
import threading
import unittest
from pathlib import Path


class TransportTests(unittest.TestCase):
    def module(self):
        try:
            from frame_testbench import transport
        except ImportError:
            self.fail('transport missing')
        return transport

    def test_socket_request_and_failure(self):
        mod = self.module()
        with tempfile.TemporaryDirectory() as tmp:
            path = str(Path(tmp) / 'control.sock')
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            server.listen()
            seen = []
            def serve():
                with server:
                    for payload in [{'ok': True, 'sequence': 42}, {'ok': False, 'error': 'not ready'}]:
                        with server.accept()[0] as client:
                            seen.append(client.recv(4096))
                            client.sendall(json.dumps(payload).encode() + b'\n')
            thread = threading.Thread(target=serve)
            thread.start()
            self.assertEqual(mod.request(path, 'status')['sequence'], 42)
            with self.assertRaisesRegex(RuntimeError, 'not ready'):
                mod.request(path, 'worn 1')
            thread.join(2)
            self.assertEqual(seen, [b'status\n', b'worn 1\n'])

    def test_reject_multiline_and_timeout(self):
        mod = self.module()
        with self.assertRaises(ValueError):
            mod.request('/nonexistent', 'status\nrelease')
        with tempfile.TemporaryDirectory() as tmp:
            path = str(Path(tmp) / 's')
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path)
                server.listen()
                with self.assertRaises(TimeoutError):
                    mod.request(path, 'status', timeout=.02)

    def test_json_process(self):
        mod = self.module()
        import sys
        self.assertEqual(mod.run_json([sys.executable, '-c', 'print(\'{"ok":true,"n":7}\')'])['n'], 7)
        with self.assertRaises(RuntimeError):
            mod.run_json([sys.executable, '-c', 'print("not json")'])
        with self.assertRaises(RuntimeError):
            mod.run_json([sys.executable, '-c', 'import sys; print(\'{"ok":false,"error":"broken"}\'); sys.exit(1)'])

    def test_remote_arguments_quoted_and_host_checked(self):
        mod = self.module()
        import shlex
        argv = mod.remote_argv('frame', '/home/test/a b', ['pose', 'set', '--position', '-1', '0', '1'])
        self.assertIn('StrictHostKeyChecking=yes', argv)
        self.assertIn('BatchMode=yes', argv)
        command = shlex.split(argv[-1])
        self.assertEqual(command[:2], ['python3', '/home/test/a b/frame-testbench'])
        self.assertEqual(command[-3:], ['-1', '0', '1'])
        for host in ['-oProxyCommand=oops', 'frame\nother', 'frame;oops']:
            with self.assertRaises(ValueError):
                mod.remote_argv(host, '/project', ['status'])


if __name__ == '__main__':
    unittest.main()
