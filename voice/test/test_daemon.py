#!/usr/bin/env python3
"""ft-voiced's input and output, offline: the text it has the relay type, push-to-talk from the
relay, and controller push-to-talk read from a stand-in for SteamVR's web socket. No mic, no
models, no SteamVR; its sockets get names of their own, so it's safe next to the live service.
Needs its Python packages (numpy, sherpa-onnx; the image has them).
Run: python3 voice/test/test_daemon.py"""
import asyncio
import importlib.machinery
import importlib.util
import json
import os
import socket
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader("ft_voiced", str(HERE.parent / "ft-voiced"))
spec = importlib.util.spec_from_loader("ft_voiced", loader)
fv = importlib.util.module_from_spec(spec)
loader.exec_module(fv)
TAG = f"ft_voice_test_{os.getpid()}"


def bare_daemon(**attrs):
    """A Voice without models or mic: only what the methods under test use."""
    v = object.__new__(fv.Voice)
    v.calls = []
    v.push_to_talk = lambda command, source, quiet=False: v.calls.append((command, source) + (("quiet",) if quiet else ()))
    for k, val in attrs.items():
        setattr(v, k, val)
    return v


class Text(unittest.TestCase):
    def test_to_text(self):
        # Accents and other scripts stay (the relay types them on the desktop's layout); typographic
        # quotes and dashes go plain, letters are composed, control characters go.
        self.assertEqual(fv.to_text("Café “hi” — it’s ok…\nyes™ für Привет"),
                         'Café "hi" - it\'s ok... yes™ für Привет')
        self.assertEqual(fv.to_text("fu\u0308r\x07"), "f\u00fcr")

    def test_chunks(self):
        self.assertEqual(list(fv.type_chunks("short")), ["short"])
        text = "word " * 1000
        pieces = list(fv.type_chunks(text, 100))
        self.assertEqual("".join(pieces), text)
        self.assertTrue(all(len(p) <= 100 for p in pieces))
        self.assertTrue(all(p.endswith(" ") for p in pieces[:-1]))
        self.assertEqual(list(fv.type_chunks("x" * 250, 100)), ["x" * 100, "x" * 100, "x" * 50])
        # Sized in UTF-8 bytes: a datagram holds TYPE_CHUNK bytes, and "ü" is two.
        wide = "\u00fc" * 120
        pieces = list(fv.type_chunks(wide, 100))
        self.assertEqual("".join(pieces), wide)
        self.assertTrue(all(len(p.encode()) <= 100 for p in pieces))

    def test_sent_to_the_relay(self):
        relay = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        relay.bind(f"\0{TAG}_relay")
        relay.settimeout(1)
        fv.RELAY = f"\0{TAG}_relay"
        out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        v = bare_daemon(relay=out, relay_said=False)
        v._send_text("Hello there. " * 300)
        got = []
        try:
            while True:
                got.append(relay.recv(65536))
        except socket.timeout:
            pass
        self.assertTrue(len(got) > 1 and all(g.startswith(b"type ") for g in got))
        self.assertEqual(b"".join(g[5:] for g in got).decode(), "Hello there. " * 300)
        relay.close()
        v._send_text("nobody listening")  # the relay gone: logged once, no exception
        self.assertTrue(v.relay_said)


class FromRelay(unittest.IsolatedAsyncioTestCase):
    async def test_push_to_talk(self):
        fv.VOICE = f"\0{TAG}_voice"
        v = bare_daemon(loop=asyncio.get_running_loop())
        sock = v.listen_relay()
        c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        for msg in (b"toggle keyboard", b"start mouse", b"stop", b"bogus", b"", b"cancel keyboard quiet"):
            c.sendto(msg, fv.VOICE)
        await asyncio.sleep(0.1)
        self.assertEqual(v.calls, [("toggle", "keyboard"), ("start", "mouse"), ("stop", "relay"),
                                   ("cancel", "keyboard", "quiet")])
        v.loop.remove_reader(sock.fileno())
        sock.close()


class FakeVrSocket:
    """input/vrws.py's VrSocket, playing a script: each messages() call returns the next batch."""
    script = []
    subscribed = []

    def __init__(self, timeout=3):
        pass

    def open(self, mailbox):
        pass

    def subscribe(self, path, on=True):
        FakeVrSocket.subscribed.append((path, on))

    def messages(self, timeout=None):
        if not FakeVrSocket.script:
            raise OSError("vrserver closed the web socket")
        return FakeVrSocket.script.pop(0)

    def close(self):
        pass


def state(device, **components):
    return json.dumps({"type": "update_component_states", "device": device,
                       "components": {f"/input/{k}/click": v for k, v in components.items()}})


