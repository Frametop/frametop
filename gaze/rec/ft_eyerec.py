#!/usr/bin/env python3
"""Frametop Eye Recorder: record your eyes for the eye dataset (gaze/rec/DESIGN.md).

A Kirigami (QML) app with a Python backend, in the dev container, as the hand recorder
(hands/rec/ft_handrec.py); what the two windows share is recorder/window.py:
  - Welcome: the consent text (CONSENT.md), the first time and again when its version changes.
    Agreeing writes profile.json with a random contributor id.
  - Bounties: the bounty board (recorder/bounties.py): what to record, the priority the
    maintainers gave it, and how often you've done it. Refresh fetches the dataset's board
    (hub.py board). Picking one goes on to the checklist.
  - Before you start: the checklist (glasses, eye makeup, face pad and strap, a measured IPD),
    kept in profile.json; the health questions behind their own tick, kept in BASE/health.json
    and encrypted in the export; free space; the eye camera check (frames from ft-eyegrab at
    90 fps, with the fix when SteamVR runs them at 15). Start hands the round to session.py.
  - Round: the runner's live status, the fit card while the headset is fitted, Next, Pause,
    Redo, Skip and Stop (Space, P, R, S and Esc while the window has focus; the button on the
    headset's right side too). The prompts and dots appear in the headset.
  - Review: rounds, their takes, and a viewer of both eye cameras a frame at a time, where
    ranges, takes and rounds can be deleted (takes.py).
  - Export: encrypt what's kept into exports/<round>/ at nice 19 (takes.py).
  - Upload: the Hugging Face login, and Upload: hub.py checks the export (validate.py) and
    uploads it as a pull request. Nothing reaches the real dataset until CONSENT.md and
    UPLOAD.md are reviewed (hub.py's gate); --hub-dry-run does everything but the network calls.
Everything lives under ~/.local/share/frametop/eyes/contrib (--base). Launch with
gaze/rec/ft-eyerec (host wrapper).
"""
import argparse
import bisect
import datetime
import json
import os
import signal
import subprocess
import sys
import threading
import time

from PySide6.QtCore import Property, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QFont, QGuiApplication, QIcon, QImage, QPainter
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider
from PySide6.QtQuickControls2 import QQuickStyle
import shiboken6

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.dirname(HERE)
REPO = os.path.dirname(GAZE)
sys.path.insert(0, HERE)
sys.path.insert(0, REPO)
import eyerec  # noqa: E402  (next to this file; it imports gaze/tracker/eyes_share.py)
import eyes_share  # noqa: E402
import hub  # noqa: E402
import takes  # noqa: E402
from recorder import bounties  # noqa: E402
from recorder.common import host_command, write_json  # noqa: E402
from recorder.files import human_bytes, read_json  # noqa: E402
from recorder.window import RecorderBackend  # noqa: E402

HUB_PATH = os.path.join(HERE, "hub.py")
EYE_RATE = os.path.join(GAZE, "eye-rate.py")
AWAKE_UNIT = "frametop-eyerec-awake.service"
# A round of about 5 recorded minutes is about 6 GB (1.2 GB a minute: DESIGN.md, "Size"), and
# its export as much again.
ROUND_BYTES = 12 * 1000 ** 3
ACTIVE_STATES = ("starting", "fitment", "ready", "countdown", "running", "paused")
EYE_FPS_OK = 60          # under this the cameras run at SteamVR's 15 fps: recordings are worth little
EYE_CHECK_S = 1.5        # how long the eye camera check counts frames
# After the eye rate fix's SteamVR restart: check the cameras this often, for at most this long.
RECHECK_S = 3
RECHECK_FOR_S = 120
FRAME_HEIGHT = 400       # the viewer's height for each camera (the cameras' own)
THUMB_HEIGHT = 96
# The checklist's choices (DESIGN.md, "Checklist"); the keys are what profile.json keeps.
GLASSES = [("none", "No glasses"), ("glasses", "Glasses"), ("inserts", "Prescription lens inserts"),
           ("contacts", "Contact lenses")]
