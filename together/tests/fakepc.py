"""A stand-in for a PC's streaming host, as far as ft-roompanels asks it (roompcs-test.py): the
Vibepollo fork's /api/frametop/together over HTTPS, with a certificate and a token of its own.
Called in, it runs the real ft-pcsend with a loop of pictures, which calls the Frame as the
host's own sender does.
"""
import http.server
import json
import os
import secrets
import ssl
import subprocess
import threading

from roomkit import Tool  # (first: it puts conf/ on the path)
from roompanels import pclink

import ft_hostapi  # isort: skip


class Pc:
    def __init__(self, root, pcsend, loop):
        self.pcsend, self.loop = pcsend, loop
        self.out = {}        # a window's address -> the size of the output it is out on
        self.asked = []      # (path, body) of every question that came with the token
        self.refused = 0     # the questions that came without it
        self.call = None     # ft-pcsend, since it was last called in
        self.told = []       # its panels, as it answers them
        self.lock = threading.Lock()
        self._token = secrets.token_hex(16)
        key, self._cert = os.path.join(root, "pc.key"), os.path.join(root, "pc.crt")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", key,
                        "-out", self._cert, "-days", "2", "-subj", "/CN=pc"], check=True, capture_output=True)
        pc = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"null")
                except ValueError:
                    body = None
                status, answer = pc.answer(self.path, body, self.headers.get("Authorization"))
                data = json.dumps(answer).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_):
                pass

        class Server(http.server.ThreadingHTTPServer):
            def handle_error(self, request, client_address):
                pass   # (a caller that hung up on the certificate)

        self._server = Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self._cert, key)
        self._server.socket = context.wrap_socket(self._server.socket, server_side=True)
        self.port = self._server.server_address[1] - 1   # its windows instance's: the API is on the port after it
        threading.Thread(target=self._server.serve_forever, daemon=True).start()

    def sign_in(self, hosts):
        """Puts the token and the pin of its key where a Frame keeps a PC's (conf/ft_hostapi.py),
        in the folder `hosts`. The token."""
        os.makedirs(hosts, mode=0o700)
        self.token_path = os.path.join(hosts, f"127.0.0.1-{self.port}.token")
        self.pin_path = self.token_path[:-len(".token")] + ".pin"
        with open(os.open(self.token_path, os.O_WRONLY | os.O_CREAT, 0o600), "w") as f:
            f.write(self._token + "\n")
        with open(self.pin_path, "w") as f:
            f.write(ft_hostapi.spki_pin(ssl.PEM_cert_to_DER_cert(open(self._cert).read())) + "\n")
        return self._token

    def answer(self, path, body, authorization):
        with self.lock:
            if authorization != "Bearer " + self._token:
                self.refused += 1
                return 401, {"error": "Unauthorized"}
            self.asked.append((path, body))
            if path == pclink.SENDER:
                self._hang_up()
                env = dict(os.environ, FT_PCSEND_CODE=body["code"])
                self.call = Tool([self.pcsend, body["address"], str(body["port"]), "-", "--loop", self.loop, "--loopback"], env=env)
            elif path == pclink.STOP:
                self._hang_up()
            elif path != pclink.PANELS:
                return 404, {"error": "Not Found"}
            elif self.call is None or self.call.proc.poll() is not None:
                return 409, {"error": "no call to a headset stands"}
            else:
                self.told = [{"number": p["number"], "output": "FT-W%d" % (p["number"] - 2), "width": self.out[p["address"]][0],
                              "height": self.out[p["address"]][1]} for p in body["panels"] if p["address"] in self.out]
            calling = self.call is not None and self.call.proc.poll() is None
            return 200, {"status": True, "calling": calling, "panels": self.told}

    def _hang_up(self):
        if self.call is not None:
            self.call.proc.terminate()   # (its Tool is still there, for what it said and how it ended)
        self.told = []

    def end(self):
        with self.lock:
            self._hang_up()
        self._server.shutdown()
        self._server.server_close()
