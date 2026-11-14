#!/usr/bin/env python3
"""G9: run TLC on G9-together.tla (module G9Together) and print what holds and what breaks, with
TLC's shortest path to each break (invariants) or its counterexample (temporal properties).
A config's rules are checked in one run. A rule that breaks is taken out and the rest run again,
so every rule gets its own answer. Traces go to G9-together-traces/. Read-only: copies the model
into a temp dir.

  python3 G9-together-check.py              every config
  python3 G9-together-check.py Room PreFix  some

TLC from TLA2TOOLS or ~/.cache/frametop-formal/tla2tools-1.7.4.jar, Java from JAVA or PATH."""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
JAR = os.environ.get('TLA2TOOLS', str(Path.home() / '.cache/frametop-formal/tla2tools-1.7.4.jar'))
JAVA = os.environ.get('JAVA', 'java')
CONFIGS = ['Room', 'Full', 'Reopen', 'Live', 'PreFix']
RULE = r'^(INVARIANT|PROPERTY) (\w+)\n'


def tlc(name, rules, work):
    """What TLC says of these rules of a config: (the one that breaks or None, its path, the states found)."""
    cfg = re.sub(RULE, '', Path(HERE, f'G9-together-{name}.cfg').read_text(), flags=re.M)
    Path(work, 'run.cfg').write_text(cfg + ''.join(f'{kind} {rule}\n' for kind, rule in rules))
    out = subprocess.run([JAVA, '-XX:+UseParallelGC', '-cp', JAR, 'tlc2.TLC', '-workers', '1', '-deadlock',
                          '-metadir', str(Path(work, 'meta')), '-config', 'run.cfg', 'G9Together.tla'],
                         cwd=work, capture_output=True, text=True, timeout=3600).stdout
    shutil.rmtree(Path(work, 'meta'), ignore_errors=True)
    states = (re.findall(r'([\d,]+) distinct states found', out) or ['?'])[-1]
    broke = re.search(r'Invariant (\w+) is violated', out)
    if broke or 'Temporal properties were violated' in out:
        rule = next(r for r in rules if r[1] == broke.group(1)) if broke else next(r for r in rules if r[0] == 'PROPERTY')
        steps = [s for s in re.findall(r'^State \d+: <(\w+)', out, re.M) if s != 'Initial']
        back = re.search(r'Back to state (\d+)', out)
        tail = ' (then stutters)' if 'Stuttering' in out else f' (then back to state {back.group(1)})' if back else ''
        Path(HERE, 'G9-together-traces').mkdir(exist_ok=True)
        Path(HERE, 'G9-together-traces', f'{name}-{rule[1]}.txt').write_text(out)
        return rule, ' -> '.join(steps) + tail, states
    if 'No error has been found' in out:
        return None, '', states
    sys.stderr.write(out[-3000:])
    sys.exit(f'TLC failed on {name}/{" ".join(rule for _, rule in rules)}')


def check(name, work):
    """{rule: ('holds' or 'breaks', the path, the states found)} for a config."""
    rules = re.findall(RULE, Path(HERE, f'G9-together-{name}.cfg').read_text(), re.M)
    got, left = {}, list(rules)
    while left:
        # temporal properties one at a time: TLC doesn't say which of several broke
        now = left if all(kind == 'INVARIANT' for kind, _ in left) else [next(r for r in left if r[0] == 'PROPERTY')]
        broke, path, states = tlc(name, now, work)
        for rule in [broke] if broke else now:
            got[rule] = ('breaks' if broke else 'holds', path, states)
            left.remove(rule)
    return [(rule[1], *got[rule]) for rule in rules]


def main():
    with tempfile.TemporaryDirectory(prefix='g9-together-') as work:
        shutil.copy(Path(HERE, 'G9-together.tla'), Path(work, 'G9Together.tla'))
        for name in sys.argv[1:] or CONFIGS:
            for rule, got, path, states in check(name, work):
                print(f'{name:7} {rule:16} {got:6} ({states} states)', flush=True)
                if path:
                    print('        path: ' + path, flush=True)


if __name__ == '__main__':
    main()
