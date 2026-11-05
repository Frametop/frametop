"""recorder/controls.py: the recorders' pause, skip, stop and redo loop (both sessions build on it),
on a fake session: what each control raises, a redo with nothing to redo, and a pause that blocks
until resume and starts the recording again."""
import threading
import time
import unittest

from recorder.controls import Controls, _Redo, _Skip, _Stop


class Panel:
    def __init__(self):
        self.sent = []

    def cmd(self, text):
        self.sent.append(("cmd", text))

    def set(self, key, text):
        self.sent.append((key, text))


class Fake(Controls):
    THREAD_NAME = "fake-session"
    RESUME_HINT, RESUME_HINT_BUTTON = "Paused (window)", "Paused (button)"

    def __init__(self, redo_ok=False, recording=True):
        self._lock, self._wake = threading.Lock(), threading.Event()
        self._want = {"pause": False, "skip": False, "stop": False, "next": False, "redo": False}
        self._thread = None
        self._status = {"state": "running"}
        self._redo_ok, self._recording, self._take = redo_ok, recording, object()
        self._panel = Panel()
        self.calls = []

    def _run(self):
        self.calls.append(("run", threading.current_thread().name))

    def _emit(self, **changes):
        self._status.update(changes)
        self.calls.append(("emit", changes))

    def _event(self, kind):
        self.calls.append(("event", kind))

    def _log(self, line):
        self.calls.append(("log", line))

    def _button_ok(self):
        return True

    def _recording_off(self):
        self._recording = False
        self.calls.append(("recording", False))

    def _recording_on(self):
        self._recording = True
        self.calls.append(("recording", True))

    def _redo_dropped(self):
        self.calls.append(("dropped",))

    def _unpaused(self):
        self.calls.append(("unpaused",))


class ControlsTest(unittest.TestCase):
    def test_nothing_wanted(self):
        s = Fake()
        self.assertFalse(s._controls())
        self.assertEqual(s.calls, [])

    def test_stop_skip_redo_raise(self):
        for wanted, exc in ((lambda s: s.stop(wait=0), _Stop), (Fake.skip, _Skip)):
            s = Fake()
            wanted(s)
            with self.assertRaises(exc):
                s._controls()
        s = Fake(redo_ok=True)
        s.redo()
        with self.assertRaises(_Redo):
            s._controls()
        self.assertFalse(s._want["redo"])

    def test_a_redo_with_nothing_to_redo_is_dropped(self):
        s = Fake(redo_ok=False)
        s.redo()
        self.assertFalse(s._controls())
        self.assertEqual(s.calls, [("dropped",)])
        self.assertFalse(s._want["redo"])

    def test_pause_blocks_until_resume(self):
        s = Fake()
        s._want["next"] = True
        s.pause()
        threading.Timer(0.3, s.resume).start()
        t0 = time.monotonic()
        self.assertTrue(s._controls())
        self.assertGreater(time.monotonic() - t0, 0.25)
        self.assertEqual(s.state, "running")
        self.assertFalse(s._want["next"])   # a Next pressed while paused doesn't count
        self.assertEqual([c for c in s.calls if c[0] != "emit"],
                         [("recording", False), ("event", "pause"), ("log", "paused"), ("unpaused",),
                          ("event", "resume"), ("recording", True), ("log", "resumed")])
        self.assertIn(("note", "note Paused (button)"), s._panel.sent)
        self.assertEqual(s._panel.sent[-2:], [("cmd", "paused off"), ("note", "note ")])

    def test_stop_while_paused_keeps_the_recording_off(self):
        s = Fake()
        s.pause()
        threading.Timer(0.3, lambda: s.stop(wait=0)).start()
        with self.assertRaises(_Stop):
            s._controls()
        self.assertFalse(s._recording)
        self.assertNotIn(("event", "resume"), s.calls)
        self.assertTrue(s._want["pause"])

    def test_redo_while_paused_ends_the_pause(self):
        s = Fake(redo_ok=True)
        s.pause()
        threading.Timer(0.3, s.redo).start()
        with self.assertRaises(_Redo):
            s._controls()
        self.assertFalse(s._want["pause"])
        self.assertNotIn(("recording", True), s.calls)

    def test_start_runs_on_the_named_thread(self):
        s = Fake()
        s.start()
        s.join(5)
        self.assertEqual(s.calls, [("run", "fake-session")])
        with self.assertRaises(RuntimeError):
            s.start()


if __name__ == "__main__":
    unittest.main()
