#!/usr/bin/env python3
"""Frametop Hand Recorder: record your hands for the open hand dataset (hands/rec/DESIGN.md).

A Kirigami (QML) app with a Python backend. It runs in the dev container:
  - Bounties (where it opens): the bounty board (recorder/bounties.py, hands/rec/bounties.json):
    short rounds on windows in the headset, what the hand tracker needs most, the most wanted
    first, with how often you've done each. Get the newest board fetches the dataset's (hub.py
    board). Picking one goes to Welcome if the consent isn't agreed to yet, then Before you start;
    a full session or a quick round can be picked there too. After a round, Export and upload
    exports it and goes on to Upload by itself.
  - Welcome: the consent text (CONSENT.md), shown the first time and again when its version
    changes. Agreeing writes profile.json with a random contributor id.
  - Before you start: the checklist (objects, controller straps, lighting, sleeves, privacy,
    free space), the camera check (hands/camcheck.py: Start stays off while the upper cameras
    are off, with a Restart SteamVR button; --ignore-cameras overrides it), a full session or a
    quick round (suggested once a full one is done), the demo clips to follow when the script
    has them (recorded hands or skeleton hands, kept in settings.json) and what will happen.
    Start hands it to the session runner (session.py).
  - Session: the runner's live status with the step's pose picture and where-to diagram, Next,
    Pause/Resume, Redo, Skip section and Stop (Space: Next, P, R, S and Esc while the window has
    focus; the button on the headset's right side is Next, pause and resume). The prompts appear
    in the headset. Each step waits for Next unless "Advance by itself" was ticked.
  - Review: sessions, their takes, and a viewer for one frame set at a time, where ranges,
    takes and sessions can be deleted (takes.py). Uploaded sessions can be deleted to free space
    while the record of their uploads stays (contributions.json), listed under "Your uploads".
  - Export: compress what's kept into exports/<session>/ at nice 19 (takes.py), with a warning
    when the headset is worn.
  - Upload: the Hugging Face login status, and Upload: hub.py checks the export (validate.py)
    and uploads it as a pull request, in a child process (Cancel ends it). UPLOAD.md explains
    the steps, with the export filled in, and the command to copy for a terminal upload.
Everything lives under ~/.local/share/frametop/hands/contrib (--base). Nothing is uploaded
unless the person presses Upload; while CONSENT.md or UPLOAD.md is a draft, Upload stays off
unless FT_HANDREC_ALLOW_UPLOAD=1 (the maintainer's rehearsal against a test repo, picked with
FT_HANDREC_DATASET). --hub-dry-run does everything but the network calls.
One window at a time (recorder/one_window.py): opening it again closes the open one, wherever it
is, unless that one is busy recording, exporting or uploading. Launch with hands/rec/ft-handrec
(host wrapper). The standalone Hand Recorder
(github.com/Frametop/frametop-hand-recorder) runs this backend on the host, from a venv, with
its own window for SteamVR's dashboard: --qml its main.qml, --style Basic.
"""
import argparse
import datetime
import json
import os
import signal
import subprocess
import sys
import time

from PySide6.QtCore import Property, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QFont, QGuiApplication, QIcon, QImage, QPainter
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider
from PySide6.QtQuickControls2 import QQuickStyle
import shiboken6

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hub  # noqa: E402
import takes  # noqa: E402  (next to this file)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
# The window's shared part (recorder/window.py): consent, export, login, upload, keeping awake.
from recorder import bounties, one_window  # noqa: E402
from recorder.common import host_command  # noqa: E402
from recorder.window import (RecorderBackend, consent_version as _consent_version, gigabytes,  # noqa: E402,F401
                             headset_worn, process_running, read_text)

# The dataset contributions go to (hub.py; FT_HANDREC_DATASET overrides it, for tests).
HF_DATASET = hub.HF_DATASET
CONSENT_PATH = hub.CONSENT_PATH
UPLOAD_PATH = hub.UPLOAD_PATH
TESTING_PATH = hub.TESTING_PATH
HUB_PATH = os.path.join(HERE, "hub.py")
VALIDATE_PATH = os.path.join(HERE, "validate.py")
AWAKE_UNIT = "frametop-handrec-awake.service"
SCRIPT_PATH = os.path.join(HERE, "script.json")
DEBUG_SCRIPT = os.path.join(HERE, "debug.json")
# The testing session (session.py's debug.json): first on the board when Frametop's hand tracking
# is installed, picked like a bounty by this id.
DEBUG_ID = "debug"
DEBUG_ROW = {"id": DEBUG_ID, "title": "Testing session", "priority": "", "minutes": 3, "script": "",
             "blurb": "Use Frametop as you always do, for up to 3 minutes, while the cameras record your hands "
                      "and what Frametop's hand tracking made of them. When the tracking gets something wrong, "
                      "press the headset button: new versions of the hand tracking are then tested on your "
                      "recording, at those moments. For testing only, never for training."}
LIVE_HANDS_UNIT = "frametop-hands.service"
# One window at a time, Frametop's or the standalone one (recorder/one_window.py)
WINDOW_NAME = "ft_handrec_window"
RECORD_BYTES = 16 * 1000 ** 3  # about what one round of recording takes (a bounty: 2.5-3 min at 30 sets/s, ~85 MB/s)
# The free space a round needs before it starts: the recording, then its export next to it
# (takes.export_need). Until 2026-10-17 only the recording was asked for, so a round could be
# recorded that couldn't be exported.
ROUND_BYTES = RECORD_BYTES + takes.export_need(RECORD_BYTES)
# The checklist's choices; the keys are what session.json stores.
OBJECTS = [("pencil", "Pencil or pen"), ("phone", "Phone"), ("cup", "Cup or mug (empty)"),
           ("keyboard", "Keyboard"), ("mouse", "Mouse"), ("gamepad", "Gamepad"),
           ("small", "Something small (a coin, a key, a bottle cap)")]
