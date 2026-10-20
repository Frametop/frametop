"""Manual release updates over a private socket. No listeners or background checks."""
import argparse
import contextlib
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import select
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
from urllib.parse import quote, urlsplit
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener

REPOSITORY = "TaiKeid/steam-frame-full-keyboard"
API = f"https://api.github.com/repos/{REPOSITORY}/releases"
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_UNPACKED = 256 * 1024 * 1024


class UpdateError(Exception):
    pass


def failure_reason(error):
    # Never forward exception text, URLs or server responses to the UI or log.
    if isinstance(error, HTTPError):
        return "rate-limit" if (error.code == 429 or
                (error.code == 403 and (error.headers or {}).get("X-RateLimit-Remaining") == "0")) else "server-unavailable"
    if isinstance(error, (URLError, ConnectionError, ssl.SSLError)):
        return "network-unavailable"
    if isinstance(error, TimeoutError):
        return "download-timeout"
    if isinstance(error, UpdateError):
        code = str(error)
        if code in {"not-installed-release", "installation-changed", "wrong-platform",
                    "version-mismatch", "startup-failed", "service-handoff-failed",
                    "install-busy", "download-timeout"}:
            return code
        if code in {"checksum-mismatch", "inconsistent-checksum", "missing-or-duplicate-checksum"}:
            return "checksum-failed"
        if code in {"incomplete-download", "download-limit", "invalid-download-size"}:
            return "download-failed"
        return "invalid-release"
    if isinstance(error, subprocess.SubprocessError):
        return "validation-failed"
    if isinstance(error, OSError):
        if error.errno in {errno.ENOSPC, errno.EDQUOT}:
            return "disk-full"
        if error.errno in {errno.EACCES, errno.EPERM, errno.EROFS}:
            return "permission-denied"
        return "files-unavailable"
    return "invalid-release"


def record_failure(executable, operation, reason):
    text = f"Update {operation} failed: {reason}.\n"
    with contextlib.suppress(OSError):
        print(text, end="", file=sys.stderr, flush=True)
    with contextlib.suppress(OSError):
        path = Path(executable).resolve()
        if (len(path.parents) >= 4 and path.name == "framekeyboard" and
                path.parent.name == "bin" and path.parents[2].name == "releases"):
            with (path.parents[3] / "update-status.log").open("a") as output:
                output.write(text)


def version(value):
    if not isinstance(value, str):
        raise UpdateError("invalid-version")
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value)
    if not match or len(value) > 40:
        raise UpdateError("invalid-version")
    return tuple(int(item) for item in match.groups())


class ReleaseRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parsed = urlsplit(newurl)
        if (parsed.scheme != "https" or parsed.hostname not in
                {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
                or parsed.username or parsed.password or parsed.port not in (None, 443)):
            raise UpdateError("invalid-redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


@contextlib.contextmanager
def network_deadline():
    def expired(signum, frame):
        raise UpdateError("download-timeout")
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, 120)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def download(url, output, limit, expected_size=None):
    with network_deadline():
        return download_response(url, output, limit, expected_size)


def download_response(url, output, limit, expected_size=None):
    # Only callers' fixed API or validated repository asset URLs reach this function.
    opener = build_opener(ProxyHandler({}), ReleaseRedirects())
    request = Request(url, headers={"User-Agent": "FullKeyboard-Updater/1",
                                   "Accept": "application/vnd.github+json"})
    started = time.monotonic()
    digest = hashlib.sha256()
    size = 0
    with opener.open(request, timeout=10) as response:
        length = response.headers.get("Content-Length")
        if length and (int(length) > limit or
                       (expected_size is not None and int(length) != expected_size)):
            raise UpdateError("invalid-download-size")
        while True:
            chunk = response.read(64 * 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > limit or time.monotonic() - started > 120:
                raise UpdateError("download-limit")
            output.write(chunk)
            digest.update(chunk)
    if expected_size is not None and size != expected_size:
        raise UpdateError("incomplete-download")
    return digest.hexdigest()


def read_url(url, limit):
    import io
    output = io.BytesIO()
    download(url, output, limit)
    return output.getvalue()


def release(tag=None):
    data = json.loads(read_url(API + ("/tags/" + quote(tag, safe="") if tag else "/latest"), 1024 * 1024))
    if not isinstance(data, dict) or data.get("draft") is not False or data.get("prerelease") is not False:
        raise UpdateError("not-stable-release")
    tag = data.get("tag_name", "")
    parts = version(tag)
    number = ".".join(map(str, parts))
    archive = f"framekeyboard-{number}-aarch64.tar.gz"
    assets = {}
    for asset in data.get("assets", []):
        if asset.get("name") not in (archive, "SHA256SUMS"):
            continue
        name = asset["name"]
        url = f"https://github.com/{REPOSITORY}/releases/download/{quote(tag, safe='')}/{name}"
        if name in assets or asset.get("browser_download_url") != url:
            raise UpdateError("invalid-asset")
        size = asset.get("size")
        if type(size) is not int or size <= 0 or size > (MAX_ARCHIVE if name == archive else 65536):
            raise UpdateError("invalid-asset-size")
        assets[name] = asset
    if set(assets) != {archive, "SHA256SUMS"}:
        raise UpdateError("missing-assets")
    return {"tag": tag, "number": number, "parts": parts, "archive": archive, "assets": assets}


def expected_checksum(contents, name):
    matches = []
    for line in contents.decode("ascii").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if match and match[2] == name:
            matches.append(match[1].lower())
    if len(matches) != 1:
        raise UpdateError("missing-or-duplicate-checksum")
    return matches[0]


def extract(archive, destination, number):
    prefix = f"framekeyboard-{number}-aarch64"
    names = set()
    total = 0
    with tarfile.open(archive, "r:gz") as source:
        members = []
        for item in source:
            parts = PurePosixPath(item.name).parts
            if (not parts or parts[0] != prefix or item.name.startswith("/") or
                    any(p in ("..", ".") for p in item.name.split("/")) or
                    "\\" in item.name or str(PurePosixPath(item.name)) in names or
                    item.name.rstrip("/") != str(PurePosixPath(item.name)) or
                    not (item.isdir() or item.isfile()) or item.mode & 0o7000):
                raise UpdateError("unsafe-archive")
            names.add(str(PurePosixPath(item.name)))
            total += item.size
            if item.size < 0 or total > MAX_UNPACKED or len(names) > 10000:
                raise UpdateError("archive-limit")
            members.append(item)
        # Reject links, devices, duplicate paths and traversal before writing any member.
        source.extractall(destination, members=members, filter="data")
    return destination / prefix


def validate(package, number):
    for filename in ("bin/framekeyboard", "bin/framekeyboard-native-guard"):
        path = package / filename
        if not path.is_file() or not os.access(path, os.X_OK):
            raise UpdateError("missing-binary")
        with path.open("rb") as binary:
            header = binary.read(20)
        if (header[:6] != b"\x7fELF\x02\x01" or len(header) != 20 or
                int.from_bytes(header[18:20], "little") != 183):
            raise UpdateError("wrong-architecture")
    for kind in ("layouts", "languages", "themes"):
        if not list((package / "share/framekeyboard" / kind).glob("*.json")):
            raise UpdateError("missing-profiles")
    binary = str(package / "bin/framekeyboard")
    result = subprocess.run([binary, "--version"], capture_output=True, timeout=20, check=True)
    if result.stdout.strip() != f"framekeyboard {number}".encode():
        raise UpdateError("version-mismatch")
    with tempfile.TemporaryDirectory(prefix=".validation-", dir=package.parent) as config:
        subprocess.run([binary, "--check", "--data-dir", str(package / "share/framekeyboard"),
                        "--config-dir", config], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30, check=True)


def parent_identity(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]
    except (OSError, IndexError):
        return None


PRODUCTION_MARKER = ".full-keyboard-production.json"


def mark_production_release(root, directory, name):
    # Written by the installer/updater, never trusted from a downloaded package.
    marker = directory / PRODUCTION_MARKER
    marker.unlink(missing_ok=True)
    marker.write_text(json.dumps({"format": 1, "root": str(root.resolve()), "release": name}) + "\n")


def remove_production_release(root, directory):
    """Caller holds install.lock. Unmarked, moved, symlinked and active releases stay."""
    root = root.resolve(strict=True)
    releases = root / "releases"
    directory = Path(directory)
    if (releases.is_symlink() or directory.is_symlink() or not directory.is_dir() or
            directory.parent.resolve() != releases):
        return False
    marker = directory / PRODUCTION_MARKER
    try:
        if marker.is_symlink() or json.loads(marker.read_text()) != {
                "format": 1, "root": str(root), "release": directory.name}:
            return False
        # A missing/broken current is ambiguous. Keep recovery material.
        current = root / "current"
        if (not current.is_symlink() or not current.resolve(strict=True).is_dir() or
                current.resolve(strict=True) == directory.resolve(strict=True)):
            return False
    except (OSError, ValueError):
        return False
    previous = root / "previous"
    old_previous = previous.is_symlink() and previous.resolve() == directory.resolve()
    shutil.rmtree(directory)
    # Only a link to the production directory just removed is known safe to drop.
    if old_previous and previous.is_symlink() and previous.resolve() == directory.resolve():
        previous.unlink()
    return True


def replace_link(root, name, target):
    if target is None:
        (root / name).unlink(missing_ok=True)
        return
    temporary = root / ("." + name + "-update-" + str(os.getpid()))
    try:
        temporary.symlink_to(target)
        temporary.replace(root / name)
    finally:
        temporary.unlink(missing_ok=True)


def restart_arguments(arguments, package):
    result = []
    skip = False
    for arg in arguments:
        if skip:
            skip = False
        elif arg == "--data-dir":
            skip = True
        else:
            result.append(arg)
    return ["--data-dir", str(package / "share/framekeyboard"), *result]


def launch(package, arguments, timeout=45):
    parent, child = socket.socketpair()
    process = None
    try:
        env = dict(os.environ, FRAMEKEYBOARD_UPDATE_READY_FD=str(child.fileno()))
        process = subprocess.Popen([str(package / "bin/framekeyboard"),
                                    *restart_arguments(arguments, package)], env=env,
                                   stdin=subprocess.DEVNULL, pass_fds=(child.fileno(),),
                                   start_new_session=True)
        child.close()
        readable, _, _ = select.select([parent], [], [], timeout)
        if readable and parent.recv(1) == b"1" and process.poll() is None:
            return process
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        raise UpdateError("startup-failed")
    except BaseException:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        raise
    finally:
        parent.close()
        child.close()


def stop_child(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def service_main_pid(unit):
    if not re.fullmatch(r"[a-zA-Z0-9_.@:-]+\.service", unit):
        raise UpdateError("service-handoff-failed")
    try:
        result = subprocess.run(["systemctl", "--user", "show", unit, "-p", "MainPID", "--value"],
                                capture_output=True, text=True, check=True, timeout=5)
        return int(result.stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        raise UpdateError("service-handoff-failed") from None


def adopt_service_child(unit, process):
    # systemd-notify waits for the manager to consume the notification. Verify
    # MainPID as well before allowing the still-live original owner to exit.
    try:
        subprocess.run(["systemd-notify", "--pid=" + str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=5)
        if service_main_pid(unit) == process.pid:
            return
    except (OSError, subprocess.SubprocessError):
        pass
    raise UpdateError("service-handoff-failed")


@contextlib.contextmanager
def activation_signals():
    # Cancellation is immediate while downloading/preparing. Once the original
    # app exits, finish activation or rollback before honoring a stop request.
    signals = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
    previous = {number: signal.getsignal(number) for number in signals}
    pending = None

    def defer(number, frame):
        nonlocal pending
        if pending is None:
            pending = number

    try:
        for number in signals:
            if previous[number] != signal.SIG_IGN:
                signal.signal(number, defer)
        yield
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)
        if pending is not None:
            # Replayed outside the transaction's rollback handler. Caught signal
            # dispositions reset on exec, so child startup/termination is normal.
            signal.raise_signal(pending)


class SettingsSnapshot:
    def __init__(self, root, name, arguments):
        base = os.environ.get("XDG_CONFIG_HOME", "")
        self.config = Path(base) / "framekeyboard" if base and Path(base).is_absolute() else Path.home() / ".config/framekeyboard"
        for i, arg in enumerate(arguments[:-1]):
            if arg == "--config-dir":
                self.config = Path(arguments[i + 1]).resolve()
        backup_root = root / "update-backups"
        backup_root.mkdir(mode=0o700, exist_ok=True)
        self.backup = backup_root / name
        self.backup.mkdir(mode=0o700)
        self.saved = {}
        for filename in ("config.json", "placement.json"):
            path = self.config / filename
            self.saved[filename] = path.exists()
            if self.saved[filename]:
                shutil.copy2(path, self.backup / filename)

    def restore(self):
        errors = []
        for filename, existed in self.saved.items():
            path = self.config / filename
            try:
                if existed:
                    with tempfile.NamedTemporaryFile(dir=self.config, delete=False) as temporary:
                        name = Path(temporary.name)
                    try:
                        shutil.copy2(self.backup / filename, name)
                        name.replace(path)
                    finally:
                        name.unlink(missing_ok=True)
                else:
                    path.unlink(missing_ok=True)
            except OSError as error:
                errors.append(error)
        if errors:
            raise errors[0]


class Installation:
    def __init__(self, executable):
        self.executable = Path(executable).resolve(strict=True)
        self.old = self.executable.parent.parent
        self.root = self.old.parent.parent
        if (self.executable.name != "framekeyboard" or self.old.parent.name != "releases" or
                not (self.root / "current").is_symlink() or
                (self.root / "current").resolve() != self.old):
            raise UpdateError("not-installed-release")
        self.old_link = os.readlink(self.root / "current")
        self.lock = None
        self.temporary = None
        self.package = None
        self.target = None

    def close(self):
        temporary, self.temporary = self.temporary, None
        lock, self.lock = self.lock, None
        try:
            if temporary:
                temporary.cleanup()
        finally:
            if lock:
                lock.close()

    def prepare(self, selected, progress):
        if platform.machine() != "aarch64":
            raise UpdateError("wrong-platform")
        self.lock = (self.root / "install.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise UpdateError("install-busy") from None
        if os.readlink(self.root / "current") != self.old_link:
            raise UpdateError("installation-changed")
        self.temporary = tempfile.TemporaryDirectory(prefix=".update-", dir=self.root / "releases")
        work = Path(self.temporary.name)
        archive_asset = selected["assets"][selected["archive"]]
        checksum_asset = selected["assets"]["SHA256SUMS"]
        sums = read_url(checksum_asset["browser_download_url"], 65536)
        expected = expected_checksum(sums, selected["archive"])
        api_digest = archive_asset.get("digest")
        if api_digest and api_digest != "sha256:" + expected:
            raise UpdateError("inconsistent-checksum")
        archive = work / "update.tar.gz"
        with archive.open("wb") as output:
            actual = download(archive_asset["browser_download_url"], output, MAX_ARCHIVE,
                              archive_asset["size"])
        progress("verify")
        if actual != expected:
            raise UpdateError("checksum-mismatch")
        self.package = extract(archive, work, selected["number"])
        progress("prepare")
        validate(self.package, selected["number"])
        # Fresh directories also permit reinstalling a version without editing a live release.
        self.target = self.root / "releases" / (selected["number"] + "-update-" + work.name[8:])
        if self.target.exists():
            raise UpdateError("release-exists")
        mark_production_release(self.root, self.package, self.target.name)

    def activate(self, arguments, adopt=None):
        with activation_signals():
            try:
                return self.activate_transaction(arguments, adopt)
            finally:
                # Cleanup and unlock before replaying a default-action signal too.
                self.close()

    def activate_transaction(self, arguments, adopt=None):
        switched = False
        promoted = False
        snapshot = None
        try:
            if os.readlink(self.root / "current") != self.old_link:
                raise UpdateError("installation-changed")
            snapshot = SettingsSnapshot(self.root, self.target.name, arguments)
            self.package.rename(self.target)
            promoted = True
            switched = True
            replace_link(self.root, "current", str(self.target.relative_to(self.root)))
            process = launch(self.target, arguments)
            if adopt:
                try:
                    adopt(process)
                except BaseException:
                    stop_child(process)
                    raise
        except BaseException:
            self.rollback(arguments, snapshot, promoted, switched, adopt)
            raise
        # Acknowledged startup commits the new release. Optional bookkeeping can
        # no longer enter rollback or remove resources from this live process.
        cleanup_errors = []
        for operation in (lambda: remove_production_release(self.root, self.old),
                          lambda: shutil.rmtree(snapshot.backup)):
            try:
                operation()
            except OSError:
                cleanup_errors.append(True)
        self.write_status("Update installed and startup acknowledged." +
                          (" Old update files could not all be removed.\n" if cleanup_errors else "\n"))
        return process

    def write_status(self, text):
        with contextlib.suppress(OSError):
            (self.root / "update-status.log").write_text(text)

    def rollback(self, arguments, snapshot, promoted, switched, adopt=None):
        errors = []

        def attempt(operation):
            try:
                operation()
            except OSError as error:
                errors.append(error)

        try:
            # Free the completed download first, without deleting the release that
            # current might still name. Link repair may itself need disk space.
            if self.temporary:
                attempt(lambda: (Path(self.temporary.name) / "update.tar.gz").unlink(missing_ok=True))
            if switched:
                attempt(lambda: replace_link(self.root, "current", self.old_link))
            if snapshot:
                attempt(snapshot.restore)
            if promoted:
                try:
                    old_current = (self.root / "current").resolve(strict=True) == self.old
                except OSError:
                    old_current = False
                # A persistent write failure must never leave current dangling.
                # Retain the new target until the old link is confirmed restored.
                if old_current:
                    attempt(lambda: shutil.rmtree(self.target))
        finally:
            # Repair failures are independent. Always attempt the known old binary
            # directly, even when current or a saved preference cannot be restored.
            try:
                process = launch(self.old, arguments)
                if adopt:
                    try:
                        adopt(process)
                    except BaseException:
                        stop_child(process)
                        raise
            except BaseException:
                self.write_status("Update failed; previous release could not restart.\n")
                raise
            status = "Update failed; previous release relaunched"
            self.write_status(status + ("; recovery writes incomplete.\n" if errors else ".\n"))


def serve(peer, executable, current, arguments):
    parent = os.getppid()
    identity = parent_identity(parent)
    installation = None
    prepared_at = None

    def send(state, **values):
        peer.sendall(json.dumps(dict(state=state, **values)).encode())

    try:
        peer.settimeout(1)
        while True:
            try:
                packet, _, flags, _ = peer.recvmsg(2048)
            except socket.timeout:
                if prepared_at and time.monotonic() - prepared_at > 120:
                    return
                continue
            if not packet:
                return
            if flags & socket.MSG_TRUNC:
                raise UpdateError("oversized-command")
            command = json.loads(packet)
            operation = "check" if command.get("op") == "check" else "install"
            try:
                if command["op"] == "check":
                    selected = release()
                    if selected["parts"] > version(current):
                        send("available", version=selected["tag"])
                    else:
                        send("latest")
                elif command["op"] == "install" and installation is None:
                    tag = command["version"]
                    version(tag)
                    selected = release(tag)
                    if selected["tag"] != tag or selected["parts"] <= version(current):
                        raise UpdateError("not-newer-release")
                    installation = Installation(executable)
                    installation.prepare(selected, lambda phase: send("progress", phase=phase))
                    prepared_at = time.monotonic()
                    send("prepared")
                elif command["op"] == "activate-supervised" and installation and prepared_at:
                    # The explicit packet still follows full VR/input/lock cleanup.
                    # Keep the original service MainPID alive throughout activation.
                    unit = os.environ.get("FRAMEKEYBOARD_SERVICE", "")
                    if not os.environ.get("NOTIFY_SOCKET") or service_main_pid(unit) != parent:
                        raise UpdateError("service-handoff-failed")
                    adopted = False

                    def adopt(process):
                        nonlocal adopted
                        adopt_service_child(unit, process)
                        adopted = True

                    try:
                        installation.activate(arguments, adopt)
                    except (OSError, ValueError, UpdateError, subprocess.SubprocessError) as error:
                        record_failure(executable, "install", failure_reason(error))
                    finally:
                        # Acknowledged adoption is final even if optional cleanup
                        # or a deferred termination signal reports an error later.
                        if adopted:
                            send("activated")
                        else:
                            send("error", reason="service-handoff-failed")
                    return
                elif command["op"] == "activate" and installation and prepared_at:
                    # Explicit handoff is sent only after VR/input/settings cleanup. Parent
                    # death by itself is cancellation, never authorization to install.
                    deadline = time.monotonic() + 15
                    while parent_identity(parent) == identity and time.monotonic() < deadline:
                        time.sleep(.05)
                    if parent_identity(parent) == identity:
                        return
                    peer.close()
                    installation.activate(arguments)
                    return
                else:
                    raise UpdateError("invalid-command")
            except (OSError, ValueError, TypeError, KeyError, UpdateError, subprocess.SubprocessError, tarfile.TarError) as error:
                reason = failure_reason(error)
                record_failure(executable, operation, reason)
                if installation:
                    with contextlib.suppress(OSError):
                        installation.close()
                    installation = None
                    prepared_at = None
                if peer.fileno() < 0:
                    return
                send("error", reason=reason)
    finally:
        if installation:
            installation.close()
        peer.close()


def main():
    # Package install.sh shares the marker and conservative cleanup rules with
    # the updater. The caller holds both installation and instance locks.
    if sys.argv[1:2] == ["--mark-production"] and len(sys.argv) == 5:
        try:
            mark_production_release(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
        except OSError:
            print("Could not mark the new production release.", file=sys.stderr)
            return 1
        return 0
    if sys.argv[1:2] == ["--remove-production"] and len(sys.argv) == 4:
        try:
            remove_production_release(Path(sys.argv[2]), Path(sys.argv[3]))
        except OSError:
            print("Could not remove the replaced production release.", file=sys.stderr)
            return 1
        return 0
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", type=int, required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    version(args.version)
    peer = socket.socket(fileno=args.fd)
    if peer.family != socket.AF_UNIX or peer.type != socket.SOCK_SEQPACKET:
        raise UpdateError("not-private-channel")
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    # SIGTERM cancels downloads and cleans staging on an ordinary app shutdown.
    def interrupted(signum, frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        serve(peer, args.executable, args.version, arguments)
    except (OSError, ValueError, UpdateError):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
