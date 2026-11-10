"""Frametop Settings' Appearance page: the hand model ft-handview draws your hands with, its look,
importing a model of your own (a VRM, glTF or FBX: ft-model-import, avatar/import), and, for a
model that is a whole avatar, wearing it and seeing yourself in the mirror (ft-avatars).

The page's backend (AvatarPages.qml; settings/ft_settings.py makes it). A choice is written to
~/.config/frametop.conf (HANDS_VIEW_MODEL, HANDS_VIEW_STYLE, HANDS_VIEW_ROOMVIEW, AVATAR_MODEL)
and sent to ft-handview now, when it runs (@ft_handview), and to the mirror while it stands
(@ft_avatars). The models, their pictures and an import are each a program of their own
(hand_models.py, avatar_models.py), one at a time, never on the UI's thread; the mirror is a
program that goes on after Settings closes.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, QProcess, QProcessEnvironment, QTimer, QUrl, Signal, Slot

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import avatar_models  # noqa: E402  (the avatars and the mirror, without Qt)
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ftipc  # noqa: E402  (questions to the datagram sockets)
import hand_models  # noqa: E402  (the models, without Qt)

CONF = os.path.expanduser("~/.config/frametop.conf")
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "frametop", "hand-thumbs")
AVATAR_CACHE = os.path.join(os.path.dirname(CACHE), "avatar-thumbs")
VIEW = "ft_handview"
MIRROR_UP_MS = 4000   # how long a started mirror gets before it is asked whether it came up


def tool_command(*cmd):
    """The command line for one of Frametop's own programs (tests replace this)."""
    return list(cmd)