# "auto": the cameras' measurement (session.classify_lighting: indoor or daylight); the rest
# correct it, since the cameras can't tell a dim room from a bright one.
LIGHTING = [("auto", "Measured by the cameras"), ("dim", "Dim: one lamp only"), ("room", "Normal room light"),
            ("daylight", "Daylight near a window")]
LIGHTING_TEXT = dict(LIGHTING[1:], indoor="Indoor light")
SLEEVES = [("short", "Short sleeves or bare arms"), ("long", "Long sleeves"), ("", "Rather not say")]
HANDEDNESS = [("", "Rather not say"), ("right", "Right-handed"), ("left", "Left-handed"),
              ("both", "Both (ambidextrous)")]
ACTIVE_STATES = ("starting", "intro", "ready", "countdown", "running", "paused", "between", "nohands")
# After Restart SteamVR: look at the cameras again this often, for at most this long.
RECHECK_S = 3
RECHECK_FOR_S = 120
# Shown side by side in the viewer at this height; thumbnails are smaller.
SET_HEIGHT = 480
THUMB_HEIGHT = 96


def consent_version():
    """The "Version: ..." line of CONSENT.md: a new one asks everyone to agree again."""
    return _consent_version(CONSENT_PATH)


def session_label(sid):
    """20261002-101500 -> 2026-10-02 10:15; 20261002-101500-2 -> 2026-10-02 10:15 (2)."""
    stamp, _, n = sid[:15], sid[15:16], sid[16:]
    try:
        label = datetime.datetime.strptime(stamp, "%Y%m%d-%H%M%S").strftime("%Y-%m-%d %H:%M")
    except ValueError:
        return sid
    return f"{label} ({n})" if n else label


def grey_image(cam):
    """A camera's raw 8-bit pixels as a QImage that owns its data."""
    img = QImage(cam["pixels"], cam["width"], cam["height"], cam["width"], QImage.Format_Grayscale8)
    return img.copy()


