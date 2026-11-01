#!/usr/bin/env python3
"""Offline characterization test of the input relay's control socket protocol
(Relay.control_command): for each datagram, which of the relay's parts it reaches, with what,
what it replies, and which state it changes, from this user or another, from a bound address
or an unbound one.

The expected results are in control-golden.json, recorded from the relay before its dispatch
was split into one handler per command (docs/refactor/PROGRESS.md). A change that means to
change the protocol records them again with --record, and the diff of that file shows what
changed. Each case runs on a new Relay whose parts are recorders: no devices, sockets the relay
serves, services or the live relay.

  input/test/control-test.py [--record] [RELAY]   (default: input/input-relay.py next to this folder)
"""
import importlib.util
import json
import os
import socket
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "control-golden.json")
args = sys.argv[1:]
record = "--record" in args
args = [a for a in args if a != "--record"]
relay_path = args[0] if args else os.path.join(HERE, "..", "input-relay.py")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))  # its game_pause and vrws
with open(relay_path) as f:
    exec(compile(f.read(), relay_path, "exec"), relay.__dict__)
gp = relay.game_pause

UID = os.getuid()
OTHER = UID + 1
NOW = 100.0
ADDR = b"\0ft_control_test_client"


class Stub:
    """Anything the relay may start in the background: does nothing."""

    connected = False
    busy = False
    keep = ()
    hands_with_desktop = False

    def __init__(self, *args, **kw):
        self.stopped = {"units": [], "remote": False, "desktop": None}

    def __getattr__(self, name):
        return lambda *a, **k: None

    def snapshot(self):
        return dict(self.stopped)


class FakeVirtual:
    def release_all(self):
        pass

    def emit(self, etype, code, value):
        pass

    def sync(self):
        pass


tag = "ft_control_test"  # fixed: the names are in the recorded results (nothing binds them)
for name in ("SCREENS", "HELPER", "FLOAT", "GAZED", "VOICE", "FRUNNER"):
    setattr(relay, name, f"\0{tag}_{name.lower()}")
OUT = tempfile.mkdtemp(prefix="ft-control-test-")
gp.STATE_PATH = os.path.join(OUT, "pause.json")
gp.SCREENS, gp.HELPER = relay.SCREENS, relay.HELPER
gp.PARK_PATH = os.path.join(OUT, "frametop-hands", "park")
gp.SOUNDS = {True: os.path.join(OUT, "no-pause.oga"), False: os.path.join(OUT, "no-resume.oga")}
gp.Worker, gp.ControllerWatch = Stub, Stub
LOG = []
relay.log = lambda *a: LOG.append(" ".join(map(str, a)))
relay.read_config = lambda path=None: {}
real_read_rules = relay.read_rules
relay.read_rules = lambda path=None: real_read_rules(os.path.join(OUT, "no-rules.json"))
control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)


def plain(v):
    """v as JSON keeps it: bytes as text, tuples and deques as lists, sets sorted."""
    if isinstance(v, bytes):
        return v.decode(errors="replace")
    if isinstance(v, dict):
        return {str(plain(k)): plain(x) for k, x in v.items()}
    if isinstance(v, (set, frozenset)):
        return sorted(plain(x) for x in v)
    if isinstance(v, (list, tuple)) or type(v).__name__ == "deque":
        return [plain(x) for x in v]
    if isinstance(v, float):
        return round(v, 6)
    return v


class Recorder:
    """A part of the relay (its pause, its gamepad mode, its screens socket) that records each
    call: "part.method(args)". returns: what a method gives back, by name."""

    def __init__(self, name, calls, returns=None, **attrs):
        self._name, self._calls, self._returns = name, calls, returns or {}
        self.__dict__.update(attrs)

    def __getattr__(self, method):
        if method.startswith("_"):
            raise AttributeError(method)

        def call(*a, **k):
            self._calls.append([f"{self._name}.{method}", plain(list(a)), plain(k)])
            return self._returns.get(method)
        return call


# The relay's own methods a command hands off to: recorded, not run.
HANDOFFS = ("vr_button", "vr_bind", "text_field", "host_message", "type_text", "kb_tell", "kb_gone", "reply",
            "load_config", "apply_roles")


def new_relay():
    r = relay.Relay(FakeVirtual(), FakeVirtual(), control, screens_sock, can_grab=False, stored={})
    calls = []
    r.pause = Recorder("pause", calls, returns={"status": {"t": "pause", "paused": False}}, paused=False)
    r.gamepad = Recorder("gamepad", calls)
    r.screens_sock = Recorder("screens_sock", calls)
    for name in HANDOFFS:
        setattr(r, name, (lambda n: lambda *a, **k: calls.append([n, plain(list(a)), plain(k)]))(name))
    r.lease.conf = None
    sent = []
    pointer = types.SimpleNamespace(active=False, last_used=0.0, gaze_awake_until=0.0,
                                    send=lambda *a: sent.append(plain(list(a))))
    r.lease.current = pointer
    return r, calls, pointer, sent


