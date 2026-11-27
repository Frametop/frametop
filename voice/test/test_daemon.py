#!/usr/bin/env python3
"""ft-voiced's input and output, offline: the text it has the relay type, push-to-talk from the
relay, a recording's way to its text (in one decode, or in pieces), and controller push-to-talk
read from a stand-in for SteamVR's web socket. No mic, no models, no SteamVR; its sockets get
names of their own, so it's safe next to the live service.
Needs its Python packages (numpy, sherpa-onnx; the image has them).
Run: python3 voice/test/test_daemon.py"""
import asyncio
import importlib.machinery
import importlib.util
import json
import os
import socket
import stat
import tempfile
import unittest
from unittest import mock
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


class Recording(unittest.IsolatedAsyncioTestCase):
    """A dictation from its audio to its text: nothing is typed until it's over, a long one is
    decoded in pieces while it's made, and the pieces are typed as one text."""

    def daemon(self, **dcfg):
        import numpy as np
        v = bare_daemon(recording=None, recording_source=None, take=None, command=None, start_chime=None,
                        sounds={}, loop=asyncio.get_running_loop(), decoding=0, typed=[], heard=[], events=[],
                        dcfg={"min_seconds": 0.3, "max_seconds": 120, "max_piece_seconds": 2.0,
                              "min_pause_seconds": 0.3, "trailing_space": True, "save_clips": 0, **dcfg})
        v.piece_vad = type("V", (), {"reset": lambda self: None})()
        v.notice = fv.notice.Notice(send=lambda text: None)
        v.update_mic = v.warm_engine = lambda: None
        v.chime = lambda name: v.events.append(("chime", name))
        v.broadcast = lambda event, **kw: v.events.append((event, kw.get("text")))

        async def type_text(text):
            v.typed.append(text)
        v.type_text = type_text

        async def transcribe(samples):
            v.heard.append(len(samples) // fv.CHUNK)  # in chunks
            await asyncio.sleep(0)
            return v.says.pop(0)
        v.transcribe = transcribe
        v.chunk = np.zeros(fv.CHUNK, np.float32)
        return v

    def talk(self, v, speech):
        """The mic's chunks, with what the VAD says of each: on_audio, without its VAD."""
        for said in speech:
            v.recording.append(v.chunk)
            v.take["speech"].append(said)
            v.take["level"].append(0.1 if said else 0.001)
            v._cut_piece(v.take)

    async def test_typed_once_when_its_over(self):
        v = self.daemon()
        v.says = ["Hello there."]
        v.start_dictation("keyboard")
        self.talk(v, [True] * 15)  # 1.4 s: under max_piece_seconds, one decode
        self.assertEqual((v.heard, v.typed), ([], []))  # nothing while it records
        await v.stop_dictation()
        self.assertEqual((v.heard, v.typed), ([15], ["Hello there. "]))
        self.assertIn(("dictation", "Hello there."), v.events)

    async def test_long_one_goes_in_pieces(self):
        v = self.daemon()
        v.says = ["We walked to the.", "Old bridge and.", "Back."]
        v.start_dictation("keyboard")
        # 2 s is 21 chunks: at the 21st, the pause in the first piece's second half ends it.
        self.talk(v, [True] * 14 + [False] * 4 + [True] * 3)
        self.assertEqual(len(v.take["pieces"]), 1)
        self.assertEqual(v.take["cut"], 16)  # the middle of the pause
        await asyncio.sleep(0.01)
        self.assertEqual((v.heard, v.typed), ([16], []))  # decoded while it records, typed later
        self.talk(v, [True] * 11 + [False] * 5 + [True] * 5 + [False] * 2)
        self.assertEqual(v.take["cut"], 16 + 5 + 11 + 3)
        await v.stop_dictation()
        self.assertEqual(v.heard, [16, 19, 9])  # every chunk, once
        self.assertEqual(v.typed, ["We walked to the old bridge and back. "])  # the seams aren't sentence ends

    async def test_no_speech_types_nothing(self):
        v = self.daemon()
        v.says = [""]
        v.start_dictation("keyboard")
        self.talk(v, [False] * 10)
        await v.stop_dictation()
        self.assertEqual(v.typed, [])
        self.assertIn(("dictation", ""), v.events)

    async def test_a_slip_isnt_decoded(self):
        v = self.daemon()
        v.start_dictation("keyboard")
        self.talk(v, [True] * 2)  # 0.19 s
        await v.stop_dictation()
        self.assertEqual((v.heard, v.typed), ([], []))
        self.assertIn(("chime", "cancel"), v.events)

    async def test_cancel_drops_the_pieces(self):
        v = self.daemon()
        v.says = ["Never typed."]
        v.start_dictation("keyboard")
        self.talk(v, [True] * 14 + [False] * 4 + [True] * 3)
        piece = v.take["pieces"][0]
        v.cancel_dictation()
        await asyncio.sleep(0.01)
        self.assertTrue(piece.cancelled())
        self.assertEqual(v.typed, [])
        self.assertIsNone(v.take)

    async def test_a_failed_decode_is_said(self):
        v = self.daemon()

        async def transcribe(samples):
            raise OSError("whisper-server isn't there")
        v.transcribe = transcribe
        v.start_dictation("keyboard")
        self.talk(v, [True] * 10)
        await v.stop_dictation()
        self.assertEqual(v.typed, [])
        self.assertEqual(v.events[-2:], [("chime", "cancel"), ("error", None)])


class Transcribe(unittest.IsolatedAsyncioTestCase):
    """transcribe(): the VAD's speech only, to the engine the language has."""

    def daemon(self, engine, spans):
        v = bare_daemon(decode_lock=asyncio.Lock(), language="en", dcfg={"engine": engine}, got=[], used=0)
        v._speech = lambda samples: fv.voice_engine.squeeze(samples, spans, 0)

        def decode(speech):
            v.got.append(("parakeet", len(speech)))
            return "  Two  words. "
        v.parakeet = type("P", (), {"decode": staticmethod(decode)})()

        async def whisper(speech):
            v.got.append(("whisper", len(speech)))
            return "From whisper."
        v._whisper = whisper

        def used():
            v.used += 1
        v.engine_used = used
        return v

    async def test_parakeet_hears_the_speech_only(self):
        samples = fv.np.zeros(3 * fv.RATE, fv.np.float32)
        v = self.daemon("parakeet", [(1000, 9000), (30000, 31000)])
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: True):
            self.assertEqual(await v.transcribe(samples), "Two words.")
        self.assertEqual((v.got, v.used), ([("parakeet", 9000)], 1))

    async def test_silence_isnt_decoded(self):
        v = self.daemon("parakeet", [])
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: True):
            self.assertEqual(await v.transcribe(fv.np.zeros(fv.RATE, fv.np.float32)), "")
        self.assertEqual(v.got, [])

    async def test_whisper_where_parakeet_isnt(self):
        samples = fv.np.zeros(fv.RATE, fv.np.float32)
        v = self.daemon("parakeet", [(0, 8000)])
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: False):  # not fetched
            self.assertEqual(await v.transcribe(samples), "From whisper.")
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: True):
            v.language = "ja"  # not one of its languages
            await v.transcribe(samples)
            v.language, v.dcfg["engine"] = "en", "whisper"  # the advanced settings ask for whisper
            await v.transcribe(samples)
        self.assertEqual((v.got, v.used), ([("whisper", 8000)] * 3, 0))


