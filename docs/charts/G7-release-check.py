#!/usr/bin/env python3
"""G7: run TLC on G7-release.tla (module G7Release), one run per config and rule, and print each
result with TLC's shortest path to a break. Traces go to G7-release-traces/. Read-only: copies
the model into a temp dir.

  G7-release-check.py            every config
  G7-release-check.py Rollback   one config (G7-release-NAME.cfg)

TLC from TLA2TOOLS or ~/.cache/frametop-formal/tla2tools-1.7.4.jar, Java from JAVA or PATH."""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
JAR = os.environ.get('TLA2TOOLS', str(Path.home() / '.cache/frametop-formal/tla2tools-1.7.4.jar'))
JAVA = os.environ.get('JAVA', 'java')
PREFIX = 'G7-release-'


def configs():
    return sorted(p.name[len(PREFIX):-4] for p in HERE.glob(f'{PREFIX}*.cfg'))


def rules(cfg):
    return re.findall(r'^(INVARIANT|PROPERTY) (\w+)', cfg, re.M)


def run(name, kind, rule, work):
    cfg = Path(HERE, f'{PREFIX}{name}.cfg').read_text()
    cfg = re.sub(r'^(INVARIANT|PROPERTY) .*\n', '', cfg, flags=re.M) + f'{kind} {rule}\n'
    Path(work, 'run.cfg').write_text(cfg)
    out = subprocess.run([JAVA, '-XX:+UseParallelGC', '-cp', JAR, 'tlc2.TLC', '-workers', '1', '-deadlock',
                          '-metadir', str(Path(work, 'meta')), '-config', 'run.cfg', 'G7Release.tla'],
                         cwd=work, capture_output=True, text=True, timeout=900).stdout
    states = re.search(r'([\d,]+) distinct states found', out)
    states = states.group(1) if states else '?'
    broke = f'Invariant {rule} is violated' in out or 'Temporal properties were violated' in out
    if broke:
        steps = [s for s in re.findall(r'^State \d+: <(\w+)', out, re.M) if s != 'Initial']
        stutter = 'Stuttering' in out
        back = re.search(r'Back to state (\d+)', out)
        tail = ' (then stutters)' if stutter else f' (then back to state {back.group(1)})' if back else ''
        Path(HERE, 'G7-release-traces').mkdir(exist_ok=True)
        Path(HERE, 'G7-release-traces', f'{name}-{rule}.txt').write_text(out)
        return 'breaks', ' -> '.join(steps) + tail, states
    if 'No error has been found' in out:
        return 'holds', '', states
    sys.stderr.write(out[-3000:])
    sys.exit(f'TLC failed on {name}/{rule}')


def main():
    with tempfile.TemporaryDirectory(prefix='g7-release-') as work:
        shutil.copy(Path(HERE, 'G7-release.tla'), Path(work, 'G7Release.tla'))
        for name in sys.argv[1:] or configs():
            for kind, rule in rules(Path(HERE, f'{PREFIX}{name}.cfg').read_text()):
                got, path, states = run(name, kind, rule, work)
                print(f'{name:14} {rule:21} {got:6} ({states} states)', flush=True)
                if path:
                    print('      path: ' + path, flush=True)


if __name__ == '__main__':
    main()
