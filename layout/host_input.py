"""A remote host's "input" in the layout: which of its keyboard and mouse it lends to Frametop
(docs/host-input.md). ft-stream reads the same values (hostinput::InputKinds in stream/hostinput.h).

  true        its keyboard and mouse
  "keyboard"  its keyboard only: its mouse stays on the computer
  "mouse"     its mouse only
  false       neither (and anything else, so a garbled value lends nothing)
"""

KINDS = ("keyboard", "mouse")


def lent(host):
    """The kinds a host's entry lends: a set of "keyboard" and "mouse"."""
    value = host.get("input")
    if value is True:
        return set(KINDS)
    return {value} if value in KINDS else set()


def value(kinds):
    """The "input" value that lends KINDS (an iterable of "keyboard" and "mouse")."""
    kinds = set(kinds)
    if kinds - set(KINDS):
        raise ValueError(f"not a kind of input: {sorted(kinds - set(KINDS))}")
    return True if kinds == set(KINDS) else next(iter(kinds)) if kinds else False


# ft-layout remote host NAME input=WORD
WORDS = {"on": set(KINDS), "off": set(), "keyboard": {"keyboard"}, "mouse": {"mouse"}}


def word(kinds):
    """The ft-layout input=WORD that lends KINDS."""
    v = value(kinds)
    return "on" if v is True else "off" if v is False else v