MAKEUP = [("mascara", "Mascara"), ("eyeliner", "Eyeliner"), ("lashes", "Lash extensions")]
PADS = [("", "Rather not say"), ("stock", "The face pad it came with"), ("other", "Another face pad")]
STRAPS = [("", "Rather not say"), ("stock", "The strap it came with"), ("other", "Another strap")]
# The health questions: only with their own tick, and only ever in health.json(.age).
HEALTH = [("surgery", "LASIK or other eye surgery"), ("strabismus", "A lazy or turned eye"),
          ("ptosis", "A droopy eyelid"), ("nystagmus", "Nystagmus (eyes that move by themselves)"),
          ("other", "Another eye condition")]
GROUP_TITLES = {"ipd": "IPD sweep", "dots": "Nine dots"}
STATUS_TEXT = {"running": "interrupted", "error": "ended with an error", "stopped": "stopped early"}


def round_label(rid, title=""):
    """20261014-120000-edges -> 2026-10-14 12:00 · View edges."""
    try:
        when = datetime.datetime.strptime(rid[:15], "%Y%m%d-%H%M%S").strftime("%Y-%m-%d %H:%M")
    except ValueError:
        return rid
    rest = rid[16:]
    n = rest.rsplit("-", 1)[1] if "-" in rest and rest.rsplit("-", 1)[1].isdigit() else ""
    return f"{when} · {title or rest}" + (f" ({n})" if n else "")


def take_title(group):
    return GROUP_TITLES.get(group) or group.replace("-", " ").replace("_", " ").capitalize()


def clock_text(seconds):
    return f"{int(seconds // 60)}:{seconds % 60:04.1f}"


def eye_check(share_path=None, seconds=EYE_CHECK_S):
    """Are the eye cameras' frames coming, and how fast? {"state": ok|slow|none, "fps": [right,
    left], "text"}. Counts frames in ft-eyegrab's share (read-only; it touches the want file,
    which wakes ft-eyegrab, as every reader does)."""
    share = eyes_share.open_share(3.0, share_path)
    if share is None:
        return {"state": "none", "fps": [0, 0],
                "text": "No eye camera frames: frametop-eyegrab.service isn't running. Frametop's gaze setup "
                        "installs it (gaze/tracker/install.sh), and SteamVR has to be running."}
    try:
        time.sleep(0.5)    # ft-eyegrab starts copying once it sees the want file
        eyes_share.touch_want()
        a, t0 = [share.count(c) for c in eyerec.CAMERAS], time.monotonic()
        time.sleep(seconds)
        b, t1 = [share.count(c) for c in eyerec.CAMERAS], time.monotonic()
    finally:
        share.close()
    fps = [round((y - x) / (t1 - t0), 1) for x, y in zip(a, b)]
    if max(fps) <= 0:
        return {"state": "none", "fps": fps,
                "text": "The eye cameras aren't sending frames. Is SteamVR running? If it is, restarting it "
                        "starts the eye cameras again."}
    if min(fps) < EYE_FPS_OK:
        return {"state": "slow", "fps": fps,
                "text": f"The eye cameras run at {min(fps):.0f} frames a second; recordings need 90. SteamVR runs "
                        "them at 90 with a setting Frametop can turn on, from SteamVR's next start."}
    return {"state": "ok", "fps": fps, "text": ""}


