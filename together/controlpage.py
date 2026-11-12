"""The room's panels on Frametop Settings' Together page, without Qt: ft-together's `state` and
`control` answers read into rows. Who asks to use which of this headset's screens, who was
given one, and every panel in the room with what its viewer can do about it.

A person's name and a panel's are text a peer chose: they are cut to one short line here, and
the page shows them as plain text. An ask or a grant that names nobody in the room, or no panel
of this headset's, is no row.

A screen that is a window of the wearer's PC is given only after a second ask (docs/together.md,
"Control and moving"): whoever has it clicks and types on that PC.
"""

PC_PANELS = range(3, 7)   # a headset's panel numbers that are windows of its person's PC (room/panels.h)


def _int(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else -1


def _line(text, otherwise):
    return " ".join(str(text or "").split())[:48] or otherwise


def _rows(found):
    return [row for row in found if isinstance(row, dict)] if isinstance(found, list) else []


def _people(state):
    """{a place: the name of who is in it}."""
    return {_int(p.get("id")): _line(p.get("name"), f"Person {_int(p.get('id'))}") for p in _rows(state.get("people"))}


def _pairs(state, pairs):
    """(a panel's number, a place, "NAME ... your SCREEN" as its two halves) for each pair that
    names someone else in the room and a panel this headset shares."""
    who, me = _people(state), _int(state.get("me"))
    mine = {_int(p.get("number")): _line(p.get("name"), f"Panel {_int(p.get('id'))}") for p in _rows(state.get("panels")) if _int(p.get("owner")) == me}
    found = []
    for pair in _rows(pairs):
        number, person = _int(pair.get("number")), _int(pair.get("person"))
        if number in mine and person in who and person != me:
            found.append((number, person, who[person], mine[number]))
    return found


def pc(number):
    """Whether a panel of this headset's with that number is a window of its person's PC."""
    return number in PC_PANELS


def asks(state, control, sure=()):
    """Those who ask to use a screen of this headset's: {"number", "person", "text", "second"}.
    `second`: what the page asks before a window of the PC's is given, for the (number, person)
    in `sure`, whose Give it was pressed once; "" otherwise."""
    return [{"number": number, "person": person, "text": f"{name} asks to use your {panel}",
             "second": f"{panel} is a window of your PC. {name} could click and type on that PC, and paste what is on its clipboard. Give it?"
             if pc(number) and (number, person) in sure else ""}
            for number, person, name, panel in _pairs(state, control.get("asks"))]


def given(state, control):
    """Those who were given one: {"number", "person", "text"}."""
    return [{"number": number, "person": person, "text": f"{name} can use your {panel}"}
            for number, person, name, panel in _pairs(state, control.get("given"))]


def note(state, control):
    """Why a screen that is asked for can't be given now, or ""."""
    if control.get("there") is True or not asks(state, control):
        return ""
    return "A screen is given only while you wear the headset, and nothing says you do right now"


def panels(state, control):
    """Every panel in the room: {"id", "title", "mine", "what", "ask"}. `what`: where its viewer
    stands with a panel of someone else's; `ask`: whether asking for it is of any use."""
    who, me = _people(state), _int(state.get("me"))
    held = {_int(h.get("panel")) for h in _rows(control.get("held"))}
    asked = {_int(a) for a in control.get("asked")} if isinstance(control.get("asked"), list) else set()
    found = []
    for p in _rows(state.get("panels")):
        ident, owner = _int(p.get("id")), _int(p.get("owner"))
        if ident < 0:
            continue
        name, mine = _line(p.get("name"), f"Panel {ident}"), owner == me
        what = "" if mine else "you can use it" if ident in held else "asked…" if ident in asked else ""
        found.append({"id": ident, "title": f"Your {name}" if mine else f"{who.get(owner, 'Someone')}'s {name}", "mine": mine, "what": what,
                      "ask": not mine and what == ""})
    return found
