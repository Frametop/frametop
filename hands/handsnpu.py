#!/usr/bin/env python3
"""handsnpu: where hand tracking's landmark model runs, as one choice (Frametop Settings' Hands page).

ft-hands asks ft-npud, the root service that runs the NPU student on the headset's DSP, for its
landmarks, and runs the CPU model itself whenever that service isn't there or stops answering
(hands/track/npulandmarks.h). HANDS_NPU in ~/.config/frametop.conf says which, read as ft-hands
reads it (npu_dir_setting in hands/track/npuserver.h): not set, auto or 1 is the NPU at
/run/ft-npud; 0 is the CPU only; anything else is the service's folder.

The service is there when its heads file is: /run/ft-npud/ft-npud.heads on the host, the same
under /run/host from the release's container (where Frametop Settings runs).

ft-hands reads the setting as it starts. --now restarts it if it runs: the hands are gone for a
second or two.

  python3 hands/handsnpu.py status [--json]   # the choice, and whether the NPU service is there
  python3 hands/handsnpu.py npu|cpu [--now]   # write the choice, and apply it now
                                              # (exit 1: not written, or not applied; it says which)

On the host, system Python, standard library only (the settings app runs it through
distrobox-host-exec and imports it to read the state).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "conf"))
sys.path.insert(0, HERE)
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import handsmode  # noqa: E402  (frametop.conf's path)

KEY = "HANDS_NPU"
CONF_PATH = handsmode.CONF_PATH
DEFAULT_DIR = "/run/ft-npud"   # kNpuDefaultDir in hands/track/npuserver.h
HEADS = "ft-npud.heads"
ROOTS = ("", "/run/host")      # the host's / from the host, and from the container


class NpuError(RuntimeError):
    """The choice couldn't be written or applied; its text says which, for the user."""


def folder(value):
    """HANDS_NPU's value as ft-npud's folder, "" for the CPU only: npu_dir_setting's rule."""
    if value in ("", "auto", "1"):
        return DEFAULT_DIR
    return value if value.startswith("/") else ""


def service_there(where=DEFAULT_DIR, roots=None):
    """Whether ft-npud runs with its folder at WHERE: its heads file, as ft-hands looks for it
    (under each of ROOTS, or the roots given)."""
    roots = ROOTS if roots is None else roots
    return any(os.path.lexists(root + where + "/" + HEADS) for root in roots)


def state(conf=None, conf_path=CONF_PATH, roots=None):
    """{"npu": the choice, "available": the NPU service is there, "note": why the choice isn't what
    runs}. Reads only."""
    conf = ft_conf.read(conf_path) if conf is None else conf
    where = folder(conf.get(KEY, ""))
    available = service_there(where or DEFAULT_DIR, roots)
    note = ""
    if where and not available:
        note = ("The NPU service isn't running on this headset, so hand tracking uses the CPU. "
                "It switches to the NPU by itself once the service runs.")
        if where != DEFAULT_DIR:
            note += f" (HANDS_NPU looks for it in {where}.)"
    return {"npu": bool(where), "available": available, "note": note}


def switch(npu, restart=None, conf_path=CONF_PATH):
    """Write the choice; one that already reads so is left as it is (a folder of its own is kept
    for the NPU, and nothing is written where it isn't set). With `restart` (which returns "" or
    what went wrong), apply it now. The text to show; NpuError when it wasn't written or applied."""
    try:
        if bool(folder(ft_conf.read(conf_path).get(KEY, ""))) != bool(npu):
            ft_conf.set_value(KEY, "auto" if npu else "0", conf_path)
    except OSError as e:
        raise NpuError(f"{conf_path} couldn't be written ({e})") from None
    said = "Hand tracking uses the NPU when it's there." if npu else "Hand tracking uses the CPU."
    if restart is None:
        return said + " It counts from hand tracking's next start."
    problem = restart()
    if problem:
        raise NpuError(f"The setting is written, but ft-hands couldn't be restarted ({problem}): "
                       "turn hand tracking off and on to apply it")
    return said


def main(argv, conf_path=CONF_PATH, roots=None, restart=None):
    cmd = argv[0] if argv else "status"
    if cmd == "status" and argv[1:] in ([], ["--json"]):
        st = state(None, conf_path, roots)
        if "--json" in argv:
            print(json.dumps(st))
        else:
            print("hand tracking: " + ("the NPU" if st["npu"] else "the CPU")
                  + ("" if not st["npu"] else ", its service running" if st["available"] else ", its service not running"))
            if st["note"]:
                print(st["note"])
        return 0
    if cmd in ("npu", "cpu") and argv[1:] in ([], ["--now"]):
        if "--now" in argv and restart is None:
            import handsrate  # (its Host restarts ft-hands on the user manager's bus)
            restart = handsrate.Host().restart_hands
        try:
            print(switch(cmd == "npu", restart if "--now" in argv else None, conf_path))
        except NpuError as e:
            print(f"handsnpu: {e}", file=sys.stderr)
            return 1
        return 0
    print("usage: handsnpu.py status [--json] | npu [--now] | cpu [--now]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
