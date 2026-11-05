# Commands

Commands are shell commands of your own, saved under a name, that run in the background on the headset. Name one once, then run it without a terminal or a keyboard: from a controller button, a mouse button, a key combination, frunner, or your voice. Its name also works as an alias in a terminal.

For example, a command called `stop-framecorder` that runs `killall framecorder` lets you stop a recording with a controller button.

## Adding one

Open Frametop Input Settings, then **Commands**. Type a name and what it runs, then press **Add**. Each command in the list has **Run now** and **Remove**. Removing a command also removes the buttons and key combinations mapped to it.

A name is one word made of letters, digits, `.`, `_` and `-` (at most 40 characters). It can't be a program you already have (such as `ls`) or a shell word (such as `cd`), because its alias would hide that program. `list`, `add`, `remove`, `stop`, `log`, `run` and `help` are taken by `ft-cmd`.

From a terminal:

```
ft-cmd add stop-framecorder killall framecorder
ft-cmd list
ft-cmd remove stop-framecorder
```

The list lives in `~/.config/frametop-commands.json`.

## Running one

| From | How |
| --- | --- |
| A controller button | Input Settings, **Controllers**: capture the button, then pick **Run command NAME** |
| A mouse button | Input Settings, **Buttons**: the same, for a button on that mouse |
| A key combination | Input Settings, **Keyboard**: pick **Run command NAME**, then **Set keys…** |
| frunner | Meta+Space, type part of the name, Enter. Shift+Enter has Run and Stop |
| Frametop Voice | The wake word, then "run" and the name, with dashes, dots and underscores said as spaces: "Hey Computer, run stop framecorder". Spaces you didn't mean don't matter ("frame corder" works too) |
| A terminal | The name itself (an alias), or `ft-cmd NAME` |

Mapped buttons and key combinations that run a command still work while Frametop is paused for a VR game.

## How they run

A command runs with `sh -c`, as you, on the headset (not in Frametop's container). Nothing waits for it, and running it again starts another copy. Frametop's own tools (`ft-layout`, `ft-float`, `ft-steam`, `ft-voice`) are on its PATH.

Each run is a transient systemd user unit named `frametop-cmd-NAME-TIME`:

```
ft-cmd list              # the commands, and which are running
ft-cmd stop NAME         # stop every running copy
ft-cmd log NAME          # its output
```

`ft-cmd log NAME` is the same as `journalctl --user -u 'frametop-cmd-NAME-*'`. The unit runs outside the Frametop desktop's session, so commands suit background jobs (stopping a program, a script, `ft-layout use Work`) more than opening windows; use frunner to open apps.

## The aliases

Frametop writes `~/.config/frametop/aliases.sh` with one alias per command, each calling `ft-cmd NAME`. The first time you save a command, it adds two lines to the end of `~/.bashrc` that read that file. New terminals get the aliases; in an open one, run `. ~/.config/frametop/aliases.sh`.

`ft-cmd` is a link in `~/.local/bin`, made by the input relay each time it starts, so it follows Frametop when an update moves it. `uninstall.sh` removes the link and the two `~/.bashrc` lines. The list and the aliases file stay with your other settings until you choose to delete those.

## The pieces

- `commands/ft_commands.py`: the list, names, the aliases file, `~/.bashrc`'s lines, the link, and running and stopping
- `commands/ft-cmd`: the command line
- `input/input-relay.py`: the `run:NAME` action for buttons and key combinations
- `input-settings/`: the Commands page, and **Run command NAME** in every action picker
- `frunner/ft_frunner.py`: commands as search results
- `voice/voice_commands.py` (`named`) and `voice/ft-voiced`: "run NAME"
- Tests: `commands/test/`, `input/test/keys-test.py`, `voice/test/test_commands.py`, `frunner/test/frunner-test.py`
