"""run_dir/needed: while the Hand Recorder records, it touches this file every second, so Frametop's own
ft-hands doesn't park when Frametop pauses for a VR game (hands/track/park.h). A parked ft-hands tells
ft-camd to stop copying the mono cameras' frames, which would starve the recording ("Waiting for the
cameras"). One older than 3 s doesn't count, so a recorder that crashed lets go by itself."""
import os
import threading

NEEDED_FILE = "needed"
BEAT_S = 1.0


class HandsNeeded:
    def __init__(self, hands_dir, beat_s=BEAT_S):
        self.path = os.path.join(hands_dir, NEEDED_FILE)
        self.beat_s = beat_s
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        if self._thread:
            return
        self._touch()
        self._stop.clear()
        self._thread = threading.Thread(target=self._beat, name="handrec-needed", daemon=True)
        self._thread.start()

    def stop(self):
        if not self._thread:
            return
        self._stop.set()
        self._thread.join(5)
        self._thread = None
        try:
            os.remove(self.path)
        except OSError:
            pass

    def _beat(self):
        while not self._stop.wait(self.beat_s):
            self._touch()

    def _touch(self):
        try:
            os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
            fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
            os.close(fd)
            os.utime(self.path, follow_symlinks=False)
        except OSError:
            pass   # the next beat tries again; ft-hands parks as before meanwhile
