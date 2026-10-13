#!/usr/bin/env python3
"""handsflag: `ft-handsctl flag [note...]`: flag a hand tracking failure (hands/README.md, Flags).

Asks ft-hands for its flag buffer (the last HANDS_FLAG_SECONDS of hand crops and what it made of
them, and the next 2 s) through its request file, run_dir/flag, and makes the folder it writes them
to: HANDS_FLAG_DIR/flag-<time>/ (default ~/.local/share/frametop/hands/flags). Once ft-hands has
taken the request (it looks 4 times a second), the drawn hands pulse cyan (ft-handview's flash
command; HANDS_FLAG_FLASH=0 turns it off) and, with HANDS_FLAG_TONE=1, a short tone plays. Then it
returns; in the background it saves one frame of what you saw (/dev/video99, the 60th: the
capture's first frames are black; HANDS_FLAG_VIEW=0 skips it) as view.jpg, ft-handview's head poses
for the window as hmd.jsonl, and what it did as ctl.json.

The note is the rest of the command line; frame-voice's voice commands pass the words after
"flag that" ("fingers crossed"), and FRAME_VOICE_WAKE_UNIX, when the wake word was heard
(said_unix in the manifest).

  python3 hands/handsflag.py [note...]
"""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import handsmode  # noqa: E402

RUN_DIR = "/run/user/%d/frametop-hands" % os.getuid()
DEFAULT_DIR = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "frametop/hands/flags")
VIEW_SOCKET = "ft_handview"
TAKE_WAIT_S = 1.5        # ft-hands looks every 0.25 s
AFTER_S = 2.0            # ft-hands keeps taking this long after the flag
VIEW_DEVICE = "/dev/video99"


def setting(key, default="", conf=None):
    """FT_<key> from the environment, else ~/.config/frametop.conf's, else default."""
    if "FT_" + key in os.environ:
        return os.environ["FT_" + key]
    conf = handsmode.read_conf() if conf is None else conf
    return conf.get(key, default) or default


def flag_dir(base, now=None):
    """A new folder base/flag-YYYYMMDD-HHMMSS (-2, -3... if taken), made private to the user."""
    os.makedirs(base, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now))
    for n in range(1, 100):
        path = os.path.join(base, "flag-" + stamp + ("" if n == 1 else "-%d" % n))
        try:
            os.mkdir(path, 0o700)
            return path
        except FileExistsError:
            continue
    raise RuntimeError("no free flag folder in " + base)


def one_line(s):
    return " ".join(str(s).split())


def request(run_dir, path, note, said_unix, flag_unix):
    """Writes run_dir/flag (key=value lines) whole, through a temporary file."""
    os.makedirs(run_dir, mode=0o700, exist_ok=True)
    lines = ["dir=" + path, "note=" + one_line(note), "flag_unix=%.3f" % flag_unix]
    if said_unix:
        lines.append("said_unix=%.3f" % float(said_unix))
    tmp = os.path.join(run_dir, "flag.tmp")
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(run_dir, "flag"))


def wait_taken(run_dir, timeout=TAKE_WAIT_S):
    """True once ft-hands has taken the request (the file is gone); else removes it, False."""
    path = os.path.join(run_dir, "flag")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if not os.path.exists(path):
            return True
        time.sleep(0.05)
    try:
        os.unlink(path)
    except FileNotFoundError:
        return True   # taken at the last moment
    return False


def ask_view(command, sock_name=VIEW_SOCKET, timeout=1.0, size=65536):
    """A command to ft-handview's socket and its reply, or None if it didn't answer."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.bind("")
        s.settimeout(timeout)
        s.sendto(command.encode(), "\0" + sock_name)
        return s.recv(size).decode(errors="replace")
    except OSError:
        return None
    finally:
        s.close()


def poses_lines(reply):
    """ft-handview's `poses` reply as hmd.jsonl lines ({"t_ns", "m": 12})."""
    out = []
    if not reply or not reply.startswith("ok"):
        return out
    for line in reply.split("\n")[1:]:
        parts = line.split()
        if len(parts) == 13:
            try:
                out.append(json.dumps({"t_ns": int(parts[0]), "m": [float(x) for x in parts[1:]]}))
            except ValueError:
                pass
    return out


