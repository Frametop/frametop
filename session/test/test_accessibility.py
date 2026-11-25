#!/usr/bin/env python3
"""Isolated AT-SPI integration tests. Run with the host's /usr/bin/python3.

Requires test-only PyGObject (Gio, GTK3), Xvfb, and installed at-spi2-core.
All displays, session buses, config, and applications are private. No input is sent.
"""
from builtins import BaseExceptionGroup, ExceptionGroup
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import gi
from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "session/ft-atspi"
DBUS = "org.freedesktop.DBus"
DBUS_PATH = "/org/freedesktop/DBus"
REGISTRY = "org.a11y.atspi.Registry"
ACCESSIBLE = "org.a11y.atspi.Accessible"


def connect(address):
    return Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)


def call(bus, dest, path, interface, method, signature=None, args=()):
    return bus.call_sync(dest, path, interface, method,
                         GLib.Variant(signature, args) if signature else None,
                         None, Gio.DBusCallFlags.NONE, 3000, None).unpack()


def eventually(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + predicate.__name__)


def process_running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(") ")[1][0] != "Z"
    except FileNotFoundError:
        return False


class ChildSignalAction(ctypes.Structure):
    # Linux/glibc sigaction ABI on the 64-bit hosts used by this test harness.
    _fields_ = [("sa_handler", ctypes.c_void_p), ("sa_mask", ctypes.c_byte * 128),
                ("sa_flags", ctypes.c_int), ("sa_restorer", ctypes.c_void_p)]


def verify_private_waiter():
    # The standalone runner's main thread is the ONLY waiter for fixture leaders.
    # No other thread/handler may wait* for them or change SIGCHLD while owned.
    # Gio's worker threads are not waiters. Never install/change a global handler.
    if (threading.current_thread() is not threading.main_thread()
            or threading.enumerate() != [threading.main_thread()]):
        raise AssertionError("private process lifecycle requires the sole main-thread waiter")
    if sys.platform != "linux" or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise AssertionError("private process reservation requires 64-bit Linux/glibc")
    action = ChildSignalAction()
    libc = ctypes.CDLL(None, use_errno=True)
    libc.sigaction.argtypes = [ctypes.c_int, ctypes.POINTER(ChildSignalAction),
                              ctypes.POINTER(ChildSignalAction)]
    libc.sigaction.restype = ctypes.c_int
    if libc.sigaction(signal.SIGCHLD, None, ctypes.byref(action)) != 0:
        raise OSError(ctypes.get_errno(), "cannot verify SIGCHLD reservation")
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL or action.sa_handler or action.sa_flags & 2:
        raise AssertionError("private process reservation requires default SIGCHLD without SA_NOCLDWAIT")


