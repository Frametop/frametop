#!/usr/bin/env python3
"""frame-voice: local push-to-talk dictation and wake word daemon.

One PipeWire capture stream feeds two consumers:
  * dictation: hotkey or socket command -> record -> whisper-server -> type via uinput
  * wake word: sherpa-onnx keyword spotter -> capture the spoken command -> hooks + events
"""
import asyncio
import io
import json
import logging
import os
import re
import signal
import socket
import struct
import time
import tomllib
import unicodedata
import urllib.request
import uuid
import wave
from pathlib import Path

import evdev

import settings
import numpy as np
import sentencepiece
import sherpa_onnx
from evdev import ecodes as e

ROOT = Path(__file__).resolve().parent
RATE = 16000
CHUNK = 1536  # 96 ms of 16 kHz mono: a whole number of 512-sample VAD windows
MODIFIER_CODES = {e.ecodes[name] for name in settings.MODIFIERS}
TYPER_NAME = "frame-voice keyboard"
KWS_DIR = ROOT / "models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01"
VAD_MODEL = ROOT / "models/silero_vad.onnx"
RUNTIME_DIR = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")) / "frame-voice"
CLIP_DIR = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "frame-voice/clips"
SOCKET_PATH = RUNTIME_DIR / "control.sock"
# Keys of keyboards Frametop's input relay has grabbed for its VR desktop: a grabbed
# keyboard can't be read here, so the relay sends its keys ("key <code> <value> <device>").
FRAMETOP_KEYS = "\0frametop_keys"

log = logging.getLogger("frame-voice")

DEFAULTS = {
    "audio": {"source": ""},
    "whisper": {
        "url": "http://127.0.0.1:8178/inference",
        "language": "en",
        "prompt": "SteamOS, Steam Frame, Godot.",
        # Smallest encoder window (50 per second of audio). medium and large-v3-turbo loop
        # and take 10-20 s on short clips below 512; 512 covers pieces up to ~10 s.
        "min_audio_ctx": 512,
    },
    "dictation": {
        "hold_delay": 0.0,  # seconds the hotkey must be held before recording starts
        "min_seconds": 2.0,  # shorter recordings are discarded, not transcribed
        "max_seconds": 120,
        "key_delay_ms": 4,
        "trailing_space": True,
        # Transcribe while recording: pieces split at pauses go to whisper as soon as they end,
        # so only the last piece is left after release.
        "streaming": True,
        # Live text by agreement: the current window of speech is re-decoded every
        # step_seconds and words two decodes in a row agree on are typed. A window closes at a
        # pause (its last words are typed then) or at its quietest point by max_window_seconds.
        # moonshine runs on the CPU (~0.1-0.3 s per decode, max ~10 s of audio); "whisper" uses
        # the whisper-server on the GPU (better punctuation, ~0.5-0.8 s per decode).
        "engine": "moonshine",
        "moonshine_model": "models/sherpa-onnx-moonshine-base-en-quantized-2026-02-27",
        "moonshine_threads": 2,
        "step_seconds": 0.8,
        "pause_seconds": 0.8,  # shorter pauses inside a sentence ended it with "?"/"." in testing
        "max_window_seconds": 8.0,
        # Keep the audio of the last N dictations in ~/.local/state/frame-voice/clips for
        # comparing models and settings. 0 = never write audio to disk.
        "save_clips": 0,
    },
    "wake": {
        "threshold": 0.25,
        "boost": 1.5,
        "command_timeout": 5.0,
        "end_silence": 0.8,
        "max_command_seconds": 20,
        "wake_hook": "",
        "command_hook": "hooks/on-command",
    },
    "sounds": {"volume": 0.5},  # on/off is in settings.json (frame-voice-config)
}


def load_config():
    path = Path(os.environ.get("FRAME_VOICE_CONFIG", "~/.config/frame-voice/config.toml")).expanduser()
    cfg = {k: dict(v) for k, v in DEFAULTS.items()}
    if path.exists():
        with open(path, "rb") as f:
            for section, values in tomllib.load(f).items():
                cfg.setdefault(section, {}).update(values)
        log.info("config: %s", path)
    return cfg


def resolve(p):
    p = Path(p).expanduser()
    return p if p.is_absolute() else ROOT / p


# ---------------------------------------------------------------- typing

def _build_keymap():
    km = {}
    for c in "abcdefghijklmnopqrstuvwxyz":
        code = getattr(e, f"KEY_{c.upper()}")
        km[c], km[c.upper()] = (code, False), (code, True)
    for c, shifted in zip("1234567890", "!@#$%^&*()"):
        code = getattr(e, f"KEY_{c}")
        km[c], km[shifted] = (code, False), (code, True)
    for plain, shifted, name in [
        ("-", "_", "MINUS"), ("=", "+", "EQUAL"), ("[", "{", "LEFTBRACE"), ("]", "}", "RIGHTBRACE"),
        ("\\", "|", "BACKSLASH"), (";", ":", "SEMICOLON"), ("'", '"', "APOSTROPHE"),
        (",", "<", "COMMA"), (".", ">", "DOT"), ("/", "?", "SLASH"), ("`", "~", "GRAVE"),
    ]:
        code = getattr(e, f"KEY_{name}")
        km[plain], km[shifted] = (code, False), (code, True)
    km[" "] = (e.KEY_SPACE, False)
    return km


