"""Uninstalling Frametop from the window, without GTK: what uninstall.sh --plan says there is to
do, the boxes it becomes, the --delete and --keep flags they give uninstall.sh --yes, and what
its exit status means.

uninstall.sh does the work, as it does in a terminal. Its --plan lines (one each):
  step1 yes|no         is there anything that makes Frametop start (step 1)
  sudo yes|no          does step 1 need the password (system files, ft-camd's capabilities)
  running NAMES        Frametop's programs running now: step 2 waits for a restart
  ask KEY DEFAULT SIZE a question step 2 would ask (SIZE "-" when it has none)
"""
from dataclasses import dataclass, field

# Step 2's questions, in uninstall.sh's order: (title, detail). Frametop itself always goes.
CHOICES = {
    "releases": ("Frametop's releases", "The installed versions, with their containers and images (an image is "
                                        "about 3 GB). Uninstalling always deletes them."),
    "code": ("Frametop's code", "The checkout Frametop runs from. Kept by default when it has changes of its own."),
    "settings": ("Your settings", "The screen layout and profiles, dashboard tiles, palm menu, button maps, "
                                  "gaze calibration, remote displays' pairings, the stereo wallpaper. Keep them "
                                  "to pick up where you left off if you reinstall."),
    "voice": ("Voice dictation's speech models", "An install downloads them again."),
    "avatars": ("Imported avatar and hand models", "The models you imported yourself."),
    "recordings": ("Eye and hand recordings", "Your recordings and gaze bench runs."),
    "container": ("The build container (dev)", "Frametop's compilers and libraries, 1-2 GB. Anything else you "
                                               "put in it goes too."),
    "distrobox": ("distrobox", "The container tool the install put in ~/.local/bin. It only goes when no other "
                               "container uses it."),
}
ALWAYS = ("releases",)   # ticked, and can't be unticked


@dataclass
class Plan:
    step1: bool = False
    sudo: bool = False
    running: str = ""
    asks: dict = field(default_factory=dict)   # KEY -> (default ticked, size or "")

    @property
    def anything(self):
        return self.step1 or bool(self.running) or bool(self.asks)


def parse_plan(text):
    plan = Plan()
    for line in text.splitlines():
        word, _, rest = line.strip().partition(" ")
        if word == "step1":
            plan.step1 = rest == "yes"
        elif word == "sudo":
            plan.sudo = rest == "yes"
        elif word == "running":
            plan.running = rest
        elif word == "ask":
            parts = rest.split()
            if len(parts) == 3 and parts[0] in CHOICES:
                plan.asks[parts[0]] = (parts[1] == "y", "" if parts[2] == "-" else parts[2])
    return plan


def boxes(plan, picks):
    """{KEY: (ticked, can change)} for the questions in PLAN, from PICKS {KEY: bool} (a question
    not picked yet takes uninstall.sh's default). distrobox goes only with the build container:
    while that stays, distrobox does too."""
    out = {}
    for key, (default, _) in plan.asks.items():
        if key in ALWAYS:
            out[key] = (True, False)
        else:
            out[key] = (picks.get(key, default), True)
    if "distrobox" in out and "container" in out and not out["container"][0]:
        out["distrobox"] = (False, False)
    return out


def flags(plan, picks):
    """uninstall.sh's --delete and --keep for each question in PLAN, as the boxes show."""
    out = []
    for key, (ticked, _) in boxes(plan, picks).items():
        out += ["--delete" if ticked else "--keep", key]
    return out


def outcome(rc):
    """What uninstall.sh's exit status means: "done", "restart" (step 1 is done; the rest goes
    after the headset restarts), "leftovers" (done, but what needs sudo stays), "unfinished" (no
    status: the headset restarted or something ended it) or "failed"."""
    return {None: "unfinished", 0: "done", 3: "restart", 5: "leftovers"}.get(rc, "failed")