class Unload(unittest.IsolatedAsyncioTestCase):
    """The recognizer's model leaves memory once nothing has used it for a while."""

    def daemon(self, after):
        v = bare_daemon(loop=asyncio.get_running_loop(), unload_timer=None, recording=None, command=None,
                        decoding=0, decode_lock=asyncio.Lock(), dcfg={"unload_after_seconds": after}, asked=[])

        def unload_idle(seconds):
            v.asked.append(seconds)
            return True
        v.parakeet = type("P", (), {"unload_idle": staticmethod(unload_idle)})()
        return v

    async def test_after_the_wait(self):
        v = self.daemon(0.02)
        v.engine_used()
        v.engine_used()  # used again: one wait, started over
        await asyncio.sleep(0.05)
        self.assertEqual(v.asked, [0.02])
        self.assertIsNone(v.unload_timer)

    async def test_not_while_recording(self):
        v = self.daemon(0.02)
        v.recording = []
        v.engine_used()
        await asyncio.sleep(0.05)
        self.assertEqual(v.asked, [])
        self.assertIsNotNone(v.unload_timer)  # it asks again later
        v.unload_timer.cancel()

    async def test_zero_keeps_it(self):
        v = self.daemon(0)
        v.engine_used()
        self.assertIsNone(v.unload_timer)


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