class OwnedProcess(subprocess.Popen):
    """Report exits without reaping: reserve the PID/PGID until teardown signals finish.

    This fixture is the sole waiter. Popen's poll/wait (including its finalizer)
    must not free the group leader while activated descendants may still exist.
    """
    def __init__(self, *args, **kwargs):
        verify_private_waiter()
        if not kwargs.get("start_new_session"):
            raise AssertionError("private process must lead a new session/group")
        super().__init__(*args, **kwargs)
        try:
            self.pidfd = os.pidfd_open(self.pid)
        except BaseException:
            # Verified default/no-autowait SIGCHLD and the sole-waiter contract
            # keep even an exited leader reserved before pidfd acquisition.
            # Do not leave a started private group behind on fixture setup failure.
            try:
                os.killpg(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            finally:
                subprocess.Popen._wait(self, timeout=5)
                if self.stdout:
                    self.stdout.close()
            raise

    def _internal_poll(self, _deadstate=None):
        if threading.current_thread() is not threading.main_thread():
            raise AssertionError("private process has a foreign waiter")
        if self.returncode is None:
            status = os.waitid(os.P_PIDFD, self.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if status is not None:
                self.returncode = (status.si_status if status.si_code == os.CLD_EXITED
                                   else -status.si_status)
        return self.returncode

    def send_signal(self, sig):
        try:
            signal.pidfd_send_signal(self.pidfd, sig)
        except ProcessLookupError:
            pass  # Exited identity: never reacquire or signal the numeric PID.

    def _wait(self, timeout):
        deadline = None if timeout is None else time.monotonic() + timeout
        while self._internal_poll() is None:
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(self.args, timeout)
            time.sleep(0.01)
        return self.returncode


class PrivateDesktop:
    """Own exact child process groups; never discover/kill production by name."""
    def __init__(self, test, native=False, available=True):
        self.test = test
        (ROOT / "build").mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="a-", dir=ROOT / "build")
        self.path = Path(self.tmp.name)
        self.runtime = self.path / "frametop"
        self.runtime.mkdir(mode=0o700)
        self.env = {"PATH": "/usr/bin:/bin", "HOME": str(self.path), "LANG": "C.UTF-8",
                    "XDG_RUNTIME_DIR": str(self.runtime), "XDG_CONFIG_HOME": str(self.path / "config"),
                    "GSETTINGS_BACKEND": "memory", "NO_AT_BRIDGE": "0", "GDK_BACKEND": "x11",
                    "GTK_THEME": "Adwaita", "XDG_CURRENT_DESKTOP": "KDE"}
        self.processes = []
        self.child_pidfds = []
        self.connections = []
        self.log = (ROOT / "build" / (test.id().split(".")[-1] + ".log")).open("w+")
        self.native = native
        self.available = available

    def spawn(self, argv, env=None, stdout=None):
        p = OwnedProcess(argv, env=env or self.env, stdin=subprocess.DEVNULL,
                             stdout=stdout or self.log, stderr=self.log, text=True,
                             start_new_session=True)
        self.processes.append(p)
        return p

    def retain_child(self, pid, owner):
        # Reserve the fixture group before trusting any freshly reported PID.
        # No later acquisition from a stored/historical descendant ID is allowed.
        if owner not in self.processes:
            raise AssertionError("child owner is not this fixture's process")
        os.waitid(os.P_PIDFD, owner.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        fd = None
        try:
            with Path(f"/proc/{pid}/stat").open() as stat:
                group = int(stat.read().rsplit(") ", 1)[1].split()[2])
                if group != owner.pid:
                    raise AssertionError("child is outside the reserved private group")
                fd = os.pidfd_open(pid)
                # The open proc inode is pinned to the original task. If that
                # task was reaped while pidfd_open ran, this re-read fails rather
                # than following a recycled PID to another task's stat file.
                stat.seek(0)
                if int(stat.read().rsplit(") ", 1)[1].split()[2]) != group:
                    raise AssertionError("private child changed process group")
                os.waitid(os.P_PIDFD, owner.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            self.child_pidfds.append(fd)
            result, fd = fd, None
            return result
        finally:
            if fd is not None:
                os.close(fd)

    def bus(self, name, services):
        config = self.path / (name + ".conf")
        config.write_text("<busconfig><type>session</type><listen>unix:tmpdir="
                          + str(self.path) + "</listen>" + "".join(
                              "<servicedir>" + str(d) + "</servicedir>" for d in services)
                          + '<policy context="default"><allow own="*"/>'
                          '<allow send_destination="*"/><allow receive_sender="*"/>'
                          '</policy></busconfig>')
        if name == "session":
            p = self.spawn(["/usr/bin/dbus-run-session", "--config-file=" + str(config), "--",
                            "/usr/bin/python3", str(Path(__file__).resolve()), "--hold-session"],
                           stdout=subprocess.PIPE)
            address = p.stdout.readline().strip()
            self.session_child_fd = self.retain_child(int(p.stdout.readline().strip()), p)
        else:
            p = self.spawn(["/usr/bin/dbus-daemon", "--nofork", "--config-file=" + str(config),
                            "--print-address"], stdout=subprocess.PIPE)
            address = p.stdout.readline().strip()
        self.test.assertTrue(address.startswith("unix:"), "private bus failed to start")
        conn = connect(address)
        self.connections.append(conn)
        return p, address, conn

    def __enter__(self):
        try:
            xvfb = self.spawn(["/usr/bin/Xvfb", "-displayfd", "1", "-screen", "0", "640x480x24",
                               "-nolisten", "tcp", "-noreset"], stdout=subprocess.PIPE)
            self.env["DISPLAY"] = ":" + xvfb.stdout.readline().strip()
            self.test.assertRegex(self.env["DISPLAY"], r"^:\d+$")
            self.session, self.address, self.connection = self.bus(
                "session", [] if self.native or not self.available else ["/usr/share/dbus-1/services"])
            self.env["DBUS_SESSION_BUS_ADDRESS"] = self.address
            if self.native:
                self.a11y_process, self.a11y_address, self.a11y = self.bus(
                    "a11y", ["/usr/share/dbus-1/accessibility-services"])
                self.provider = self.spawn(["/usr/bin/python3", str(Path(__file__).resolve()),
                                            "--provide", self.address, self.a11y_address])
                eventually(lambda: call(self.connection, DBUS, DBUS_PATH, DBUS, "NameHasOwner",
                                        "(s)", ("org.a11y.Bus",))[0])
            elif self.available:
                self.a11y_address = call(self.connection, "org.a11y.Bus", "/org/a11y/bus",
                                         "org.a11y.Bus", "GetAddress")[0]
                self.a11y = connect(self.a11y_address)
                self.connections.append(self.a11y)
            if self.native or self.available:
                broker = call(self.a11y, DBUS, DBUS_PATH, DBUS,
                              "GetConnectionUnixProcessID", "(s)", (DBUS,))[0]
                self.a11y_broker_fd = self.retain_child(
                    broker, self.a11y_process if self.native else self.session)
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def helper(self, env=None):
        return self.spawn([str(HELPER)], env=env)

    def owner(self):
        if not call(self.a11y, DBUS, DBUS_PATH, DBUS, "NameHasOwner", "(s)", (REGISTRY,))[0]:
            return None
        return call(self.a11y, DBUS, DBUS_PATH, DBUS, "GetConnectionUnixProcessID",
                    "(s)", (REGISTRY,))[0]

    def wait_owner(self, helper):
        def registry_started():
            pid = self.owner()
            if pid:
                return pid
            if helper.poll() is not None:
                self.log.flush()
                raise AssertionError("session startup did not start a registry: "
                                     + (ROOT / "build" / (self.test.id().split(".")[-1] + ".log")).read_text())
            return None
        return eventually(registry_started)

    def registry_env(self, pid):
        return dict(s.split("=", 1) for s in Path(f"/proc/{pid}/environ").read_bytes().decode().split("\0") if "=" in s)

    def app_tree(self):
        app = self.spawn(["/usr/bin/python3", str(Path(__file__).resolve()), "--gtk"])
        def names():
            found = []
            def visit(dest, path, depth=0):
                if depth > 5:
                    return
                name = call(self.a11y, dest, path, "org.freedesktop.DBus.Properties",
                            "Get", "(ss)", (ACCESSIBLE, "Name"))[0]
                found.append(name)
                for child_dest, child_path in call(self.a11y, dest, path, ACCESSIBLE, "GetChildren")[0]:
                    visit(child_dest, child_path, depth + 1)
            visit(REGISTRY, "/org/a11y/atspi/accessible/root")
            return found if "Private AT-SPI button" in found else None
        tree = eventually(names)
        self.test.assertIsNone(app.poll())
        print(f"TREE {self.test.id()}: {tree}", flush=True)
        return tree

    def stop_session(self):
        # End exactly the command owned by dbus-run-session, as Plasma ending would.
        try:
            signal.pidfd_send_signal(self.session_child_fd, signal.SIGTERM)
        except ProcessLookupError:
            pass  # The retained command identity has exited; never reacquire its PID.
        self.session.wait(timeout=5)

    def __exit__(self, *exc):
        errors = []
        owners = {}
        for p in self.processes:
            try:
                # WNOWAIT plus our sole-waiter OwnedProcess contract reserves the
                # actual group leader, not a historical PID or a /proc timestamp.
                os.waitid(os.P_PIDFD, p.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                owners[p.pid] = p
            except Exception as error:
                errors.append(RuntimeError(f"lost ownership of private group {p.pid}: {error}"))
        for conn in self.connections:
            try:
                if not conn.is_closed():
                    conn.close_sync(None)
            except Exception as error:
                errors.append(error)
        # All group signalling finishes BEFORE any owner is reaped. Even an
        # exited leader reserves its PGID, so no recycled group can be targeted.
        for sig in (signal.SIGTERM, signal.SIGKILL):
            for pgid in reversed(owners):
                try:
                    os.killpg(pgid, sig)
                except ProcessLookupError:
                    pass
                except Exception as error:
                    errors.append(error)
            if sig == signal.SIGTERM:
                deadline = time.monotonic() + 3
                for p in owners.values():
                    try:
                        p.wait(timeout=max(0, deadline - time.monotonic()))
                    except Exception as error:
                        errors.append(error)
        deadline = time.monotonic() + 5
        # A broken Popen.wait must not skip OS-level exit observation or reaping.
        for pid in owners:
            try:
                while os.waitid(os.P_PIDFD, owners[pid].pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None:
                    if time.monotonic() >= deadline:
                        raise AssertionError(f"private group owner {pid} survived SIGKILL")
                    time.sleep(0.01)
            except Exception as error:
                errors.append(error)
        children = {}
        try:
            # Native D-Bus activation inherits these reserved groups, including
            # grandchildren adopted by this test runner's subreaper. No ancestry
            # expansion from transient/historical descendant identifiers.
            try:
                paths = list(Path("/proc").iterdir())
            except Exception as error:
                errors.append(error)
                paths = []
            for path in paths:
                if not path.name.isdigit() or int(path.name) in owners:
                    continue
                fd = None
                try:
                    with (path / "stat").open() as stat:
                        group = int(stat.read().rsplit(") ", 1)[1].split()[2])
                        if group not in owners:
                            continue
                        fd = os.pidfd_open(int(path.name))
                        # This open proc file refers to the original task. Re-read
                        # AFTER acquiring the pidfd: recycling makes this read fail,
                        # rather than follow a newly allocated numeric PID.
                        stat.seek(0)
                        if int(stat.read().rsplit(") ", 1)[1].split()[2]) != group:
                            raise AssertionError("private child changed process group")
                    children[int(path.name)] = fd
                    fd = None
                except (FileNotFoundError, ProcessLookupError):
                    pass
                except Exception as error:
                    errors.append(error)
                finally:
                    if fd is not None:
                        os.close(fd)
            for pid, fd in children.items():
                try:
                    while True:
                        try:
                            status = os.waitid(os.P_PIDFD, fd, os.WEXITED | os.WNOHANG)
                        except ChildProcessError:
                            # A grandchild may have been reaped by its real parent.
                            # fdinfo is tied to this identity, not a reusable PID.
                            if "Pid:\t-1\n" in Path(f"/proc/self/fdinfo/{fd}").read_text():
                                break
                            status = None
                        if status is not None:
                            break
                        if time.monotonic() >= deadline:
                            raise AssertionError(f"private child {pid} leaked during teardown")
                        time.sleep(0.01)
                except Exception as error:
                    errors.append(error)
        except Exception as error:
            errors.append(error)
        finally:
            # Reap direct leaders through their ORIGINAL stable descriptors,
            # independently of dup, proc discovery, descendant waits or cached
            # Popen returncodes. Keep groups reserved through the entire scan.
            for pid, p in owners.items():
                try:
                    while True:
                        status = os.waitid(os.P_PIDFD, p.pidfd, os.WEXITED | os.WNOHANG)
                        if status is not None:
                            p.returncode = (status.si_status if status.si_code == os.CLD_EXITED
                                            else -status.si_status)
                            break
                        if time.monotonic() >= deadline:
                            raise AssertionError(f"private group owner {pid} was not reaped")
                        time.sleep(0.01)
                except Exception as error:
                    errors.append(error)
            for fd in self.child_pidfds:
                try:
                    os.close(fd)
                except Exception as error:
                    errors.append(error)
            for fd in children.values():
                try:
                    os.close(fd)
                except Exception as error:
                    errors.append(error)
            for p in self.processes:
                try:
                    os.close(p.pidfd)
                except Exception as error:
                    errors.append(error)
                try:
                    if p.stdout:
                        p.stdout.close()
                except Exception as error:
                    errors.append(error)
            try:
                for error in errors:
                    print(f"TEARDOWN ERROR: {type(error).__name__}: {error}", file=self.log)
                self.log.flush()
            except Exception as error:
                errors.append(error)
            try:
                self.log.close()
            except Exception as error:
                errors.append(error)
            try:
                self.tmp.cleanup()
                self.test.assertFalse(self.path.exists())
            except Exception as error:
                errors.append(error)
        if errors:
            if len(exc) > 1 and exc[1] is not None:
                errors.insert(0, exc[1])  # Preserve the test/body failure as well.
            raise BaseExceptionGroup("private desktop cleanup failed", errors)
        print(f"TEARDOWN {self.test.id()}: private processes reaped; runtime removed", flush=True)


class CleanupTests(unittest.TestCase):
    def test_pidfd_acquisition_failure_kills_reserved_group_then_consumes_wait(self):
        import errno
        from unittest.mock import Mock, patch
        events = []
        stdout = Mock()
        failure = OSError(errno.EMFILE, "pidfd acquisition exhausted")
        verify = verify_private_waiter

        def checked_contract():
            verify()  # Real, read-only SIGCHLD query; never change its disposition.
            events.append("verified")

        def spawn(child, *args, **kwargs):
            child.pid, child.returncode, child.stdout = 70001, None, stdout
            child._child_created = False
            events.append("spawn")

        def acquire(pid):
            self.assertEqual(pid, 70001)
            events.append("acquire")
            raise failure

        def killpg(pid, sig):
            self.assertEqual((pid, sig), (70001, signal.SIGKILL))
            events.append("last group signal")

        def consuming_wait(child, timeout):
            self.assertEqual((child.pid, timeout), (70001, 5))
            events.append("consuming base wait")
            child.returncode = -signal.SIGKILL
            return child.returncode

        disposition = signal.getsignal(signal.SIGCHLD)
        with patch.dict(globals(), verify_private_waiter=checked_contract), \
                patch.object(subprocess.Popen, "__init__", autospec=True, side_effect=spawn), \
                patch("os.pidfd_open", side_effect=acquire), patch("os.killpg", side_effect=killpg), \
                patch.object(subprocess.Popen, "_wait", autospec=True, side_effect=consuming_wait):
            with self.assertRaises(OSError) as raised:
                OwnedProcess(["/usr/bin/true"], start_new_session=True)
        self.assertIs(raised.exception, failure)
        self.assertEqual(events, ["verified", "spawn", "acquire", "last group signal", "consuming base wait"])
        stdout.close.assert_called_once_with()
        self.assertEqual(signal.getsignal(signal.SIGCHLD), disposition)

    def test_emfile_still_consuming_reaps_original_owner_after_last_group_signal(self):
        import errno
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop(self, available=False)
        child = Mock(pid=70001, pidfd=41, returncode=0, stdout=None)
        desktop.processes = [child]  # Already polled/waited: cached status is NOT a reap.
        events = []

        def waitid(idtype, fd, options):
            self.assertEqual((idtype, fd), (os.P_PIDFD, 41))
            events.append(("observe" if options & os.WNOWAIT else "consume", fd))
            return Mock(si_status=0, si_code=os.CLD_EXITED)

        def close(fd):
            events.append(("close", fd))

        start = time.monotonic()
        with patch("os.waitid", side_effect=waitid), \
                patch("os.killpg", side_effect=lambda pid, sig: events.append(("signal", sig))), \
                patch("os.dup", side_effect=OSError(errno.EMFILE, "dup exhausted")), \
                patch.object(Path, "iterdir", side_effect=OSError(errno.EMFILE, "scan exhausted")), \
                patch("os.close", side_effect=close):
            with self.assertRaises(ExceptionGroup) as raised:
                desktop.__exit__(None, None, None)
        self.assertIn("scan exhausted", str(raised.exception.exceptions))
        self.assertEqual([event for event in events if event[0] == "consume"], [("consume", 41)],
                         "descriptor exhaustion skipped consuming waitid on the ORIGINAL pidfd")
        self.assertLess(events.index(("signal", signal.SIGKILL)), events.index(("consume", 41)))
        self.assertLess(events.index(("consume", 41)), events.index(("close", 41)))
        self.assertTrue(desktop.log.closed)
        self.assertFalse(desktop.path.exists())
        self.assertLess(time.monotonic() - start, 1)

    def test_owned_process_signalling_uses_original_pidfd(self):
        import threading
        from unittest.mock import patch
        child = OwnedProcess.__new__(OwnedProcess)
        child.pid, child.pidfd, child.returncode = 70002, 42, None
        child._waitpid_lock = threading.Lock()
        child._child_created = False
        for method, sig in (("terminate", signal.SIGTERM), ("kill", signal.SIGKILL),
                            ("send_signal", signal.SIGINT)):
            with self.subTest(method=method), patch.object(child, "_internal_poll", return_value=None), \
                    patch("os.kill") as kill, patch("os.pidfd_open") as acquire, \
                    patch("signal.pidfd_send_signal") as send:
                getattr(child, method)(sig) if method == "send_signal" else getattr(child, method)()
                kill.assert_not_called()
                acquire.assert_not_called()
                send.assert_called_once_with(42, sig)

    def test_owned_process_rejects_unsafe_waiter_contract_before_spawn(self):
        import threading
        from unittest.mock import Mock, patch

        def fake_spawn(child, *args, **kwargs):
            child.pid, child.returncode, child.stdout = 70001, None, None
            child._child_created = False

        def sigaction(sig, new, old):
            self.assertEqual(sig, signal.SIGCHLD)
            self.assertIsNone(new, "checking the waiter contract changed a global handler")
            old._obj.sa_handler = None
            old._obj.sa_flags = flags
            return 0

        libc = Mock(sigaction=Mock(side_effect=sigaction))
        for case, disposition, flags, current_thread, private in (
                ("ignored SIGCHLD", signal.SIG_IGN, 0, threading.main_thread(), True),
                ("SA_NOCLDWAIT", signal.SIG_DFL, 2, threading.main_thread(), True),
                ("foreign waiter thread", signal.SIG_DFL, 0, object(), True),
                ("another Python waiter", signal.SIG_DFL, 0, threading.main_thread(), True),
                ("unreserved group", signal.SIG_DFL, 0, threading.main_thread(), False)):
            with self.subTest(case=case), patch("signal.getsignal", return_value=disposition), \
                    patch("ctypes.CDLL", return_value=libc), \
                    patch("threading.current_thread", return_value=current_thread), \
                    patch("threading.enumerate", return_value=[threading.main_thread()]
                          + ([Mock()] if case == "another Python waiter" else [])), \
                    patch.object(subprocess.Popen, "__init__", autospec=True, side_effect=fake_spawn) as spawn, \
                    patch("os.pidfd_open", return_value=42), patch("os.killpg") as killpg:
                with self.assertRaises(AssertionError):
                    OwnedProcess(["/usr/bin/true"], start_new_session=private)
                spawn.assert_not_called()
                killpg.assert_not_called()

    def test_broker_loss_signals_retained_identity_without_pid_lookup(self):
        from unittest.mock import Mock, patch
        desktop = Mock(a11y_broker_fd=43)
        desktop.__enter__ = Mock(return_value=desktop)
        desktop.__exit__ = Mock(return_value=False)
        desktop.wait_owner.return_value = 70002
        desktop.helper.return_value.returncode = 0
        test = AccessibilityTests("test_accessibility_bus_loss_stops_only_owned_registry")
        with patch.dict(globals(), PrivateDesktop=Mock(return_value=desktop),
                        call=Mock(return_value=(70002,)), process_running=Mock(return_value=False)), \
                patch("os.kill") as kill, patch("os.pidfd_open") as acquire, \
                patch("signal.pidfd_send_signal") as send:
            test.test_accessibility_bus_loss_stops_only_owned_registry()
        kill.assert_not_called()
        acquire.assert_not_called()
        send.assert_called_once_with(43, signal.SIGTERM)

    def test_desktop_setup_retains_broker_from_reserved_private_group(self):
        from io import StringIO
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop(self)
        owner = Mock(pid=70001, pidfd=41)
        desktop.processes = [owner]
        xvfb = Mock(stdout=StringIO("42\n"))
        stat = StringIO("70003 (broker) S 70001 70001 70001 0\n")
        query = Mock(side_effect=[("unix:a11y",), (70003,)])
        try:
            with patch.object(desktop, "spawn", return_value=xvfb), \
                    patch.object(desktop, "bus", return_value=(owner, "unix:session", Mock())), \
                    patch.dict(globals(), connect=Mock(), call=query), \
                    patch.object(Path, "open", return_value=stat), \
                    patch("os.waitid", return_value=None), patch("os.pidfd_open", return_value=44):
                desktop.__enter__()
            self.assertEqual(getattr(desktop, "a11y_broker_fd", None), 44,
                             "setup did not retain the private broker identity")
            self.assertEqual(desktop.child_pidfds, [44])
            self.assertEqual(query.call_args.args[4], "GetConnectionUnixProcessID")
        finally:
            xvfb.stdout.close()
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_session_command_acquisition_rejects_unproven_identity(self):
        from io import StringIO
        from unittest.mock import MagicMock, Mock, patch
        desktop = PrivateDesktop(self, available=False)
        owned = "70002 (command) S 70001 70001 70001 0\n"
        foreign = "70002 (other) S 1 80001 80001 0\n"
        try:
            for case, first, second, wait_error in (
                    ("foreign group", foreign, foreign, None),
                    ("recycled during acquisition", owned, ProcessLookupError(), None),
                    ("changed group", owned, foreign, None),
                    ("historical owner", owned, owned, ChildProcessError())):
                with self.subTest(case=case):
                    owner = Mock(pid=70001, pidfd=41, stdout=StringIO("unix:private\n70002\n"))
                    desktop.processes = [owner]
                    stat = MagicMock()
                    stat.__enter__.return_value = stat
                    stat.read.side_effect = [first, second]
                    with patch.object(desktop, "spawn", return_value=owner), \
                            patch.dict(globals(), connect=Mock()), patch.object(Path, "write_text"), \
                            patch.object(Path, "open", return_value=stat), \
                            patch("os.waitid", side_effect=wait_error, return_value=None), \
                            patch("os.pidfd_open", return_value=42) as acquire, \
                            patch("os.close") as close, patch("os.kill") as kill, \
                            patch("signal.pidfd_send_signal") as send:
                        with self.assertRaises((AssertionError, ProcessLookupError, ChildProcessError)):
                            desktop.bus("session", [])
                        if case in ("foreign group", "historical owner"):
                            acquire.assert_not_called()
                        else:
                            close.assert_called_once_with(42)
                        kill.assert_not_called()
                        send.assert_not_called()
                    owner.stdout.close()
        finally:
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_session_command_identity_is_retained_during_bus_setup(self):
        from io import StringIO
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop(self, available=False)
        owner = Mock(pid=70001, pidfd=41, stdout=StringIO("unix:private\n70002\n"))
        desktop.processes = [owner]
        desktop.child_pidfds = []
        stat = StringIO("70002 (command) S 70001 70001 70001 0\n")
        try:
            with patch.object(desktop, "spawn", return_value=owner), \
                    patch.dict(globals(), connect=Mock()), patch.object(Path, "write_text"), \
                    patch.object(Path, "open", return_value=stat), \
                    patch("os.waitid", return_value=None) as reserve, \
                    patch("os.pidfd_open", return_value=42) as acquire:
                desktop.bus("session", [])
            self.assertEqual(getattr(desktop, "session_child_fd", None), 42,
                             "bus setup did not retain the command's proven identity")
            self.assertEqual(desktop.child_pidfds, [42])
            reserve.assert_called_with(os.P_PIDFD, 41, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            acquire.assert_called_once_with(70002)
        finally:
            owner.stdout.close()
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_session_stop_uses_retained_identity_not_historical_pid(self):
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop.__new__(PrivateDesktop)
        desktop.session = Mock()
        desktop.session_child = 70002  # Model an already-reaped wrapper command.
        desktop.session_child_fd = 42  # Its original retained identity, never reused.
        with patch("os.kill") as kill, patch("os.pidfd_open") as acquire, \
                patch("signal.pidfd_send_signal", side_effect=ProcessLookupError) as send:
            desktop.stop_session()
        kill.assert_not_called()
        acquire.assert_not_called()
        send.assert_called_once_with(42, signal.SIGTERM)
        desktop.session.wait.assert_called_once_with(timeout=5)

    def test_native_adopted_children_cleanup_leaves_independent_fixture_alive(self):
        # Both are private. No host display, host bus, PID recycling or broad kills.
        from unittest.mock import patch
        with PrivateDesktop(self) as sibling:
            sibling_pid = sibling.wait_owner(sibling.helper())
            sibling_env = sibling.registry_env(sibling_pid)
            native = PrivateDesktop(self, native=True)
            real_killpg = os.killpg

            def owned_signal(pgid, sig):
                self.assertIn(pgid, {p.pid for p in native.processes})
                # Audit every real, isolated signal while ownership is retained.
                os.waitid(os.P_PID, pgid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                real_killpg(pgid, sig)

            registry_fd = None
            try:
                with patch("os.killpg", side_effect=owned_signal), native:
                    helper = native.helper()
                    registry = native.wait_owner(helper)
                    helper.wait(timeout=8)
                    try:
                        status = os.waitid(os.P_PID, helper.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                    except ChildProcessError:
                        self.fail("wait released the private group owner before cleanup")
                    self.assertIsNotNone(status)
                    self.assertEqual(os.getpgid(registry), native.a11y_process.pid)
                    self.assertEqual(int(Path(f"/proc/{registry}/stat").read_text()
                                         .rsplit(") ", 1)[1].split()[1]), os.getpid(),
                                     "native registry was not adopted by the test subreaper")
                    registry_fd = os.pidfd_open(registry)
                    native.app_tree()
                    native.stop_session()  # Another waited leader stays reserved.
                self.assertIn("Pid:\t-1\n", Path(f"/proc/self/fdinfo/{registry_fd}").read_text())
                self.assertFalse(native.path.exists())
                self.assertTrue(native.log.closed)
            finally:
                if registry_fd is not None:
                    os.close(registry_fd)
            self.assertTrue(process_running(sibling_pid))
            self.assertEqual(sibling.owner(), sibling_pid)
            self.assertEqual(sibling.registry_env(sibling_pid), sibling_env)
            self.assertTrue(call(sibling.connection, DBUS, DBUS_PATH, DBUS, "GetId")[0])
            sibling.app_tree()

    def test_cleanup_aggregates_close_and_wait_failures_and_escalates(self):
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop(self, available=False)
        child = desktop.spawn(["/usr/bin/python3", "-c",
                               "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                               "print('ready', flush=True); time.sleep(60)"], stdout=subprocess.PIPE)
        self.assertEqual(child.stdout.readline().strip(), "ready")
        pidfd = os.pidfd_open(child.pid)
        bad = Mock()
        bad.is_closed.return_value = False
        bad.close_sync.side_effect = OSError("close failure")
        good = Mock()
        good.is_closed.return_value = False
        desktop.connections.extend((bad, good))
        try:
            start = time.monotonic()
            with patch.object(child, "wait", side_effect=OSError("wait failure")), \
                    patch("os.killpg", wraps=os.killpg) as killpg:
                try:
                    desktop.__exit__(None, None, None)
                except ExceptionGroup as errors:
                    self.assertIn("close failure", str(errors.exceptions))
                    self.assertIn("wait failure", str(errors.exceptions))
                except OSError:
                    pass  # RED must fail on incomplete cleanup, not the injected exception.
                else:
                    self.fail("teardown silently discarded real failures")
                self.assertFalse(desktop.path.exists(), "failure interrupted runtime cleanup")
                self.assertTrue(desktop.log.closed)
                good.close_sync.assert_called_once_with(None)
                self.assertIn((child.pid, signal.SIGKILL),
                              [args for args, _ in killpg.call_args_list])
            self.assertLess(time.monotonic() - start, 8)
            with self.assertRaises(ChildProcessError):
                os.waitpid(child.pid, os.WNOHANG)
            evidence = (ROOT / "build" / (self.id().split(".")[-1] + ".log")).read_text()
            self.assertIn("close failure", evidence)
            self.assertIn("wait failure", evidence)
        finally:
            # pidfd targets only the isolated child, even if RED aborted cleanup.
            try:
                signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                os.waitpid(child.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
            os.close(pidfd)
            if child.stdout:
                child.stdout.close()
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_proc_scan_failure_still_reaps_owned_children(self):
        from unittest.mock import patch
        desktop = PrivateDesktop(self, available=False)
        child = desktop.spawn(["/usr/bin/true"])
        child.wait(timeout=5)
        try:
            with patch.object(Path, "iterdir", side_effect=OSError("scan failure")), \
                    patch("os.killpg"):
                with self.assertRaises(ExceptionGroup) as raised:
                    desktop.__exit__(None, None, None)
                self.assertIn("scan failure", str(raised.exception.exceptions))
            self.assertFalse(desktop.path.exists())
            self.assertTrue(desktop.log.closed)
            with self.assertRaises(ChildProcessError):
                os.waitpid(child.pid, os.WNOHANG)
        finally:
            try:
                os.waitpid(child.pid, os.WNOHANG)
            except ChildProcessError:
                pass
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_recycled_numeric_child_identity_cannot_restore_group_ownership(self):
        from unittest.mock import Mock, patch
        desktop = PrivateDesktop(self, available=False)
        child = desktop.spawn(["/usr/bin/true"])
        child.wait(timeout=5)
        os.waitpid(child.pid, 0)
        waitid = os.waitid
        replacement = Mock(si_status=0, si_code=os.CLD_EXITED)

        def recycled_wait(idtype, identity, options):
            # Model a different adopted child reusing the historical numeric PID.
            # No real reuse, unrelated process or real signal is involved.
            if idtype == os.P_PID and identity == child.pid:
                return replacement
            return waitid(idtype, identity, options)

        try:
            with patch("os.waitid", side_effect=recycled_wait), patch("os.killpg") as killpg, \
                    patch("os.kill") as kill, patch.object(Path, "iterdir", return_value=iter(())):
                try:
                    desktop.__exit__(None, None, None)
                except ExceptionGroup:
                    pass
                killpg.assert_not_called()
                kill.assert_not_called()
            self.assertFalse(desktop.path.exists())
            self.assertTrue(desktop.log.closed)
        finally:
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_reaped_historical_group_is_not_signalled_or_discovered(self):
        from unittest.mock import patch
        desktop = PrivateDesktop(self, available=False)
        child = desktop.spawn(["/usr/bin/true"])
        child.wait(timeout=5)
        os.waitpid(child.pid, 0)  # Deliberately revoke ownership; never force PID reuse.
        historical = desktop.path / "70002"
        historical.mkdir()
        (historical / "stat").write_text(f"70002 (unrelated) S 1 {child.pid} 0\n")
        try:
            with patch("os.killpg") as killpg, patch("os.kill") as kill, \
                    patch("os.pidfd_open") as pidfd_open, \
                    patch.object(Path, "iterdir", return_value=iter((historical,))):
                try:
                    desktop.__exit__(None, None, None)
                except ExceptionGroup as errors:
                    self.assertIn("ownership", str(errors.exceptions))
                killpg.assert_not_called()
                kill.assert_not_called()
                pidfd_open.assert_not_called()
            self.assertFalse(desktop.path.exists())
            self.assertTrue(desktop.log.closed)
        finally:
            desktop.log.close()
            desktop.tmp.cleanup()

    def test_waited_and_polled_children_keep_group_ownership_until_last_signal(self):
        from unittest.mock import patch
        desktop = PrivateDesktop(self, available=False)
        waited = desktop.spawn(["/usr/bin/true"])
        polled = desktop.spawn(["/usr/bin/true"])
        waited.wait(timeout=5)
        eventually(lambda: polled.poll() is not None)
        signals = []

        def protected_group(pgid, sig):
            # Mock every signal: never induce reuse or signal an unrelated group.
            try:
                status = os.waitid(os.P_PID, pgid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            except ChildProcessError:
                self.fail("cleanup signalled a reaped/historical process group")
            self.assertIsNotNone(status, "the exited group owner must remain unreaped")
            signals.append((pgid, sig))

        try:
            with patch("os.killpg", side_effect=protected_group):
                desktop.__exit__(None, None, None)
            self.assertEqual({pgid for pgid, _ in signals}, {waited.pid, polled.pid})
            for child in (waited, polled):
                with self.assertRaises(ChildProcessError):
                    os.waitpid(child.pid, os.WNOHANG)
        finally:
            # Also keep RED safe if teardown aborts at the ownership assertion.
            for child in (waited, polled):
                try:
                    os.waitpid(child.pid, os.WNOHANG)
                except ChildProcessError:
                    pass
            desktop.log.close()
            desktop.tmp.cleanup()


class AccessibilityTests(unittest.TestCase):
    def test_accessibility_bus_loss_stops_only_owned_registry(self):
        with PrivateDesktop(self) as desktop:
            helper = desktop.helper()
            pid = desktop.wait_owner(helper)
            signal.pidfd_send_signal(desktop.a11y_broker_fd, signal.SIGTERM)
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertFalse(process_running(pid))
            self.assertTrue(call(desktop.connection, DBUS, DBUS_PATH, DBUS, "GetId")[0])

    def test_watcher_termination_reaps_only_its_registry_child(self):
        with PrivateDesktop(self) as desktop:
            helper = desktop.helper()
            pid = desktop.wait_owner(helper)
            helper.terminate()
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertFalse(process_running(pid))
            self.assertTrue(call(desktop.connection, DBUS, DBUS_PATH, DBUS, "GetId")[0])
            self.assertTrue(call(desktop.a11y, DBUS, DBUS_PATH, DBUS, "GetId")[0])

    def test_accessibility_watcher_stays_owned_by_desktop_unit(self):
        source = (ROOT / "session/keep-apps.sh").read_text()
        own = source[source.index("own='"):source.index("keep=()")]
        result = subprocess.run(["bash", "-c", own + "\n[[ ft-atspi =~ $own ]]"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, "desktop stop would preserve its accessibility watcher as an app")

    def test_desktop_stop_ends_any_releases_helpers_and_keeps_programs(self):
        source = (ROOT / "session/keep-apps.sh").read_text()
        ours = source[source.index("root=$("):source.index("while read -r pid")]
        with tempfile.TemporaryDirectory() as tmp:
            tmp = os.path.realpath(tmp)
            release = os.path.join(tmp, "frametop/releases/0.3.0-exp.27")
            checkout = os.path.join(tmp, "src/frametop")
            for tree, command, ends in (
                    (release, f"python3 {release}/frunner/ft-frunner", True),
                    (release, f"/usr/bin/python3 -I {tmp}/frametop/releases/0.3.0-exp.26/floats/ft-floatd", True),
                    (release, f"distrobox enter box -- {tmp}/frametop/releases/0.3.0-exp.26/x", True),
                    (release, "podman exec box ft-screens", True),
                    (release, f"tail -f {tmp}/frametop/releases/current/notes", False),
                    (release, f"tmux new {tmp}/frametop/releases/current/desktops.sh restart", False),
                    (release, f"tmux new -c {tmp}/frametop/releases/current", False),
                    (release, f"du -sh {tmp}/frametop/releases/", False),
                    (release, "konsole", False),
                    (checkout, f"python3 {checkout}/frunner/ft-frunner", True),
                    (checkout, f"bash {checkout}/desktops.sh restart", True),
                    (checkout, f"python3 {tmp}/src/other/tool", False)):
                session = os.path.join(tree, "session")
                os.makedirs(session, exist_ok=True)
                result = subprocess.run(["bash", "-c", ours + '\nours "$1"', session + "/keep-apps.sh", command])
                self.assertEqual(result.returncode == 0, ends, command)

    def test_update_check_reports_missing_accessibility_host_files(self):
        import runpy
        from unittest.mock import patch
        module = runpy.run_path(str(ROOT / "scripts/update-check.py"))
        check_host = module["check_host"]
        missing = {"/usr/bin/python3", "/usr/bin/gdbus", "/usr/lib/at-spi-bus-launcher", "/usr/lib/at-spi2-registryd",
                   "/usr/share/dbus-1/services/org.a11y.Bus.service",
                   "/usr/share/dbus-1/accessibility-services/org.a11y.atspi.Registry.service"}
        reports = []
        exists = os.path.exists
        # Stub unrelated systemd/launcher checks; exercise the real host-file check.
        with patch.dict(check_host.__globals__,
                        report=lambda *args: reports.append(args),
                        systemctl=lambda *args: "", launcher_session=lambda: None), \
                patch("os.path.exists", side_effect=lambda p: False if p in missing else exists(p)):
            check_host()
        reported = {args[1].removesuffix(" missing") for args in reports if args[0] == "warn"}
        self.assertTrue(missing <= reported, "accessibility dependencies are not checked after SteamOS updates")
        self.assertIn("at-spi2-core", module["PACKAGES"])

    def test_session_stop_restart_uses_a_fresh_registry(self):
        with PrivateDesktop(self) as desktop:
            helper = desktop.helper()
            pid = desktop.wait_owner(helper)
            launcher = call(desktop.connection, DBUS, DBUS_PATH, DBUS,
                            "GetConnectionUnixProcessID", "(s)", ("org.a11y.Bus",))[0]
            desktop.app_tree()
            old_address = desktop.a11y_address
            desktop.stop_session()
            eventually(lambda: not process_running(pid))
            eventually(lambda: not process_running(launcher))
            print(f"STOP: registry {pid}, launcher {launcher} exited on private session-bus loss", flush=True)
        with PrivateDesktop(self) as restarted:
            helper = restarted.helper(dict(restarted.env, AT_SPI_BUS_ADDRESS=old_address))
            new_pid = restarted.wait_owner(helper)
            self.assertNotEqual(new_pid, pid)
            self.assertNotEqual(restarted.a11y_address, old_address)
            self.assertEqual(restarted.registry_env(new_pid)["AT_SPI_BUS_ADDRESS"], restarted.a11y_address)
            restarted.app_tree()
            print(f"RESTART: fresh registry {new_pid} on {restarted.a11y_address}", flush=True)

    def test_existing_registry_is_not_replaced_or_reconfigured(self):
        with PrivateDesktop(self) as desktop:
            existing = desktop.spawn(["/usr/lib/at-spi2-registryd"],
                                     env=dict(desktop.env, AT_SPI_BUS_ADDRESS=desktop.a11y_address))
            pid = desktop.wait_owner(existing)
            before = desktop.registry_env(pid)
            helper = desktop.helper()
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertEqual(desktop.owner(), pid)
            self.assertEqual(desktop.registry_env(pid), before)
            self.assertIsNone(existing.poll())

    def test_concurrent_start_keeps_only_one_registry(self):
        with PrivateDesktop(self) as desktop:
            first, second = desktop.helper(), desktop.helper()
            pid = desktop.wait_owner(first)
            eventually(lambda: first.poll() is not None or second.poll() is not None)
            self.assertEqual(desktop.owner(), pid)
            self.assertEqual(sum(p.poll() is None for p in (first, second)), 1)
            exited = first if first.poll() is not None else second
            self.assertEqual(exited.returncode, 0)

    def test_inherited_host_registry_is_preserved(self):
        # The "host" here is another PRIVATE Xvfb/bus, not the running desktop.
        with PrivateDesktop(self) as host:
            host_pid = host.wait_owner(host.helper())
            before = host.registry_env(host_pid)
            with PrivateDesktop(self) as nested:
                helper = nested.helper(dict(nested.env, AT_SPI_BUS_ADDRESS=host.a11y_address))
                nested_pid = nested.wait_owner(helper)
                self.assertNotEqual(nested_pid, host_pid)
                self.assertEqual(nested.registry_env(nested_pid)["AT_SPI_BUS_ADDRESS"], nested.a11y_address)
                nested.app_tree()
                self.assertEqual(host.owner(), host_pid)
                self.assertEqual(host.registry_env(host_pid), before)
            self.assertTrue(process_running(host_pid))

    def test_unavailable_accessibility_does_not_fail_startup(self):
        with PrivateDesktop(self, available=False) as desktop:
            helper = desktop.helper()
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertFalse(call(desktop.connection, DBUS, DBUS_PATH, DBUS,
                                  "NameHasOwner", "(s)", ("org.a11y.Bus",))[0])

    def test_missing_gdbus_is_optional(self):
        with PrivateDesktop(self) as desktop:
            helper = desktop.spawn(["/usr/bin/python3", str(HELPER)],
                                   env=dict(desktop.env, PATH="/unavailable"))
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertIsNone(desktop.owner())

    def test_outside_nested_runtime_does_not_autolaunch_a_bus(self):
        result = subprocess.run(["/usr/bin/python3", str(HELPER)],
                                env={"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(ROOT / "build"),
                                     "DBUS_SESSION_BUS_ADDRESS": "unix:path=/not-a-session"},
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "")

    def test_session_autostarts_registry_after_nested_display_is_ready(self):
        source = (ROOT / "session/frametop-session.sh").read_text()
        marker = "# AT-SPI:"
        self.assertIn(marker, source, "the registry has no nested-session startup integration")
        block = source[source.index(marker):source.index("# ft-floatd (floating windows)")]
        with tempfile.TemporaryDirectory(prefix="autostart-", dir=ROOT / "build") as tmp:
            result = subprocess.run(["bash", "-eu", "-c", block],
                                    env={"PATH": "/usr/bin:/bin", "XDG_CONFIG_HOME": tmp,
                                         "here": str(ROOT / "session")}, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            entry = Path(tmp, "autostart/frametop-atspi.desktop").read_text()
            self.assertIn('Exec="' + str(HELPER) + '"', entry)
            self.assertIn("X-KDE-autostart-phase=2", entry)
            self.assertIn("OnlyShowIn=KDE;", entry)
            self.assertTrue(os.access(HELPER, os.X_OK))

    def test_session_drops_inherited_host_accessibility_address(self):
        # Execute only the existing environment-sanitizing preamble, never the desktop.
        preamble = (ROOT / "session/frametop-session.sh").read_text().split("conf=$HOME", 1)[0]
        result = subprocess.run(["bash", "-c", preamble + "\n/usr/bin/printenv AT_SPI_BUS_ADDRESS"],
                                env={"PATH": "/usr/bin:/bin", "HOME": str(ROOT / "build"),
                                     "AT_SPI_BUS_ADDRESS": "unix:path=/inherited-host-bus"},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, "the nested session still inherits the host AT-SPI bus")
        self.assertEqual(result.stdout, "")

    def test_steamos_broker_without_systemd_starts_one_registry(self):
        with PrivateDesktop(self) as desktop:
            helper = desktop.helper(dict(desktop.env, AT_SPI_BUS_ADDRESS="unix:path=/does-not-exist"))
            pid = desktop.wait_owner(helper)
            self.assertEqual(Path(f"/proc/{helper.pid}/comm").read_text().strip(), "ft-atspi")
            self.assertEqual(desktop.registry_env(pid)["AT_SPI_BUS_ADDRESS"], desktop.a11y_address)
            self.assertEqual(desktop.registry_env(pid)["DISPLAY"], desktop.env["DISPLAY"])
            second = desktop.helper()
            second.wait(timeout=8)
            self.assertEqual(second.returncode, 0)
            self.assertEqual(desktop.owner(), pid)
            desktop.app_tree()

    def test_native_activation_routes_registry_to_live_bus(self):
        with PrivateDesktop(self, native=True) as desktop:
            # A stale Steam/host address must never be used, including by activation.
            env = dict(desktop.env, AT_SPI_BUS_ADDRESS="unix:path=/does-not-exist")
            helper = desktop.helper(env)
            pid = desktop.wait_owner(helper)
            helper.wait(timeout=8)
            self.assertEqual(helper.returncode, 0)
            self.assertNotEqual(pid, helper.pid, "native activation should not manually spawn a registry")
            self.assertEqual(desktop.registry_env(pid)["AT_SPI_BUS_ADDRESS"], desktop.a11y_address)
            desktop.app_tree()


def provide(session, accessibility):
    bus = connect(session)
    xml = Gio.DBusNodeInfo.new_for_xml("<node><interface name='org.a11y.Bus'>"
                                      "<method name='GetAddress'><arg type='s' direction='out'/>"
                                      "</method></interface></node>")
    def invoked(connection, sender, path, interface, method, parameters, invocation):
        invocation.return_value(GLib.Variant("(s)", (accessibility,)))
    bus.register_object("/org/a11y/bus", xml.interfaces[0], invoked, None, None)
    call(bus, DBUS, DBUS_PATH, DBUS, "RequestName", "(su)", ("org.a11y.Bus", 4))
    GLib.MainLoop().run()


def gtk():
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk
    window = Gtk.Window(title="Private Frametop AT-SPI fixture")
    window.add(Gtk.Button(label="Private AT-SPI button"))
    window.connect("destroy", Gtk.main_quit)
    window.show_all()
    Gtk.main()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--provide":
        provide(*sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "--hold-session":
        print(os.environ["DBUS_SESSION_BUS_ADDRESS"], flush=True)
        print(os.getpid(), flush=True)
        GLib.MainLoop().run()
    elif len(sys.argv) > 1 and sys.argv[1] == "--gtk":
        gtk()
    else:
        # Make test teardown account for grandchildren, without touching unrelated processes.
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
        unittest.main(verbosity=2)
