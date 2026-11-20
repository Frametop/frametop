"""Steam's text fields for Full Keyboard: steamhook.js in Steam's UI, driven from a thread.

The thread keeps one connection to Steam's SharedJSContext (steam/steamui.py), installs the
hook, and polls it every POLL seconds, which renews its lease and says whether Steam's fields
may take the keyboard (take). Typing for a field goes in at once. What happens comes back to
the main loop through results(), and fileno() becomes readable when there's some.

While Steam isn't reachable it tries again every RETRY seconds; Steam's own keyboard works
meanwhile, since an unrenewed hook takes itself out after LEASE_MS.
"""
import json
import os
import queue
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "steam"))
import steamui  # noqa: E402

POLL = 0.1
RETRY = 2.0
LEASE_MS = 1500
SOURCE = HERE / "steamhook.js"
GLOBAL = "globalThis.__frametopKeyboardV1"


class SteamHook:
    def __init__(self, session_factory=steamui.Session, source=None):
        self.session_factory = session_factory
        self.source = source if source is not None else SOURCE.read_text()
        self.take = False
        self.commands = queue.Queue()
        self.out = queue.Queue()
        self.wake_r, self.wake_w = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, name="steam-hook", daemon=True)

    def start(self):
        self.thread.start()

    def fileno(self):
        return self.wake_r

    # From the main loop. done(ok) runs there later, through results().
    def type(self, session, text, done):
        self.commands.put(("type", session, text, done))

    def key(self, session, name, modifiers, done):
        self.commands.put(("key", session, [name, list(modifiers)], done))

    def close_session(self, session):
        self.commands.put(("close", session, None, None))

    def results(self):
        """What the thread produced since the last call: ("event", {...}) from the hook, ("done",
        done, ok) for typing, ("steam", True|False) when the hook went in or Steam went away."""
        try:
            while os.read(self.wake_r, 4096):
                pass
        except BlockingIOError:
            pass
        out = []
        while True:
            try:
                out.append(self.out.get_nowait())
            except queue.Empty:
                return out

    def stop(self, timeout=3):
        """Takes the hook out of Steam (Steam's own keyboard works at once) and ends the thread."""
        self.stopping.set()
        self.commands.put(None)
        if self.thread.is_alive():
            self.thread.join(timeout=timeout)
        if self.thread.is_alive():
            return   # still in a call to Steam: its pipe stays, since the thread may yet write to it
        for fd in (self.wake_r, self.wake_w):
            try:
                os.close(fd)
            except OSError:
                pass

    def _post(self, item):
        self.out.put(item)
        try:
            os.write(self.wake_w, b"x")
        except BlockingIOError:
            pass  # already readable
        except OSError:
            pass  # stopped: nobody reads it

    def _run(self):
        while not self.stopping.is_set():
            session = None
            try:
                session = self.session_factory()
                if session.evaluate(f"({self.source})(globalThis, {LEASE_MS})") is not True:
                    raise steamui.SteamUnreachable("the hook didn't go in")
                self._post(("steam", True))
                self._serve(session)
                session.evaluate(f"{GLOBAL}?.dispose()")   # stopping: out at once, not at lease's end
            except steamui.SteamUnreachable:
                pass
            finally:
                if session is not None:
                    self._post(("steam", False))
                    try:
                        session.close()
                    except OSError:
                        pass
            self._fail_waiting()  # typing for a session that went with Steam
            self.stopping.wait(RETRY)

    def _serve(self, session):
        """Polls and types until Steam goes away (SteamUnreachable) or stop()."""
        while not self.stopping.is_set():
            state = session.evaluate(f"{GLOBAL}?.poll({json.dumps(bool(self.take))}) ?? null")
            if state is None:
                raise steamui.SteamUnreachable("the hook is gone (Steam's UI reloaded)")
            for event in state.get("events") or []:
                if isinstance(event, dict):
                    self._post(("event", event))
            try:
                command = self.commands.get(timeout=POLL)
            except queue.Empty:
                continue
            while command is not None:
                self._command(session, command)
                try:
                    command = self.commands.get_nowait()
                except queue.Empty:
                    break

    def _command(self, session, command):
        kind, sid, argument, done = command
        if kind == "close":
            session.evaluate(f"{GLOBAL}?.close({json.dumps(sid)})")
            return
        arguments = [sid, argument] if kind == "type" else [sid, *argument]
        try:
            ok = session.evaluate(f"{GLOBAL}?.{kind}(...{json.dumps(arguments)}) === true")
        except steamui.SteamUnreachable:
            self._post(("done", done, False))
            raise
        self._post(("done", done, ok is True))

    def _fail_waiting(self):
        while True:
            try:
                command = self.commands.get_nowait()
            except queue.Empty:
                return
            if command and command[3]:
                self._post(("done", command[3], False))
