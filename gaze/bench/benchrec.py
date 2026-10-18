"""Gaze bench recording (README.md): everything a run needs to replay gaze techniques later.

A run is one scenario played once. Its folder, under BASE:
  run.json          the scenario, the panel, the settings, and the gaze service's and our
                    tracker's status at the start and the end
  trials.jsonl      a line per target: where it was, when it showed, and when you marked it
                    (gazebench.py's own record: "click" is the mark, by the button or a key)
  targets.jsonl     the panel's record of what the dot did, with when each change reached the
  poses.jsonl       eyes; the headset's pose 250 times a second; SteamVR's IPD setting
  ipd.jsonl         (ft-gazepanel's log, gaze/panel/ft-gazepanel.cpp)
  gaze.jsonl        ft-gaze's samples, every source but SteamVR's input action (the one that
                    calls into vrserver): SteamVR's eyes (mmap1, mmap2, left, right), our
                    tracker's published gaze (own), the head ray, and where each lands on
                    Frametop's screens, in pixels, with pixels per degree there (ft-gaze.cpp)
  own.jsonl         our tracker's finds, a line per eye-camera frame (ft-eyes "finds start"):
                    pupil, ellipse, glints, and each eye's gaze as it published it
  lessons.jsonl     the gaze service's lesson log (pointer-lessons.jsonl) over the run: each
                    nudge the pointer sent, its raw gaze and the true direction
  eyes-clicks.jsonl our tracker's clicks over the run (clicks.jsonl): what each nudge taught it
  start/            copies of our tracker's calibration, its dots and its state (the shift
                    clicks taught it), and the gaze service's calibration, lessons and fusion
                    (gazed-fusion.json), as they were when the run started, so a replay starts
                    where the live tracker did
  images/           both eye cameras, zstd chunks (gaze/rec/eyerec.py), unless images are off

Eye images identify a person: they stay on this headset (and the maintainer's own training
machines). Nothing here uploads anything.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAZE = HERE.parent

# ft-gaze: this tree's build (gaze/build.sh), else the installed release's (a worktree with no build)
FT_GAZES = [Path(p) for p in (os.environ.get("FT_GAZE"), GAZE / "build" / "ft-gaze",
                              Path.home() / ".local/share/frametop/releases/current/gaze/build/ft-gaze") if p]
BASE = Path(os.environ.get("FT_BENCH_BASE", Path.home() / ".local/share/frametop/gaze/bench"))
STATE = Path.home() / ".local/state/frametop/gaze"
EYES_STATE = STATE / "eyes"
LESSON_LOG = STATE / "pointer-lessons.jsonl"
CONF = Path.home() / ".config/frametop.conf"
CONF_KEYS = ("GAZE_", "POINTER_GAZE", "EYES_MODEL")
SOURCES = "mmap1,mmap2,left,right,own"
EYES_SOCK = os.environ.get("FT_BENCH_EYES_SOCKET", "ft_eyes")     # (the tests' stand-ins)
GAZED_SOCK = os.environ.get("FT_BENCH_GAZED_SOCKET", "ft_gazed")
RECENT = 3.0        # s of parsed samples kept for the app's quick scores
RESTART = 3.0       # s before ft-gaze is started again after it stopped (SteamVR restarted)
KEEPALIVE = 2.0     # s between checks that our tracker is still logging finds
LOG_SLACK = 3.0     # s either side of the run when copying the lesson logs


def raw_now():
    return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)


class Sock:
    """A datagram client for one of the gaze services' abstract sockets (@ft_eyes, @ft_gazed)."""

    def __init__(self, name):
        self.name = "\0" + name
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind("")

    def ask(self, text, timeout=0.5):
        """Send, and the reply (text) or None."""
        self.sock.setblocking(False)
        while True:  # drop late replies to earlier questions
            try:
                self.sock.recv(65536)
            except OSError:
                break
        try:
            self.sock.sendto(text.encode(), self.name)
            self.sock.settimeout(timeout)
            return self.sock.recv(1 << 20).decode(errors="replace")
        except OSError:
            return None

    def json(self, text, timeout=0.5):
        r = self.ask(text, timeout)
        try:
            return json.loads(r) if r else None
        except ValueError:
            return None

    def close(self):
        self.sock.close()