class Whisper(unittest.TestCase):
    """whisper-server runs only while it's what recognizes the speech."""

    def daemon(self, language, **cfg):
        v = bare_daemon(language=language, wake_enabled=False, whisper_wanted=False,
                        dcfg={"engine": "parakeet", **cfg}, ran=[])
        v._run = lambda argv, **kw: argv
        v.spawn = v.ran.append
        return v

    def test_wanted_for_languages_parakeet_doesnt_know(self):
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: True):
            v = self.daemon("en")
            v.update_whisper()
            self.assertEqual(v.ran, [])  # Parakeet knows English; nothing uses whisper
            v.wake_enabled = True
            v.update_whisper()
            self.assertEqual(v.ran, [])  # wake word commands go to Parakeet too
            v.language = "de"
            v.update_whisper()
            self.assertEqual(v.ran, [])
            for language in ("ja", "auto"):
                v.language, v.whisper_wanted = language, False
                v.update_whisper()
                self.assertEqual(v.ran[-1][-2:], ["start", "frametop-voice-whisper.service"])
            v.update_whisper(restart=True)
            self.assertEqual(v.ran[-1][-2:], ["restart", "frametop-voice-whisper.service"])
            v.language = "en"
            v.update_whisper(restart=True)  # not wanted: stopped, not restarted
            self.assertEqual(v.ran[-1][-2:], ["stop", "frametop-voice-whisper.service"])

    def test_wanted_without_parakeet(self):
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: False):
            v = self.daemon("en")  # its model not fetched: whisper recognizes English
            v.update_whisper()
            self.assertEqual(v.ran[-1][-2:], ["start", "frametop-voice-whisper.service"])
        with mock.patch.object(fv.voice_settings, "parakeet_installed", lambda: True):
            v = self.daemon("en", engine="whisper")  # or the advanced settings ask for whisper
            v.update_whisper()
            self.assertEqual(v.ran[-1][-2:], ["start", "frametop-voice-whisper.service"])


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


class Clips(unittest.TestCase):
    """Saved clips are the user's voice: 0600 whatever the umask, and a link isn't followed."""
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name) / "clips"
        for patch in (mock.patch.object(fv, "CLIP_DIR", self.dir),
                      mock.patch.object(fv.time, "strftime", lambda fmt: "20261031-120000.wav")):
            patch.start()
            self.addCleanup(patch.stop)
        old = os.umask(0)
        self.addCleanup(os.umask, old)
        self.v = bare_daemon(dcfg={"save_clips": 3})
        import numpy as np
        self.samples = np.zeros(1600, dtype=np.float32)

    def test_private(self):
        self.v.save_clip(self.samples)
        clip = self.dir / "20261031-120000.wav"
        self.assertEqual(stat.S_IMODE(clip.stat().st_mode), 0o600)
        self.assertEqual(clip.read_bytes()[:4], b"RIFF")
        clip.chmod(0o644)  # one from before this fix, at the same name: rewritten private
        self.v.save_clip(self.samples)
        self.assertEqual(stat.S_IMODE(clip.stat().st_mode), 0o600)

    def test_link_refused(self):
        self.dir.mkdir()
        target = self.dir.parent / "elsewhere"
        target.write_bytes(b"keep")
        target.chmod(0o644)
        (self.dir / "20261031-120000.wav").symlink_to(target)
        with self.assertRaises(OSError):
            self.v.save_clip(self.samples)
        self.assertEqual(target.read_bytes(), b"keep")
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o644)



class NoticePill(unittest.IsolatedAsyncioTestCase):
    """The pill in the headset (notice.py) when dictations and a wake command overlap: the one
    listening keeps it, and "Typing…" comes back while a dictation is still decoded."""

    def daemon(self, sent):
        import numpy as np
        v = bare_daemon(recording=None, recording_source=None, take=None, command=None, start_chime=None,
                        dcfg={"min_seconds": 0.1})
        v.chime = v.broadcast = lambda *a, **kw: None
        v.update_mic = v.drop_start_chime = v.warm_engine = lambda: None
        v.cmd_vad = type("V", (), {"reset": lambda self: None})()
        v.notice = fv.notice.Notice(send=sent.append, clock=lambda: 100.0)
        v.chunks = lambda: [np.zeros(fv.CHUNK, np.float32)] * 20
        return v

    async def test_wake_command_while_typing(self):
        sent = []
        v = self.daemon(sent)
        gate = asyncio.Event()

        async def slow(*a):
            await gate.wait()
        v._finish_dictation = slow
        v.recording = v.chunks()
        done = asyncio.create_task(v.stop_dictation())
        await asyncio.sleep(0)
        v.listen_for_command(100.0)
        gate.set()
        await done
        self.assertEqual(sent, ["voice typing", "voice listen command 0"])  # the command keeps its pill
        self.assertEqual(v.notice.mode, "command")

    async def test_second_dictation_typing(self):
        sent = []
        v = self.daemon(sent)
        gates = [asyncio.Event(), asyncio.Event()]

        async def slow(*a):
            await gates.pop(0).wait()
        v._finish_dictation = slow
        g1, g2 = gates
        v.recording = v.chunks()
        first = asyncio.create_task(v.stop_dictation())
        await asyncio.sleep(0)
        v.recording = v.chunks()
        v.notice.listening("hold")
        second = asyncio.create_task(v.stop_dictation())
        await asyncio.sleep(0)
        g1.set()
        await first
        self.assertEqual(sent[-1], "voice typing")  # the second's words are still on their way
        g2.set()
        await second
        self.assertEqual(sent[-1], "voice off")

if __name__ == "__main__":
    unittest.main()
