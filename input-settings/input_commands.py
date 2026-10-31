"""Input Settings' Commands page: your commands (commands/ft_commands.py) and the bindings
that run them. Plain Python (no Qt): ft_input_settings' Backend calls these and says the
returned text in the window. `host` is its host runner (a function like ft_input_settings.host),
passed in at call time. Tests: input-settings/test/test_input_commands.py.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "commands"))
import ft_commands  # noqa: E402  (your commands: standard library only)

RUN = "run:"   # an action that runs a command: "run:NAME"


def listed():
    """The commands, each with how to say it."""
    return [dict(c, said=ft_commands.spoken(c["name"])) for c in ft_commands.load()]


def name_problem(name, host):
    """Why `name` can't be a new command's name ("" if it can): checked here and on the host,
    whose PATH an alias would hide things on."""
    name = name.strip()
    problem = ft_commands.name_problem(name, {c["name"] for c in ft_commands.load()})
    if not problem and host("sh", "-c", 'command -v -- "$1"', "sh", name, timeout=5).returncode == 0:
        problem = f"{name} is a program on the headset; an alias by that name would hide it"
    return problem


def add(name, run, problem_of):
    """Save a new command. (saved, text): text is the problem, the error or the done message.
    problem_of(name) is the name check (Backend.commandNameProblem)."""
    name, run = name.strip(), run.strip()
    problem = problem_of(name) or ("" if run else "what should it run?")
    if problem:
        return False, problem
    try:
        ft_commands.add(name, run)
    except (OSError, ft_commands.CommandsFileError) as e:
        return False, f"Couldn't save {name}: {e}"
    return True, (f"{name} saved: say \"run {ft_commands.spoken(name)}\", search it in frunner, "
                  "or map it to a button")


def drop_bindings(rules, name):
    """Take out the buttons, controller buttons and key combinations that ran `name` (in place);
    how many went."""
    gone = RUN + name
    dropped = 0
    for table in [rules.get("controller_buttons", {}), rules.get("key_bindings", {}),
                  *rules.get("buttons", {}).values()]:
        if isinstance(table, dict):
            for k in [k for k, v in table.items() if v == gone]:
                del table[k]
                dropped += 1
    return dropped


def removed_text(name, dropped):
    return f"{name} removed" + (f", and {dropped} binding{'s' if dropped != 1 else ''} to it" if dropped else "")


def run_now(name, host, host_ft_cmd):
    """Run it now, on the host (ft-cmd, which the relay links into ~/.local/bin at host_ft_cmd).
    (text, is_error)."""
    ft_cmd = host_ft_cmd if os.path.exists(host_ft_cmd) else ft_commands.FT_CMD
    r = host(ft_cmd, "run", name, timeout=15)
    text = (r.stdout or r.stderr).strip() or f"{name}: started"
    return text, r.returncode != 0
