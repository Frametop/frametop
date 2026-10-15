"""What the recorders' windows share (hands/rec/ft_handrec.py, gaze/rec/ft_eyerec.py): a QObject
backend with the consent and profile, the session's controls, export on a thread, the Hugging Face
login and upload in hub.py child processes, the Frame kept awake while they run, and small
helpers. Each recorder's Backend inherits it, adds its checklist, session and review, and keeps
these slot and property names, which its main.qml calls.

A "session" here is what the recorder records in one go: a hand session, or an eye round.

Needs PySide6 (the dev container has it; the standalone recorders' venvs too)."""
import collections
import datetime
import json
import os
import re
import subprocess
import sys
import threading
import uuid

from PySide6.QtCore import Property, QObject, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QDesktopServices, QGuiApplication, QPalette

from recorder import hub as rhub
from recorder import units
from recorder.common import write_json
from recorder.files import Cancelled, free_bytes, human_bytes, tree_bytes

# The headset counts as worn while vrcompositor runs and a display panel is lit: SteamVR turns
# the panels off 5 s after the headset comes off (frame-job's check; the proximity sensor's
# readings are too noisy). The dev container sees the host's processes and /sys.
BACKLIGHTS = "/sys/class/backlight"


def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""


def consent_version(path):
    """The "Version: ..." line of a CONSENT.md: a new one asks everyone to agree again."""
    m = re.search(r"^Version:\s*(\S+)", read_text(path), re.M)
    return m.group(1) if m else "unknown"


def process_running(name):
    """Like pgrep -x NAME, from /proc."""
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm") as f:
                    if f.read().strip() == name:
                        return True
            except OSError:
                pass
    return False


def headset_worn():
    """True while vrcompositor runs and any panel's backlight is on; False if unreadable."""
    lit = False
    try:
        for name in os.listdir(BACKLIGHTS):
            try:
                with open(os.path.join(BACKLIGHTS, name, "brightness")) as f:
                    lit = lit or int(f.read().strip()) > 0
            except (OSError, ValueError):
                pass
    except OSError:
        return False
    return lit and process_running("vrcompositor")


def gigabytes(n):
    return f"{n / 1000 ** 3:.1f} GB"


def _last_json(text):
    for line in reversed(text.strip().splitlines()):
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict):
            return obj
    return None


