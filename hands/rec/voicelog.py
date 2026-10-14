"""A testing session's voice, if the person ticked "Record my voice": the microphone during the
recording, so they can say what went wrong as it happens ("that opened the lazy susan, I didn't
mean to"). Transcribed later on the maintainer's machines (to_wav, then whisper.cpp), the speech
becomes flags at its times, beside the button's marks.

pw-record captures the default microphone (16 kHz, mono, s16) in raw mode; each chunk goes into
the take's voice.bin with the time it was read (CLOCK_MONOTONIC, the session's clock).

voice.bin: a 16-byte header, then records.
  header: 8s magic "FHVOIC01", u32 version (1), u32 sample rate
  record: u64 read_ns (when the chunk was read: its last sample is about then), u32 bytes,
          then that many bytes of s16le samples
"""
import shutil
import struct
import subprocess
import threading
import time

MAGIC = b"FHVOIC01"
HDR = struct.Struct("<8sII")
REC = struct.Struct("<QI")
RATE = 16000
CHUNK = RATE // 10 * 2      # 100 ms of s16 mono


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class VoiceLog:
    """Records the microphone into path until stop(). A missing pw-record or a microphone that
    fails doesn't stop the session: stop() reports it ("error")."""

    def __init__(self, path, rate=RATE):
        self.path, self.rate = path, rate
        self.proc = None
        self._thread = None
        self._file = None
        self.samples = 0
        self.chunks = 0
        self.peak = 0
        self.error = ""

    def start(self):
        exe = shutil.which("pw-record")
        if not exe:
            self.error = "pw-record isn't installed"
            return self
        self._file = open(self.path, "wb")
        self._file.write(HDR.pack(MAGIC, 1, self.rate))
        try:
            self.proc = subprocess.Popen([exe, "--raw", "--rate", str(self.rate), "--channels", "1",
                                          "--format", "s16", "--latency", "20ms", "-"],
                                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL)
        except OSError as e:
            self.error = "pw-record didn't start: %s" % e
            return self
        self._thread = threading.Thread(target=self._run, name="handrec-voicelog", daemon=True)
        self._thread.start()
        return self

    def _run(self):
        out = self.proc.stdout
        while True:
            data = out.read1(CHUNK) if hasattr(out, "read1") else out.read(CHUNK)
            if not data:
                break
            now = mono_ns()
            if len(data) % 2:
                data = data[:-1]
            self._file.write(REC.pack(now, len(data)) + data)
            self.samples += len(data) // 2
            self.chunks += 1
            peak = max(abs(v) for v in memoryview(data).cast("h")) if data else 0
            self.peak = max(self.peak, peak)

    def stop(self):
        """Ends the recording: {"seconds", "chunks", "peak" (0..1: 0 means a muted microphone),
        "error"}."""
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            if self.proc.returncode not in (0, -15, 143) and not self.chunks and not self.error:
                self.error = "pw-record exited (%s)" % self.proc.returncode
        if self._thread is not None:
            self._thread.join(3)
        if self.proc is not None:
            self.proc.stdout.close()
        if self._file is not None:
            self._file.close()
        return {"seconds": round(self.samples / self.rate, 2), "chunks": self.chunks,
                "peak": round(self.peak / 32768, 3), "error": self.error}


def read(path):
    """(rate, [(read_ns, bytes)]) of a voice.bin. Stops at a cut-off end."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HDR.size or data[:8] != MAGIC:
        raise ValueError("%s: not a voice.bin" % path)
    _, _, rate = HDR.unpack_from(data, 0)
    out, off = [], HDR.size
    while off + REC.size <= len(data):
        read_ns, n = REC.unpack_from(data, off)
        end = off + REC.size + n
        if end > len(data):
            break
        out.append((read_ns, data[off + REC.size:end]))
        off = end
    return rate, out


def to_wav(path, dest):
    """A voice.bin as a WAV file, gaps between chunks filled with silence. Returns the time
    (CLOCK_MONOTONIC ns) of the WAV's first sample."""
    import wave
    rate, records = read(path)
    if not records:
        raise ValueError("%s: no sound" % path)
    first_ns = records[0][0] - len(records[0][1]) // 2 * 1_000_000_000 // rate
    with wave.open(dest, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        written = 0
        for read_ns, data in records:
            start = (read_ns - first_ns) * rate // 1_000_000_000 - len(data) // 2
            if start > written:
                w.writeframes(b"\0\0" * (start - written))
                written = start
            w.writeframes(data)
            written += len(data) // 2
    return first_ns


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "--test":
        log = VoiceLog(sys.argv[2]).start()
        time.sleep(2)
        print(log.stop())
    elif len(sys.argv) == 3:
        print("first sample at %d ns" % to_wav(sys.argv[1], sys.argv[2]))
    else:
        sys.exit("voicelog.py VOICE.BIN OUT.WAV | --test OUT.BIN")