class Timeline:
    """A take's frames for the viewer: the camera with more frames sets the timeline, and the
    other camera's frame nearest in time goes beside it. Chunks are decompressed when needed
    and the last few kept."""
    KEEP = 4

    def __init__(self, take_dir, clock):
        self.dir = take_dir
        self.clock = clock or []
        frames = {c: [] for c in eyerec.CAMERAS}
        for c, chunk, i, _slot, t in eyerec.read_index(take_dir):
            if c in frames:
                frames[c].append((t, chunk, i))
        for c in frames:
            frames[c].sort()
        self.main = max(eyerec.CAMERAS, key=lambda c: (len(frames[c]), -c))
        self.frames = frames
        self.times = {c: [f[0] for f in frames[c]] for c in frames}
        self._chunks = {}
        self._lock = threading.Lock()

    def __len__(self):
        return len(self.frames[self.main])

    def time(self, i):
        return self.frames[self.main][i][0]

    def mono_ns(self, t_raw):
        """A frame's RAW seconds as CLOCK_MONOTONIC ns (take.json's clock samples, turned round)."""
        raw = int(round(t_raw * 1e9))
        guess = raw - takes.raw_offset_ns(raw, self.clock)
        return raw - takes.raw_offset_ns(guess, self.clock)

    def _chunk(self, cam, chunk):
        with self._lock:
            key = (cam, chunk)
            if key in self._chunks:
                self._chunks[key] = self._chunks.pop(key)   # the newest at the end
                return self._chunks[key]
        frames = eyerec.read_chunk(self.dir, cam, chunk)
        with self._lock:
            self._chunks[key] = frames
            while len(self._chunks) > self.KEEP:
                self._chunks.pop(next(iter(self._chunks)))
        return frames

    def pair(self, i):
        """{camera: bytes} at timeline index i: the main camera's frame and the other's nearest."""
        out = {}
        t, chunk, k = self.frames[self.main][i]
        out[self.main] = self._chunk(self.main, chunk)[k]
        for c in eyerec.CAMERAS:
            if c == self.main or not self.frames[c]:
                continue
            j = bisect.bisect_left(self.times[c], t)
            j = min(range(max(0, j - 1), min(len(self.times[c]), j + 1)), key=lambda n: abs(self.times[c][n] - t))
            _, chunk, k = self.frames[c][j]
            out[c] = self._chunk(c, chunk)[k]
        return out


class Timelines:
    """Timelines by take, made again when the take's index.tsv or take.json changes."""

    def __init__(self, store):
        self.store = store
        self._made = {}
        self._lock = threading.Lock()

    def get(self, rnd, take):
        d = self.store.take_dir(rnd, take)
        stamp = tuple(os.path.getmtime(os.path.join(d, n)) if os.path.exists(os.path.join(d, n)) else 0
                      for n in ("index.tsv", "take.json"))
        with self._lock:
            got = self._made.get((rnd, take))
            if got and got[0] == stamp:
                return got[1]
        tl = Timeline(d, read_json(os.path.join(d, "take.json")).get("clock"))
        with self._lock:
            self._made[(rnd, take)] = (stamp, tl)
            if len(self._made) > 8:
                self._made.pop(next(iter(self._made)))
        return tl


def grey_image(data):
    img = QImage(data, eyerec.W, eyerec.H, eyerec.W, QImage.Format_Grayscale8)
    return img.copy()


