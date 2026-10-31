# frametop.conf

`~/.config/frametop.conf` holds Frametop's settings as `NAME=value` lines. `session/frametop.conf.example` lists every key and what it does. This page says how the file is written, so that the session (which sources it with bash) and every other program (which parse it) read the same thing.

One parser per language reads it: `conf/ft_conf.py` for Python and `conf/ft_conf.h` for C and C++. `conf/test/conf-test.py` runs the Python, C and C++ readers, and bash, on the same test vectors (`conf/test/vectors.json`). Don't add another parser: import one of these.

## The grammar

A line is one of:

- blank, or only spaces and tabs;
- a comment: `#` as its first character after any spaces;
- a setting: `NAME=value`, then optionally spaces or tabs and a `# comment`.

`NAME` is upper-case letters, digits and `_`, not starting with a digit. Lower-case names are the session script's own variables, and a few upper-case ones are bash's (`UID`, `EUID`, `PPID`, `SHELLOPTS`, `BASHOPTS` and `BASH_VERSINFO` are read-only, so setting one stopped the session; `IFS`, `PATH`, `HOME`, `CDPATH`, `ENV`, `BASH_ENV`, `GLOBIGNORE`, `OPTIND` and `PS4` change how its shell runs): a line setting one of them is outside the grammar. Spaces may come before the name, but none around the `=`: `HANDS_NEAR = 0` isn't a setting (bash would run a program called `HANDS_NEAR`).

The value is literal. It's made of these, side by side with nothing between them:

- bare characters: anything but spaces, tabs, quotes, and ``\ ` $ ; & | < > ( )``;
- `'single quotes'`: anything but a `'`;
- `"double quotes"`: anything but `"`, `$`, `` ` `` and `\`.

So `'a b'c` is `a bc`, and a `'` inside a value is written `'"'"'` (`'it'"'"'s'` is `it's`). A `#` inside a value is part of it (`a#b`); only a `#` after whitespace starts a comment. The value ends at the first space or tab outside quotes.

Readers don't expand anything: `$HOME`, `$(...)`, backquotes and backslashes are outside the grammar, and `~` stays `~` (bash turns a leading `~` into the home folder; the programs that take a path, such as `HANDS_FLAG_DIR`, do the same). Bash also expands `~user`, and a `~` after a `:`, which no reader does: don't write those. Writers quote any value with a `~`.

The last line of a name counts, as it does for bash.

Lines are separated by newlines. A carriage return or NUL anywhere in a line puts it outside the grammar.

## A line outside the grammar

Bash would read it differently from the parsers, or run part of it as a command. So:

- every parser leaves it out and warns on stderr (the program's log) with the line number and why; the name keeps an earlier line's value, or the program's default;
- the session (`session/frametop-session.sh`) checks the file with `conf/ft_conf.py check` before sourcing it. When a line is outside the grammar it doesn't source the file: it loads the valid settings from `ft_conf.py shell` instead, which writes each one quoted, so nothing in the file runs. When the check itself can't run (no `python3`), the session sources the file as it always did.

## Values

- Lists (`POINTER_IGNORE`) are comma-separated, with no space after the comma. Readers trim spaces around items and drop empty ones. An item can't contain a comma.
- Numbers are `[+-]digits[.digits][e[+-]digits]`. Anything else (`abc`, `0x10`, `1_000`, `inf`) is a bad value: the program warns and uses the setting's default. It never crashes, and C never reads garbage as 0.
- Switches are `1` and `0`, keywords lower-case (`on`, `off`, `auto`). The few readers that have always accepted any case of a keyword (`GAZE_TRACKER`, `GAZE_EYE`, `EYES_SIZE_GUARD`) still do; writers write lower-case.
- `HANDS_NEAR=0` turns direct touch off in every reader (ft-screens, the pointer helper, the relay, Input Settings).

## Writing

Programs write a setting with `ft_conf.set_value(NAME, value)`. It:

- changes the last line that sets `NAME` (the one that counts, even if it's outside the grammar), keeping its comment, or adds a line at the end; every other line stays as it is;
- writes the value bare when it's only `A-Z a-z 0-9 _ . , : / @ % + = * ? ^ -`, and in single quotes otherwise, so spaces, `$()`, `;` and quotes in it are only text to bash;
- refuses a value with a newline, carriage return or NUL (`ValueError`);
- writes a temp file next to the conf, flushes it to disk and renames it over the conf (keeping its mode), so a reader never sees half a file.

Writers don't lock: two programs writing at the same moment can still lose one change.

The `FT_<NAME>` environment variables override the file in the session and in the programs that say so (`ft_conf_setting`, `ft_conf::Setting`).