def start_detached(argv):
    """Start a program that goes on after Settings closes (tests replace this). False: it didn't start."""
    started = QProcess.startDetached(argv[0], argv[1:])   # (it did, its pid), or just whether it did
    return bool(started[0] if isinstance(started, tuple) else started)


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)
    _ipcDone = Signal(object, object)  # a callback and its answer, from the ftipc worker

    def __init__(self, conf=None, folder=None, cache=None, avatar_cache=None):
        super().__init__()
        self._conf = conf or CONF
        self._folder = folder or hand_models.folder()
        self._cache = cache or CACHE
        self._avatar_cache = avatar_cache or AVATAR_CACHE
        self._avatars = []    # your models that are whole avatars, as avatar_models.listed() found them
        self._mirror = False  # the mirror stands in the room
        self._wear = ""       # a model just imported: worn once it's found to be an avatar
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
        that are missing; and find your avatars and whether the mirror stands."""
        self._run([hand_models.HANDVIEW, "--models"], self._models_listed, {"FT_AVATARS_DIR": self._folder})
        self._ipc.run(self._scan, self._scanned)

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
        env = {"FT_AVATARS_DIR": self._folder, "FT_HANDS_VIEW_MODEL": name, "FT_HANDS_VIEW_STYLE": "solid"}
        self._picture([hand_models.HANDVIEW, "--thumb", path, "--res", str(hand_models.THUMB_PX)], env, self._cache, name, path)

    def _picture(self, argv, env, cache, name, path):
        """Queue the program that draws a model's picture into `path`; its older pictures go once it has."""
        os.makedirs(cache, exist_ok=True)

        def drawn(code, _out):
            if code != 0:
                return
            self._forget_pictures(cache, name, path)
            self.changed.emit()

        self._run(argv, drawn, env)

    @staticmethod
    def _forget_pictures(cache, name, keep=""):
        for old in hand_models.stale_thumbs(cache, name, keep):
            try:
                os.remove(old)
            except OSError:
                pass

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
    def _save(self, key, value):
        """The setting written. False (and a message) when it couldn't be."""
        try:
            ft_conf.set_value(key, value, self._conf)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't save the setting: {e}", True)
            return False
        return True

    def _put(self, key, value, command):
        if self._save(key, value):
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
                self._wear = name
                self.refresh()
            self.changed.emit()

        self._run(argv, imported, first=True)

    @Slot(str)
    def remove(self, name):
        """Delete one of your models (its file in your folder); the Generic hand takes its place."""
        if (name, "own") not in self._listed and not any(name == a["name"] for a in self._avatars):
            return
        path = os.path.join(self._folder, name + ".ftmodel")
        was_current = name == self.model
        try:
            os.remove(path)
        except OSError as e:
            self.message.emit(f"Couldn't remove {name}: {e}", True)
            return
        self._forget_pictures(self._cache, name)
        self._forget_pictures(self._avatar_cache, name)
        self._listed = [m for m in self._listed if m[0] != name]
        if name == self.avatar:   # nobody to show: the mirror goes, and the setting names no file
            self.setMirror(False)
            self._save("AVATAR_MODEL", "")
        self._avatars = [a for a in self._avatars if a["name"] != name]
        if was_current:
            self._put("HANDS_VIEW_MODEL", "asset", "model asset")
        self.changed.emit()
        self.refresh()

    # --- avatars and the mirror ---
    def _scan(self, client):
        """On the worker's thread (the models' files are read): your avatars, and whether the
        mirror stands."""
        answer = client.ask(avatar_models.SOCKET, "state", 0.3)
        return avatar_models.listed(self._folder), bool(answer and answer.startswith("ok"))

    def _scanned(self, found):
        self._avatars, self._mirror = found
        for item in self._avatars:
            name = item["name"]
            path = hand_models.thumb_path(self._avatar_cache, name, "own", self._folder)
            if not os.path.exists(path):
                self._picture(avatar_models.thumb_command(self._folder, name, path), {}, self._avatar_cache, name, path)
        wear, self._wear = self._wear, ""
        if wear:
            self.wear(wear)
        self.changed.emit()

    def _tell_mirror(self, text):
        """A command to the mirror, if it stands."""
        self._ipc.ask(avatar_models.SOCKET, text)

    @Property("QVariantList", notify=changed)
    def avatars(self):
        current, out = self.avatar, []
        for item in self._avatars:
            path = hand_models.thumb_path(self._avatar_cache, item["name"], "own", self._folder)
            out.append({**item, "thumb": QUrl.fromLocalFile(path).toString() if os.path.exists(path) else "",
                        "current": item["name"] == current})
        return out

    @Property(str, notify=changed)
    def avatar(self):
        """The avatar you wear: one of yours, or "" for none."""
        name = self._setting("AVATAR_MODEL", "")
        return name if any(name == a["name"] for a in self._avatars) else ""

    @Property(bool, notify=changed)
    def mirror(self):
        return self._mirror

    @Property(bool, constant=True)
    def canMirror(self):
        return os.path.exists(avatar_models.AVATARS)

    @Slot(str)
    def wear(self, name):
        """Wear one of your avatars: the mirror shows its body, and its hands become yours (the
        hand model can be changed after)."""
        if not any(name == a["name"] for a in self._avatars) or name == self.avatar:
            return
        if not self._save("AVATAR_MODEL", name):
            return
        if self._mirror:
            self._tell_mirror("model " + avatar_models.path(self._folder, name))
        self.choose(name)
        self.changed.emit()

    @Slot(bool)
    def setMirror(self, on):
        """Stand the mirror in the room in front of you, with the avatar you wear in it, or take it away."""
        if on == self._mirror or (on and not self.avatar):
            return
        if not on:
            self._tell_mirror("quit")
        elif not start_detached(tool_command(*avatar_models.mirror_command(self._folder, self.avatar))):
            self.message.emit("Couldn't start the mirror", True)
            return
        else:
            QTimer.singleShot(MIRROR_UP_MS, self, self._mirror_up)
        self._mirror = on
        self.changed.emit()

    def _mirror_up(self):
        """A while after it was started: a mirror that doesn't answer didn't come up (it needs SteamVR)."""
        def answered(answer):
            if self._mirror and not (answer and answer.startswith("ok")):
                self._mirror = False
                self.message.emit("The mirror didn't come up. Is SteamVR running?", True)
                self.changed.emit()
        self._ipc.ask(avatar_models.SOCKET, "state", answered, 0.3)
