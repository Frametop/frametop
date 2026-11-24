#!/usr/bin/python3
"""The Frametop Installer's helper on the host: it answers sudo for the install, and sets a
password for a user that has none. It runs on the host (flatpak-spawn --host), not in the
app's sandbox, because the sandbox has its own process IDs: there, the install's processes
can't be told apart from anyone else's.

  helper.py serve --socket PATH (--unit NAME | --cgroup PATH)
  helper.py setpw

serve: the install's sudo runs SUDO_ASKPASS (installer/host/askpass), which connects to PATH.
Only a process of this user in the install's service (its cgroup, from systemctl, or --cgroup)
gets an answer; any other connection is closed unanswered. The window talks to this over
stdin and stdout, one line each:
  in:  "pw HEX"  the password (hex), kept in memory and given to every askpass that asks
       "refuse"  close the askpass connections that are waiting (that part is skipped)
  out: "ready"   listening
       "ask"     an askpass is waiting and there's no password yet
When stdin closes (the window went), it wipes the password, removes the socket and exits, so
nobody is left to answer: sudo then fails, and install.sh says which parts it skipped.

setpw: a line on stdin with the new password (hex); it runs passwd on a pseudo-terminal and
answers its prompts. A user without a password isn't asked for the current one (an empty
answer is given if it is). Exit status 0 when passwd took it; otherwise what passwd said.

The password is never written to a file, a log, an environment or a command line.
"""
import os
import pty
import re
import select
import selectors
import socket
import struct
import subprocess
import sys
import time


def wipe(b):
    if b:
        b[:] = bytes(len(b))


def cgroup_of(pid):
    try:
        with open(f"/proc/{pid}/cgroup") as f:
            for line in f:
                if line.startswith("0::"):
                    return line[3:].strip()
    except OSError:
        pass
    return None


def unit_cgroup(unit):
    out = subprocess.run(["systemctl", "--user", "show", "-P", "ControlGroup", f"{unit}.service"],
                         capture_output=True, text=True).stdout.strip()
    return out or None


def allowed(conn, want_cgroup):
    pid, uid, _ = struct.unpack("3i", conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                      struct.calcsize("3i")))
    if uid != os.getuid() or pid <= 0 or not want_cgroup:
        return False
    cg = cgroup_of(pid)
    return cg is not None and (cg == want_cgroup or cg.startswith(want_cgroup.rstrip("/") + "/"))


def serve(path, unit=None, cgroup=None):
    folder = os.path.dirname(path)
    os.makedirs(folder, mode=0o700, exist_ok=True)
    st = os.lstat(folder)
    if st.st_uid != os.getuid() or not os.path.isdir(folder) or os.path.islink(folder):
        print(f"{folder} isn't a folder of yours", file=sys.stderr)
        return 1
    os.chmod(folder, 0o700)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(path)
    sock.listen(8)
    password = None
    waiting = []
    sel = selectors.DefaultSelector()
    sel.register(sock, selectors.EVENT_READ, "accept")
    sel.register(sys.stdin.buffer, selectors.EVENT_READ, "stdin")
    say = lambda word: (sys.stdout.write(word + "\n"), sys.stdout.flush())
    buf = b""

    def answer(conn):
        try:
            conn.setblocking(True)
            conn.settimeout(5)
            conn.sendall(bytes(password) if password else b"")
        except OSError:
            pass
        conn.close()

    say("ready")
    try:
        while True:
            for key, _ in sel.select():
                if key.data == "accept":
                    try:
                        conn, _ = sock.accept()
                    except OSError:
                        continue
                    if not allowed(conn, cgroup or unit_cgroup(unit)):
                        conn.close()
                    elif password:
                        answer(conn)
                    else:
                        waiting.append(conn)
                        say("ask")
                    continue
                chunk = os.read(sys.stdin.fileno(), 4096)
                if not chunk:
                    return 0
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.startswith(b"pw "):
                        wipe(password)
                        try:
                            password = bytearray(bytes.fromhex(line[3:].decode()))
                        except ValueError:
                            password = None
                        while waiting and password:
                            answer(waiting.pop())
                    elif line == b"refuse":
                        while waiting:
                            waiting.pop().close()
                    line = b""
    finally:
        wipe(password)
        for c in waiting:
            c.close()
        sock.close()
        try:
            os.unlink(path)
        except OSError:
            pass


PROMPT_NEW = re.compile(rb"(new|retype|again|repeat)[^:\n]*:\s*$", re.I)
PROMPT_OLD = re.compile(rb"(current|old)[^:\n]*password[^:\n]*:\s*$", re.I)


def setpw(passwd="passwd"):  # passwd from PATH (the tests put a stand-in first)
    line = sys.stdin.buffer.readline().strip()
    try:
        pw = bytearray(bytes.fromhex(line.decode()))
    except ValueError:
        print("no password given")
        return 2
    if not pw:
        print("no password given")
        return 2
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execvp(passwd, [passwd])
        finally:
            os._exit(127)
    seen = b""
    news = olds = 0
    deadline = time.monotonic() + 30
    try:
        while time.monotonic() < deadline:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                continue
            try:
                data = os.read(fd, 1024)
            except OSError:
                break  # passwd ended
            if not data:
                break
            seen += data
            tail = seen[-200:]
            if PROMPT_OLD.search(tail):
                olds += 1
                if olds > 1:
                    break  # it wants a current password we don't have
                os.write(fd, b"\n")
                seen = b""
            elif PROMPT_NEW.search(tail):
                news += 1
                if news > 2:
                    break  # it refused the first one and asks again
                os.write(fd, bytes(pw) + b"\n")
                seen = b""
    finally:
        wipe(pw)
    try:
        os.kill(pid, 15)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    code = os.waitstatus_to_exitcode(status)
    if code != 0:
        said = " ".join(seen.decode("utf-8", "replace").split())
        print(said[-300:] or "passwd didn't take it")
        return 1
    return 0


def main(argv):
    if argv[:1] == ["serve"]:
        args = dict(zip(argv[1::2], argv[2::2]))
        if "--socket" not in args or not ({"--unit", "--cgroup"} & args.keys()):
            print(__doc__, file=sys.stderr)
            return 2
        return serve(args["--socket"], unit=args.get("--unit"), cgroup=args.get("--cgroup"))
    if argv[:1] == ["setpw"]:
        return setpw()
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
