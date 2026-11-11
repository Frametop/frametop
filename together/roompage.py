"""What Frametop Settings' Together page shows and sends, without Qt: the programs' answers read
into what the page says (ft-together's `state` and `here`, ft-roomvoice's and ft-roompanels'
`state`, ft-screens' `screens`), and what its person typed read into a question.

Everything read here is text another program sent, and some of it text a peer chose (names,
logins): what doesn't parse is nothing, and the page shows names as plain text.
"""
import json
import re

PORT = 47193   # ft-together's own, unless it was started with another
SHARES = 3     # the screens a Frame shares at most (roompanels/keeper.py)
AWAY = {0: "", 1: "in a game", 2: "headset off", 3: "headset off"}
HOST = re.compile(r"[A-Za-z0-9]([A-Za-z0-9._-]{0,251}[A-Za-z0-9.])?")
CODE = re.compile(r"[A-Za-z0-9-]{4,32}")


def told(reply):
    """The JSON after "ok ", or None."""
    if not reply or not reply.startswith("ok {"):
        return None
    try:
        got = json.loads(reply[3:])
    except ValueError:
        return None
    return got if isinstance(got, dict) else None


def why_not(reply, otherwise="No answer"):
    """What a refusal said, for its person: the text after "error "."""
    if reply and reply.startswith("error "):
        text = reply[6:].strip()
        return text[:1].upper() + text[1:]
    return otherwise


def here(reply):
    """ft-together's `here`: {"address", "port", "name"}, or None."""
    words = (reply or "").split(None, 3)
    if len(words) != 4 or words[0] != "ok" or not words[2].isdigit():
        return None
    return {"address": words[1], "port": int(words[2]), "name": words[3]}


def here_text(at):
    """This headset's address as its person reads it out to a guest."""
    if not at:
        return ""
    return at["address"] if at["port"] == PORT else f"{at['address']}:{at['port']}"


def code(reply):
    """An invite's code from `invite`'s answer, or ""."""
    words = (reply or "").split()
    return words[1] if len(words) == 2 and words[0] == "ok" and CODE.fullmatch(words[1]) else ""


def join_words(address, typed_code):
    """The `join` for what was typed: an address as "host", "host:port" or "host port", and the
    code. (question, "") or (None, what is wrong)."""
    words = address.replace(",", " ").split()
    if len(words) == 1 and words[0].count(":") == 1:
        words = words[0].split(":")
    if not words or len(words) > 2 or not HOST.fullmatch(words[0]):
        return None, "Type the address of their headset: its tailnet name or number"
    port = PORT
    if len(words) == 2:
        if not words[1].isdigit() or not 1 <= int(words[1]) <= 65535:
            return None, "The port after the address is a number up to 65535"
        port = int(words[1])
    typed_code = "".join(typed_code.split())
    if not CODE.fullmatch(typed_code):
        return None, "Type the invite code they gave you"
    return f"join {words[0]} {port} {typed_code}", ""


def screens(reply):
    """ft-screens' `screens`: [(number, "WxH")], or [] when it didn't say."""
    words = (reply or "").split()
    if len(words) < 2 or words[0] != "ok":
        return []
    found = []
    for word in words[2:]:
        parts = word.split(":")
        if len(parts) < 2 or not parts[0].isdigit() or not re.fullmatch(r"\d+x\d+", parts[1]):
            return []
        found.append((int(parts[0]), parts[1]))
    return found


def status(installed, on, reply, why):
    """(what the switch says, whether ft-together answers) from its `state` answer; `why` is
    ftipc's reason when there was none."""
    if told(reply) is not None:
        return "On", True
    if reply is not None:
        return why_not(reply, "On"), False   # "Tailscale isn't running on this headset"
    if not installed:
        return "Not installed", False
    if not on:
        return "Off", False
    return ("Starting…" if why == "nobody" else "Not answering"), False


def room_text(state):
    """One line on the room this headset is in."""
    mode, people = state.get("mode"), state.get("people") or []
    if mode == "hosting":
        others = len(people) - 1
        return "Your room is open: nobody else is in it yet" if others < 1 else f"Your room is open, with {others} other{'s' if others > 1 else ''} in it"
    if mode == "calling":
        return "Asking to come in…"
    if mode == "joined":
        owner = next((p.get("name") for p in people if p.get("id") == state.get("owner")), None)
        return f"You are in {owner}'s room" if owner else "You are in a room"
    return "You are in no room"


def people(state):
    """The room's people as rows: {"id", "name", "login", "what"}."""
    rows = []
    for p in state.get("people") or []:
        if not isinstance(p, dict):
            continue
        what = [w for w in ("you" if p.get("id") == state.get("me") else "", "the room's owner" if p.get("id") == state.get("owner") else "",
                            AWAY.get(p.get("away"), "away"),
                            "" if p.get("linked") or p.get("id") == state.get("me") else "connecting") if w]
        rows.append({"id": int(p.get("id", -1)), "name": str(p.get("name", "")), "login": str(p.get("login", "")), "what": ", ".join(what)})
    return rows


def asks(state):
    """Those who ask to come in: {"id", "name", "login", "address"}."""
    return [{"id": int(a.get("id", 0)), "name": str(a.get("name", "")), "login": str(a.get("login", "")), "address": str(a.get("address", ""))}
            for a in state.get("asks") or [] if isinstance(a, dict)]


def shares(found, panels):
    """A row per screen of the desktop: {"screen", "title", "shared", "live", "can"}; `found` is
    screens(), `panels` ft-roompanels' state (or None when it doesn't answer)."""
    mine = {s.get("screen"): bool(s.get("told")) for s in (panels or {}).get("shares") or [] if isinstance(s, dict)}
    full = len(mine) >= SHARES
    return [{"screen": n, "title": f"Screen {n} ({size.replace('x', ' × ')})", "shared": n in mine, "live": mine.get(n, False),
             "can": panels is not None and (n in mine or not full)} for n, size in found]