class GazeStream:
    """ft-gaze, for the app's life (it takes a moment to connect to SteamVR and the screens), and
    again RESTART seconds after it stops: it quits when SteamVR does. Its lines go to the current
    run's gaze.jsonl while one records, and the last RECENT seconds are kept parsed for the app's
    quick scores and status."""

    def __init__(self, prog=None, log=print):
        self.prog = Path(prog) if prog else next((p for p in FT_GAZES if p.exists()), FT_GAZES[0])
        self.log = log
        self.proc = None
        self.out = None
        self.lock = threading.Lock()
        self.recent = deque()
        self.lines = 0
        self.error = None
        self.restarts = 0
        self.stopping = threading.Event()
        self.reader = None

    def start(self):
        if not self.prog.exists():
            self.error = f"{self.prog} isn't built (gaze/build.sh)"
            return self
        self.reader = threading.Thread(target=self._run, name="ft-gaze", daemon=True)
        self.reader.start()
        return self

    def _run(self):
        while True:
            with self.lock:
                if self.stopping.is_set():
                    return
                self.proc = subprocess.Popen([str(self.prog), "--watch-stdin", "--sources", SOURCES],
                                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            before = self.lines
            self._read()
            code = self.proc.wait()
            self.proc.stdout.close()
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            if self.stopping.is_set():
                return
            self.error = f"ft-gaze stopped (exit {code}); starting it again"
            if self.lines > before or not self.restarts:   # not every few seconds while SteamVR is down
                self.log(self.error)
            self.restarts += 1
            self.stopping.wait(RESTART)

    def _read(self):
        for line in self.proc.stdout:
            with self.lock:
                if self.out:
                    self.out.write(line)
            self.lines += 1
            try:
                s = json.loads(line)
            except ValueError:
                continue
            self.error = None
            with self.lock:
                self.recent.append(s)
                while self.recent and self.recent[0].get("t", 0) < s.get("t", 0) - RECENT:
                    self.recent.popleft()

    def to(self, path):
        """Write the samples to path from now on (None: stop writing)."""
        with self.lock:
            if self.out:
                self.out.close()
            self.out = open(path, "ab") if path else None

    def window(self, t0, t1):
        with self.lock:
            return [s for s in self.recent if t0 <= s.get("t", 0) <= t1]

    def latest(self):
        with self.lock:
            return self.recent[-1] if self.recent else None

    def stop(self):
        self.to(None)
        with self.lock:
            self.stopping.set()
            proc = self.proc
        if proc:
            try:
                proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
        if self.reader:
            self.reader.join(2)


def conf_lines():
    try:
        return [l for l in CONF.read_text().splitlines() if l.startswith(CONF_KEYS)]
    except OSError:
        return []


def eyes_state_dir(status):
    """Our tracker's state folder for the model it runs (ft-eyes: net-NAME/ for a net)."""
    model = (status or {}).get("model")
    if model:
        try:
            name = json.loads((Path(model) / "model.json").read_text())["name"]
        except (OSError, ValueError, KeyError):
            name = Path(model).name
        return EYES_STATE / ("net-" + str(name))
    return EYES_STATE


def last_time(path, before):
    """The latest "time" (wall clock) in a JSON lines log before `before`, or None: the last nudge
    before a run, for compare.py's time since the last nudge."""
    last = None
    try:
        with open(path) as f:
            for line in f:
                try:
                    tm = json.loads(line).get("time")
                except ValueError:
                    continue
                if tm is not None and tm < before and "refused" not in line:
                    last = tm if last is None else max(last, tm)
    except OSError:
        pass
    return last


def copy_rows(src, dst, t0, t1):
    """The JSON lines of src whose "time" (wall clock) is within [t0, t1], appended to dst."""
    n = 0
    try:
        with open(src) as f, open(dst, "a") as out:
            for line in f:
                try:
                    tm = json.loads(line).get("time", 0)
                except ValueError:
                    continue
                if t0 <= tm <= t1:
                    out.write(line if line.endswith("\n") else line + "\n")
                    n += 1
    except OSError:
        pass
    return n


class Run:
    """One scenario played once: its folder, and what records into it (see the top)."""

    def __init__(self, scenario, gaze, images=True, base=BASE, log=print, info=None):
        self.scenario, self.gaze, self.images, self.log = scenario, gaze, images, log
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.dir = Path(base) / f"{stamp}-{scenario['id']}"
        self.info = dict(info or {})
        self.eyes = Sock(EYES_SOCK)
        self.gazed = Sock(GAZED_SOCK)
        self.writer = None
        self.finds_on = False
        self.checked = 0.0
        self.trials = None

    def status(self):
        return {"eyes": self.eyes.json("status"), "gazed": self.gazed.json("status"), "t_raw": raw_now(),
                "time": time.time()}

    def start(self):
        self.dir.mkdir(parents=True)
        (self.dir / "start").mkdir()
        st = self.status()
        estate = eyes_state_dir(st["eyes"])
        for name in ("calibration.json", "calibration-dots.jsonl", "state.json"):
            if (estate / name).exists():
                shutil.copy2(estate / name, self.dir / "start" / ("eyes-" + name))
        for name in ("calibration.json", "pointer-lessons.json", "fusion.json"):
            if (STATE / name).exists():
                shutil.copy2(STATE / name, self.dir / "start" / ("gazed-" + name))
        self.info.update(scenario=self.scenario, start=st, conf=conf_lines(), eyes_state=str(estate),
                         images=self.images, sources=SOURCES, last_lesson=last_time(LESSON_LOG, st["time"]))
        self.write_info()
        self.trials = open(self.dir / "trials.jsonl", "a", buffering=1)
        self.lines0 = self.gaze.lines
        self.gaze.to(self.dir / "gaze.jsonl")
        self.arm_finds()
        if self.images:
            try:
                if str(GAZE / "rec") not in sys.path:
                    sys.path.insert(0, str(GAZE / "rec"))
                import eyerec
                self.writer = eyerec.Writer(str(self.dir / "images"), log=self.log).start()
                self.writer.record(True)
            except Exception as e:  # no zstd, no share: record the rest
                self.log(f"images: {e}")
                self.writer = None
                self.info["images_error"] = str(e)
        return self

    def arm_finds(self):
        r = self.eyes.ask("finds start " + str(self.dir / "own.jsonl"))
        self.finds_on = bool(r and r.startswith("ok"))
        if not self.finds_on:
            self.log(f"our tracker isn't logging finds ({r or 'no reply: is it running?'})")
        return self.finds_on

    def keepalive(self):
        """Call now and then: our tracker may have restarted (or started), and dropped the log."""
        if time.monotonic() - self.checked < KEEPALIVE:
            return
        self.checked = time.monotonic()
        st = self.eyes.json("status")
        if st is None:
            self.finds_on = False
        elif not self.finds_on:
            self.arm_finds()

    def trial(self, rec):
        if self.trials:
            self.trials.write(json.dumps(rec) + "\n")

    def write_info(self):
        tmp = self.dir / "run.json.tmp"
        tmp.write_text(json.dumps(self.info, indent=1))
        tmp.replace(self.dir / "run.json")

    def stop(self, summary=None):
        rows = self.eyes.ask("finds stop")
        self.gaze.to(None)
        if self.trials:
            self.trials.close()
            self.trials = None
        if self.writer:
            self.info["images_stats"] = self.writer.stop()
        time.sleep(0.5)  # a nudge's lesson reaches the logs just after its click
        st = self.status()
        t0, t1 = self.info["start"]["time"] - LOG_SLACK, st["time"] + LOG_SLACK
        n_lessons = copy_rows(LESSON_LOG, self.dir / "lessons.jsonl", t0, t1)
        n_clicks = copy_rows(Path(self.info["eyes_state"]) / "clicks.jsonl", self.dir / "eyes-clicks.jsonl", t0, t1)
        self.info.update(end=st, finds_rows=rows, lessons=n_lessons, eyes_clicks=n_clicks,
                         gaze_lines=self.gaze.lines - self.lines0, summary=summary)
        self.write_info()
        self.eyes.close()
        self.gazed.close()
        return self.info