KEYMAP = _build_keymap()
ASCII_FIXUPS = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"',
                              "–": "-", "—": "-", "…": "...", "\n": " ", "\t": " "})


INPUT_EVENT = struct.Struct("llHHi")  # struct input_event on 64-bit


def inherited_fd(name):
    """An fd systemd handed back from its fd store (LISTEN_FDS protocol), or None."""
    if os.environ.get("LISTEN_PID") != str(os.getpid()):
        return None
    names = os.environ.get("LISTEN_FDNAMES", "").split(":")
    count = int(os.environ.get("LISTEN_FDS", "0"))
    return next((3 + i for i in range(count) if i < len(names) and names[i] == name), None)


def sd_notify(message, fds=()):
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return False
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack(f"{len(fds)}i", *fds))] if fds else []
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        # socket.send_fds() ignores its address argument on Python 3.12.
        sock.sendmsg([message.encode()], ancillary, 0, addr)
    return True


class RelayKeyboard:
    """Stands in for an evdev device in on_key() for keys from Frametop's input relay."""
    path = "frametop-relay"
    name = "Frametop relay"


class Typer:
    """Types text through a virtual uinput keyboard. Assumes a US layout on the receiving side.

    SteamVR opens /dev/input devices only when it starts, and gamescope gets its keyboard
    input from SteamVR. So the device must exist before SteamVR starts and must never be
    recreated: under systemd its fd is parked in the unit's fd store and reused on restart.
    """

    FDNAME = "typer2"  # v2 is a full keyboard; v1 ("typer") had only the typing keys

    def __init__(self, key_delay_ms):
        self.delay = key_delay_ms / 1000
        self.ui = None
        old = inherited_fd("typer")
        if old is not None:
            sd_notify("FDSTOREREMOVE=1\nFDNAME=typer")
            os.close(old)
            log.warning("typer: replaced the v1 virtual keyboard; restart SteamVR once so it opens the new one")
        self.fd = inherited_fd(self.FDNAME)
        if self.fd is not None:
            log.info("typer: reusing virtual keyboard from the systemd fd store")
            return
        self.ui = evdev.UInput({e.EV_KEY: list(range(e.KEY_ESC, e.KEY_MICMUTE + 1))}, name=TYPER_NAME)
        self.fd = self.ui.fd
        if sd_notify(f"FDSTORE=1\nFDNAME={self.FDNAME}", [self.fd]):
            log.info("typer: new virtual keyboard stored in the systemd fd store")
            self.ui = None  # never UI_DEV_DESTROY it; the fd store owns the device now
        else:
            log.warning("typer: not under systemd; SteamVR only sees this keyboard if it starts after us")

    def _emit(self, type_, code, value):
        os.write(self.fd, INPUT_EVENT.pack(0, 0, type_, code, value))

    def _key(self, code, value):
        self._emit(e.EV_KEY, code, value)
        self._emit(e.EV_SYN, e.SYN_REPORT, 0)
        time.sleep(self.delay)

    def type(self, text):
        text = unicodedata.normalize("NFKD", text.translate(ASCII_FIXUPS))
        for ch in text:
            if ch not in KEYMAP:
                continue  # drops combining accents and anything outside the US keymap
            code, shift = KEYMAP[ch]
            if shift:
                self._key(e.KEY_LEFTSHIFT, 1)
            self._key(code, 1)
            self._key(code, 0)
            if shift:
                self._key(e.KEY_LEFTSHIFT, 0)

    def close(self):
        if self.ui:  # only when not handed to systemd
            self.ui.close()


def drop_repeats(text):
    """Collapse sentences whisper repeated back to back ("Ask not. Ask not. Ask not.")."""
    out = []
    for sentence in re.split(r"(?<=[.!?])\s+", text):
        if not out or sentence.strip(" .!?,").lower() != out[-1].strip(" .!?,").lower():
            out.append(sentence)
    return " ".join(out)


def common_prefix(a, b):
    """How many leading words two hypotheses share, ignoring case and punctuation."""
    n = 0
    for x, y in zip(a, b):
        if re.sub(r"[^\w']", "", x.lower()) != re.sub(r"[^\w']", "", y.lower()):
            break
        n += 1
    return n


def feed_vad(vad, samples):
    """Silero VAD only handles input correctly in whole 512-sample windows."""
    size = vad.config.silero_vad.window_size
    for i in range(0, len(samples) - size + 1, size):
        vad.accept_waveform(samples[i:i + size])


# ---------------------------------------------------------------- sounds

def make_sounds(volume):
    """Short tones so state changes are audible inside VR. Returns name -> wav path."""
    specs = {"start": [660, 880], "stop": [880, 660], "wake": [988], "cancel": [330, 262]}
    out = {}
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    for name, freqs in specs.items():
        parts = []
        for f in freqs:
            t = np.arange(int(RATE * 0.11)) / RATE
            env = np.minimum(1, np.minimum(t, t[::-1]) * 200)
            parts.append(np.sin(2 * np.pi * f * t) * env * volume)
        path = RUNTIME_DIR / f"{name}.wav"
        path.write_bytes(to_wav(np.concatenate(parts).astype(np.float32)))
        out[name] = path
    return out