class FrameProvider(QQuickImageProvider):
    """image://frames/set/SESSION/TAKE/INDEX: one frame set, every camera side by side.
    image://frames/thumb/SESSION/TAKE: the take's first slam_left image, small.
    Anything after "?" is ignored (it makes QML load the image again)."""

    def __init__(self, store):
        super().__init__(QQuickImageProvider.ImageType.Image)
        self.store = store

    def requestImage(self, ident, size, requested):
        parts = ident.split("?", 1)[0].split("/")
        try:
            if parts[0] == "thumb" and len(parts) == 3:
                return self._thumb(parts[1], parts[2])
            if parts[0] == "set" and len(parts) == 4:
                return self._set(parts[1], parts[2], int(parts[3]),
                                 requested.height() if requested.height() > 0 else SET_HEIGHT)
        except (OSError, ValueError, IndexError, KeyError) as e:
            print(f"ft-handrec: image {ident}: {e}", file=sys.stderr)
        img = QImage(4, 3, QImage.Format_Grayscale8)
        img.fill(0)
        return img

    def _index(self, session, take):
        return takes.take_index(self.store.take_dir(session, take))

    def _thumb(self, session, take):
        index = self._index(session, take)
        if not len(index):
            raise ValueError("no sets")
        names = [c["name"] for c in index.cams]
        cams = index.read_set(0, only="slam_left" if "slam_left" in names else names[0])
        return grey_image(cams[0]).scaledToHeight(THUMB_HEIGHT, Qt.SmoothTransformation)

    def _set(self, session, take, i, height):
        cams = self._index(session, take).read_set(i)
        scaled = [grey_image(c).scaledToHeight(height, Qt.SmoothTransformation) for c in cams]
        gap = 8
        out = QImage(sum(s.width() for s in scaled) + gap * (len(scaled) - 1), height, QImage.Format_RGB32)
        out.fill(QColor(30, 30, 30))
        p = QPainter(out)
        font = QFont()
        font.setPixelSize(max(12, height // 28))
        p.setFont(font)
        x = 0
        for cam, img in zip(cams, scaled):
            p.drawImage(x, 0, img)
            p.setPen(QColor(255, 200, 80))
            p.drawText(x + 6, 6 + font.pixelSize(), cam["name"])
            x += img.width() + gap
        p.end()
        return out


class Backend(RecorderBackend):
    """The hand recorder's window backend: recorder/window.py's, plus the checklist, the light
    and camera checks, the session and the review."""
    APP = "Frametop Hand Recorder"
    AWAKE_UNIT = AWAKE_UNIT
    AWAKE_WHY = "Exporting or uploading hand recordings"
    HUB_PATH = HUB_PATH
    CONSENT_PATH = CONSENT_PATH
    CONSENT_PATHS = {"testing": TESTING_PATH}   # the bounties' test phase (DESIGN.md "Testing")
    CONSENT_KIND = "testing" if takes.TEST_PHASE else "public"
    UPLOAD_PATH = UPLOAD_PATH
    ACTIVE_STATES = ACTIVE_STATES
    ROUND_BYTES = ROUND_BYTES
    PROFILE_KEEP = ("bounties",)

    # A property here can't notify with the base's signals (PySide6 6.11 leaves it without
    # one), so these relay them.
    profileRelay = Signal()
    sessionsRelay = Signal()
    lightingChanged = Signal()
    _lightingArrived = Signal(str, str)
    cameraChanged = Signal()
    _cameraArrived = Signal(dict)
    guidanceChanged = Signal()
    boardChanged = Signal()
    _boardArrived = Signal(dict)
    readyChanged = Signal()
    _readyPart = Signal(str, str, str)
    _readyDone = Signal(dict)

    def __init__(self, store, session_options=None, hub_dry_run=False, ignore_cameras=False):
        super().__init__(store, hub, hub_dry_run)
        self.profileChanged.connect(self.profileRelay)
        self.sessionsChanged.connect(self.sessionsRelay)
        self._session_options = session_options or {}  # Session options: dry_run, speed, poses_dir (tests), button
        self._session_mod = None
        self._session_error = ""
        self._lighting_note = ""
        self._lighting_measured = ""
        self._lighting_busy = False
        self._camd_started = False   # ft-camd started for the light check: stopped on quit
        self._ignore_cameras = ignore_cameras
        self._camera = {}            # the last camera check (session.camera_check)
        self._camera_busy = False
        self._restarting = False
        self._cameraArrived.connect(self._on_camera)
        self._lightingArrived.connect(self._on_lighting)
        self.profileChanged.connect(self.boardChanged)    # the board's done marks
        self._board = {"state": "", "text": ""}
        self._board_busy = False
        self._boardArrived.connect(self._on_board)
        self._frametop_hands = None   # Frametop's hand tracking is installed (frametopHands)
        self._ready = {"state": "", "error": "", "round": "", "parts": {}}   # getReady
        self._ready_units = []       # units getReady started: stopped on quit
        self._ready_panel = None     # the ft-handpanel getReady started: ends on quit
        self._readyPart.connect(self._on_ready_part)
        self._readyDone.connect(self._on_ready_done)
        try:   # uploads older recorders kept only in session.json join the contributions log
            store.log_uploads()
        except (OSError, ValueError) as e:
            print(f"contributions log: {e}", file=sys.stderr, flush=True)

    def _texts_draft(self):
        return hub.texts_draft(self._consent_kind)

    def _upload_allowed(self):
        """Real uploads under the window's consent may start: its texts aren't drafts, or
        FT_HANDREC_ALLOW_UPLOAD=1."""
        return hub.upload_allowed(self._consent_kind)

    def _upload_allowed_for(self, session):
        return hub.upload_allowed(hub.session_kind(self.store, session))

    def _dataset(self):
        try:
            return hub.dataset_id(self._consent_kind)
        except hub.HubError:
            return hub.HF_TEST_DATASET if self._consent_kind == "testing" else hub.HF_DATASET

    def _session_label(self, session):
        return session_label(session)

    def _session_dataset(self, session):
        try:
            return hub.dataset_id(hub.session_kind(self.store, session))
        except (hub.HubError, OSError, ValueError):
            return self._dataset()

    @Slot(str, result=str)
    def roundConsentKind(self, bounty_id):
        """The consent a round picked on the Bounties page is recorded under: a bounty's or the
        testing session's ("testing" in the test phase), or a full session's ("public")."""
        return takes.round_consent_kind(bool(bounty_id), bounty_id == DEBUG_ID)

    # --- the session runner, imported when first needed (it's written separately)
    def _runner(self):
        if self._session_mod is None and not self._session_error:
            try:
                import session as mod
                self._session_mod = mod
            except Exception as e:  # missing, or broken: say so, the rest of the app still works
                self._session_error = f"The session runner (hands/rec/session.py) can't be loaded: {e}"
        return self._session_mod

    def _runner_error(self):
        self._runner()
        return self._session_error

    # --- consent and profile
    @Property(str, constant=True)
    def toolVersion(self):
        """What session.json records as the tool (takes.tool_version), for the window to show."""
        return takes.tool_version()

    @Property(str, notify=profileRelay)
    def handedness(self):
        return (self.store.profile().get("optional") or {}).get("handedness", "")

    @Property("QVariantList", constant=True)
    def handednessChoices(self):
        return [{"value": k, "text": v} for k, v in HANDEDNESS]

    @Slot(bool, bool, bool, str)
    def acceptConsent(self, adult, region, agree, handedness):
        """Write profile.json (recorder/window.py), with the handedness."""
        profile = self.store.profile()
        optional = profile.get("optional") if isinstance(profile.get("optional"), dict) else {}
        optional["handedness"] = handedness if handedness in dict(HANDEDNESS) else ""
        optional.setdefault("notes", "")
        self._write_consent(adult, region, agree, optional)

    # --- the demo clips to follow (the v2 script): kept in BASE/settings.json
    @Property(bool, constant=True)
    def scriptHasDemos(self):
        mod = self._runner()
        try:
            script = mod.load_script(SCRIPT_PATH) if mod else {}
        except Exception:
            return False
        return any(p.get("demo") for sec in script.get("sections", []) for p in sec.get("prompts", []))

    @Property("QVariantList", constant=True)
    def guidanceChoices(self):
        mod = self._runner()
        return [{"value": k, "text": v} for k, v in (mod.GUIDANCE if mod else ())]

    @Property(str, notify=guidanceChanged)
    def guidance(self):
        mod = self._runner()
        if not mod:
            return ""
        chosen = takes.read_json(os.path.join(self.store.base, "settings.json")).get("guidance")
        return chosen if chosen in dict(mod.GUIDANCE) else mod.default_guidance(
            self._session_options.get("recorded_demos_dir"))

    @Slot(str)
    def setGuidance(self, value):
        mod = self._runner()
        if not mod or value not in dict(mod.GUIDANCE) or value == self.guidance:
            return
        path = os.path.join(self.store.base, "settings.json")
        settings = takes.read_json(path)
        settings["guidance"] = value
        takes.write_json(path, settings)
        self.guidanceChanged.emit()

    # --- the bounty board (recorder/bounties.py): short rounds for what the tracker needs most
    def _board_now(self):
        mod = self._runner()
        return mod.load_board(self.store.base) if mod else None

    @Property("QVariantList", notify=boardChanged)
    def bounties(self):
        """The board's bounties (bounties.view), with "lastText" for the last round."""
        mod = self._runner()
        if not mod:
            return []
        rows = bounties.view(self._board_now(), self.store.profile(), {},
                             lambda n: mod.have_bounty_script(n, base_dir=self.store.base))
        if self.frametopHands:
            rows.insert(0, self._debug_row())
        for b in rows:
            b["lastText"] = session_label(b["last"]) if b["last"] else ""
            b["minutesText"] = f"about {b['minutes']:g} min" if b["minutes"] else ""
        return rows

    @Property(bool, constant=True)
    def frametopHands(self):
        """Frametop's hand tracking is installed here (its unit is known to systemd): the testing
        session records it in use, so it's offered only then."""
        if self._frametop_hands is None:
            forced = os.environ.get("FT_HANDREC_FRAMETOP_HANDS", "")   # tests: 1 or 0
            if forced in ("0", "1"):
                self._frametop_hands = forced == "1"
                return self._frametop_hands
            try:
                r = subprocess.run(host_command("systemctl", "--user", "show", "-p", "LoadState", "--value",
                                                LIVE_HANDS_UNIT), capture_output=True, text=True, timeout=10)
                self._frametop_hands = r.stdout.strip() == "loaded"
            except (OSError, subprocess.SubprocessError):
                self._frametop_hands = False
        return self._frametop_hands

    def _debug_row(self):
        """The testing session as a row of the board (bounties.view's keys). Uploaded ones whose
        local copy was deleted count from the contributions log."""
        done = {s["id"] for s in self.store.sessions() if s.get("debug") and s["status"] == "done" and not s["dry_run"]}
        done |= {c["session"] for c in self.store.contributions() if c.get("debug")}
        last = max(done) if done else ""
        return dict(DEBUG_ROW, done=len(done), last=last, available="")

    @Property(str, notify=boardChanged)
    def boardUpdated(self):
        board = self._board_now()
        return board["updated"] if board else ""

    @Property("QVariantMap", notify=boardChanged)
    def boardFetch(self):
        """The last fetch: {"state": ""|fetching|ok|error, "text"}."""
        return self._board

    @Slot()
    def refreshBoard(self):
        """hub.py board in a child process: the dataset's board into board-cache.json."""
        if self._board_busy or self._hub_dry_run:
            return
        self._board_busy = True
        self._board = {"state": "fetching", "text": "Getting the newest board"}
        self.boardChanged.emit()
        argv = self._hub_argv("board", "--json")

        def run():
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=60)
                lines = [json.loads(x) for x in r.stdout.splitlines() if x.startswith("{")]
                result = lines[-1] if lines else {"error": {"text": (r.stderr.strip().splitlines() or ["hub.py failed"])[-1]}}
            except (OSError, ValueError, subprocess.TimeoutExpired) as e:
                result = {"error": {"text": f"The board didn't come: {e}"}}
            self._boardArrived.emit(result)
        self._thread(run)

    def _on_board(self, result):
        self._board_busy = False
        if "done" in result:
            self._board = {"state": "ok", "text": f"The newest board, of {result['done'].get('updated', '?')}"}
        else:
            self._board = {"state": "error", "text": (result.get("error") or {}).get("text", "The board didn't come")
                           + " The board that came with the recorder is used."}
        self.boardChanged.emit()

    def _bounty(self, bounty_id):
        """The board's entry for a bounty that can be recorded, with the board's date, or None.
        (The testing session isn't a bounty: DEBUG_ID gives None.)"""
        b = next((b for b in self.bounties if b["id"] == bounty_id and b["available"] == ""), None)
        return dict(b, updated=self.boardUpdated) if b else None

    # --- the checklist
    @Property("QVariantList", constant=True)
    def objects(self):
        return [{"value": k, "text": v} for k, v in OBJECTS]

    @Property("QVariantList", constant=True)
    def lightingChoices(self):
        return [{"value": k, "text": v} for k, v in LIGHTING]

    @Property("QVariantList", constant=True)
    def sleeveChoices(self):
        return [{"value": k, "text": v} for k, v in SLEEVES]

    @Property(str, notify=lightingChanged)
    def lightingNote(self):
        """A warning: this light is like an earlier round's, or it couldn't be measured."""
        return self._lighting_note

    @Property(str, notify=lightingChanged)
    def lightingMeasured(self):
        """What the cameras see: "Indoor light" or "Daylight…", "Measuring…", or ""."""
        return "Measuring…" if self._lighting_busy else self._lighting_measured

    @Slot(str)
    def checkLighting(self, chosen):
        """Measure the light (starting ft-camd if nothing runs it) and compare it with earlier
        rounds (session.similar_lighting): a round in light like an earlier one adds less."""
        mod = self._runner()
        if not mod or self._lighting_busy:
            return
        base = self.store.base
        self._lighting_busy = True
        self.lightingChanged.emit()

        def run():
            measured = ""
            try:
                ring = mod.ring_lighting()
                # A dry run starts nothing, the camera broker included.
                if ring is None and not self.sessionActive and not self._session_options.get("dry_run"):
                    if mod.start_camd():
                        self._camd_started = True
                    time.sleep(1.0)    # the first near-black frames
                    ring = mod.ring_lighting()
                if ring is None:
                    note = ("The cameras aren't running, so the light can't be measured now. The session "
                            "measures it when it starts.")
                else:
                    kind = mod.classify_lighting(ring)
                    ir = mod.ambient_ir(ring)
                    measured = (f"{LIGHTING_TEXT.get(kind, kind)} (infrared {ir:.1f})" if kind
                                else "The cameras couldn't measure the light yet.")
                    match = mod.similar_lighting(base, {"chosen": chosen, "ring": ring})
                    note = "" if not match else (
                        f"The cameras see about the same light as in your round of {session_label(match[0])} "
                        f"({LIGHTING_TEXT.get(match[1], match[1] or 'not given')}). A different light helps the "
                        "dataset more: change the lighting if you can, or go ahead anyway.")
            except Exception as e:
                note = f"Couldn't measure the light: {e}"
            self._lightingArrived.emit(note, measured)
        self._thread(run)

    def _on_lighting(self, note, measured):
        self._lighting_busy = False
        self._lighting_note = note
        self._lighting_measured = measured
        self.lightingChanged.emit()

    # --- the camera check (hands/camcheck.py through session.camera_check)
    @Property(str, notify=cameraChanged)
    def cameraState(self):
        """ok, degraded, unknown; checking while it runs; "" before the first check."""
        if self._camera_busy and not self._camera:
            return "checking"
        return self._camera.get("status", "")

    @Property(str, notify=cameraChanged)
    def cameraText(self):
        """What to tell the person when the cameras aren't all running ("" otherwise)."""
        mod = self._runner()
        return mod.camera_text(self._camera) if mod and self._camera else ""

    @Property(str, notify=cameraChanged)
    def cameraSummary(self):
        return self._camera.get("summary", "")

    @Property(str, notify=cameraChanged)
    def cameraEvidence(self):
        return "\n".join(self._camera.get("evidence", []))

    @Property(bool, constant=True)
    def camerasIgnored(self):
        return self._ignore_cameras

    @Property(bool, notify=cameraChanged)
    def cameraBusy(self):
        return self._camera_busy

    @Property(bool, notify=cameraChanged)
    def restartingSteamVR(self):
        return self._restarting

    @Property(bool, notify=cameraChanged)
    def camerasBlockStart(self):
        """Start stays off: the check found the cameras degraded (unless --ignore-cameras)."""
        return not self._ignore_cameras and self._camera.get("status") == "degraded"

    def _check_now(self, repair=False):
        """repair (off this thread only: it may restart ft-camd, up to 15 s): see session.camera_check."""
        mod = self._runner()
        if not mod or self._session_options.get("dry_run"):
            return {"status": "unknown", "summary": "not checked (dry run)", "reason": "dry run", "evidence": []}
        return mod.camera_check(repair=repair)

    @Slot()
    def checkCameras(self):
        """Run the camera check off this thread (it reads the XRService log: a few MB)."""
        if self._camera_busy:
            return
        self._camera_busy = True
        self.cameraChanged.emit()
        self._thread(lambda: self._cameraArrived.emit(self._check_now(repair=True)))

    def _on_camera(self, result):
        self._camera = result
        self._camera_busy = self._restarting
        self.cameraChanged.emit()

    @Slot()
    def restartSteamVR(self):
        """systemctl --user restart steamvr.service on the host (--no-block: the job runs in
        systemd, so it finishes even when this window closes with the Frametop desktop), then
        the camera check every few seconds until the new XRService has opened its cameras."""
        mod = self._runner()
        if not mod or self._restarting or self.sessionActive:
            return
        self._restarting = self._camera_busy = True
        self.cameraChanged.emit()
        before = self._camera.get("log", "") if self._camera else ""   # each XRService start writes a new log

        def run():
            result = None
            try:
                r = subprocess.run(mod.host_command("systemctl", "--user", "restart", "--no-block",
                                                    "steamvr.service"), capture_output=True, text=True, timeout=60)
                if r.returncode != 0:
                    raise RuntimeError((r.stderr or r.stdout).strip() or "exit %d" % r.returncode)
                end = time.monotonic() + RECHECK_FOR_S
                time.sleep(RECHECK_S * 2)
                while time.monotonic() < end:
                    result = self._check_now(repair=True)
                    # Done once a new XRService (a new log) has opened its cameras, ok or not.
                    if result.get("status") in ("ok", "degraded") and result.get("log") != before:
                        break
                    time.sleep(RECHECK_S)
            except Exception as e:
                result = {"status": "unknown", "summary": f"unknown: couldn't restart SteamVR ({e})",
                          "reason": str(e), "evidence": []}
            self._restarting = False
            self._cameraArrived.emit(result or self._check_now())
        self._thread(run)

    # --- the session
    @Slot("QVariantMap", bool, bool, result=str)       # the standalone recorder's window: no bounties yet
    @Slot("QVariantMap", bool, bool, str, result=str)
    def planText(self, checklist, auto, quick, bounty_id=""):
        """How long a session with these answers takes (session.plan_summary): a full one, a
        quick round, or a bounty's round (bounty_id, else "")."""
        mod = self._runner()
        if not mod:
            return ""
        debug = bounty_id == DEBUG_ID
        entry = self._bounty(bounty_id) if bounty_id and not debug else None
        if bounty_id and not entry and not debug:
            return "Pick a bounty"
        try:
            script = mod.load_script(DEBUG_SCRIPT if debug else
                                     mod.bounty_script(entry["script"], base_dir=self.store.base) if entry else SCRIPT_PATH)
            quick = quick and not entry
            plan, _ = mod.build_plan(script, dict(checklist), quick=quick)
            return mod.plan_summary(script, plan, auto=auto)
        except Exception as e:
            return f"Couldn't read the script: {e}"

    @Property(bool, notify=sessionsRelay)
    def hasFullSession(self):
        """A full session (not a quick round, not a dry run) went to the end: the window then
        suggests a quick round, in another light."""
        for s in self.store.sessions():
            meta = takes.read_json(os.path.join(self.store.session_dir(s["id"]), "session.json"))
            if (meta.get("status") == "done" and not meta.get("quick") and not meta.get("bounty")
                    and not meta.get("dry_run")):
                return True
        return False

    @Slot("QVariantMap", str, bool, bool, result=bool)  # the standalone recorder's window: no bounties yet
    @Slot("QVariantMap", str, bool, bool, str, result=bool)
    def startSession(self, checklist, lighting, auto, quick, bounty_id=""):
        """Start a session: a full one, a quick round, or a bounty's round (bounty_id, else "")."""
        if self.sessionActive:
            return False
        mod = self._runner()
        if not mod:
            self.message.emit(self._session_error, True)
            return False
        debug = bounty_id == DEBUG_ID
        kind = takes.round_consent_kind(bool(bounty_id), debug)
        if not self.has_consent(kind):
            self.setConsentKind(kind)
            self.message.emit("Agree to the consent text first (Welcome page)", True)
            return False
        if not self._ignore_cameras and not self._session_options.get("dry_run"):
            self._camera = self._check_now()   # fresh: it takes a fraction of a second
            self.cameraChanged.emit()
            if self._camera.get("status") == "degraded":
                self.message.emit(mod.camera_text(self._camera), True)
                return False
        entry = None
        if bounty_id and not debug:
            entry = self._bounty(bounty_id)
            if not entry:
                self.message.emit("That bounty can't be recorded with this recorder", True)
                return False
        checklist = dict(checklist)
        checklist["objects"] = [o for o in checklist.get("objects", []) if o in dict(OBJECTS)]
        checklist["own_objects"] = [o.strip() for o in checklist.get("own_objects", []) if str(o).strip()]
        try:
            script = DEBUG_SCRIPT if debug else mod.bounty_script(entry["script"], base_dir=self.store.base) if entry else SCRIPT_PATH
            self._session = mod.Session(self.store.base, self.store.profile(), checklist, lighting, script,
                                        on_status=lambda s: self._statusArrived.emit(dict(s)), auto=auto,
                                        quick=quick and not entry, guidance=self.guidance, bounty=entry,
                                        **self._session_options)
        except Exception as e:
            self.message.emit(f"Couldn't set up the session: {e}", True)
            return False
        self._session_id = ""
        self._status = {"state": "starting"}
        self.statusChanged.emit()
        session = self._session

        def run():
            try:
                session.start()
            except Exception as e:
                self._statusArrived.emit({"state": "error", "error": str(e)})
        self._thread(run)
        return True

    @Slot()
    def markMoment(self):
        """A testing session: the tracking went wrong just now (the headset button's press)."""
        if self._session is not None and self.sessionActive:
            self._session.mark()

    @Slot()
    def finishSession(self):
        """A testing session: end the recording here; the session is done (the button's hold)."""
        if self._session is not None and self.sessionActive:
            self._session.finish()

    def _on_status(self, status):
        if status.get("camera") and status.get("camera") is not self._camera:
            self._camera = status["camera"]   # the session's own check (at its start, or after no hands)
            self.cameraChanged.emit()
        super()._on_status(status)
        if status.get("state") == "done":
            self.profileChanged.emit()    # a bounty's round is marked done

    def busy_text(self):
        """What the window is doing that closing it would cut short ("" if nothing), for another
        launch's message (recorder/one_window.py)."""
        if self.sessionActive:
            return "in the middle of a round"
        if self.exporting:
            return "exporting a recording"
        if self.uploading:
            return "uploading a recording"
        if self._upload_queue:
            return "waiting to upload a recording"
        return ""

    def shutdown(self):
        """The window closes: recorder/window.py's, then the panel and units getReady started,
        and ft-camd if the light check started it."""
        super().shutdown()
        if self._ready_panel is not None:
            try:
                self._ready_panel.stdin.close()
                self._ready_panel.wait(3)
            except Exception:
                self._ready_panel.kill()
        for unit in reversed(self._ready_units):
            try:
                self._session_mod.stop_unit(unit)
            except Exception:
                pass
        if self._camd_started and self._session_mod:
            try:
                self._session_mod.stop_unit(self._session_mod.CAMD_UNIT)
            except Exception:
                pass

    # --- getting ready: what a round needs, started after the consent (session.get_ready)
    @Slot(str)
    def getReady(self, bounty_id):
        """Start (off this thread) what the round bounty_id needs before its checklist: SteamVR
        checked, the cameras, hand tracking and the headset panel."""
        if self._ready["state"] == "busy":
            return
        mod = self._runner()
        self._ready = {"state": "busy", "error": "", "round": bounty_id, "parts": {}}
        self.readyChanged.emit()
        if self._session_options.get("dry_run") or self.sessionActive:
            self._readyDone.emit({"ok": True, "error": "", "units": []})
            return
        if not mod:
            self._readyDone.emit({"ok": False, "error": self._session_error, "units": []})
            return
        frametop_hands, debug = self.frametopHands, bounty_id == DEBUG_ID

        def work():
            try:
                r = mod.get_ready(frametop_hands=frametop_hands, need_live=debug,
                                  on_part=lambda key, state, text: self._readyPart.emit(key, state, text))
            except Exception as e:
                r = {"ok": False, "error": f"{type(e).__name__}: {e}", "units": [], "panel": None}
            panel = r.pop("panel", None)
            if panel is not None:      # kept here: a signal's dict carries no process
                self._ready_panel = panel
            self._readyDone.emit(r)
        self._thread(work)

    def _on_ready_part(self, key, state, text):
        self._ready["parts"][key] = {"state": state, "text": text}
        self.readyChanged.emit()

    def _on_ready_done(self, result):
        self._ready_units += [u for u in result.get("units", []) if u not in self._ready_units]
        self._ready["state"] = "ok" if result.get("ok") else "failed"
        self._ready["error"] = result.get("error", "")
        self.readyChanged.emit()

    @Property(str, notify=readyChanged)
    def readyState(self):
        """"" (not asked yet), "busy", "ok" or "failed"."""
        return self._ready["state"]

    @Property(str, notify=readyChanged)
    def readyError(self):
        return self._ready["error"]

    @Property(str, notify=readyChanged)
    def readyRound(self):
        """The round the last getReady was for."""
        return self._ready["round"]

    @Property(list, notify=readyChanged)
    def readyParts(self):
        """[{key, title, state ("", "busy", "ok", "failed"), text}] in order."""
        mod = self._session_mod
        parts = mod.READY_PARTS if mod else ()
        return [dict(key=k, title=t, **self._ready["parts"].get(k, {"state": "", "text": ""})) for k, t in parts]

    # --- review
    def _session_rows(self):
        out = []
        for s in self.store.sessions():
            s["label"] = session_label(s["id"]) + (f" · {s['bounty']}" if s["bounty"] else
                                                   " · testing session" if s.get("debug") else "")
            s["sizeText"] = takes.human_bytes(s["bytes"])
            s["exportText"] = takes.human_bytes(s["export_bytes"]) if s["exported"] else ""
            s["lightingText"] = LIGHTING_TEXT.get(s["lighting"], "")
            s["active"] = self.sessionActive and s["id"] == self._session_id
            s["statusText"] = {"recording": "" if s["active"] else "interrupted", "error": "ended with an error",
                               "stopped": "stopped early"}.get(s["status"], "")
            s["freeable"] = s["freeable"] and not s["active"]
            s["freeText"] = takes.human_bytes(s["bytes"] + s["export_bytes"])
            s["uploadText"] = ("uploaded" if s["freeable"] else "uploaded, changed since") if s["uploaded"] else ""
            out.append(s)
        return out

    @Slot(str, result="QVariantList")
    def takeList(self, session):
        try:
            rows = self.store.takes(session)
        except ValueError:
            return []
        for t in rows:
            t["durationText"] = f"{int(t['duration_s'] // 60)}:{int(t['duration_s'] % 60):02d}"
            t["sizeText"] = takes.human_bytes(t["bytes"])
            t.pop("ranges")
        return rows

    @Slot(str, str, result="QVariantMap")
    def takeInfo(self, session, take):
        """For the viewer: {count, title, cams, ranges: [[first, last] set indexes]}."""
        try:
            index = takes.take_index(self.store.take_dir(session, take))
            ranges = self.store.ranges(session, take)
        except ValueError:
            return {"count": 0, "title": take, "cams": [], "ranges": []}
        marks = []
        for a, b in ranges:
            inside = [i for i in range(len(index)) if a <= index.time_ns(i) <= b]
            marks.append([inside[0], inside[-1]] if inside else [-1, -1])
        return {"count": len(index), "title": self.store.take_meta(session, take).get("title") or take,
                "cams": [c["name"] for c in index.cams], "ranges": marks}

    @Slot(str, str, int, result=str)
    def setTime(self, session, take, i):
        """Set i's time from the take's first set, m:ss.s."""
        try:
            index = takes.take_index(self.store.take_dir(session, take))
            t = (index.time_ns(i) - index.time_ns(0)) / 1e9
        except (ValueError, IndexError):
            return ""
        return f"{int(t // 60)}:{t % 60:04.1f}"

    @Slot(str, str, int, int)
    def deleteRange(self, session, take, first, last):
        """Leave sets first..last (indexes, either order) out of the export."""
        if self._active_guard(session):
            return
        index = takes.take_index(self.store.take_dir(session, take))
        first, last = sorted((max(0, first), min(len(index) - 1, last)))
        if first > last:
            return
        self.store.delete_range(session, take, index.time_ns(first), index.time_ns(last))
        self.message.emit(f"Sets {first + 1} to {last + 1} won't be exported", False)
        self.sessionsChanged.emit()

    @Slot(str, str, int)
    def restoreRange(self, session, take, k):
        self.store.restore_range(session, take, k)
        self.message.emit("Range restored", False)
        self.sessionsChanged.emit()

    @Slot(str, str)
    def deleteTake(self, session, take):
        if self._active_guard(session):
            return
        try:
            self.store.delete_take(session, take)
            self.message.emit(f"Deleted {take}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete {take}: {e}", True)
        self.refreshSessions()

    @Slot(str)
    def deleteSession(self, session):
        if self._active_guard(session) or self._busy_guard(session):
            return
        try:
            self.store.delete_session(session)
            self.message.emit(f"Deleted the session of {session_label(session)}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete the session: {e}", True)
        self.refreshSessions()

    # --- freeing space: uploaded rounds' local copies go, the contributions log keeps the record
    def _freeable(self):
        busy = {self._session_id if self.sessionActive else "", self._export_session if self.exporting else "",
                (self._upload or {}).get("session", "") if self.uploading else ""}
        return [f for f in self.store.freeable() if f["id"] not in busy and not self._queued(f["id"])]

    @Property(int, notify=sessionsRelay)
    def freeableCount(self):
        """Uploaded rounds whose local copies can be deleted (nothing changed since the upload)."""
        return len(self._freeable())

    @Property(str, notify=sessionsRelay)
    def freeableText(self):
        """What deleting them all frees, "" when nothing can go."""
        rows = self._freeable()
        return takes.human_bytes(sum(f["bytes"] for f in rows)) if rows else ""

    @Property(str, constant=True)
    def roundNeedText(self):
        return gigabytes(self.ROUND_BYTES)

    @Property("QVariantList", notify=sessionsRelay)
    def contributions(self):
        """The contributions log for the Review page, newest first: {session, label, title,
        uploadedText, prUrl, prText, local, testing}."""
        out = []
        for c in self.store.contributions():
            title = (c.get("bounty") or {}).get("title") or ("Testing session" if c.get("debug") else "Session")
            when = (c.get("uploaded") or "")[:10]
            out.append({"session": c["session"], "label": session_label(c["session"]), "title": title,
                        "uploadedText": f"uploaded {when}" if when else "uploaded",
                        "prUrl": c.get("pr_url", ""), "prText": f"pull request #{c['pr_num']}" if c.get("pr_num") else "",
                        "local": c["local"], "testing": c.get("kind") == "testing",
                        "deletedText": "deleted here, still in your record" if not c["local"] else ""})
        return out

    def _free(self, sids):
        freed, failed = 0, []
        for sid in sids:
            try:
                freed += self.store.free_session(sid)
            except (OSError, ValueError) as e:
                failed.append(f"{session_label(sid)}: {e}")
        return freed, failed

    @Slot(str)
    def freeSession(self, session):
        """Delete one uploaded session's recordings and export; its record stays."""
        if self._active_guard(session):
            return
        if session not in {f["id"] for f in self._freeable()}:
            self.message.emit("Only an uploaded round, unchanged since its upload, can be deleted this way", True)
            return
        freed, failed = self._free([session])
        if failed:
            self.message.emit("Couldn't delete it: " + failed[0], True)
        else:
            self.message.emit(f"Deleted the local copy of {session_label(session)}: {takes.human_bytes(freed)} freed. "
                              "Your record of the upload stays.", False)
        self.refreshSessions()

    @Slot()
    def freeAllUploaded(self):
        """Delete every uploaded, unchanged round's local copy; their records stay."""
        sids = [f["id"] for f in self._freeable()]
        if not sids:
            self.message.emit("No uploaded rounds to delete", False)
            return
        freed, failed = self._free(sids)
        done = len(sids) - len(failed)
        if failed:
            self.message.emit(f"Deleted {done} of {len(sids)} rounds ({takes.human_bytes(freed)} freed). "
                              "Couldn't delete " + "; ".join(failed), True)
        else:
            self.message.emit(f"Deleted {done} uploaded round{'s' if done != 1 else ''}: {takes.human_bytes(freed)} "
                              "freed. Your record of the uploads stays.", False)
        self.refreshSessions()

    # --- export
    @Property(bool, constant=True)
    def zstdFound(self):
        return takes.find_zstd() is not None


MESSAGE_QML = b"""
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ApplicationWindow {
    visible: true
    title: "Frametop Hand Recorder"
    width: 600
    height: 240
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16
        Label {
            text: message
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        Button {
            text: "OK"
            Layout.alignment: Qt.AlignRight
            onClicked: Qt.quit()
        }
    }
}
"""


def _message(app, text):
    """A small window with text and OK, for a launch that can't have the window. Its exit code."""
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("message", text)
    engine.loadData(MESSAGE_QML)
    if not engine.rootObjects():
        return 1
    app.exec()
    shiboken6.delete(engine)
    return 1


def main():
    ap = argparse.ArgumentParser(description="Frametop Hand Recorder")
    ap.add_argument("--base", default=takes.DEFAULT_BASE, help="where profile.json, sessions/ and exports/ go")
    ap.add_argument("--page", default="", help="open on this page: bounties (the default), welcome, checklist, session, review, export, upload")
    ap.add_argument("--dry-run", action="store_true",
                    help="test: sessions start no processes and print the panel's commands")
    ap.add_argument("--speed", type=float, default=1.0, help="test, with --dry-run: run sessions this much faster")
    ap.add_argument("--poses", help="test: the pose pictures' folder (default hands/rec/poses)")
    ap.add_argument("--script", help="the recording script (default hands/rec/script.json; script-v2.json: "
                                     "the v2 script with demo clips)")
    ap.add_argument("--demos", help="the skeleton demo clips' folder, <id>.gif (default hands/rec/demos)")
    ap.add_argument("--recorded-demos", help="the recorded demo clips' folder (default hands/rec/demos-recorded)")
    ap.add_argument("--no-headset-button", action="store_true",
                    help="sessions don't read the headset's button (Next, pause, resume)")
    ap.add_argument("--hub-dry-run", action="store_true",
                    help="test: Upload checks the export and says what it would send, with no network calls")
    ap.add_argument("--ignore-cameras", action="store_true",
                    help="start sessions even if the camera check (hands/camcheck.py) finds the upper cameras off")
    ap.add_argument("--qml", default=os.path.join(HERE, "main.qml"),
                    help="the window (default: this folder's main.qml, which needs Kirigami)")
    ap.add_argument("--style", default="org.kde.desktop", help="the Qt Quick Controls style")
    a, qt_args = ap.parse_known_args()
    app = QGuiApplication([sys.argv[0]] + qt_args)
    app.setApplicationName("ft-handrec")
    app.setApplicationDisplayName("Frametop Hand Recorder")
    app.setDesktopFileName("ft-handrec")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle(a.style)
    # A window already open (in SteamVR's dashboard, say, when this one opens on the desktop)
    # closes if it's idle; a busy one stays and this launch says so. Dry runs (tests) have their
    # own name, so they never close a real window.
    try:
        window_sock = one_window.take(WINDOW_NAME + ("_dry" if a.dry_run else ""), os.path.basename(__file__),
                                      log=lambda line: print(line, file=sys.stderr, flush=True))
    except one_window.Taken as e:
        print("one window: " + e.text, file=sys.stderr, flush=True)
        sys.exit(_message(app, e.text))
    store = takes.Store(a.base)
    engine = QQmlApplicationEngine()
    engine.addImageProvider("frames", FrameProvider(store))
    options = {"dry_run": True, "speed": a.speed} if a.dry_run else {}
    if a.poses:
        options["poses_dir"] = a.poses
    if a.demos:
        options["demos_dir"] = a.demos
    if a.recorded_demos:
        options["recorded_demos_dir"] = a.recorded_demos
    if a.script:
        global SCRIPT_PATH
        SCRIPT_PATH = os.path.abspath(a.script if os.path.exists(a.script) else os.path.join(HERE, a.script))
    if a.no_headset_button:
        options["button"] = False
    if a.ignore_cameras:
        options["check_cameras"] = False
    backend = Backend(store, options, hub_dry_run=a.hub_dry_run, ignore_cameras=a.ignore_cameras)
    app.aboutToQuit.connect(backend.shutdown)
    mod = None if a.dry_run else backend._runner()
    if mod:   # Frametop's drawn hands and hand pointer, if a session that was killed left them paused
        backend._thread(lambda: mod.resume_units(os.path.join(mod.run_dir(), mod.PAUSED_UNITS_FILE)))
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", a.page)
    engine.load(QUrl.fromLocalFile(os.path.abspath(a.qml)))
    if not engine.rootObjects():
        sys.exit(1)

    # SIGTERM and Ctrl+C quit as closing does, so a running session still stops cleanly. Python
    # runs signal handlers between bytecodes: the timer gives it some while Qt waits.
    def on_signal(*_):
        engine.rootObjects()[0].setProperty("quitting", True)
        app.quit()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    def show():
        w = engine.rootObjects()[0]
        w.show()
        w.raise_()
        w.requestActivate()

    def quit_if_idle():
        if backend.busy_text():
            return False
        print("one window: another launch takes the window", file=sys.stderr, flush=True)
        QTimer.singleShot(0, on_signal)   # after the answer is sent
        return True
    listener = one_window.Listener(window_sock, backend.busy_text, show, quit_if_idle)  # noqa: F841
    tick = QTimer(interval=500, timeout=lambda: None)
    tick.start()
    code = app.exec()
    # The window goes before the backend: otherwise its bindings run again against a deleted
    # backend as Python tears down ("Cannot read property ... of null", one per binding).
    shiboken6.delete(engine)
    sys.exit(code)


if __name__ == "__main__":
    main()