def tone():
    """A short soft tone (HANDS_FLAG_TONE=1), if the session has pw-play."""
    import math
    import struct
    import tempfile
    rate, secs = 48000, 0.18
    n = int(rate * secs)
    frames = b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 1175 * i / rate) *
                                            min(1, i / 400, (n - i) / 2000))) for i in range(n))
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(frames)) + frames)
    try:
        subprocess.Popen(["pw-play", f.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def grab_view(path, device=VIEW_DEVICE):
    """One frame of what the user sees, the 60th (the capture's first frames are black), as path.
    On the low cores at the lowest priority; a few seconds at most."""
    if not os.path.exists(device):
        return "no " + device
    cmd = ["nice", "-n", "19", "taskset", "-c", "0-4", "timeout", "15", "ffmpeg", "-loglevel", "error", "-y", "-f", "v4l2",
           "-i", device, "-vf", r"select=gte(n\,59)", "-frames:v", "1", "-q:v", "3", path]
    try:
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return str(e)
    return "ok" if r.returncode == 0 and os.path.exists(path) else (r.stderr.decode(errors="replace").strip()[-200:] or "failed")


def background(path, ctl, grab=True, sock_name=VIEW_SOCKET, seconds=14.0, wait_s=AFTER_S + 0.7):
    """After the flag: the view frame, then (once ft-hands' window has closed) the head poses, and ctl.json."""
    if grab:
        ctl["view"] = grab_view(os.path.join(path, "view.jpg"))
        ctl["view_unix"] = time.time()
    rest = ctl["flag_unix"] + wait_s - time.time()
    if rest > 0:
        time.sleep(rest)
    lines = poses_lines(ask_view("poses %.1f" % (seconds + 1), sock_name, size=1 << 18))
    if lines:
        with open(os.path.join(path, "hmd.jsonl"), "w") as f:
            f.write("\n".join(lines) + "\n")
    ctl["hmd_poses"] = len(lines)
    with open(os.path.join(path, "ctl.json"), "w") as f:
        json.dump(ctl, f, indent=1)
        f.write("\n")


def main(argv, run_dir=RUN_DIR, sock_name=VIEW_SOCKET, detach=True):
    note = " ".join(argv).strip()
    conf = handsmode.read_conf()
    base = os.path.expanduser(setting("HANDS_FLAG_DIR", DEFAULT_DIR, conf))
    flag_unix = time.time()
    path = flag_dir(base, flag_unix)
    said = os.environ.get("FRAME_VOICE_WAKE_UNIX") or None
    request(run_dir, path, note, said, flag_unix)
    taken = wait_taken(run_dir)
    ctl = {"flag_unix": flag_unix, "note": note or None, "said_unix": float(said) if said else None, "taken": taken}
    if taken:
        if setting("HANDS_FLAG_FLASH", "1", conf) != "0":
            ctl["flash"] = ask_view("flash", sock_name, timeout=0.5)
        if setting("HANDS_FLAG_TONE", "0", conf) == "1":
            tone()
        print("flag: %s (written in about %.0f s)" % (path, AFTER_S + 0.5))
    else:
        print("flag: ft-hands didn't take it (is hand tracking on? ft-handsctl status); saving the view only: " + path,
              file=sys.stderr)
    sys.stdout.flush()
    grab = setting("HANDS_FLAG_VIEW", "1", conf) != "0"
    seconds = float(setting("HANDS_FLAG_SECONDS", "12", conf) or 12) + AFTER_S
    if detach and os.fork() > 0:
        return 0 if taken else 1
    if detach:
        os.setsid()
        devnull = os.open(os.devnull, os.O_RDWR)
        for fd in (0, 1, 2):
            os.dup2(devnull, fd)
    try:
        background(path, ctl, grab, sock_name, seconds)
    finally:
        if detach:
            os._exit(0)
    return 0 if taken else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
