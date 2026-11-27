"""The parts the Frametop Installer offers as checkboxes, and the get.sh options they turn into.

Every part is ticked by default. The install runs get.sh --release --yes, so a part that --yes
installs by itself is left out with its --no- option, and a part --yes leaves out (Frametop
Together, the Bluetooth fixes) is put in with its own option. A release takes only the options
its install-release.sh knows: frametop-release.json lists them (install_options, from
pack/release-info.py); older releases don't, and supported_options() works them out instead.
installer/tests/test_options.py checks this table against install.sh, get.sh and
install-release.sh, so it can't drift from them.
"""
import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Part:
    key: str
    title: str
    detail: str
    off: str = ""          # the option that leaves it out
    on: str = ""           # the option that puts it in (a part --yes leaves out)
    sudo: bool = False     # needs your password
    needs: str = ""        # the part it needs (unticked, this one goes too)
    added: bool = False    # releases before it don't have it: absent where off isn't taken


PARTS = (
    Part("voice", "Voice dictation",
         "A Right Ctrl tap or holding the left d-pad down, and what you say is typed. Nothing leaves "
         "the headset. Downloads about 870 MB of speech models.", off="--no-voice"),
    Part("hands", "Hand tracking (experimental)",
         "Poke, pinch and grab the screens with your hands. It uses a lot of the headset's CPU.",
         off="--no-hands", sudo=True),
    Part("npu", "NPU hand tracking (recommended)",
         "Hand tracking's model runs on the headset's NPU, so it needs less of the CPU, which VR "
         "needs. Hand tracking uses the CPU whenever it can't. A small system service.",
         off="--no-npu", sudo=True, needs="hands", added=True),
    Part("hand_recorder", "Hand Recorder",
         "Records your hands for Frametop's open hand dataset, which trains hand tracking. Nothing "
         "leaves the headset until you press Upload.", off="--no-hand-recorder", sudo=True, needs="hands"),
    Part("gaze", "Gaze mode (experimental)",
         "The pointer goes where you look. Turn it on and calibrate it on the Eye tracking page of Frametop "
         "Settings.", off="--no-gaze"),
    Part("eye_tracker", "Our own eye tracker for gaze mode",
         "More accurate than SteamVR's. Its frame grabber is a small system service.",
         off="--no-eye-tracker", sudo=True, needs="gaze"),
    Part("eye_recorder", "Eye Recorder",
         "Records your eyes for Frametop's eye tracking dataset. Nothing leaves the headset until "
         "you press Upload, and uploads are encrypted.", off="--no-eye-recorder", needs="eye_tracker"),
    Part("keyboard", "Frametop's keyboard (Full Keyboard)",
         "The keyboard Frametop's screens open for text fields. Untick it only to keep a Full "
         "Keyboard you installed yourself.", off="--no-keyboard"),
    Part("together", "Frametop Together (experimental)",
         "See each other, talk, and share screens with up to three other Steam Frames. It needs "
         "Tailscale, and stays off until you turn it on in Frametop Settings.",
         on="--together", off="--no-together"),
    Part("bluetooth", "Bluetooth fixes",
         "Lets Bluetooth LE mice and keyboards, like the Swiftpoint Z3, reconnect after they sleep. "
         "System files and a system service.", on="--bluetooth", off="--no-bluetooth", sudo=True),
)
BY_KEY = {p.key: p for p in PARTS}

# What every release since 0.3.0 takes (0.3.0's install-release.sh): the fallback when neither
# frametop-release.json nor the release's own install-release.sh says.
BASELINE = ("--no-voice", "--no-hands", "--no-hand-recorder", "--no-eye-tracker", "--no-eye-recorder",
            "--no-bluetooth", "--bluetooth")
# install-release.sh's line that passes options on, before it kept them in INSTALL_OPTIONS
_OLD_CASE = re.compile(r"^\s*((?:--[a-z-]+\|)*--[a-z-]+)\)\s*pass\+=", re.M)
_NEW_LIST = re.compile(r"^INSTALL_OPTIONS=\(([^)]*)\)", re.M)


def options_from_script(text):
    """The options an install-release.sh passes to install.sh, read from its source, or None."""
    m = _NEW_LIST.search(text)
    if m:
        return tuple(m[1].split())
    for m in _OLD_CASE.finditer(text):
        opts = tuple(m[1].split("|"))
        if "--no-voice" in opts:
            return opts
    return None


def supported_options(release_json, script_text=None):
    """The install options a release takes: its frametop-release.json's list, else what its
    install-release.sh's source shows, else BASELINE."""
    listed = release_json.get("install_options") if isinstance(release_json, dict) else None
    if isinstance(listed, list) and all(isinstance(o, str) for o in listed):
        return tuple(listed)
    if script_text:
        found = options_from_script(script_text)
        if found:
            return found
    return BASELINE


def availability(part, supported):
    """'choice' (a checkbox), 'always' (the release installs it, and can't leave it out), or
    'absent' (the release doesn't have it)."""
    if part.on:
        return "choice" if part.on in supported and (not part.off or part.off in supported) else "absent"
    if part.off in supported:
        return "choice"
    return "absent" if part.added else "always"


def effective(picked, supported):
    """Picked parts with the ones whose part they need left out, and 'always' parts in."""
    out = {}
    for part in PARTS:
        kind = availability(part, supported)
        if kind == "absent":
            on = False
        elif kind == "always":
            on = True
        else:
            on = bool(picked.get(part.key, True))
        if part.needs and not out[part.needs]:
            on = False   # install.sh leaves it out with the part it needs
        out[part.key] = on
    return out


def boxes(wanted, supported):
    """What the box of each part that needs another listed part shows, {key: (ticked, can_change)}:
    unticked and locked while the part it needs is off, and what was picked for it once that's on."""
    on = effective(wanted, supported)
    out = {}
    for part in PARTS:
        if (part.needs and availability(part, supported) != "absent"
                and availability(BY_KEY[part.needs], supported) != "absent"):
            parent_on = on[part.needs]
            out[part.key] = (bool(wanted.get(part.key, True)) and parent_on, parent_on)
    return out


def install_flags(picked, supported):
    """get.sh's options for these picks: --yes, then a --no- for each part left out and an
    option for each part --yes would leave out."""
    flags = ["--yes"]
    on = effective(picked, supported)
    for part in PARTS:
        kind = availability(part, supported)
        if kind != "choice":
            continue
        if on[part.key]:
            if part.on:
                flags.append(part.on)
        else:
            flags.append(part.off)
    return flags


def needs_password(picked, supported):
    return any(on and BY_KEY[key].sudo for key, on in effective(picked, supported).items())