def to_wav(samples):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((np.clip(samples, -1, 1) * 32767).astype(np.int16).tobytes())
    return buf.getvalue()


# ---------------------------------------------------------------- daemon

class FrameVoice:
    def __init__(self, cfg):
        self.cfg = cfg
        self.dcfg, self.wcfg = cfg["dictation"], cfg["wake"]
        self.hotkeys = {}  # evdev code -> name, from load_settings()
        self.typer = Typer(self.dcfg["key_delay_ms"])
        self.type_lock = asyncio.Lock()
        self.sounds = {}  # apply_options() makes them when enabled
        self.subscribers = set()

        self.recording = None  # list of chunks while dictating
        self.recording_source = None
        self.stream = None  # streaming state of the current dictation, see start_dictation
        self.whisper_lock = asyncio.Lock()  # one request at a time, in order
        self.command = None  # dict while capturing a post-wake command
        self.wake_enabled = settings.DEFAULT["wake_word"]  # apply_options() sets the saved value
        self.mic_task = None
        self.mic_proc = None
        self.audio_session_ready = False
        self.tasks = set()  # keep references so fire-and-forget tasks are not collected early

        self.hk_down = False
        self.hk_chord = False
        self.kb_mode = settings.DEFAULT["keyboard_mode"]
        self.applied = {}  # settings.json options last applied, so a save only changes what changed
        self.mods_down = set()  # (device path, code) of modifiers held on any keyboard
        self.mods_released = asyncio.Event()
        self.mods_released.set()
        self._held_logged = None  # the modifiers last logged as holding typing back
        self.hk_timer = None

        self.kws = self.kws_stream = None  # built from the wake phrases in settings.json
        self.cmd_vad = self._make_vad(self.wcfg["end_silence"])
        self.clip_vad = self._make_vad(0.5)
        self.piece_vad = self._make_vad(0.1)  # per-chunk speech/silence labels for live windows
        self.moonshine = None
        if self.dcfg["engine"] == "moonshine":
            m = resolve(self.dcfg["moonshine_model"])
            self.moonshine = sherpa_onnx.OfflineRecognizer.from_moonshine_v2(
                encoder=str(m / "encoder_model.ort"), decoder=str(m / "decoder_model_merged.ort"),
                tokens=str(m / "tokens.txt"), num_threads=self.dcfg["moonshine_threads"])
            log.info("live dictation engine: moonshine (%s)", m.name)
        self.load_settings()

    # --- models

    def _make_kws(self, phrases):
        """Open-vocabulary spotter for the phrases: they are spelled with the model's BPE
        tokens at load time, so any English words work without training."""
        if not phrases:
            log.info("wake phrases: none")
            return None
        sp = sentencepiece.SentencePieceProcessor(model_file=str(KWS_DIR / "bpe.model"))
        lines = []
        for phrase in phrases:
            tokens = " ".join(sp.encode(phrase, out_type=str))
            lines.append(f"{tokens} :{self.wcfg['boost']} #{self.wcfg['threshold']} @{phrase.replace(' ', '_')}")
        RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
        kw_file = RUNTIME_DIR / "keywords.txt"
        kw_file.write_text("\n".join(lines) + "\n")
        log.info("wake phrases: %s", ", ".join(phrases))
        return sherpa_onnx.KeywordSpotter(
            tokens=str(KWS_DIR / "tokens.txt"),
            encoder=str(KWS_DIR / "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"),
            decoder=str(KWS_DIR / "decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"),
            joiner=str(KWS_DIR / "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"),
            keywords_file=str(kw_file),
            num_threads=1,
        )

    def _make_vad(self, min_silence):
        config = sherpa_onnx.VadModelConfig()
        config.silero_vad.model = str(VAD_MODEL)
        config.silero_vad.min_silence_duration = min_silence
        config.silero_vad.min_speech_duration = 0.2
        config.sample_rate = RATE
        return sherpa_onnx.VoiceActivityDetector(config, buffer_size_in_seconds=180)

    # --- state

    def state(self):
        return {
            "dictating": self.recording is not None,
            "awaiting_command": self.command is not None,
            "wake_enabled": self.wake_enabled,
            "mic_open": self.mic_proc is not None and self.mic_proc.returncode is None,
        }

    def broadcast(self, event, **fields):
        msg = (json.dumps({"event": event, "time": time.time(), **fields}) + "\n").encode()
        for w in list(self.subscribers):
            try:
                w.write(msg)
            except Exception:
                self.subscribers.discard(w)

    def chime(self, name):
        if name in self.sounds:
            # SteamOS runs every playback stream through a per-stream spatializer that takes
            # ~0.35 s to set up, late enough to bury a start tone under the first word. Its
            # WirePlumber script skips streams named like filter chains, so use such a name.
            props = '{ node.name = "frame-voice-chime-filter-chain" media.role = "Notification" }'
            task = asyncio.create_task(self._run(["pw-play", "-P", props, str(self.sounds[name])], quiet=True))
            self.tasks.add(task)
            task.add_done_callback(self.tasks.discard)

    async def _run(self, argv, env=None, quiet=False):
        try:
            proc = await asyncio.create_subprocess_exec(
                *argv, env={**os.environ, **(env or {})},
                stdout=asyncio.subprocess.DEVNULL if quiet else None,
                stderr=asyncio.subprocess.DEVNULL if quiet else None)
            rc = await proc.wait()
            if rc and not quiet:
                log.warning("%s exited %s", argv[0], rc)
        except OSError as err:
            log.warning("could not run %s: %s", argv[0], err)

    def run_hook(self, key, *args, env=None):
        hook = self.wcfg.get(key)
        if not hook:
            return
        path = resolve(hook)
        if not os.access(path, os.X_OK):
            log.warning("hook %s not executable: %s", key, path)
            return
        asyncio.create_task(self._run([str(path), *args], env=env))

    # --- microphone

    def mic_needed(self):
        return (self.wake_enabled and self.kws is not None) or self.recording is not None or self.command is not None

    def reset_kws(self):
        if self.kws:
            self.kws.reset_stream(self.kws_stream)

    async def _rebuild_kws(self, phrases):
        kws = await asyncio.to_thread(self._make_kws, phrases)
        self.kws, self.kws_stream = kws, kws.create_stream() if kws else None
        self.update_mic()

    def update_mic(self):
        if self.mic_needed() and (self.mic_task is None or self.mic_task.done()):
            self.mic_task = asyncio.create_task(self._capture())
        elif not self.mic_needed() and self.mic_proc and self.mic_proc.returncode is None:
            self.mic_proc.terminate()  # fully close the mic when nothing is listening

    async def _wait_for_audio_session(self):
        """Don't open the mic while SteamOS is still building its mic filter chain.

        At boot this daemon starts before SteamVR, and WirePlumber only after it. A capture
        stream that connects mid-setup can leave the echo-cancel link stuck, and the mic then
        reads silence until PipeWire restarts. This unit can't be ordered after WirePlumber
        (cycle through steamvr.service), so wait here instead.
        """
        while True:
            proc = await asyncio.create_subprocess_exec(
                "systemctl", "--user", "show", "wireplumber", "-p", "ActiveState", "-p", "ActiveEnterTimestampMonotonic",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
            props = dict(line.split("=", 1) for line in (await proc.communicate())[0].decode().split())
            if props.get("ActiveState") == "active":
                started = int(props.get("ActiveEnterTimestampMonotonic", "0")) / 1e6
                settle = started + 10 - time.clock_gettime(time.CLOCK_MONOTONIC)
                if settle > 0:
                    await asyncio.sleep(settle)
                return
            await asyncio.sleep(1)

    async def _capture(self):
        if not self.audio_session_ready:
            log.info("waiting for WirePlumber before opening the mic")
            await self._wait_for_audio_session()
            self.audio_session_ready = True
        while self.mic_needed():
            argv = ["pw-record", "--raw", "--rate", str(RATE), "--channels", "1",
                    "--format", "s16", "--latency", "50ms", "-"]
            if self.cfg["audio"]["source"]:
                argv[1:1] = ["--target", self.cfg["audio"]["source"]]
            self.mic_proc = await asyncio.create_subprocess_exec(
                *argv, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
            log.info("mic open")
            try:
                while True:
                    data = await self.mic_proc.stdout.readexactly(CHUNK * 2)
                    self.on_audio(np.frombuffer(data, np.int16).astype(np.float32) / 32768)
            except asyncio.IncompleteReadError:
                pass
            await self.mic_proc.wait()
            log.info("mic closed")
            if self.mic_needed():
                await asyncio.sleep(1)

    def on_audio(self, chunk):
        if self.recording is not None:
            self.recording.append(chunk)
            if self.stream:
                feed_vad(self.piece_vad, chunk)
                self.stream["speech"].append(self.piece_vad.is_speech_detected())
                self.stream["level"].append(float(np.sqrt(np.mean(chunk ** 2))))
                while not self.piece_vad.empty():
                    self.piece_vad.pop()
                self.stream["wake"].set()
            if len(self.recording) * CHUNK / RATE >= self.dcfg["max_seconds"]:
                asyncio.create_task(self.stop_dictation())
        elif self.command is not None:
            self._command_audio(chunk)
        elif self.wake_enabled and self.kws:
            self.kws_stream.accept_waveform(RATE, chunk)
            while self.kws.is_ready(self.kws_stream):
                self.kws.decode_stream(self.kws_stream)
                keyword = self.kws.get_result(self.kws_stream)
                if keyword:
                    self.reset_kws()
                    self.on_wake(keyword.replace("_", " "))
                    break

    # --- transcription

    def trim_to_speech(self, samples):
        """Cut leading/trailing silence. Returns None when there is no speech at all."""
        vad = self.clip_vad
        vad.reset()
        feed_vad(vad, samples)
        vad.flush()
        first = last = None
        while not vad.empty():
            seg = vad.front
            first = seg.start if first is None else first
            last = seg.start + len(seg.samples)
            vad.pop()
        if first is None:
            return None
        pad = int(0.25 * RATE)
        return samples[max(0, first - pad):last + pad]

    async def transcribe(self, samples):
        speech = self.trim_to_speech(samples)
        if speech is None:
            log.info("no speech detected; skipping transcription")
            return ""
        return await self.transcribe_speech(speech)

    async def transcribe_speech(self, speech, context="", quiet=False):
        """Transcribe audio that is already speech. `context` is the text said just before."""
        dur = len(speech) / RATE
        # Whisper's encoder always runs a 30 s window (1500 frames, 50/s). Shrinking it to the
        # clip length is the main latency win on the Adreno GPU. It must still cover the whole
        # clip, and very small contexts make the decoder loop, hence the floor.
        audio_ctx = 0 if dur > 28 else min(1500, max(self.cfg["whisper"]["min_audio_ctx"], int(dur * 50 * 1.1) + 64))
        async with self.whisper_lock:
            t0 = time.monotonic()
            text = await asyncio.to_thread(self._post, to_wav(speech), audio_ctx, context)
        text = re.sub(r"\s+", " ", re.sub(r"\[[^\]]*\]|\([^)]*\)", "", text)).strip()
        text = drop_repeats(text)
        (log.debug if quiet else log.info)("transcribed %.1fs audio in %.2fs: %r", dur, time.monotonic() - t0, text)
        return text

    def _post(self, wav_bytes, audio_ctx, context=""):
        w = self.cfg["whisper"]
        # temperature_inc 0: no fallback re-decodes, which cost 10+ s on short pieces with turbo.
        fields = {"response_format": "json", "temperature": "0", "temperature_inc": "0",
                  "no_timestamps": "true", "language": w["language"], "audio_ctx": str(audio_ctx)}
        prompt = " ".join(filter(None, [w["prompt"], context]))
        if prompt:
            fields["prompt"] = prompt
        boundary = uuid.uuid4().hex
        parts = [f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode()
                 for k, v in fields.items()]
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="clip.wav"\r\n'
                     f"Content-Type: audio/wav\r\n\r\n".encode() + wav_bytes + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        req = urllib.request.Request(w["url"], data=b"".join(parts), headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}"})
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.load(resp).get("text", "")

    async def type_text(self, text):
        async with self.type_lock:
            await asyncio.to_thread(self.typer.type, text)

    # --- dictation

    def start_dictation(self, source):
        if self.recording is not None:
            return
        if self.command is not None:
            self.command = None  # dictation wins over a pending wake command
        self.recording, self.recording_source = [], source
        if self.dcfg["streaming"]:
            self.piece_vad.reset()
            self.stream = {"source": source, "t0": time.monotonic(), "chunks": self.recording,
                           "speech": [], "level": [], "wake": asyncio.Event(), "final": False,
                           "win": 0, "decoded_at": 0, "prev": [], "wdone": [],
                           "words": [], "typed": 0}
            self.stream["worker"] = asyncio.create_task(self._stream_worker(self.stream))
        self.update_mic()
        self.chime("start")
        self.broadcast("dictation_start", source=source)
        log.info("dictation start (%s)", source)

    def cancel_dictation(self, chime=True):
        if self.recording is None:
            return
        self.recording = self.recording_source = None
        if self.stream:
            self.stream["worker"].cancel()
            self.stream = None
        self.update_mic()
        if chime:
            self.chime("cancel")
        self.broadcast("dictation_cancel")
        log.info("dictation cancelled")

    async def stop_dictation(self):
        if self.recording is None:
            return
        chunks, self.recording, self.recording_source = self.recording, None, None
        stream, self.stream = self.stream, None
        self.update_mic()
        seconds = len(chunks) * CHUNK / RATE
        if seconds < self.dcfg["min_seconds"]:
            if stream:
                stream["worker"].cancel()
            log.info("dictation discarded: %.1fs is under the %.1fs minimum", seconds, self.dcfg["min_seconds"])
            self.chime("cancel")
            self.broadcast("dictation_cancel", reason="too_short", seconds=seconds)
            return
        self.chime("stop")
        if stream:
            per = CHUNK / RATE
            log.info("recorded %.1fs: %.1fs labeled speech, peak level %.4f", seconds,
                     sum(stream["speech"]) * per, max(stream["level"], default=0))
        if self.dcfg["save_clips"]:
            self.save_clip(np.concatenate(chunks))
        try:
            if stream:
                stream["final"] = True
                stream["wake"].set()
                await stream["worker"]
                text = " ".join(stream["words"])
                if not text:
                    log.info("no speech detected")
            else:
                text = await self.transcribe(np.concatenate(chunks))
        except Exception as err:
            log.error("transcription failed: %s", err)
            self.chime("cancel")
            self.broadcast("error", message=str(err))
            return
        self.broadcast("dictation", text=text)
        if stream:
            await self._flush_typing(stream, final=True)
        elif text:
            await self.type_text(text + (" " if self.dcfg["trailing_space"] else ""))

    def save_clip(self, samples):
        CLIP_DIR.mkdir(parents=True, exist_ok=True)
        (CLIP_DIR / time.strftime("%Y%m%d-%H%M%S.wav")).write_bytes(to_wav(samples))
        for old in sorted(CLIP_DIR.glob("*.wav"))[:-self.dcfg["save_clips"]]:
            old.unlink()

    async def _decode(self, audio, context):
        """Text for a window of audio from the live engine, without the "..." whisper puts on
        text that stops mid-sentence."""
        if self.moonshine:
            async with self.whisper_lock:
                text = await asyncio.to_thread(self._moonshine_decode, audio)
            text = drop_repeats(re.sub(r"\s+", " ", text).strip())
        else:
            text = await self.transcribe_speech(audio, context, quiet=True)
        text = re.sub(r"([,;:!?])\1+", r"\1", text)  # "so,," -> "so,"
        return re.sub(r"^(\.\.\.|\u2026)\s*|\s*(\.\.\.|\u2026)$", "", text)

    def _moonshine_decode(self, audio):
        # Quiet speech is scaled up to a fixed peak first, as Faceclaw does (helped at low levels).
        peak = float(np.abs(audio).max()) or 1.0
        audio = audio * min(10.0, 0.5 / peak)
        stream = self.moonshine.create_stream()
        stream.accept_waveform(RATE, audio)
        self.moonshine.decode_stream(stream)
        return stream.result.text

    async def _stream_worker(self, stream):
        """Live transcription by agreement.

        The current window (speech since the last pause) is re-decoded every step_seconds of
        new audio. Words that two decodes in a row agree on are committed and typed, except
        the last agreed word, whose punctuation can still change. At a pause, or at the
        window's quietest point once it is max_window_seconds long, the window closes: it is
        decoded once more and the rest of its words are committed. Nothing is cut
        mid-sentence while it is still being decoded, so there are no split or doubled words
        at piece edges.
        """
        per = CHUNK / RATE
        step = max(1, round(self.dcfg["step_seconds"] / per))
        while True:
            await stream["wake"].wait()
            stream["wake"].clear()
            speech, win = stream["speech"], stream["win"]
            end = len(speech)
            window = speech[win:end]
            if stream["final"]:
                if any(window):
                    await self._close_window(stream, end)
                return
            if not any(window):
                if len(window) * per > 1.0:
                    stream["win"] = stream["decoded_at"] = end - int(0.3 / per)  # keep a lead-in
                continue
            silence = len(window) - (max(i for i, v in enumerate(window) if v) + 1)
            if silence * per >= self.dcfg["pause_seconds"]:
                await self._close_window(stream, end - silence + int(0.15 / per))
            elif len(window) * per >= self.dcfg["max_window_seconds"]:
                # No pause: close at the quietest chunk of the second half, preferring ones
                # the VAD calls non-speech (a breath between words rather than inside one).
                level = stream["level"]
                first = win + int(len(window) / 2)
                cut = min(range(first, end), key=lambda i: (speech[i], level[i], -i)) + 1
                await self._close_window(stream, cut)
            elif end - stream["decoded_at"] >= step:
                stream["decoded_at"] = end
                hyp = (await self._decode(np.concatenate(stream["chunks"][win:end]), self._context(stream))).split()
                agreed = common_prefix(stream["prev"], hyp)
                stream["prev"] = hyp
                done = len(stream["wdone"])
                if agreed - 1 > done:
                    self._commit(stream, hyp[done:agreed - 1])

    async def _close_window(self, stream, cut):
        win = stream["win"]
        words = (await self._decode(np.concatenate(stream["chunks"][win:cut]), self._context(stream))).split()
        # Words already typed stay as typed; the rest of the final decode follows them.
        self._commit(stream, words[len(stream["wdone"]):])
        log.info("window %.1fs: %s", (cut - win) * CHUNK / RATE, " ".join(words))
        stream.update(win=cut, decoded_at=cut, prev=[], wdone=[])

    def _context(self, stream):
        return " ".join(stream["words"][-30:])

    def _commit(self, stream, words):
        if not words:
            return
        skip = 0
        if not stream["wdone"]:
            # A window's first words can repeat the end of the previous one when it closed
            # inside a phrase.
            skip = max((k for k in range(1, 4) if common_prefix([w for w in stream["words"][-k:]], words[:k]) == k
                        and len(stream["words"]) >= k), default=0)
        stream["wdone"].extend(words)
        words = words[skip:]
        if not words:
            return
        stream["words"].extend(words)
        self.broadcast("dictation_piece", text=" ".join(words))
        task = asyncio.create_task(self._flush_typing(stream))
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    async def _flush_typing(self, stream, final=False):
        """Type committed words not typed yet: live while recording when allowed, else at the end."""
        if not final:
            live = (self.dcfg["live_typing"] and stream is self.stream
                    and time.monotonic() - stream["t0"] >= self.dcfg["min_seconds"])
            if not live or self.mods_down:
                if live:
                    self._log_held_modifiers()
                return  # a held Ctrl/Alt/Super would turn letters into shortcuts; retried on release
        elif self.mods_down:
            self._log_held_modifiers()
            await self.mods_released.wait()
        new = stream["words"][stream["typed"]:]
        text = (" " if stream["typed"] and new else "") + " ".join(new)
        stream["typed"] += len(new)
        if final and stream["words"] and self.dcfg["trailing_space"]:
            text += " "
        if text:
            if not final:
                log.info("typing %d words live (%.1fs in)", len(new), time.monotonic() - stream["t0"])
            await self.type_text(text)

    async def toggle_dictation(self, source):
        if self.recording is None:
            self.start_dictation(source)
        else:
            await self.stop_dictation()

    # --- wake word

    def on_wake(self, keyword):
        log.info("wake word: %s", keyword)
        self.chime("wake")
        self.broadcast("wake", keyword=keyword)
        self.run_hook("wake_hook", keyword, env={"FRAME_VOICE_EVENT": "wake"})
        self.command = {"chunks": [], "t0": time.monotonic(), "heard": False}
        self.cmd_vad.reset()
        self.update_mic()

    def _command_audio(self, chunk):
        cmd = self.command
        cmd["chunks"].append(chunk)
        feed_vad(self.cmd_vad, chunk)
        while not self.cmd_vad.empty():
            self.cmd_vad.pop()
        speaking = self.cmd_vad.is_speech_detected()
        cmd["heard"] |= speaking
        elapsed = time.monotonic() - cmd["t0"]
        if cmd["heard"] and not speaking or elapsed > self.wcfg["max_command_seconds"]:
            self.command = None
            asyncio.create_task(self.finish_command(np.concatenate(cmd["chunks"])))
        elif not cmd["heard"] and elapsed > self.wcfg["command_timeout"]:
            self.command = None
            log.info("wake: no command heard")
            self.chime("cancel")
            self.broadcast("command_timeout")
        if self.command is None:
            self.reset_kws()
            self.update_mic()

    async def finish_command(self, samples):
        self.chime("stop")
        try:
            text = await self.transcribe(samples)
        except Exception as err:
            log.error("transcription failed: %s", err)
            self.broadcast("error", message=str(err))
            return
        self.broadcast("command", text=text)
        if text:
            self.run_hook("command_hook", text, env={"FRAME_VOICE_EVENT": "command"})

    # --- hotkey

    def on_key(self, dev, code, value):
        loop = asyncio.get_running_loop()
        if code in MODIFIER_CODES:
            self._track_modifier(dev, code, value)
        if code in self.hotkeys:
            if value == 1:
                self.hk_down, self.hk_chord = True, False
                if self.kb_mode == "hold":
                    self.hk_timer = loop.call_later(self.dcfg["hold_delay"], self._hold_start)
            elif value == 0:
                self.hk_down = False
                if self.hk_timer:
                    self.hk_timer.cancel()
                if self.kb_mode == "hold":
                    if self.recording_source == "hotkey":
                        asyncio.create_task(self.stop_dictation())
                elif not self.hk_chord:
                    asyncio.create_task(self.toggle_dictation("hotkey"))
        elif value == 1 and self.hk_down:
            # Hotkey used as a modifier (e.g. Ctrl+C): not a dictation gesture.
            self.hk_chord = True
            if self.hk_timer:
                self.hk_timer.cancel()
            if self.kb_mode == "hold" and self.recording_source == "hotkey":
                self.cancel_dictation(chime=False)

    def _track_modifier(self, dev, code, value):
        key = (dev.path, code)
        if value:
            self.mods_down.add(key)
            self.mods_released.clear()
        else:
            self.mods_down.discard(key)
            self._modifiers_changed()

    def _log_held_modifiers(self):
        """Say which modifier holds typing back, once per set of held keys."""
        held = sorted(self.mods_down)
        if held != self._held_logged:
            self._held_logged = held
            log.info("typing waits for release of %s",
                     ", ".join(f"{e.KEY.get(code, code)} on {path}" for path, code in held))

    def _modifiers_changed(self):
        if not self.mods_down:
            self.mods_released.set()
            self._held_logged = None
            if self.stream:  # type words committed while a modifier was held
                asyncio.create_task(self._flush_typing(self.stream))

    def _hold_start(self):
        if self.hk_down and not self.hk_chord:
            self.start_dictation("hotkey")

    async def watch_devices(self):
        watching = {}
        while True:
            for path in evdev.list_devices():
                if path in watching and not watching[path].done():
                    continue
                try:
                    dev = evdev.InputDevice(path)
                except OSError:
                    continue
                keys = set(dev.capabilities().get(e.EV_KEY, []))
                if dev.name == TYPER_NAME or not (self.hotkeys.keys() | MODIFIER_CODES) & keys:
                    dev.close()
                    continue
                log.info("hotkey: watching %s (%s)", dev.name, path)
                watching[path] = asyncio.create_task(self._read_device(dev))
            if settings.mtime() != self.settings_mtime:
                self.load_settings()
                continue  # rescan now for devices with the new keys
            await asyncio.sleep(2)

    def load_settings(self):
        self.settings_mtime = settings.mtime()
        b = settings.load()  # defaults when the file doesn't exist yet
        self.hotkeys = {}
        for name in b["keyboard"]:
            code = e.ecodes.get(name)
            if isinstance(code, int):
                self.hotkeys[code] = name
            else:
                log.warning("settings: unknown key %r", name)
        log.info("keyboard/mouse push-to-talk: %s", ", ".join(self.hotkeys.values()) or "none")
        self.apply_options(b)

    def apply_options(self, b):
        """Settings from the config app. Only changed ones are applied, so a save doesn't
        undo e.g. `frame-voice-ctl wake off`."""
        changed = {k: b[k] for k in ("keyboard_mode", "wake_phrases", *settings.OPTIONS) if self.applied.get(k) != b[k]}
        self.applied.update(changed)
        if "keyboard_mode" in changed:
            self.kb_mode = changed["keyboard_mode"]
        if "live_typing" in changed:
            self.dcfg["live_typing"] = changed["live_typing"]
        if "sounds" in changed:
            self.sounds = make_sounds(self.cfg["sounds"]["volume"]) if changed["sounds"] else {}
        if "wake_word" in changed and changed["wake_word"] != self.wake_enabled:
            self.wake_enabled = changed["wake_word"]
            self.reset_kws()
            try:
                asyncio.get_running_loop()
                self.update_mic()
            except RuntimeError:
                pass  # at startup; run() opens the mic
        if "wake_phrases" in changed:
            try:
                asyncio.get_running_loop()
                task = asyncio.create_task(self._rebuild_kws(changed["wake_phrases"]))
                self.tasks.add(task)
                task.add_done_callback(self.tasks.discard)
            except RuntimeError:  # at startup
                self.kws = self._make_kws(changed["wake_phrases"])
                self.kws_stream = self.kws.create_stream() if self.kws else None
        if changed:
            log.info("settings: %s", ", ".join(f"{k}={v}" for k, v in changed.items()))

    def listen_frametop_keys(self):
        """Keys from Frametop's input relay, for the keyboards it grabs. Only from our own
        user: the socket is abstract, so any local user could send to it."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)  # the kernel adds the sender's uid
        try:
            sock.bind(FRAMETOP_KEYS)
        except OSError as err:
            log.warning("frametop keys: can't listen (%s)", err)
            sock.close()
            return None
        relay = RelayKeyboard()
        creds = struct.Struct("3i")  # struct ucred: pid, uid, gid

        def readable():
            while True:
                try:
                    data, anc, _, _ = sock.recvmsg(64, socket.CMSG_SPACE(creds.size))
                except BlockingIOError:
                    return
                uid = next((creds.unpack(d[:creds.size])[1] for level, kind, d in anc
                            if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS), None)
                words = data.split(maxsplit=3)
                if uid != os.getuid() or len(words) < 3 or words[0] != b"key":
                    continue
                if len(words) > 3 and words[3].decode(errors="replace") == TYPER_NAME:
                    continue  # our own typing, grabbed along with the real keyboards
                try:
                    code, value = int(words[1]), int(words[2])
                except ValueError:
                    continue
                self.on_key(relay, code, value)

        asyncio.get_running_loop().add_reader(sock.fileno(), readable)
        return sock

    async def _read_device(self, dev):
        try:
            async for ev in dev.async_read_loop():
                if ev.type == e.EV_KEY:
                    self.on_key(dev, ev.code, ev.value)
        except OSError:
            log.info("hotkey: lost %s", dev.name)
        finally:
            self.mods_down = {m for m in self.mods_down if m[0] != dev.path}
            self._modifiers_changed()
            dev.close()

    # --- control socket

    async def handle_client(self, reader, writer):
        try:
            while line := await reader.readline():
                cmd, _, arg = line.decode().strip().partition(" ")
                reply = await self.command_reply(cmd, arg, writer)
                if reply is None:
                    return  # subscribed to events; keep the connection open
                writer.write((json.dumps(reply) + "\n").encode())
                await writer.drain()
        except (ConnectionError, UnicodeDecodeError):
            pass
        finally:
            if writer not in self.subscribers:
                writer.close()

    async def command_reply(self, cmd, arg, writer):
        match cmd:
            case "toggle":
                await self.toggle_dictation(arg or "socket")
            case "start":
                self.start_dictation(arg or "socket")
            case "stop":
                await self.stop_dictation()
            case "cancel":
                self.cancel_dictation()
            case "type":
                await self.type_text(arg)
            case "wake":
                if arg in ("on", "off", "toggle"):
                    self.wake_enabled = {"on": True, "off": False}.get(arg, not self.wake_enabled)
                    self.reset_kws()
                    self.update_mic()
                else:
                    self.on_wake("manual")
            case "status":
                pass
            case "reload":
                self.load_settings()
            case "events":
                self.subscribers.add(writer)
                writer.write((json.dumps({"event": "subscribed", **self.state()}) + "\n").encode())
                return None
            case _:
                return {"ok": False, "error": f"unknown command {cmd!r}"}
        return {"ok": True, **self.state()}

    async def run(self):
        RUNTIME_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
        SOCKET_PATH.unlink(missing_ok=True)
        old_umask = os.umask(0o177)  # socket is owner-only: it can type into any focused app
        server = await asyncio.start_unix_server(self.handle_client, path=str(SOCKET_PATH))
        os.umask(old_umask)
        log.info("control socket: %s", SOCKET_PATH)
        sd_notify("READY=1")
        log.info("dictation: keyboard %s, min %.1fs", self.kb_mode, self.dcfg["min_seconds"])
        self.update_mic()
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, stop.set)
        watcher = asyncio.create_task(self.watch_devices())
        frametop_keys = self.listen_frametop_keys()
        await stop.wait()
        watcher.cancel()
        if frametop_keys:
            asyncio.get_running_loop().remove_reader(frametop_keys.fileno())
            frametop_keys.close()
        server.close()
        if self.mic_proc and self.mic_proc.returncode is None:
            self.mic_proc.terminate()
        self.typer.close()
        SOCKET_PATH.unlink(missing_ok=True)


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(FrameVoice(load_config()).run())


if __name__ == "__main__":
    main()
