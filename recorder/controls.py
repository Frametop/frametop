"""The recorders' session controls: steering a session from any thread (next, pause, resume,
redo, skip, stop) and the session thread's side of it, _controls(), which raises _Stop, _Skip or
_Redo and blocks through a pause. hands/rec/session.py and gaze/rec/session.py's Session classes
build on Controls; what differs between them is in the hooks at the end."""
import threading


class _Skip(Exception):
    pass


class _Stop(Exception):
    pass


class _Redo(Exception):
    pass


class Controls:
    """Needs, from the session: _lock, _wake (a threading.Event), _want (a dict with pause, skip,
    stop, next and redo), _thread, _run(), _status, _redo_ok, _recording, _take, _panel, _emit(),
    _event(), _log() and _button_ok(), plus RESUME_HINT and RESUME_HINT_BUTTON."""
    THREAD_NAME = "session"
    RESUME_HINT = RESUME_HINT_BUTTON = ""

    # --- controls (any thread)
    def start(self):
        if self._thread:
            raise RuntimeError("the session has started already")
        self._thread = threading.Thread(target=self._run, name=self.THREAD_NAME, daemon=True)
        self._thread.start()

    def _set(self, key, value):
        with self._lock:
            self._want[key] = value
        self._wake.set()

    def next_step(self):
        """Step mode: start the step that's waiting (its countdown)."""
        self._set("next", True)

    def redo(self):
        self._set("redo", True)

    def pause(self):
        self._set("pause", True)

    def resume(self):
        self._set("pause", False)

    def skip(self):
        self._set("skip", True)

    def stop(self, wait=20.0):
        """Stop. The take in progress is kept as far as it got. Blocks until the session has
        written its files (up to wait seconds), unless called from the session thread."""
        self._set("stop", True)
        if self._thread and threading.current_thread() is not self._thread and wait:
            self._thread.join(wait)

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout)

    @property
    def state(self):
        return self._status["state"]

    # --- the session thread
    def _controls(self):
        """Handle a pause (blocking until resumed), skip, stop and redo (only where _redo_ok:
        else it's dropped, see _redo_dropped). True if a pause happened."""
        with self._lock:
            want = dict(self._want)
            self._want["skip"] = self._want["redo"] = False
        if want["stop"]:
            raise _Stop()
        if want["skip"]:
            raise _Skip()
        if want["redo"] and self._redo_ok:
            raise _Redo()
        if want["redo"]:
            self._redo_dropped()
        if not want["pause"]:
            return False
        self._pause()
        outcome = None
        while outcome is None:
            self._wake.wait(0.2)
            self._wake.clear()
            with self._lock:
                want = dict(self._want)
                self._want["skip"] = self._want["redo"] = False
                redo = want["redo"] and self._redo_ok
                if want["skip"] or redo:
                    self._want["pause"] = False
            outcome = _Stop if want["stop"] else _Skip if want["skip"] else _Redo if redo else \
                True if not want["pause"] else None
        self._unpause(record=outcome is True)
        if outcome is not True:
            raise outcome()
        return True

    def _pause(self):
        self._paused = True
        self._state_before = self._status["state"]
        self._paused_recording = self._recording
        if self._recording:
            self._recording_off()
            self._event("pause")
        hint = self.RESUME_HINT_BUTTON if self._button_ok() else self.RESUME_HINT
        self._panel.cmd("paused on")
        self._panel.set("note", "note " + hint)
        self._emit(state="paused", note=hint)
        self._log("paused")

    def _unpause(self, record=True):
        self._paused = False
        self._panel.cmd("paused off")
        self._panel.set("note", "note ")
        self._unpaused()
        with self._lock:
            self._want["next"] = False   # a Next pressed while paused doesn't count
        if self._take and self._paused_recording and record:
            self._event("resume")
            self._recording_on()
        self._emit(state=self._state_before, note="")
        self._log("resumed")

    # --- hooks: where the recorders differ
    def _recording_off(self):
        """Stop the recording for a pause."""
        raise NotImplementedError

    def _recording_on(self):
        """Start it again after the pause."""
        raise NotImplementedError

    def _redo_dropped(self):
        """A redo came where there's nothing to redo (not _redo_ok)."""

    def _unpaused(self):
        """Leaving a pause, before the recording starts again."""