def state(r, pointer, sent):
    return {
        "desktop_until": r.desktop_until, "shown_until": r.shown_until,
        "text_input": plain(r.text_input), "text_input_until": r.text_input_until,
        "calpanel_until": r.calpanel_until, "vr_capture_until": r.vr_capture_until,
        "watchers": plain(r.watchers),
        "kb": [plain(r.kb.addr), r.kb.until, r.kb.said], "typing_queue": plain(r.typing.queue),
        "pointer": [pointer.active, pointer.last_used, pointer.gaze_awake_until, sent],
    }


def run(text, uid=UID, addr=ADDR, kb_addr=None, paused=False, no_pointer=False):
    """One datagram, text, on a new relay: what it did."""
    r, calls, pointer, sent = new_relay()
    r.pause.paused = paused
    if kb_addr is not None:
        r.kb.addr = kb_addr
    if no_pointer:
        r.lease.current = None
    before = state(r, pointer, sent)
    data = text.encode()
    words = data.decode(errors="replace").split()
    cmd = words[0] if words else ""
    LOG.clear()
    try:
        r.control_command(cmd, words, data, addr, 4242, uid, NOW)
        raised = None
    except Exception as e:  # handle_control logs it and goes on; the type is what's kept
        raised = type(e).__name__
    after = state(r, pointer, sent)
    return {"calls": calls, "changed": {k: v for k, v in after.items() if before[k] != v},
            "log": list(LOG), "raised": raised}


# (label, datagram, options): every command and its main branches.
CASES = [("empty", "", {}), ("unknown, bound", "frobnicate 1", {}), ("unknown, unbound", "frobnicate 1", {"addr": b""})]
for w in ("keyboard desktop 1 0", "keyboard desktop 1 1", "keyboard steam 0", "keyboard desktop", "keyboard"):
    CASES += [(w, w, {}), (w + ", other user", w, {"uid": OTHER}), (w + ", paused", w, {"paused": True})]
