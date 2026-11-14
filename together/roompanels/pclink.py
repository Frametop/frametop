"""Its person's PC, kept sending the windows they share with the room (docs/together.md, "A PC's
panels"). The PC's streaming host has four questions for it (the Vibepollo fork's
/api/frametop/together): this asks them.

  - While windows are wanted and ft-together says no PC is here, the PC is called in: it is
    handed where this Frame takes calls ("here") and a one-time code ("sender"), and calls.
  - The PC is told which of its windows are its panels, each by the window's address. Its answer
    lists the ones that stand and the size it encodes each at.
  - A PC that says it has no call (409) while ft-together has a PC's link isn't the one that
    is here: it is called in, and takes the other's place.
  - A wanted window that isn't a panel there two looks in a row is gone (it closed, or the PC
    took it back): whoever wanted it takes that up (`take_gone`).
  - When nothing is wanted any more, the PC is told to end its call.

The rules only. `together(text)` asks ft-together; `api(pc, path, body)` is one POST to the PC
`pc` (its address and its windows instance's port), (status, answer) back, status 0 when it
didn't answer. `step` waits on the PC, so it runs on a thread of its own (ft-roompanels has
it); what it shares with the keeper's thread is behind a lock.

The code never goes anywhere but into the question to the PC.
"""
import json
import threading

STEP = 2.0          # seconds between looks at the PC
CALL_EVERY = 6.0    # ... between calls, at the least: a call takes a moment to stand, and each uses up a code
GONE_AFTER = 2      # looks in a row that a wanted window is no panel on the PC
FIRST, LAST = 3, 6  # a PC's panels (together/proto/sender.h)
SIDES = range(128, 4097)   # a panel's video, each side (together/proto/panel.h)
PANELS, SENDER, STOP = "/api/frametop/together/panels", "/api/frametop/together/sender", "/api/frametop/together/stop"
NO_CALL = 409       # what a PC with no call to a headset answers when told its panels
_ADDRESS = set("0123456789abcdefABCDEF.:")


def _here(reply):
    """(address, port) of ft-together's "here", or None."""
    words = (reply or "").split()
    if len(words) < 3 or words[0] != "ok" or not 2 <= len(words[1]) <= 45 or not set(words[1]) <= _ADDRESS:
        return None
    if not words[2].isdigit() or not 0 < int(words[2]) < 65536:
        return None
    return words[1], int(words[2])


def _code(reply):
    """The code of ft-together's "sender", or None."""
    words = (reply or "").split()
    return words[1] if len(words) == 2 and words[0] == "ok" and len(words[1]) == 8 and words[1].isalnum() and words[1].isascii() else None


def _why(status, answer):
    """What went wrong with a question to the PC, in a line for the state."""
    if not status:
        return "the PC doesn't answer"
    said = answer.get("error") if isinstance(answer, dict) else None
    said = "".join(c for c in said if c.isprintable())[:120] if isinstance(said, str) else ""
    return f"the PC says: {said}" if said else f"the PC answers {status}"


class PcLink:
    def __init__(self, together, api, now):
        self.together, self.api, self.now = together, api, now
        self._lock = threading.Lock()
        self._wanted = {}    # a panel's number -> its Window (pcwindows.py)
        self._sizes = {}     # ... -> (width, height) the PC encodes it at
        self._missing = {}   # ... -> looks in a row it was no panel there
        self._gone = set()
        self._why = ""
        self._pc = None          # the PC that sends, or was called to
        self._called_at = None
        self._uncalled = None    # the PC that said it has no call

    # ---- the keeper's thread ----

    def want(self, wanted):
        """The windows that are to be panels from now on: {a panel's number: its Window}."""
        with self._lock:
            for number in [n for n in self._sizes if wanted.get(n) != self._wanted.get(n)]:
                del self._sizes[number]
            self._missing = {n: m for n, m in self._missing.items() if wanted.get(n) == self._wanted.get(n)}
            self._gone &= {n for n in wanted if wanted[n] == self._wanted.get(n)}
            self._wanted = dict(wanted)

    def sizes(self):
        """{a panel's number: the size the PC encodes it at}, for the ones that stand there."""
        with self._lock:
            return dict(self._sizes)

    def take_gone(self):
        """The wanted panels whose windows went from the PC since this was last asked."""
        with self._lock:
            gone, self._gone = self._gone, set()
            return gone

    def why(self):
        """What keeps the PC from sending, or "" when nothing does."""
        with self._lock:
            return self._why

    # ---- its own thread ----

    def step(self):
        with self._lock:
            wanted = dict(self._wanted)
        pcs = {(w.address, w.port) for w in wanted.values()}
        pc = pcs.pop() if len(pcs) == 1 else None
        if self._pc is not None and self._pc != pc:
            self.api(self._pc, STOP, {})   # nothing of that PC's is shared any more
            self._pc = self._called_at = None
        if pc is None:
            self._say("")
            return
        here = self._pc_here()
        if here is None:
            return   # ft-together says nothing this time
        if (not here or self._uncalled == pc) and not self._call(pc):
            return
        body = {"panels": [{"number": n, "address": w.window} for n, w in wanted.items()]}
        status, answer = self.api(pc, PANELS, body)
        self._uncalled = pc if status == NO_CALL else None   # (the PC that is here is another, or its host started again)
        if status != 200 or not isinstance(answer, dict) or not isinstance(answer.get("panels"), list):
            self._say(_why(status, answer))
            return
        self._pc = pc
        self._took(wanted, answer["panels"])

    def _say(self, why):
        with self._lock:
            self._why = why

    def _pc_here(self):
        """Whether ft-together has a PC's link: True, False, or None when it doesn't say."""
        reply = self.together("media")
        try:
            said = json.loads(reply[3:]) if reply and reply.startswith("ok {") else None
        except ValueError:
            said = None
        here = said.get("sender") if isinstance(said, dict) else None
        return here if isinstance(here, bool) else None

    def _call(self, pc):
        """Has the PC call this Frame. False when it wasn't asked, or didn't take it."""
        now = self.now()
        if self._called_at is not None and now - self._called_at < CALL_EVERY:
            return False
        here, code = _here(self.together("here")), None
        if here is not None:
            code = _code(self.together("sender"))
        if code is None:
            self._say("Frametop Together doesn't say where the PC is to call")
            return False
        self._called_at = now
        status, answer = self.api(pc, SENDER, {"address": here[0], "port": here[1], "code": code})
        if status != 200:
            self._say(_why(status, answer))
            return False
        self._pc = pc
        return True

    def _took(self, wanted, panels):
        standing = {}
        for p in panels:
            if not isinstance(p, dict):
                continue
            number, size = p.get("number"), (p.get("width"), p.get("height"))
            if type(number) is int and all(type(v) is int and v in SIDES for v in size):
                standing[number] = size
        with self._lock:
            self._why = ""
            for number, window in wanted.items():
                if self._wanted.get(number) != window:
                    continue   # (no longer wanted, or another window by now)
                if number in standing:
                    self._sizes[number] = standing[number]
                    self._missing.pop(number, None)
                    continue
                self._sizes.pop(number, None)
                self._missing[number] = self._missing.get(number, 0) + 1
                if self._missing[number] >= GONE_AFTER:
                    self._gone.add(number)
