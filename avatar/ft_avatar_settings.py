"""Frametop Settings' Appearance page: the hand model ft-handview draws your hands with, its look,
and importing a model of your own (a VRM, glTF or FBX: ft-model-import, avatar/import).

The page's backend (AvatarPages.qml; settings/ft_settings.py makes it). A choice is written to
~/.config/frametop.conf (HANDS_VIEW_MODEL, HANDS_VIEW_STYLE, HANDS_VIEW_ROOMVIEW) and sent to
ft-handview now, when it runs (@ft_handview). The models, their pictures and an import are each
a program of their own (hand_models.py), one at a time, never on the UI's thread.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, QProcess, QProcessEnvironment, QUrl, Signal, Slot

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ftipc  # noqa: E402  (questions to the datagram sockets)
import hand_models  # noqa: E402  (the models, without Qt)

CONF = os.path.expanduser("~/.config/frametop.conf")
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "frametop", "hand-thumbs")
VIEW = "ft_handview"


def tool_command(*cmd):
    """The command line for one of Frametop's own programs (tests replace this)."""
    return list(cmd)


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)
    _ipcDone = Signal(object, object)  # a callback and its answer, from the ftipc worker

    def __init__(self, conf=None, folder=None, cache=None):
        super().__init__()
        self._conf = conf or CONF
        self._folder = folder or hand_models.folder()
        self._cache = cache or CACHE
        self._listed = list(hand_models.FALLBACK)   # (name, kind), as ft-handview last listed them
        self._busy = ""       # what is being imported, for the page
        self._jobs = []       # programs waiting to run: (argv, environment, then)
        self._running = None  # the one running
        self._ipcDone.connect(lambda then, answer: then(answer))
        self._ipc = ftipc.Worker(self._ipcDone.emit)

    # --- programs, one at a time ---
    def _run(self, argv, then, env=None, first=False):
        """Queue a program; then(exit status, its stdout) when it's done (status -1: it didn't run)."""
        job = (tool_command(*argv), env or {}, then)
        self._jobs.insert(0, job) if first else self._jobs.append(job)
        self._next()

    def _next(self):
        if self._running is not None or not self._jobs:
            return
        argv, env, then = self._jobs.pop(0)
        proc = self._running = QProcess(self)
        if env:
            environment = QProcessEnvironment.systemEnvironment()
            for key, value in env.items():
                environment.insert(key, value)
            proc.setProcessEnvironment(environment)

        def finished(code=-1, *_):
            if self._running is not proc:
                return
            self._running = None
            try:
                out = bytes(proc.readAllStandardOutput()).decode(errors="replace")
            except RuntimeError:  # the process went with this backend (the app is quitting)
                return
            proc.deleteLater()
            then(code, out)
            self._next()

        proc.finished.connect(finished)
        proc.errorOccurred.connect(lambda err: err == QProcess.FailedToStart and finished(-1))
        proc.start(argv[0], argv[1:])

    def _send(self, text):
        """A command to ft-handview, if it runs: the change shows now instead of at its next start."""
        self._ipc.ask(VIEW, text)

    # --- the models ---
    @Slot()
    def refresh(self):
        """Ask ft-handview which models there are (it reads your folder), then draw the pictures
        that are missing."""
        self._run([hand_models.HANDVIEW, "--models"], self._models_listed, {"FT_AVATARS_DIR": self._folder})

    def _models_listed(self, code, out):
        listed = hand_models.parse_models(out) if code == 0 else []
        self._listed = listed or list(hand_models.FALLBACK)
        if listed:
            for name, kind in listed:
                path = hand_models.thumb_path(self._cache, name, kind, self._folder)
                if not os.path.exists(path):
                    self._draw(name, kind, path)
        self.changed.emit()

    def _draw(self, name, kind, path):
        os.makedirs(self._cache, exist_ok=True)
        env = {"FT_AVATARS_DIR": self._folder, "FT_HANDS_VIEW_MODEL": name, "FT_HANDS_VIEW_STYLE": "solid"}

        def drawn(code, _out):
            if code != 0:
                return
            for old in hand_models.stale_thumbs(self._cache, name, path):
                try:
                    os.remove(old)
                except OSError:
                    pass
            self.changed.emit()

        self._run([hand_models.HANDVIEW, "--thumb", path, "--res", str(hand_models.THUMB_PX)], drawn, env)

    @Property("QVariantList", notify=changed)
    def models(self):
        current, out = self.model, []
        for name, kind in self._listed:
            item = hand_models.describe(name, kind, self._folder)
            path = hand_models.thumb_path(self._cache, name, kind, self._folder)
            item["thumb"] = QUrl.fromLocalFile(path).toString() if os.path.exists(path) else ""
            item["current"] = name == current
            out.append(item)
        return out

    def _setting(self, key, default):
        return ft_conf.read(self._conf).get(key, default)

    @Property(str, notify=changed)
    def model(self):
        name = self._setting("HANDS_VIEW_MODEL", "asset")
        return name if any(name == n for n, _ in self._listed) else "asset"

    @Property(str, notify=changed)
    def style(self):
        style = self._setting("HANDS_VIEW_STYLE", "ghost")
        return style if any(style == s for s, _ in hand_models.STYLES) else "ghost"

    @Property("QVariantList", constant=True)
    def styles(self):
        return [{"value": value, "text": text} for value, text in hand_models.STYLES]

    @Property(bool, notify=changed)
    def inRoom(self):
        return self._setting("HANDS_VIEW_ROOMVIEW", "1") != "0"

    @Property(str, notify=changed)
    def busy(self):
        return self._busy

    @Property(bool, constant=True)
    def canImport(self):
        return os.path.exists(hand_models.IMPORTER)

    @Property(str, constant=True)
    def sourceFilter(self):
        return hand_models.SOURCES

    @Property(str, constant=True)
    def folder(self):
        """Where your models are kept, as the page says it ("~" for your home folder)."""
        home = os.path.expanduser("~")
        return "~" + self._folder[len(home):] if self._folder.startswith(home + os.sep) else self._folder

    # --- changes ---
    def _put(self, key, value, command):
        try:
            ft_conf.set_value(key, value, self._conf)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't save the setting: {e}", True)
            return
        self._send(command)
        self.changed.emit()

    @Slot(str)
    def choose(self, name):
        if any(name == n for n, _ in self._listed) and name != self.model:
            self._put("HANDS_VIEW_MODEL", name, f"model {name}")

    @Slot(str)
    def setStyle(self, style):
        if any(style == s for s, _ in hand_models.STYLES) and style != self.style:
            self._put("HANDS_VIEW_STYLE", style, f"style {style}")

    @Slot(bool)
    def setInRoom(self, on):
        if on != self.inRoom:
            self._put("HANDS_VIEW_ROOMVIEW", "1" if on else "0", f"inroom {1 if on else 0}")

    @Slot(str, bool)
    def importFile(self, url, may_share):
        """Import a model's file (a file dialog's URL, or a path) and switch to it. may_share: the
        person may pass it on (a VRM's own licence decides instead)."""
        source = QUrl(url).toLocalFile() if "://" in url else url
        if not source or self._busy:
            return
        reserved = {n for n, kind in self._listed if kind != "own"}
        stem = os.path.splitext(os.path.basename(source))[0]
        name = hand_models.free_name(hand_models.slug(stem), reserved)
        argv = [hand_models.IMPORTER, source, "--name", name, "--to", self._folder] + (["--may-share"] if may_share else [])
        self._busy = f"Importing {os.path.basename(source)}…"
        self.changed.emit()

        def imported(_code, out):
            self._busy = ""
            said = hand_models.parse_import(out)
            self.message.emit(*hand_models.import_message(said, source))
            if "ok" in said:
                self._listed = [m for m in self._listed if m[0] != name] + [(name, "own")]
                self._put("HANDS_VIEW_MODEL", name, f"model {name}")
                self.refresh()
            self.changed.emit()

        self._run(argv, imported, first=True)

    @Slot(str)
    def remove(self, name):
        """Delete one of your models (its file in your folder); the Generic hand takes its place."""
        if (name, "own") not in self._listed:
            return
        path = os.path.join(self._folder, name + ".ftmodel")
        was_current = name == self.model
        try:
            os.remove(path)
        except OSError as e:
            self.message.emit(f"Couldn't remove {name}: {e}", True)
            return
        for old in hand_models.stale_thumbs(self._cache, name, ""):
            try:
                os.remove(old)
            except OSError:
                pass
        self._listed = [m for m in self._listed if m[0] != name]
        if was_current:
            self._put("HANDS_VIEW_MODEL", "asset", "model asset")
        self.changed.emit()
        self.refresh()