CASES += [
    ("vrbtn ok", "vrbtn left/view 1", {}), ("vrbtn release", "vrbtn right/a 0", {}),
    ("vrbtn bad button", "vrbtn left/nose 1", {}), ("vrbtn bad value", "vrbtn left/view 2", {}),
    ("vrbtn short", "vrbtn left/view", {}),
    ("vrhello", "vrhello", {}), ("vrhello unbound", "vrhello", {"addr": b""}),
    ("vrgame 1", "vrgame 1", {}), ("vrgame 0", "vrgame 0", {}), ("vrgame bare", "vrgame", {}),
    ("vrdash 1", "vrdash 1", {}), ("vrdash 0", "vrdash 0", {}), ("vrdash long", "vrdash 1 2", {}),
    ("gamepad desktop", "gamepad desktop", {}), ("gamepad off", "gamepad off", {}),
    ("gamepad remote, other user", "gamepad remote", {"uid": OTHER}), ("gamepad bad", "gamepad nope", {}),
    ("vrpad ok", "vrpad 1 0 0 0 0 0 0", {}), ("vrpad other user", "vrpad 1 0 0 0 0 0 0", {"uid": OTHER}),
    ("vrpad junk", "vrpad a b c d e f g", {}), ("vrpad short", "vrpad 1 0", {}),
    ("pause on", "pause on", {}), ("pause off gesture", "pause off gesture", {}),
    ("pause toggle", "pause toggle", {}), ("pause ?", "pause ?", {}), ("pause ? unbound", "pause ?", {"addr": b""}),
    ("pause on, other user", "pause on", {"uid": OTHER}),
    ("pause enforce", "pause enforce", {}), ("pause defer remote", "pause defer remote", {}),
    ("pause enforce, other user", "pause enforce", {"uid": OTHER}),
    ("pause enforce unbound", "pause enforce", {"addr": b""}), ("pause bare", "pause", {}),
    ("pause bad", "pause sideways", {}),
    ("textfield 1", "textfield 1", {}), ("textfield 0", "textfield 0", {}),
    ("textfield 1 renew", "textfield 1 renew", {}), ("textfield 1 unbound", "textfield 1", {"addr": b""}),
    ("textfield 1, other user", "textfield 1", {"uid": OTHER}), ("textfield bad", "textfield 2", {}),
    ("hostdev ok", "hostdev pc.1 mouse x", {}), ("hostev keyboard", "hostev pc-2 keyboard 1 30 1", {}),
    ("hostbye", "hostbye pc mouse", {}), ("hostdev bad id", "hostdev pc/1 mouse", {}),
    ("hostdev bad kind", "hostdev pc pad", {}), ("hostdev other user", "hostdev pc mouse", {"uid": OTHER}),
    ("type", "type hello there", {}), ("type other user", "type hi", {"uid": OTHER}),
    ("kbhello first", "kbhello 1", {}), ("kbhello closed", "kbhello 0", {}),
    ("kbhello restarted", "kbhello", {"kb_addr": b"\0old_kb"}), ("kbhello same", "kbhello 1", {"kb_addr": ADDR}),
    ("kbhello unbound", "kbhello 1", {"addr": b""}), ("kbhello other user", "kbhello 1", {"uid": OTHER}),
    ("kbkey", "kbkey 30 1", {"kb_addr": ADDR}), ("kbkey not hello'd", "kbkey 30 1", {}),
    ("kbkey big", "kbkey 999 1", {"kb_addr": ADDR}), ("kbkey bad value", "kbkey 30 2", {"kb_addr": ADDR}),
    ("kbtext", "kbtext héllo  wörld", {"kb_addr": ADDR}), ("kbtext other user", "kbtext x", {"kb_addr": ADDR, "uid": OTHER}),
    ("kbpanel show", "kbpanel show", {}), ("kbpanel hide other", "kbpanel hide", {"uid": OTHER}),
    ("kbpanel bad", "kbpanel spin", {}),
    ("kbpoke hover", "kbpoke left hover", {}), ("kbpoke press xyz", "kbpoke right press 0.1 -0.25 1.5", {}),
    ("kbpoke bad coord", "kbpoke right press 0.1 x 1", {}), ("kbpoke bad side", "kbpoke up hover", {}),
    ("kbpoke other user", "kbpoke left hover", {"uid": OTHER}),
    ("calpanel 1", "calpanel 1", {}), ("calpanel 0", "calpanel 0", {}), ("calpanel other", "calpanel 1", {"uid": OTHER}),
    ("gazeawake 1", "gazeawake 1", {}), ("gazeawake 0", "gazeawake 0", {}),
    ("gazeawake no pointer", "gazeawake 1", {"no_pointer": True}),
    ("gazewoke", "gazewoke", {}), ("gazewoke no pointer", "gazewoke", {"no_pointer": True}),
    ("devices", "devices", {}), ("devices unbound", "devices", {"addr": b""}),
    ("watch", "watch", {}), ("watch 5", "watch 5", {}), ("watch long", "watch 9999", {}), ("watch junk", "watch x", {}),
    ("reload", "reload", {}), ("reload no pointer", "reload", {"no_pointer": True}),
    ("vrcapture", "vrcapture", {}), ("vrcapture 0", "vrcapture 0", {}), ("vrcapture long", "vrcapture 500", {}),
]


def results():
    out = {}
    for label, text, opts in CASES:
        assert label not in out, label
        out[label] = {"datagram": text, **{k: plain(v) for k, v in opts.items()}, **run(text, **opts)}
    return out


got = results()
if record:
    with open(GOLDEN, "w") as f:
        json.dump(got, f, indent=1, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    print(f"recorded {len(got)} cases in {GOLDEN}")
    sys.exit(0)
with open(GOLDEN) as f:
    want = json.load(f)
failures = []
for label in sorted(set(want) | set(got)):
    w, g = want.get(label), json.loads(json.dumps(got.get(label)))
    ok = w == g
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f"\n  got  {g}\n  want {w}"), flush=True)
    if not ok:
        failures.append(label)
print(f"{len(got) - len(failures)} of {len(set(want) | set(got))} cases as recorded")

# The sender's pid and uid, from the kernel's credentials in a datagram's ancillary data.
peer_creds = getattr(relay, "peer_creds", None)
if peer_creds:
    for label, anc, want_creds in (
            ("credentials", [(socket.SOL_SOCKET, socket.SCM_CREDENTIALS, relay.CREDS.pack(41, 1000, 1000))], (41, 1000)),
            ("no ancillary data", [], (None, None)),
            ("other ancillary data only", [(socket.SOL_SOCKET, socket.SCM_RIGHTS, b"\0" * 4)], (None, None))):
        ok = peer_creds(anc) == want_creds
        print(("ok    " if ok else "FAIL  ") + "peer_creds: " + label, flush=True)
        if not ok:
            failures.append("peer_creds: " + label)
sys.exit(1 if failures else 0)
