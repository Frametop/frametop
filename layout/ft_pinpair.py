"""ft-layout remote add --pin's wait (issue 107): ft-stream pairing with a PIN typed in on the
host, its lines passed on as they come (the PIN first), and how that wait ends: the PIN typed in,
the time up, or remote cancel-pin.

ft-stream runs in its container (scripts/in-box), and no signal to ft-layout, to scripts/in-box
or to distrobox-host-exec (Remote Displays runs ft-layout through it) reaches it there. A
pairing left running keeps the host's request open, with the PIN shown, and blocks that client's
next one. So coreutils' timeout runs next to it in the container, and remote cancel-pin ends it
by its command line (WAITING says which): the container shares the host's processes.
"""
import os
import re
import subprocess
import threading

PIN_PAIR_S = 180  # how long the PIN waits to be typed in on the host
TIMED_OUT = -9    # run_live's exit when the time ran out
CANCELLED = -15   # and when remote cancel-pin ended it
WAITING = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "frametop-pin-pairing")
# What WAITING may hold: ft-stream's pair command, its parts as ft-layout checks them
COMMAND = re.compile(r"/[A-Za-z0-9/._+-]+ pair [A-Za-z0-9.:-]+ --id [A-Za-z0-9._-]+ --pin")


def run_live(program, args, box, timeout=PIN_PAIR_S):
    """(exit, stdout, stderr) of `program args`, each line of its stdout printed as it comes.
    box(command): the command that runs it in its container. Ends after `timeout` (TIMED_OUT),
    or by cancel() (CANCELLED)."""
    command = [program, *args]
    cmd = box(["timeout", "--kill-after=5", str(max(1, int(timeout))), *command])
    said = " ".join(command)
    _mark(said)
    try:
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                              text=True) as proc:
            timer = threading.Timer(timeout + 15, proc.kill)  # in case the container itself hangs
            timer.start()
            err = []
            reader = threading.Thread(target=lambda: err.append(proc.stderr.read()), daemon=True)
            reader.start()
            out = []
            try:
                for line in proc.stdout:
                    out.append(line)
                    print(line, end="", flush=True)
                proc.wait()
            finally:
                timer.cancel()
            reader.join(2)
    finally:
        _unmark(said)
    code = proc.returncode
    # timeout's TERM, its KILL and ours; a TERM from cancel() (timeout passes its signal on)
    code = TIMED_OUT if code in (124, 137, -9) else CANCELLED if code in (143, -15) else code
    return code, "".join(out), "".join(err)


def _mark(command):
    fd = os.open(WAITING, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(command + "\n")


def _unmark(command):
    try:
        with open(WAITING) as f:
            if f.read().strip() == command:
                os.unlink(WAITING)
    except OSError:
        pass


def _ere(text):
    """text as a POSIX extended regular expression that matches only it (pkill's)."""
    return re.sub(r"([.\[\]{}()*+?^$|\\])", r"\\\1", text)


def cancel():
    """remote cancel-pin: ends the ft-stream waiting for its PIN. True if one was."""
    try:
        with open(WAITING) as f:
            command = f.read().strip()
    except OSError:
        return False
    if not COMMAND.fullmatch(command):
        return False
    return subprocess.run(["pkill", "-TERM", "-u", str(os.getuid()), "-x", "-f", _ere(command)],
                          capture_output=True).returncode == 0
