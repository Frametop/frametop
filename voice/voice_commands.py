"""Voice commands: run a program when a wake word command starts with a set phrase.

From "commands" in ~/.config/frametop-voice.json (voice_settings.py):
  [{"say": ["flag that", "flag"], "run": ["~/.local/bin/my-tool", "flag"], "rest": "arg"}]

say:  phrases. A command matches when its words equal a phrase or start with it.
      Case and punctuation don't matter. The longest matching phrase of all entries wins.
run:  the program and its arguments, run directly (never through a shell). "~" is expanded.
rest: "arg" (default) appends the words after the phrase as one extra argument when there
      are any. "none" never appends them.

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


class Match(NamedTuple):
    phrase: str  # as written in the settings
    rest: str  # the user's words after the phrase, "" when none
    argv: list


def words(text):
    """Lowercase words with punctuation as separators: "Flag that, OK." -> ("flag", "that", "ok")."""
    return tuple(w.lower() for w in WORD.findall(text))


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


def match(text, commands):
    """The command `text` starts with, or None. Ties go to the earlier entry."""
    found = list(WORD.finditer(text or ""))
    heard = tuple(m.group().lower() for m in found)
    best = None
    for cmd in commands:
        for phrase, pw in cmd.phrases:
            if heard[:len(pw)] == pw and (best is None or len(pw) > len(best[2])):
                best = (cmd, phrase, pw)
    if best is None:
        return None
    cmd, phrase, pw = best
    rest = text[found[len(pw) - 1].end():]
    rest = re.sub(r"^\W+", "", rest).rstrip(" \t\n.")
    argv = list(cmd.run) + ([rest] if rest and cmd.rest == "arg" else [])
    return Match(phrase, rest, argv)