class RecorderBackend(QObject):
    """The shared part of a recorder's window backend. A subclass sets the class attributes
    below and passes its store (takes.Store: base, profile(), profile_path, export(),
    export_dir(), delete_export(), exports_dir) and its hub module (hub.py: dataset_id,
    meta_path, HF_DATASET, DATASET_ENV, ALLOW_ENV)."""
    APP = "Frametop recorder"           # systemd-inhibit's --who, and the units' descriptions
    AWAKE_UNIT = ""                     # the host unit that keeps the Frame awake
    AWAKE_WHY = "Exporting or uploading recordings"
    HUB_PATH = ""                       # the recorder's hub.py
    CONSENT_PATH = ""
    UPLOAD_PATH = ""
    ACTIVE_STATES = ()                  # the session states in which it records or waits
    ROUND_BYTES = 10 * 1000 ** 3        # free space a session needs
    PROFILE_KEEP = ()                   # profile.json keys a new consent keeps
    # A second consent text the window can switch to (the hand recorder's TESTING.md for its test
    # phase): {kind: path}, each agreed to under its own profile.json key (CONSENT_KEYS).
    CONSENT_PATHS = {}
    CONSENT_KEYS = {"public": "consent", "testing": "testing_consent"}
    CONSENT_KIND = "public"             # the kind the window starts with

    profileChanged = Signal()
    diskChanged = Signal()
    statusChanged = Signal()
    sessionsChanged = Signal()
    exportChanged = Signal()
    exportDone = Signal(str, bool)   # session, exported (False: failed or cancelled), after the list is fresh
    message = Signal(str, bool)  # text, is error
    loginChanged = Signal()
    uploadChanged = Signal()
    # From other threads (the session runner, export, hub.py's readers): queued to this one.
    _statusArrived = Signal(dict)
    _exportProgress = Signal(float, str)
    _exportFinished = Signal(str, str)  # path, error ("" when it worked; "cancelled")
    _loginArrived = Signal(dict)
    _loginLine = Signal(dict)
    _uploadLine = Signal(dict)
    _uploadFinished = Signal(dict)

    def __init__(self, store, hub, hub_dry_run=False):
        super().__init__()
        self.store = store
        self.hub = hub
        self._consent_kind = self.CONSENT_KIND
        self._hub_dry_run = hub_dry_run
        self._login = {"state": "dry" if hub_dry_run else "unknown"}
        self._login_busy = False
        self._login_proc = None      # hub.py login: waiting for the browser
        self._login_cancelled = False
        self._upload_proc = None
        self._upload_thread = None
        self._upload_cancelled = False
        self._upload = {}
        os.makedirs(store.base, mode=0o700, exist_ok=True)
        self._session = None
        self._session_id = ""
        self._status = {}
        self._awake = set()          # what keeps the Frame awake now: "export", "upload"
        self._awake_lock = threading.Lock()
        self._export_cancel = None
        self._export_thread = None
        self._export_fraction = 0.0
        self._export_text = ""
        self._export_session = ""
        self._statusArrived.connect(self._on_status)
        self._exportProgress.connect(self._on_export_progress)
        self._exportFinished.connect(self._on_export_finished)
        self._loginArrived.connect(self._on_login)
        self._loginLine.connect(self._on_login_line)
        self._uploadLine.connect(self._on_upload_line)
        self._uploadFinished.connect(self._on_upload_finished)
        self.disk_timer = QTimer(interval=30000, timeout=self.diskChanged.emit)
        self.disk_timer.start()

    def _thread(self, fn):
        thread = threading.Thread(target=fn, daemon=True)
        thread.start()
        return thread

    # --- hooks for the subclass. (A subclass's own properties can't notify with this class's
    # signals: PySide6 6.11 leaves them without one. It declares its own, or relays these.)
    def _runner_error(self):
        """Why the session runner can't be loaded, or ""."""
        return ""

    def _session_rows(self):
        """The sessions for the Review, Export and Upload pages: dicts with at least id, label,
        takes, sizeText, exported, export_stale, exportText, active."""
        return []

    def _texts_draft(self):
        """CONSENT.md or UPLOAD.md is still a draft."""
        return any(rhub.is_draft(p) for p in (self.CONSENT_PATH, self.UPLOAD_PATH))

    def _upload_allowed(self):
        return False

    def _upload_allowed_for(self, session):
        """Real uploads of this session may start (a subclass with more than one dataset says)."""
        return self._upload_allowed()

    def _session_dataset(self, session):
        """The dataset this session's export goes to."""
        return self._dataset()

    def _upload_values(self, session, path):
        """UPLOAD.md's @NAME@ placeholders beyond the export's path and size, the contributor and
        the dataset."""
        return {"SESSION": session}

    def _keep_awake(self):
        """False: don't start the awake unit (a dry run)."""
        return not self._hub_dry_run

    # --- consent and profile
    def _consent_path(self, kind=None):
        kind = kind or self._consent_kind
        return self.CONSENT_PATHS.get(kind) or self.CONSENT_PATH

    def has_consent(self, kind=None):
        """profile.json holds an agreement to the current version of that kind's text (default:
        the window's kind) by an adult, with a contributor id."""
        kind = kind or self._consent_kind
        profile = self.store.profile()
        consent = profile.get(self.CONSENT_KEYS.get(kind, "consent")) or {}
        return bool(profile.get("contributor") and consent.get("adult") is True
                    and consent.get("version") == consent_version(self._consent_path(kind)))

    @Property(str, notify=profileChanged)
    def consentKind(self):
        return self._consent_kind

    @Slot(str)
    def setConsentKind(self, kind):
        """Which consent the Welcome page shows and needsConsent asks about: "public" (CONSENT.md)
        or one of CONSENT_PATHS (the hand recorder's "testing")."""
        if kind in self.CONSENT_KEYS and (kind == "public" or kind in self.CONSENT_PATHS) and kind != self._consent_kind:
            self._consent_kind = kind
            self.profileChanged.emit()

    @Property(str, notify=profileChanged)
    def consentText(self):
        path = self._consent_path()
        return read_text(path) or f"{os.path.basename(path)} is missing."

    @Property(str, notify=profileChanged)
    def consentVersion(self):
        return consent_version(self._consent_path())

    @Property(bool, notify=profileChanged)
    def textsDraft(self):
        return self._texts_draft()

    @Property(bool, notify=profileChanged)
    def needsConsent(self):
        return not self.has_consent()

    @Property(str, notify=profileChanged)
    def contributor(self):
        return self.store.profile().get("contributor", "")

    @Property(str, notify=profileChanged)
    def consentAccepted(self):
        return (self.store.profile().get(self.CONSENT_KEYS.get(self._consent_kind, "consent")) or {}).get("accepted", "")

    def _write_consent(self, adult, region, agree, optional):
        """Write profile.json: a contributor id, once made, stays (a new consent version keeps it),
        and the PROFILE_KEEP keys stay. optional: the profile's "optional" answers. region: not
        living in Illinois, Texas or Washington (CONSENT.md "Who can take part")."""
        if not (adult and region and agree):
            self.message.emit("All three boxes need ticking to take part", True)
            return False
        old = self.store.profile()
        key = self.CONSENT_KEYS.get(self._consent_kind, "consent")
        profile = {"schema": 1, "contributor": old.get("contributor") or str(uuid.uuid4()),
                   key: {"version": consent_version(self._consent_path()),
                         "accepted": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                         "adult": True, "region_ok": True},
                   "optional": optional}
        # the other consent's record stays: agreeing to one text doesn't withdraw the other
        profile.update({k: old[k] for k in tuple(self.PROFILE_KEEP) + tuple(self.CONSENT_KEYS.values())
                        if k in old and k not in profile})
        write_json(self.store.profile_path, profile)
        self.profileChanged.emit()
        self.message.emit("Thank you. Your contributor id: " + profile["contributor"], False)
        return True

    @Property(str, notify=diskChanged)
    def freeText(self):
        return gigabytes(free_bytes(self.store.base))

    @Property(bool, notify=diskChanged)
    def diskOk(self):
        return free_bytes(self.store.base) >= self.ROUND_BYTES

    # --- the session's controls (the subclass starts it: self._session, with next_step(),
    # redo(), pause(), resume(), skip(), stop() and a session_dir)
    @Property(str, notify=statusChanged)
    def runnerError(self):
        return self._runner_error()

    @Property("QVariantMap", notify=statusChanged)
    def status(self):
        return self._status

    @Property(bool, notify=statusChanged)
    def sessionActive(self):
        return self._session is not None and self._status.get("state", "starting") in self.ACTIVE_STATES

    @Property(str, notify=statusChanged)
    def sessionId(self):
        return self._session_id

    def _on_status(self, status):
        self._status = status
        if self._session is not None and not self._session_id:
            self._session_id = os.path.basename(str(getattr(self._session, "session_dir", "") or ""))
        self.statusChanged.emit()
        if status.get("state") in ("done", "stopped", "error"):
            self.sessionsChanged.emit()

    def _control(self, name):
        if self._session is None:
            return
        try:
            getattr(self._session, name)()
        except Exception as e:
            self.message.emit(f"{name}: {e}", True)

    @Slot()
    def nextStep(self):
        if self.sessionActive and self._status.get("waiting"):
            self._control("next_step")

    @Slot()
    def redo(self):
        if self.sessionActive and self._status.get("can_redo"):
            self._control("redo")

    @Slot()
    def togglePause(self):
        if self._status.get("state") == "paused":
            self._control("resume")
        elif self.sessionActive:
            self._control("pause")

    @Slot()
    def skipSection(self):
        if self.sessionActive:
            self._control("skip")

    @Slot()
    def stopSession(self):
        """Stop: the take in progress is kept, as far as it got. It can block while the
        recording is written out, so it runs off this thread."""
        if self.sessionActive:
            session = self._session
            self._thread(lambda: session.stop())

    def _active_guard(self, session):
        if self.sessionActive and session == self._session_id:
            self.message.emit("This session is still recording: stop it first", True)
            return True
        return False

    @Property("QVariantList", notify=sessionsChanged)
    def sessions(self):
        return self._session_rows()

    @Slot()
    def refreshSessions(self):
        self.sessionsChanged.emit()
        self.diskChanged.emit()

    def shutdown(self):
        """The window closes: end a running session (blocking, so its files are complete), the
        export (waiting, so it can remove its half-written copy), the login and the upload."""
        if self.sessionActive:
            try:
                self._session.stop()
            except Exception:
                pass
        if self._export_cancel:
            self._export_cancel.set()
            self._export_thread.join(15)
        self.cancelLogin()
        if self._upload_proc:
            self.cancelUpload()
            self._upload_thread.join(10)
        if self._awake and self.AWAKE_UNIT:
            try:
                units.stop_unit(self.AWAKE_UNIT)
            except Exception:
                pass

    # --- export
    @Slot(result=bool)
    def headsetWorn(self):
        return headset_worn()

    @Property(bool, notify=exportChanged)
    def exporting(self):
        return self._export_cancel is not None

    @Property(float, notify=exportChanged)
    def exportFraction(self):
        return self._export_fraction

    @Property(str, notify=exportChanged)
    def exportText(self):
        return self._export_text

    @Property(str, notify=exportChanged)
    def exportSessionId(self):
        return self._export_session

    @Property(str, constant=True)
    def exportsDir(self):
        return self.store.exports_dir

    def _stay_awake(self, why, on):
        """Hold off sleep while an export or upload runs, so the headset can be taken off and
        left plugged in: a host unit running systemd-inhibit, started with the first reason
        and stopped with the last."""
        if not self.AWAKE_UNIT or not self._keep_awake():
            return

        def run():
            with self._awake_lock:
                had = bool(self._awake)
                (self._awake.add if on else self._awake.discard)(why)
                try:
                    if self._awake and not had:
                        units.start_unit(self.AWAKE_UNIT, "keeps the Frame awake while exporting or uploading",
                                         ["systemd-inhibit", "--what=sleep:idle", "--mode=block",
                                          "--who=" + self.APP, "--why=" + self.AWAKE_WHY, "sleep", "infinity"],
                                         app=self.APP)
                    elif had and not self._awake:
                        units.stop_unit(self.AWAKE_UNIT)
                except Exception as e:
                    print(f"keeping the Frame awake: {e}", file=sys.stderr, flush=True)
        self._thread(run)

    @Slot(str, bool)
    def exportSession(self, session, keep_notes):
        if self.exporting or self._active_guard(session):
            return
        cancel = threading.Event()
        self._export_cancel = cancel
        self._export_session = session
        self._export_fraction = 0.0
        self._export_text = "Starting"
        self.exportChanged.emit()
        self._stay_awake("export", True)

        def run():
            try:
                path = self.store.export(session, progress=lambda f, text: self._exportProgress.emit(f, text),
                                         cancel=cancel, keep_notes=keep_notes)
                self._exportFinished.emit(path, "")
            except Cancelled:
                self._exportFinished.emit("", "cancelled")
            except Exception as e:
                self._exportFinished.emit("", str(e) or type(e).__name__)
        self._export_thread = self._thread(run)

    @Slot()
    def cancelExport(self):
        if self._export_cancel:
            self._export_cancel.set()
            self._export_text = "Cancelling"
            self.exportChanged.emit()

    def _on_export_progress(self, fraction, text):
        if self._export_cancel and not self._export_cancel.is_set():
            self._export_fraction, self._export_text = fraction, text
            self.exportChanged.emit()

    def _on_export_finished(self, path, error):
        self._export_cancel = None
        self._stay_awake("export", False)
        if error == "cancelled":
            self._export_text = "Cancelled: nothing was kept"
        elif error:
            self._export_text = "Failed: " + error
            self.message.emit("Export failed: " + error, True)
        else:
            self._export_fraction = 1.0
            self._export_text = f"Exported to {path} ({human_bytes(tree_bytes(path))})"
            self.message.emit("Export ready", False)
        self.exportChanged.emit()
        self.refreshSessions()
        self.exportDone.emit(self._export_session, not error)

    @Slot(str)
    def deleteExport(self, session):
        try:
            self.store.delete_export(session)
            self.message.emit("Export deleted; the recordings stay", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete the export: {e}", True)
        self.refreshSessions()

    # --- upload
    def _dataset(self):
        """The dataset the window's consent kind uploads to (a subclass with more than one says)."""
        try:
            return self.hub.dataset_id()
        except rhub.HubError:
            return os.environ.get(self.hub.DATASET_ENV, self.hub.HF_DATASET)

    @Property(str, notify=profileChanged)
    def dataset(self):
        return self._dataset()

    @Property(str, notify=profileChanged)
    def datasetUrl(self):
        return rhub.dataset_url(self.dataset)

    @Slot(str, result=str)
    def datasetFor(self, session):
        """The dataset a session's export goes to (the Upload page's)."""
        return self._session_dataset(session) if session else self._dataset()

    @Slot(str, result=str)
    def datasetUrlFor(self, session):
        return rhub.dataset_url(self.datasetFor(session))

    @Slot(str, result=bool)
    def uploadAllowedFor(self, session):
        return self._upload_allowed_for(session) if session else self._upload_allowed()

    @Property(bool, constant=True)
    def hubDryRun(self):
        return self._hub_dry_run

    @Property(bool, notify=profileChanged)
    def uploadAllowed(self):
        """Real uploads may start (the subclass's _upload_allowed: the texts are reviewed, or a
        rehearsal)."""
        return self._upload_allowed()

    @Property(bool, constant=True)
    def allowUploadSet(self):
        return os.environ.get(self.hub.ALLOW_ENV) == "1"

    def _hub_argv(self, *args):
        return [sys.executable, self.HUB_PATH, "--base", self.store.base, *args]

    # The login: hub.py whoami in a child process (it asks huggingface.co)
    @Property("QVariantMap", notify=loginChanged)
    def login(self):
        """{"state": unknown|checking|ok|none|read|error|missing|dry|starting|waiting, "name", "role",
        "text", "link"}; while waiting, "url" and "code" (the browser login's)."""
        return self._login

    @Slot()
    def checkLogin(self):
        if self._hub_dry_run or self._login_busy:
            return
        self._login_busy = True
        self._login = {"state": "checking", "text": "Checking your Hugging Face login"}
        self.loginChanged.emit()
        argv = self._hub_argv("whoami", "--json")

        def run():
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=60)
                result = _last_json(r.stdout) or {"error": {"kind": "hub", "text": (r.stderr.strip().splitlines()
                                                                                     or ["hub.py failed"])[-1]}}
            except (OSError, subprocess.TimeoutExpired) as e:
                result = {"error": {"kind": "network", "text": f"The login check didn't finish: {e}"}}
            self._loginArrived.emit(result)
        self._thread(run)

    def _on_login(self, result):
        self._login_busy = False
        self._login_proc = None
        if result.get("cancelled"):
            self._login = {"state": "unknown"}
            self.checkLogin()   # whatever was saved before is still there
            return
        if "done" in result:
            who = result["done"]
            role = who.get("role", "")
            if role == "read":
                self._login = {"state": "read", "name": who.get("name", ""), "role": role, "link": rhub.TOKENS_URL,
                               "text": f"Logged in as {who.get('name', '')}, but with a read-only token, which "
                                       "can't upload. Log in again."}
            else:
                self._login = {"state": "ok", "name": who.get("name", ""), "role": role,
                               "text": f"Logged in as {who.get('name', '')}"}
        else:
            e = result.get("error") or {}
            state = {"login": "none", "missing": "missing"}.get(e.get("kind"), "error")
            self._login = {"state": state, "text": e.get("text", "The login check failed"), "link": e.get("link", "")}
        self.loginChanged.emit()

    def _read_lines(self, proc, line_signal, cancelled, stopped):
        """hub.py's JSON lines: progress to line_signal, then its last (done or error), or one
        made from stderr when there's none. stderr is read as it comes, keeping its last lines: read
        only at the end, more than a pipe holds there (huggingface_hub's retry warnings on a bad
        network) would stop hub.py and this reader both."""
        last = {}
        said = collections.deque(maxlen=50)
        drain = threading.Thread(target=lambda: said.extend(proc.stderr), name="hub-stderr", daemon=True)
        drain.start()
        for line in proc.stdout:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict):
                if "done" in obj or "error" in obj:
                    last = obj
                else:
                    line_signal.emit(obj)
        proc.wait()
        drain.join(5)   # stderr ends with hub.py, unless something it started still holds it
        err = "".join(said)
        proc.stdout.close()
        if not drain.is_alive():
            proc.stderr.close()
        if not last and not cancelled():
            tail = (err.strip().splitlines() or [f"exit status {proc.returncode}"])[-1]
            last = {"error": {"kind": "hub", "text": f"{stopped}: {tail}"}}
        return last

    # The browser login: hub.py login --json in a child process. It gets a link and a short code;
    # the link opens in the browser, the person types the code there (Hugging Face doesn't fill it
    # in, 2026-10-03) and approves, and hub.py saves the token. The token never reaches this process.
    @Slot()
    def logIn(self):
        if self._hub_dry_run or self._login_proc or self._login_busy:
            return
        try:
            proc = subprocess.Popen(self._hub_argv("login", "--json"), stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            self._on_login({"error": {"kind": "hub", "text": f"Couldn't start the login: {e}"}})
            return
        self._login_proc = proc
        self._login_cancelled = False
        self._login_busy = True
        self._login = {"state": "starting", "text": "Getting a login link from Hugging Face"}
        self.loginChanged.emit()

        def run():
            last = self._read_lines(proc, self._loginLine, lambda: self._login_cancelled, "The login stopped")
            self._loginArrived.emit({"cancelled": True} if self._login_cancelled else last)
        self._thread(run)

    @Slot()
    def cancelLogin(self):
        if self._login_proc and self._login_proc.poll() is None:
            self._login_cancelled = True
            self._login_proc.terminate()

    def _on_login_line(self, obj):
        if obj.get("phase") == "code" and obj.get("url"):
            self._login = {"state": "waiting", "url": obj["url"], "code": obj.get("code", ""),
                           "expires_in": obj.get("expires_in", 0), "text": obj.get("text", "")}
            self.loginChanged.emit()
            QDesktopServices.openUrl(QUrl(obj["url"]))

    # The upload: hub.py upload --json in a child process; its lines say how it goes
    @Property(bool, notify=uploadChanged)
    def uploading(self):
        return self._upload_proc is not None

    @Property("QVariantMap", notify=uploadChanged)
    def upload(self):
        """{"session", "phase", "text", "fraction" (-1: unknown), "log", "result": {...}, "error": {...}}."""
        return self._upload

    @Slot(str, result="QVariantMap")
    def uploadInfo(self, session):
        """{"previous": the last upload of this same export or {}, "uploads": how many in all}."""
        try:
            export = self.store.export_dir(session)
            meta = self.hub.meta_path(self.store, session)
            before = rhub.previous_upload(meta, rhub.export_sha(export))
            count = len(rhub.uploads(meta))
        except (OSError, ValueError):
            return {"previous": {}, "uploads": 0}
        return {"previous": before or {}, "uploads": count}

    @Slot(str, bool)
    def startUpload(self, session, again):
        if self.uploading or self.exporting or self._active_guard(session):
            return
        if not self._hub_dry_run and not self._upload_allowed_for(session):
            self.message.emit("Contributions aren't open yet", True)
            return
        argv = self._hub_argv("upload", session, "--json")
        if self._hub_dry_run:
            argv.append("--dry-run")
        if again:
            argv.append("--again")
        try:
            proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, bufsize=1)
        except OSError as e:
            self.message.emit(f"Couldn't start the upload: {e}", True)
            return
        self._upload_proc = proc
        self._upload_cancelled = False
        self._upload = {"session": session, "phase": "check", "text": "Starting", "fraction": 0.0, "log": "",
                        "result": {}, "error": {}, "pr_url": ""}
        self.uploadChanged.emit()
        self._stay_awake("upload", True)

        def run():
            last = self._read_lines(proc, self._uploadLine, lambda: self._upload_cancelled, "The upload stopped")
            self._uploadFinished.emit(last or {"error": {"kind": "cancelled", "text": "Cancelled"}})
        self._upload_thread = self._thread(run)

    @Slot()
    def cancelUpload(self):
        if self._upload_proc and self._upload_proc.poll() is None:
            self._upload_cancelled = True
            self._upload_proc.terminate()
            self._upload = dict(self._upload, text="Cancelling")
            self.uploadChanged.emit()

    def _on_upload_line(self, obj):
        up = dict(self._upload)
        if "log" in obj:
            up["log"] = (up.get("log", "") + obj["log"] + "\n")[-20000:]
        if "phase" in obj:
            f = obj.get("fraction")
            up.update(phase=obj["phase"], text=obj.get("text", ""), fraction=-1.0 if f is None else float(f))
        if obj.get("pr_url"):
            up["pr_url"] = obj["pr_url"]   # the pull request is open: the files are on their way
        self._upload = up
        self.uploadChanged.emit()

    def _on_upload_finished(self, last):
        self._upload_proc = None
        self._stay_awake("upload", False)
        up = dict(self._upload)
        if "done" in last:
            result = last["done"]
            up.update(result=result, error={}, phase="done", fraction=1.0,
                      text="Dry run: nothing was uploaded" if result.get("dry_run") else "Uploaded")
            if not result.get("dry_run"):
                self.message.emit("Uploaded: the pull request is open", False)
        else:
            e = last.get("error") or {}
            cancelled = e.get("kind") == "cancelled"
            up.update(error=e, result={}, phase="failed", fraction=0.0,
                      text=("Cancelled. Its pull request stays open: press Upload again to finish it there."
                            if up.get("pr_url") else "Cancelled") if cancelled else e.get("text", ""))
            if not cancelled:
                self.message.emit("Upload failed", True)
        self._upload = up
        self.uploadChanged.emit()
        self.sessionsChanged.emit()

    @Slot(str, result=str)
    def uploadText(self, session):
        """UPLOAD.md with this export's path and size filled in."""
        try:
            path = self.store.export_dir(session)
        except ValueError:
            return ""
        values = {"EXPORT_PATH": path, "EXPORT_SIZE": human_bytes(tree_bytes(path)),
                  "CONTRIBUTOR": self.contributor or "CONTRIBUTOR", "DATASET": self._session_dataset(session)}
        values.update(self._upload_values(session, path))
        text = read_text(self.UPLOAD_PATH) or "UPLOAD.md is missing."
        if rhub.is_draft(self.UPLOAD_PATH):
            text = text.split("\n", 1)[-1]   # the page's banner says it already
        return re.sub(r"@([A-Z_]+)@", lambda m: values.get(m.group(1), m.group(0)), text)

    # --- small things
    @Slot(QColor)
    def setLinkColor(self, color):
        palette = QGuiApplication.palette()
        palette.setColor(QPalette.Link, color)
        QGuiApplication.setPalette(palette)

    @Slot(str)
    def copy(self, text):
        QGuiApplication.clipboard().setText(text)
        self.message.emit("Copied", False)
