"""Running commands on the headset's host system.

Frametop installs user services, a SteamVR driver and podman containers, and three of its parts
need sudo, so the install can't run inside the Flatpak's sandbox. The window stays in the
sandbox; everything that installs runs on the host through flatpak-spawn --host (the app's
--talk-name=org.freedesktop.Flatpak). Started outside a Flatpak (from the repo, for testing),
the same commands run directly.

The host scripts (installer/host/, get.sh and uninstall.sh) are copied to ~/.cache/frametop-installer/bin,
the one host folder the sandbox can write (--filesystem=~/.cache/frametop-installer:create), so
the host can run them: it can't see the app's /app.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

IN_FLATPAK = Path("/.flatpak-info").exists()
UID = os.getuid()
HOME = Path.home()
CACHE = HOME / ".cache" / "frametop-installer"
BIN = CACHE / "bin"
PKG = Path(__file__).resolve().parent
DATA = PKG.parent                       # installer/ in the repo, /app/share/frametop-installer in the app
HOST_FILES = ("helper.py", "askpass", "run-install.sh", "fake-install.sh", "debug-report.sh", "debug-lib.sh",
             "debug-features.sh", "debug-mask.py")


def _script(name):
    """get.sh or uninstall.sh: beside the package in the app, at the repo's top from the repo."""
    for p in (DATA / name, DATA.parent / name):
        if p.is_file():
            return p
    raise FileNotFoundError(f"{name} isn't in the installer")


def env_prefix():
    """The user's own runtime folder and bus (a VR desktop's terminal has its own), host paths,
    and none of Steam's preloads."""
    return ["env", "-u", "LD_PRELOAD", "-u", "LD_LIBRARY_PATH", f"XDG_RUNTIME_DIR=/run/user/{UID}",
            f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{UID}/bus", "PATH=/usr/local/bin:/usr/bin:/bin",
            f"HOME={HOME}"]


def command(argv, watch_bus=False):
    """argv as the host runs it."""
    full = env_prefix() + list(argv)
    if IN_FLATPAK:
        return ["flatpak-spawn", "--host"] + (["--watch-bus"] if watch_bus else []) + full
    return full


def run(argv, input=None, timeout=60):
    """Run on the host; returns (returncode, stdout, stderr) as bytes. A missing flatpak-spawn or
    a timeout comes back as returncode 127 or 124."""
    try:
        r = subprocess.run(command(argv), input=input, capture_output=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except FileNotFoundError as e:
        return 127, b"", str(e).encode()
    except subprocess.TimeoutExpired:
        return 124, b"", b"timed out"


def text(argv, timeout=30):
    code, out, _ = run(argv, timeout=timeout)
    return out.decode("utf-8", "replace").strip() if code == 0 else ""


def popen(argv):
    """A long-running host process with pipes (the askpass helper). --watch-bus ends it on the
    host when this app goes, even if its stdin somehow doesn't close."""
    return subprocess.Popen(command(argv, watch_bus=True), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)


def _put(src, dest):
    """Copy src to dest by a rename, so a script the install is running keeps its old copy (bash
    reads a script as it goes)."""
    fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=f".{dest.name}.")
    try:
        with os.fdopen(fd, "wb") as f, open(src, "rb") as s:
            shutil.copyfileobj(s, f)
        os.chmod(tmp, 0o755)
        os.replace(tmp, dest)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def stage():
    """Put the host scripts in BIN. Returns BIN."""
    BIN.mkdir(parents=True, exist_ok=True)
    os.chmod(CACHE, 0o700)
    for name in HOST_FILES:
        _put(DATA / "host" / name, BIN / name)
    for name in ("get.sh", "uninstall.sh"):
        _put(_script(name), BIN / name)
    return BIN


def installed():
    """(version, channel) of the Frametop release installed now, or ("", "")."""
    out = text(["sed", "-n", "s/^VERSION=//p;s/^CHANNEL=//p",
                str(HOME / ".local/share/frametop/releases/current/.frametop-release")])
    lines = out.splitlines()
    return (lines[0] if lines else "", lines[1] if len(lines) > 1 else "")


def password_set():
    """Does this user have a password sudo can take? SteamOS starts without one."""
    out = text(["passwd", "-S", os.environ.get("USER") or HOME.name]).split()
    return len(out) < 2 or out[1] == "P"   # P: usable; NP: none; L: locked; unknown: let sudo say


def check_password(pw):
    """Does sudo take it? Cached credentials are ignored, and forgotten after."""
    code, _, _ = run(["sudo", "-S", "-k", "-v", "-p", ""], input=bytes(pw) + b"\n", timeout=30)
    run(["sudo", "-k"], timeout=10)
    return code == 0


def set_password(pw):
    """Give this user a password with passwd (the helper answers its prompts on a pty).
    Returns "" or what went wrong. This page comes before an install ever started, so the helper
    is put on the host here first."""
    try:
        stage()
    except OSError as e:
        return f"couldn't put the installer's scripts in {BIN}: {e}"
    code, out, err = run(["python3", str(BIN / "helper.py"), "setpw"], input=bytes(pw).hex().encode() + b"\n",
                         timeout=60)
    msg = (out.strip() or err.strip()).decode("utf-8", "replace")
    return "" if code == 0 else (msg or "passwd didn't take it")
