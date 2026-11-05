"""Voice commands: run a program when a wake word command starts with a set phrase.

From "commands" in ~/.config/frametop-voice.json (voice_settings.py):
  [{"say": ["flag that", "flag"], "run": ["~/.local/bin/my-tool", "flag"], "rest": "arg"}]

say:  phrases. A command matches when its words equal a phrase or start with it.
      Case and punctuation don't matter. The longest matching phrase of all entries wins.
run:  the program and its arguments, run directly (never through a shell). "~" is expanded.
rest: "arg" (default) appends the words after the phrase as one extra argument when there
      are any. "none" never appends them.

Your Frametop commands (Frametop Input Settings, Commands; commands/ft_commands.py) are voice
commands too: "run NAME", NAME's dashes, dots and underscores said as spaces (named()).

Pure standard library, so the tests can import it without the audio stack.
"""
import os
import re
from typing import NamedTuple

REST_MODES = ("arg", "none")
WORD = re.compile(r"\w+")


class Command(NamedTuple):
    phrases: tuple  # (as written, normalised word tuple)
    run: tuple
    rest: str
    loose: bool = False  # the phrase matches with the spaces between its words ignored (named())


class Match(NamedTuple):
    phrase: str  # as written in the settings
    rest: str  # the user's words after the phrase, "" when none
    argv: list


def words(text):
    """Lowercase words with punctuation as separators: "Flag that, OK." -> ("flag", "that", "ok")."""
    return tuple(w.lower() for w in WORD.findall(text))


def strip_wake(text, phrases):
    """`text` without a wake phrase it starts with, and whether one was there. The recognizer
    often catches the end of the wake phrase as the command starts ("Hey, Computer. Flag that."),
    and mishears its first words ("A Computer"), so the phrase's last word
    within the phrase's length marks it: everything up to it goes."""
    found = list(WORD.finditer(text or ""))
    heard = [m.group().lower() for m in found]
    best = -1
    for phrase in phrases:
        pw = words(phrase)
        if pw and pw[-1] in heard[:len(pw)]:
            best = max(best, heard[:len(pw)].index(pw[-1]))
    if best < 0:
        return text, False
    return re.sub(r"^\W+", "", text[found[best].end():]), True


def normalise(text):
    return " ".join(words(text))


def parse(entries):
    """Valid commands from the settings list, and a warning for each entry skipped."""
    commands, warnings = [], []
    if not isinstance(entries, list):
        if entries is not None:
            warnings.append(f"commands: expected a list, got {type(entries).__name__}")
        return commands, warnings
    for i, entry in enumerate(entries):
        problem = None
        if not isinstance(entry, dict):
            problem = "not an object"
        else:
            say = entry.get("say")
            if isinstance(say, str):
                say = [say]
            run = entry.get("run")
            rest = entry.get("rest", "arg")
            if not isinstance(say, list) or not all(isinstance(p, str) for p in say):
                problem = '"say" must be a list of phrases'
            elif not isinstance(run, list) or not run or not all(isinstance(a, str) for a in run) or not run[0]:
                problem = '"run" must be a list: program, then arguments'
            elif rest not in REST_MODES:
                problem = f'"rest" must be one of {", ".join(REST_MODES)}'
            else:
                phrases = tuple((p.strip(), words(p)) for p in say if words(p))
                if not phrases:
                    problem = '"say" has no words'
        if problem:
            warnings.append(f"commands[{i}] skipped: {problem}")
            continue
        commands.append(Command(phrases, tuple(os.path.expanduser(a) for a in run), rest))
    return commands, warnings


def named(commands, ft_cmd):
    """Voice commands for your Frametop commands ([{"name", "run"}]): "run NAME" runs
    `ft_cmd run NAME`, the words after it ignored. Loose: the recognizer splits and joins made-up
    words as it likes ("framecorder", "frame corder"), so only the letters count."""
    out = []
    for c in commands:
        said = " ".join(w for w in re.split(r"[._-]+", c["name"].lower()) if w)
        phrase = "run " + said
        if words(phrase)[1:]:
            out.append(Command(((phrase, words(phrase)),), (ft_cmd, "run", c["name"]), "none", loose=True))
    return out


def loose_length(heard, pw):
    """How many of the heard words spell the phrase's letters, spaces aside (0: they don't)."""
    want, got = "".join(pw), ""
    for i, w in enumerate(heard):
        got += w
        if got == want:
            return i + 1
        if not want.startswith(got):
            return 0
    return 0


def match(text, commands):
    """The command `text` starts with, or None. Ties go to the earlier entry."""
    found = list(WORD.finditer(text or ""))
    heard = tuple(m.group().lower() for m in found)
    best = None  # (cmd, phrase, phrase's words, how many heard words it took)
    for cmd in commands:
        for phrase, pw in cmd.phrases:
            n = len(pw) if heard[:len(pw)] == pw else loose_length(heard, pw) if cmd.loose else 0
            if n and (best is None or len(pw) > len(best[2])):
                best = (cmd, phrase, pw, n)
    if best is None:
        return None
    cmd, phrase, pw, n = best
    rest = text[found[n - 1].end():]
    rest = re.sub(r"^\W+", "", rest).rstrip(" \t\n.")
    argv = list(cmd.run) + ([rest] if rest and cmd.rest == "arg" else [])
    return Match(phrase, rest, argv)