class EyeProvider(QQuickImageProvider):
    """image://eyes/pair/ROUND/TAKE/INDEX: both eye cameras at a timeline index, the right eye
    (camera 0) first. image://eyes/thumb/ROUND/TAKE: the main camera mid-take, small. Anything
    after "?" is ignored (it makes QML load the image again)."""

    def __init__(self, timelines):
        super().__init__(QQuickImageProvider.ImageType.Image)
        self.timelines = timelines

    def requestImage(self, ident, size, requested):
        parts = ident.split("?", 1)[0].split("/")
        try:
            if parts[0] == "thumb" and len(parts) == 3:
                tl = self.timelines.get(parts[1], parts[2])
                if not len(tl):
                    raise ValueError("no frames")
                data = tl.pair(len(tl) // 2)[tl.main]
                return grey_image(data).scaledToHeight(THUMB_HEIGHT, Qt.SmoothTransformation)
            if parts[0] == "pair" and len(parts) == 4:
                return self._pair(self.timelines.get(parts[1], parts[2]), int(parts[3]),
                                  requested.height() if requested.height() > 0 else FRAME_HEIGHT)
        except (OSError, ValueError, IndexError, KeyError) as e:
            print(f"ft-eyerec: image {ident}: {e}", file=sys.stderr)
        img = QImage(4, 3, QImage.Format_Grayscale8)
        img.fill(0)
        return img

    def _pair(self, tl, i, height):
        frames = tl.pair(i)
        gap = 8
        scaled = [(c, grey_image(frames[c]).scaledToHeight(height, Qt.SmoothTransformation))
                  for c in eyerec.CAMERAS if c in frames]
        out = QImage(sum(s.width() for _, s in scaled) + gap * (len(scaled) - 1), height, QImage.Format_RGB32)
        out.fill(QColor(30, 30, 30))
        p = QPainter(out)
        font = QFont()
        font.setPixelSize(max(12, height // 22))
        p.setFont(font)
        x = 0
        for c, img in scaled:
            p.drawImage(x, 0, img)
            p.setPen(QColor(255, 200, 80))
            p.drawText(x + 6, 6 + font.pixelSize(), ("Right eye" if c == 0 else "Left eye") + f" (camera {c})")
            x += img.width() + gap
        p.end()
        return out


class Backend(RecorderBackend):
    """The eye recorder's window backend: recorder/window.py's, plus the bounty board, the
    checklist and health answers, the eye camera check, the round and the review."""
    APP = "Frametop Eye Recorder"
    AWAKE_UNIT = AWAKE_UNIT
    AWAKE_WHY = "Exporting or uploading eye recordings"
    HUB_PATH = HUB_PATH
    CONSENT_PATH = hub.CONSENT_PATH
    UPLOAD_PATH = hub.UPLOAD_PATH
    ACTIVE_STATES = ACTIVE_STATES
    ROUND_BYTES = ROUND_BYTES
    PROFILE_KEEP = ("checklist", "bounties", "eyes_ipd")

    # A property here can't notify with the base's signals (PySide6 6.11 leaves it without
    # one), so these relay them.
    profileRelay = Signal()
    sessionsRelay = Signal()
    boardChanged = Signal()
    healthChanged = Signal()
    eyesChanged = Signal()
    _eyesArrived = Signal(dict)
    _boardArrived = Signal(dict)

    def __init__(self, store, session_options=None, hub_dry_run=False, ignore_eyes=False, timelines=None):
        super().__init__(store, hub, hub_dry_run)
        self.profileChanged.connect(self.profileRelay)
        self.sessionsChanged.connect(self.sessionsRelay)
        self.profileChanged.connect(self.boardChanged)    # the board's done marks and needs
        self._session_options = session_options or {}  # Session options: dry_run, speed, cams, button
        self._session_mod = None
        self._session_error = ""
        self._timelines = timelines or Timelines(store)
        self._ignore_eyes = ignore_eyes
        self._eyes = {}
        self._eyes_busy = False
        self._restarting = False
        self._board = {"state": "", "text": ""}
        self._board_busy = False
        self._eyesArrived.connect(self._on_eyes)
        self._boardArrived.connect(self._on_board)

    # --- the session runner, imported when first needed
    def _runner(self):
        if self._session_mod is None and not self._session_error:
            try:
                import session as mod
                self._session_mod = mod
            except Exception as e:  # missing, or broken: say so, the rest of the app still works
                self._session_error = f"The session runner (gaze/rec/session.py) can't be loaded: {e}"
        return self._session_mod

    def _runner_error(self):
        self._runner()
        return self._session_error

    def _texts_draft(self):
        return not hub.texts_ready()

    def _upload_allowed(self):
        try:
            return hub.upload_allowed(hub.dataset_id())
        except hub.HubError:
            return False

    def _upload_values(self, session, path):
        return {"ROUND": session}

    @Property(str, constant=True)
    def toolVersion(self):
        return takes.tool_version()

    # --- consent
    @Slot(bool, bool, bool)
    def acceptConsent(self, adult, region, agree):
        """Write profile.json (recorder/window.py); its checklist and bounties stay."""
        optional = self.store.profile().get("optional")
        self._write_consent(adult, region, agree, optional if isinstance(optional, dict) else {})

    # --- the bounty board
    def _checklist(self):
        c = self.store.profile().get("checklist")
        return c if isinstance(c, dict) else {}

    def _board_now(self):
        mod = self._runner()
        return mod.load_board(self.store.base) if mod else None

    @Property("QVariantList", notify=boardChanged)
    def bounties(self):
        """The board's bounties (bounties.view), with "lastText" for the last round."""
        mod = self._runner()
        if not mod:
            return []
        rows = bounties.view(self._board_now(), self.store.profile(), self._checklist(), mod.have_round)
        for b in rows:
            b["lastText"] = round_label(b["last"]).split(" · ")[0] if b["last"] else ""
            b["minutesText"] = f"about {b['minutes']:g} min" if b["minutes"] else ""
        return rows

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

    @Slot(str, result=str)
    def planText(self, bounty_id):
        """How long a bounty's round takes (session.plan_seconds), with the fit check before it."""
        mod = self._runner()
        b = next((b for b in self.bounties if b["id"] == bounty_id), None)
        if not mod or not b:
            return ""
        try:
            plan = mod.build_plan(mod.load_round(mod.round_path(b["script"])), 0)
        except (OSError, ValueError) as e:
            return f"Couldn't read the round: {e}"
        return (f"{len(plan)} steps, about {mod.plan_seconds(plan) / 60:.0f} min recorded, after putting the "
                "headset on")

    # --- the checklist and health answers
    @Property("QVariantMap", notify=profileRelay)
    def checklist(self):
        c = self._checklist()
        return {"glasses": c.get("glasses") if c.get("glasses") in dict(GLASSES) else "none",
                "makeup": [m for m in c.get("makeup") or [] if m in dict(MAKEUP)],
                "pad": c.get("pad") if c.get("pad") in dict(PADS) else "",
                "strap": c.get("strap") if c.get("strap") in dict(STRAPS) else "",
                "ipd_mm": c.get("ipd_mm") if isinstance(c.get("ipd_mm"), (int, float)) else 0}

    @Property("QVariantList", constant=True)
    def glassesChoices(self):
        return [{"value": k, "text": v} for k, v in GLASSES]

    @Property("QVariantList", constant=True)
    def makeupChoices(self):
        return [{"value": k, "text": v} for k, v in MAKEUP]

    @Property("QVariantList", constant=True)
    def padChoices(self):
        return [{"value": k, "text": v} for k, v in PADS]

    @Property("QVariantList", constant=True)
    def strapChoices(self):
        return [{"value": k, "text": v} for k, v in STRAPS]

    @Property("QVariantList", constant=True)
    def healthChoices(self):
        return [{"value": k, "text": v} for k, v in HEALTH]

    @Slot("QVariantMap")
    def saveChecklist(self, answers):
        """Keep the checklist in profile.json: only the known answers. ipd_mm: 0 when not known,
        else 50 to 80 mm."""
        a = dict(answers)
        try:
            ipd = round(float(a.get("ipd_mm") or 0), 1)
        except (TypeError, ValueError):
            ipd = 0.0
        checklist = {"glasses": a.get("glasses") if a.get("glasses") in dict(GLASSES) else "none",
                     "makeup": sorted({m for m in a.get("makeup") or [] if m in dict(MAKEUP)}),
                     "pad": a.get("pad") if a.get("pad") in dict(PADS) else "",
                     "strap": a.get("strap") if a.get("strap") in dict(STRAPS) else "",
                     "ipd_mm": ipd if 50 <= ipd <= 80 else None}
        profile = self.store.profile()
        if not profile or profile.get("checklist") == checklist:
            return
        profile["checklist"] = checklist
        write_json(self.store.profile_path, profile)
        self.profileChanged.emit()

    def _health_path(self):
        return os.path.join(self.store.base, "health.json")

    def _health(self):
        h = read_json(self._health_path())
        return h if h.get("opt_in") is True else {}

    @Property(bool, notify=healthChanged)
    def healthOptIn(self):
        return bool(self._health())

    @Property("QVariantList", notify=healthChanged)
    def healthAnswers(self):
        return [k for k in self._health().get("conditions") or [] if k in dict(HEALTH)]

    @Slot(bool, "QVariantList")
    def saveHealth(self, opt_in, conditions):
        """The health answers, in BASE/health.json (only this user can read it), never in
        profile.json; each round gets a copy, which its export encrypts. Opting out deletes them."""
        path = self._health_path()
        if not opt_in:
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
        else:
            data = {"schema": 1, "opt_in": True, "conditions": sorted({c for c in conditions if c in dict(HEALTH)})}
            if read_json(path) == data:
                return
            fd = os.open(path + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                json.dump(data, f, indent=1)
                f.write("\n")
            os.replace(path + ".tmp", path)
        self.healthChanged.emit()

    # --- the eye camera check
    @Property(str, notify=eyesChanged)
    def eyeState(self):
        """ok, slow, none; checking while it runs; "" before the first check."""
        if self._eyes_busy and not self._eyes:
            return "checking"
        return self._eyes.get("state", "")

    @Property(str, notify=eyesChanged)
    def eyeText(self):
        return self._eyes.get("text", "")

    @Property(str, notify=eyesChanged)
    def eyeFpsText(self):
        fps = self._eyes.get("fps") or []
        return " · ".join(f"{name} {f:.0f} fps" for name, f in zip(("right", "left"), fps))

    @Property(bool, notify=eyesChanged)
    def eyeBusy(self):
        return self._eyes_busy

    @Property(bool, notify=eyesChanged)
    def restartingSteamVR(self):
        return self._restarting

    @Property(bool, constant=True)
    def eyesIgnored(self):
        return self._ignore_eyes or bool(self._session_options.get("dry_run"))

    @Property(bool, notify=eyesChanged)
    def eyesBlockStart(self):
        return not self.eyesIgnored and self._eyes.get("state") != "ok"

    def _check_now(self):
        if self.eyesIgnored:
            return {"state": "ok", "fps": [], "text": ""}
        try:
            return eye_check(self._session_options.get("cams"))
        except Exception as e:
            return {"state": "none", "fps": [], "text": f"The eye camera check failed: {e}"}

    @Slot()
    def checkEyes(self):
        if self._eyes_busy or self.sessionActive:
            return
        self._eyes_busy = True
        self.eyesChanged.emit()
        self._thread(lambda: self._eyesArrived.emit(self._check_now()))

    def _on_eyes(self, result):
        self._eyes = result
        self._eyes_busy = self._restarting
        self.eyesChanged.emit()

    @Slot(bool)
    def restartSteamVR(self, fast):
        """fast: first turn on SteamVR's 90 fps eye cameras (gaze/eye-rate.py on, on the host).
        Then systemctl --user restart --no-block steamvr.service (the job runs in systemd, so it
        finishes even when this window closes with the Frametop desktop), and the eye check every
        few seconds until the frames come at 90."""
        if self._restarting or self.sessionActive:
            return
        self._restarting = self._eyes_busy = True
        self.eyesChanged.emit()

        def run():
            result = None
            try:
                if fast:
                    r = subprocess.run(host_command("python3", EYE_RATE, "on"), capture_output=True, text=True,
                                       timeout=60)
                    if r.returncode != 0:
                        raise RuntimeError((r.stderr or r.stdout).strip() or "eye-rate.py: exit %d" % r.returncode)
                r = subprocess.run(host_command("systemctl", "--user", "restart", "--no-block", "steamvr.service"),
                                   capture_output=True, text=True, timeout=60)
                if r.returncode != 0:
                    raise RuntimeError((r.stderr or r.stdout).strip() or "exit %d" % r.returncode)
                end = time.monotonic() + RECHECK_FOR_S
                time.sleep(RECHECK_S * 3)
                while time.monotonic() < end:
                    result = self._check_now()
                    if result.get("state") == "ok":
                        break
                    time.sleep(RECHECK_S)
            except Exception as e:
                result = {"state": "none", "fps": [], "text": f"Couldn't restart SteamVR: {e}"}
            self._restarting = False
            self._eyesArrived.emit(result or self._check_now())
        self._thread(run)

    # --- the round
    @Slot(str, result=bool)
    def startSession(self, bounty_id):
        if self.sessionActive:
            return False
        mod = self._runner()
        if not mod:
            self.message.emit(self._session_error, True)
            return False
        if self.needsConsent:
            self.message.emit("Agree to the consent text first (Welcome page)", True)
            return False
        entry = next((b for b in self.bounties if b["id"] == bounty_id), None)
        if not entry:
            self.message.emit("Pick a bounty first", True)
            return False
        if entry["available"]:
            self.message.emit(entry["available"], True)
            return False
        if not self.eyesIgnored:
            self._eyes = self._check_now()   # fresh: it takes two seconds
            self.eyesChanged.emit()
            if self._eyes.get("state") != "ok":
                self.message.emit(self._eyes.get("text", "The eye cameras aren't ready"), True)
                return False
        board = self._board_now() or {}
        try:
            self._session = mod.Session(self.store.base, entry["script"], bounty=dict(entry, updated=board.get("updated")),
                                        profile=self.store.profile(), checklist=self._checklist(), health=self._health(),
                                        on_status=lambda s: self._statusArrived.emit(dict(s)),
                                        profile_path=self.store.profile_path, **self._session_options)
        except Exception as e:
            self.message.emit(f"Couldn't set up the round: {e}", True)
            return False
        self._session_id = ""
        self._status = {"state": "starting", "bounty": bounty_id}
        self.statusChanged.emit()
        session = self._session

        def run():
            try:
                session.start()
            except Exception as e:
                self._statusArrived.emit({"state": "error", "error": str(e)})
        self._thread(run)
        return True

    def _on_status(self, status):
        super()._on_status(status)
        if status.get("state") == "done":
            self.profileChanged.emit()    # the bounty is marked done, and the IPD kept

    # --- review
    def _session_rows(self):
        out = []
        for r in self.store.rounds():
            r["label"] = round_label(r["id"], r["title"])
            r["sizeText"] = human_bytes(r["bytes"])
            r["exportText"] = human_bytes(r["export_bytes"]) if r["exported"] else ""
            r["active"] = self.sessionActive and r["id"] == self._session_id
            r["statusText"] = "" if r["active"] else STATUS_TEXT.get(r["status"], "")
            out.append(r)
        return out

    @Slot(str, result="QVariantList")
    def takeList(self, rnd):
        try:
            rows = self.store.takes(rnd)
        except ValueError:
            return []
        for t in rows:
            t["title"] = take_title(t["group"])
            t["durationText"] = f"{int(t['duration_s'] // 60)}:{int(t['duration_s'] % 60):02d}"
            t["sizeText"] = human_bytes(t["bytes"])
            # Per eye, as the viewer steps (the camera with more frames)
            t["frameCount"] = max(t["frames"], default=0)
            t["deletedCount"] = max(t["deleted_frames"], default=0)
            t.pop("ranges")
        return rows

    @Slot(str, str, result="QVariantMap")
    def takeInfo(self, rnd, take):
        """For the viewer: {count, title, ranges: [[first, last] timeline indexes]}."""
        try:
            tl = self._timelines.get(rnd, take)
            ranges = self.store.ranges(rnd, take)
            meta = self.store.take_meta(rnd, take)
        except (OSError, ValueError):
            return {"count": 0, "title": take, "ranges": []}
        marks = []
        monos = [tl.mono_ns(t) for t in tl.times[tl.main]]
        for a, b in ranges:
            first, last = bisect.bisect_left(monos, a), bisect.bisect_right(monos, b) - 1
            marks.append([first, last] if first <= last else [-1, -1])
        return {"count": len(tl), "title": take_title(meta.get("group", take.split("-", 1)[-1])), "ranges": marks}

    @Slot(str, str, int, result=str)
    def frameTime(self, rnd, take, i):
        """Frame i's time from the take's first, m:ss.s."""
        try:
            tl = self._timelines.get(rnd, take)
            return clock_text(tl.time(i) - tl.time(0))
        except (OSError, ValueError, IndexError):
            return ""

    @Slot(str, str, int, int)
    def deleteRange(self, rnd, take, first, last):
        """Leave frames first..last (timeline indexes, either order) out of the export: both
        cameras', by time."""
        if self._active_guard(rnd):
            return
        tl = self._timelines.get(rnd, take)
        first, last = sorted((max(0, first), min(len(tl) - 1, last)))
        if first > last:
            return
        # Half a millisecond either side: index.tsv keeps whole microseconds, and the clocks'
        # conversion rounds; the frames are 11 ms apart.
        self.store.delete_range(rnd, take, tl.mono_ns(tl.time(first)) - 500000, tl.mono_ns(tl.time(last)) + 500000)
        self.message.emit(f"Frames {first + 1} to {last + 1} won't be exported", False)
        self.sessionsChanged.emit()

    @Slot(str, str, int)
    def restoreRange(self, rnd, take, k):
        self.store.restore_range(rnd, take, k)
        self.message.emit("Range restored", False)
        self.sessionsChanged.emit()

    @Slot(str, str)
    def deleteTake(self, rnd, take):
        if self._active_guard(rnd):
            return
        try:
            self.store.delete_take(rnd, take)
            self.message.emit(f"Deleted {take}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete {take}: {e}", True)
        self.refreshSessions()

    @Slot(str)
    def deleteSession(self, rnd):
        if self._active_guard(rnd):
            return
        try:
            self.store.delete_round(rnd)
            self.message.emit(f"Deleted the round of {round_label(rnd)}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete the round: {e}", True)
        self.refreshSessions()

    # --- export
    @Property(str, constant=True)
    def exportProblem(self):
        """Why nothing can be exported here, or ""."""
        if eyerec.compress is None:
            return "zstd isn't installed in the dev container, so nothing can be exported. Run setup/dev-container.sh."
        try:
            import cryptography  # noqa: F401  (recorder/age.py's)
        except ImportError:
            return ("The cryptography library isn't installed in the dev container, so nothing can be encrypted "
                    "for export. Run setup/dev-container.sh.")
        return ""


def main():
    ap = argparse.ArgumentParser(description="Frametop Eye Recorder")
    ap.add_argument("--base", default=takes.DEFAULT_BASE, help="where profile.json, rounds/ and exports/ go")
    ap.add_argument("--page", default="",
                    help="open on this page: welcome, bounties, checklist, session, review, export, upload")
    ap.add_argument("--dry-run", action="store_true",
                    help="test: rounds start no processes, make up the panel's replies and print its commands")
    ap.add_argument("--speed", type=float, default=1.0, help="test, with --dry-run: run rounds this much faster")
    ap.add_argument("--cams", help="test: read eye frames from this share (lab/ft-eyes-replay's)")
    ap.add_argument("--no-headset-button", action="store_true",
                    help="rounds don't read the headset's button (Next, pause, resume)")
    ap.add_argument("--hub-dry-run", action="store_true",
                    help="test: Upload checks the export and says what it would send, with no network calls")
    ap.add_argument("--ignore-eyes", action="store_true",
                    help="start rounds even if the eye camera check finds no frames, or slow ones")
    ap.add_argument("--qml", default=os.path.join(HERE, "main.qml"),
                    help="the window (default: this folder's main.qml, which needs Kirigami)")
    ap.add_argument("--style", default="org.kde.desktop", help="the Qt Quick Controls style")
    a, qt_args = ap.parse_known_args()
    app = QGuiApplication([sys.argv[0]] + qt_args)
    app.setApplicationName("ft-eyerec")
    app.setApplicationDisplayName("Frametop Eye Recorder")
    app.setDesktopFileName("ft-eyerec")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle(a.style)
    store = takes.Store(a.base)
    timelines = Timelines(store)
    engine = QQmlApplicationEngine()
    engine.addImageProvider("eyes", EyeProvider(timelines))
    options = {"dry_run": True, "speed": a.speed} if a.dry_run else {}
    if a.cams:
        options["cams"] = a.cams
    if a.no_headset_button or a.dry_run:
        options["button"] = False
    backend = Backend(store, options, hub_dry_run=a.hub_dry_run, ignore_eyes=a.ignore_eyes, timelines=timelines)
    app.aboutToQuit.connect(backend.shutdown)
    mod = None if a.dry_run else backend._runner()
    if mod:   # Frametop's gaze service, if a round that was killed left it paused
        backend._thread(lambda: mod.resume_units(os.path.join(mod.run_dir(), mod.PAUSED_UNITS_FILE)))
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", a.page)
    engine.load(QUrl.fromLocalFile(os.path.abspath(a.qml)))
    if not engine.rootObjects():
        sys.exit(1)

    # SIGTERM and Ctrl+C quit as closing does, so a running round still stops cleanly. Python
    # runs signal handlers between bytecodes: the timer gives it some while Qt waits.
    def on_signal(*_):
        engine.rootObjects()[0].setProperty("quitting", True)
        app.quit()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    tick = QTimer(interval=500, timeout=lambda: None)
    tick.start()
    code = app.exec()
    # The window goes before the backend (as the hand recorder: bindings would run against it).
    shiboken6.delete(engine)
    sys.exit(code)


if __name__ == "__main__":
    main()
