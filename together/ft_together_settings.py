"""Frametop Settings' Together page: turn Frametop Together on, open a room or join someone's,
let people in, choose what of yours the room gets (your voice, your avatar, your screens), and
go to someone or to a panel in it. Who may use which screen is ft_together_control.py's part,
whether the room gets where you look ft_together_gaze.py's.

The page's backend (TogetherPages.qml; settings/ft_settings.py makes it). It only asks: the
room is ft-together's (@ft_together), the voice ft-roomvoice's, the shared screens
ft-roompanels', and each is asked on a worker thread, once a second while the page shows. The
avatar its person has on is read there too (roomavatar.py), and marked as one the others may
have by ft-model-import, once its person said so. The
switch enables and starts frametop-together.service on the host, which the room's other
programs start and stop with (together/run.sh, "Running it" in together/README.md). What the
answers mean for the page is roompage.py's, without Qt.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, QProcess, QTimer, Signal, Slot
from PySide6.QtGui import QGuiApplication

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import ftipc  # noqa: E402  (questions to the datagram sockets)
import ft_together_control  # noqa: E402  (who may use which screen)
import ft_together_gaze  # noqa: E402  (whether the room gets where you look)
import roomavatar  # noqa: E402  (the avatar its person has on)
import roompage  # noqa: E402  (what the answers mean, without Qt)
from fthost import host_command  # noqa: E402  (commands on the SteamOS host)

UNIT = "frametop-together.service"
UNITS = os.path.expanduser("~/.config/systemd/user")
TOGETHER, VOICE, PANELS, SCREENS = "ft_together", "ft_roomvoice", "ft_roompanels", "ft_screens"
WHO = {TOGETHER: "Frametop Together", VOICE: "The room's voice", PANELS: "The room's panels", SCREENS: "The desktop"}


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)
    _ipcDone = Signal(object, object)  # a callback and its answer, from the ftipc worker

    def __init__(self, units=None, sockets=None, worn=None):
        super().__init__()
        self._units = units or UNITS
        self._worn = worn or roomavatar.Worn()
        self._avatar = None    # what its file says of the avatar its person has on
        self._allowing = None  # the ft-model-import running
        self._to = dict({name: name for name in WHO}, **(sockets or {}))
        self._answers = {}     # what was asked for -> (the answer or None, why there was none)
        self._code = ""        # the invite made last, while its room stands
        self._switching = None   # the systemctl running
        self._ipcDone.connect(lambda then, answer: then(answer))
        self._ipc = ftipc.Worker(self._ipcDone.emit)
        self._control = ft_together_control.Control(self._told, lambda text: self._do(TOGETHER, text))
        self.changed.connect(self._control.changed)
        self._gaze = ft_together_gaze.Gaze(self._told, lambda text: self._do(TOGETHER, text))
        self.changed.connect(self._gaze.changed)
        self.poll = QTimer(interval=1000, timeout=self.refresh)

    # --- asking ---
    def _send(self, to, text, then=None):
        """A question to one of the room's programs, off this thread; then(answer or None, why)."""
        def job(client):
            return client.ask(self._to[to], text), client.why
        self._ipc.run(job, None if then is None else lambda got: then(*(got or (None, "failed"))))
        return True

    def _ask(self, key, to, text):
        def answered(reply, why):
            if self._answers.get(key) != (reply, why):
                self._answers[key] = (reply, why)
                self._settle()
                self.changed.emit()
        self._send(to, text, answered)

    def _settle(self):
        """What goes with an answer that is gone: the others' answers, and a room's invite."""
        if not self.ready:
            for key in ("here", "looks", "voice", "panels", "screens"):
                self._answers.pop(key, None)
        if self.mode != "hosting":
            self._code = ""

    def _do(self, to, text, done=None):
        def answered(reply, why):
            if reply is None:
                self.message.emit(f"{WHO[to]} isn't answering", True)
            elif not reply.startswith("ok"):
                self.message.emit(roompage.why_not(reply), True)
            elif done is not None:
                done(reply)
            self.refresh()
        self._send(to, text, answered)

    @Slot(bool)
    def setActive(self, on):
        """Ask now and every second while the page shows; not at all while it doesn't."""
        if on:
            self.refresh()
            self.poll.start()
        else:
            self.poll.stop()

    @Slot()
    def refresh(self):
        if self._ipc.pending():
            return   # the last round's answers are still coming
        self._ask("state", TOGETHER, "state")
        if self.ready:
            self._ask("here", TOGETHER, "here")
            self._ask("looks", TOGETHER, "looks")
            self._ask("control", TOGETHER, "control")
            self._ask("gaze", TOGETHER, "gaze")
            self._ipc.run(lambda _client: self._worn.look(), self._wears)
            self._ask("voice", VOICE, "state")
            self._ask("panels", PANELS, "state")
            self._ask("screens", SCREENS, "screens")

    def _told(self, key):
        return roompage.told(self._answers.get(key, (None, ""))[0])

    def _state(self):
        return self._told("state") or {}

    # --- the switch ---
    @Property(bool, notify=changed)
    def installed(self):
        return os.path.isfile(os.path.join(self._units, UNIT))

    @Property(bool, notify=changed)
    def on(self):
        return os.path.lexists(os.path.join(self._units, "default.target.wants", UNIT))

    @Property(bool, notify=changed)
    def busy(self):
        return self._switching is not None

    @Property(str, notify=changed)
    def status(self):
        if self._switching is not None:
            return "Starting…" if self._switching[1] else "Stopping…"
        if "state" not in self._answers and self.installed and self.on:
            return "Checking…"
        return roompage.status(self.installed, self.on, *self._answers.get("state", (None, "")))[0]

    @Property(bool, notify=changed)
    def ready(self):
        return self._told("state") is not None

    @Slot(bool)
    def setOn(self, on):
        if not self.installed or self._switching is not None:
            self.message.emit("Still busy: try again in a moment" if self.installed else "Frametop Together isn't installed", True)
            self.changed.emit()
            return
        argv = host_command("systemctl", "--user", "enable" if on else "disable", "--now", UNIT)
        proc = QProcess(self)
        self._switching = (proc, on)

        def finished(code=1, *_):
            if self._switching is None or self._switching[0] is not proc:
                return
            self._switching = None
            proc.deleteLater()
            if code != 0:
                self.message.emit(f"Frametop Together didn't turn {'on' if on else 'off'} (systemctl: exit code {code})", True)
            self._answers.clear()
            self._settle()
            self.changed.emit()
            self.refresh()
        proc.finished.connect(finished)
        proc.errorOccurred.connect(lambda err: err == QProcess.FailedToStart and finished(1))
        proc.start(argv[0], argv[1:])
        self.changed.emit()

    # --- the room ---
    @Property(str, notify=changed)
    def here(self):
        return roompage.here_text(roompage.here(self._answers.get("here", (None, ""))[0]))

    @Property(str, notify=changed)
    def mode(self):
        return str(self._state().get("mode", "idle")) if self.ready else "idle"

    @Property(str, notify=changed)
    def roomText(self):
        return roompage.room_text(self._state())

    @Property(str, notify=changed)
    def note(self):
        return str(self._state().get("note", ""))

    @Property("QVariantList", notify=changed)
    def people(self):
        return roompage.people(self._state())

    @Property("QVariantList", notify=changed)
    def asks(self):
        return roompage.asks(self._state())

    @Property(str, notify=changed)
    def code(self):
        return self._code

    @Slot()
    def openRoom(self):
        self._do(TOGETHER, "open")

    @Slot()
    def invite(self):
        def made(reply):
            self._code = roompage.code(reply)
            self.changed.emit()
        self._do(TOGETHER, "invite", made)

    @Slot(str, str)
    def join(self, address, code):
        question, wrong = roompage.join_words(address, code)
        if question is None:
            self.message.emit(wrong, True)
            return
        self._do(TOGETHER, question)

    @Slot(int)
    def accept(self, ask):
        self._do(TOGETHER, f"accept {int(ask)}")

    @Slot(int)
    def decline(self, ask):
        self._do(TOGETHER, f"decline {int(ask)}")

    @Slot()
    def leave(self):
        self._do(TOGETHER, "leave")

    @Property(QObject, constant=True)
    def control(self):
        return self._control

    @Property(QObject, constant=True)
    def gaze(self):
        return self._gaze

    @Slot(str, int)
    def goTo(self, kind, ident):
        """Its person jumps to in front of someone ("person", their place) or a panel ("panel",
        its id in the room)."""
        if kind in ("person", "panel"):
            self._do(PANELS, f"go {kind} {int(ident)}")

    @Slot(str)
    def copy(self, text):
        if text:
            QGuiApplication.clipboard().setText(text)
            self.message.emit("Copied", False)

    # --- your voice ---
    @Property(bool, notify=changed)
    def voiceUp(self):
        return self._told("voice") is not None

    @Property(bool, notify=changed)
    def muted(self):
        return bool((self._told("voice") or {}).get("muted"))

    @Property(str, notify=changed)
    def voiceText(self):
        voice = self._told("voice")
        if voice is None:
            return "The room's voice isn't running"
        if voice.get("mic") == "failed":
            return "The microphone didn't open"
        if voice.get("muted"):
            return "Muted: the room doesn't hear you"
        return "The room hears you" if voice.get("mic") == "open" else "Your microphone opens when you are in a room, with the headset on"

    @Slot(bool)
    def setMuted(self, on):
        self._do(VOICE, "mute" if on else "unmute")

    # --- your avatar ---
    def _wears(self, avatar):
        if avatar != self._avatar:
            self._avatar = avatar
            self.changed.emit()

    def _avatar_row(self):
        return roompage.avatar(self._avatar, self._told("looks"))

    @Property(str, notify=changed)
    def avatarText(self):
        return self._avatar_row()["text"]

    @Property(bool, notify=changed)
    def avatarAllow(self):
        return self._avatar_row()["allow"] and self._allowing is None

    @Slot()
    def allowAvatar(self):
        """Its person said the others may have the avatar they have on: its file is marked, and
        the room has it the next time ft-together reads the file."""
        if not self.avatarAllow:
            return
        argv = roomavatar.allow_command(self._avatar["path"])
        proc = QProcess(self)
        self._allowing = proc
        out = []

        def finished(code=1, *_):
            if self._allowing is not proc:
                return
            self._allowing = None
            proc.deleteLater()
            ok, text = roomavatar.allowed(code, "".join(out))
            self.message.emit(text, not ok)
            self.changed.emit()
            self.refresh()
        proc.readyReadStandardOutput.connect(lambda: out.append(bytes(proc.readAllStandardOutput()).decode("utf-8", "replace")))
        proc.finished.connect(finished)
        proc.errorOccurred.connect(lambda err: err == QProcess.FailedToStart and finished(1))
        proc.start(argv[0], argv[1:])
        self.changed.emit()

    # --- your screens ---
    @Property("QVariantList", notify=changed)
    def shares(self):
        return roompage.shares(roompage.screens(self._answers.get("screens", (None, ""))[0]), self._told("panels"))

    @Slot(int, bool)
    def setShared(self, screen, on):
        self._do(PANELS, f"{'share' if on else 'unshare'} {int(screen)}")