class Controller(unittest.TestCase):
    def run_session(self, buttons, mode, script):
        fv.vrws.VrSocket = FakeVrSocket
        fv.vrws.controllers = lambda timeout=3: {"/user/hand/left": "left", "/devices/cv/R1": "right"}
        FakeVrSocket.script, FakeVrSocket.subscribed = list(script), []
        v = bare_daemon(controller=(buttons, mode))
        v._from_controller = lambda command: v.calls.append(command)
        with self.assertRaises(OSError):  # the script ends as SteamVR going away
            v._controller_session()
        return v.calls

    def test_hold(self):
        L = "/user/hand/left"
        calls = self.run_session(("left/dpad_down",), "hold", [
            [state(L, dpad_down=True)], [state(L, thumbstick=True)], [state(L, dpad_down=False)],
            [state(L, dpad_down=True)]])
        self.assertEqual(calls, ["start", "stop", "start", "stop"])  # the last: SteamVR went with it held
        self.assertEqual(sorted(FakeVrSocket.subscribed), [("/devices/cv/R1", True), ("/user/hand/left", True)])

    def test_toggle_and_sides(self):
        L, R = "/user/hand/left", "/devices/cv/R1"
        calls = self.run_session(("right/a",), "toggle", [
            [state(L, a=True)], [state(R, a=True)], [state(R, a=False)], [state(R, a=True), state(R, a=False)]])
        self.assertEqual(calls, ["toggle", "toggle"])  # the left one's A isn't the right one's

    def test_two_buttons_count_as_one(self):
        L, R = "/user/hand/left", "/devices/cv/R1"
        calls = self.run_session(("left/dpad_down", "right/b"), "hold", [
            [state(L, dpad_down=True)], [state(R, b=True)], [state(L, dpad_down=False)], [state(R, b=False)]])
        self.assertEqual(calls, ["start", "stop"])


class ShortWords(unittest.IsolatedAsyncioTestCase):
    """Moonshine hearing nothing in a lone short word, or looping on it."""

    def daemon(self, heard):
        v = bare_daemon(moonshine=object(), language="en", whisper_lock=asyncio.Lock(), tried=[])
        v.trim_to_speech = lambda audio: audio[100:-100]

        def decode(audio):
            v.tried.append(len(audio))
            return heard(audio)
        v._moonshine_decode = decode
        return v

    async def test_retries_cut_then_after_silence(self):
        audio = fv.np.ones(fv.RATE, fv.np.float32)
        v = self.daemon(lambda a: "Way." if len(a) > fv.RATE else "")  # only with the 0.2 s lead
        self.assertEqual(await v._decode(audio, ""), "Way.")
        self.assertEqual(v.tried, [fv.RATE, fv.RATE - 200, int(0.2 * fv.RATE) + fv.RATE - 200])
        v = self.daemon(lambda a: "")
        self.assertEqual(await v._decode(audio, ""), "")
        v = self.daemon(lambda a: "")
        await v._decode(fv.np.ones(4 * fv.RATE, fv.np.float32), "")
        self.assertEqual(v.tried, [4 * fv.RATE])  # a long window isn't a lone word

    async def test_one_word_looped(self):
        v = self.daemon(lambda a: "Way, way, way,")
        self.assertEqual(await v._decode(fv.np.ones(fv.RATE, fv.np.float32), ""), "Way,")
        v = self.daemon(lambda a: "no no")  # two can be meant
        self.assertEqual(await v._decode(fv.np.ones(fv.RATE, fv.np.float32), ""), "no no")
        v = self.daemon(lambda a: "very, very, very")
        self.assertEqual(await v._decode(fv.np.ones(5 * fv.RATE, fv.np.float32), ""), "very, very, very")


class Whisper(unittest.TestCase):
    def daemon(self, language, **cfg):
        v = bare_daemon(moonshine=object(), language=language, wake_enabled=False, whisper_wanted=False,
                        dcfg={"engine": "moonshine", "streaming": True, **cfg}, ran=[])
        v._run = lambda argv, **kw: argv
        v.spawn = v.ran.append
        return v

    def test_wanted_for_other_languages(self):
        v = self.daemon("en")
        v.update_whisper()
        self.assertEqual(v.ran, [])  # Moonshine types English; nothing uses whisper
        v.language = "de"
        self.assertFalse(v.live_moonshine())
        v.update_whisper()
        self.assertEqual(v.ran[-1][-2:], ["start", "frametop-voice-whisper.service"])
        v.update_whisper(restart=True)
        self.assertEqual(v.ran[-1][-2:], ["restart", "frametop-voice-whisper.service"])
        v.language = "en"
        v.update_whisper(restart=True)  # not wanted: stopped, not restarted
        self.assertEqual(v.ran[-1][-2:], ["stop", "frametop-voice-whisper.service"])


class Chimes(unittest.IsolatedAsyncioTestCase):
    async def test_one_at_a_time_and_the_mic_waits(self):
        v = bare_daemon(sounds={"start": "s.wav", "stop": "t.wav"}, chime_task=None, tasks=set(),
                        played=[], mic_updates=0)
        gate = asyncio.Event()

        async def run(argv, **kw):
            v.played.append(("begin", argv[-1]))
            await gate.wait()
            v.played.append(("end", argv[-1]))
        v._run = run

        def update_mic():
            v.mic_updates += 1
        v.update_mic = update_mic
        v.chime("start")
        v.chime("stop")
        v.chime("missing")  # no such sound: nothing
        await asyncio.sleep(0.01)
        self.assertTrue(v.chime_playing())
        self.assertEqual(v.played, [("begin", "s.wav")])
        gate.set()
        await asyncio.sleep(0.01)
        self.assertEqual(v.played, [("begin", "s.wav"), ("end", "s.wav"), ("begin", "t.wav"), ("end", "t.wav")])
        self.assertFalse(v.chime_playing())
        self.assertEqual(v.mic_updates, 1)  # once, after the last tone


if __name__ == "__main__":
    unittest.main()
