#!/usr/bin/env python3
"""G2: run TLC on the G2 desktop model, one run per config and rule, and print each result with
TLC's shortest path to a break. Traces go to G2-desktop-traces/. Read-only: copies each model into
a temp dir.

  G2-desktop-check.py                   every config
  G2-desktop-check.py Desktop:HeadOff   one config

  Desktop  G2-desktop.tla (module G2Desktop): when a desktop screen shows (visibility modes, the
           hide/show switch, conceal, the pause, a gaze calibration, ft-layout at desktop start)
           and when remote access (VNC) runs

TLC from TLA2TOOLS or ~/.cache/frametop-formal/tla2tools-1.7.4.jar, Java from JAVA or PATH."""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
JAR = os.environ.get('TLA2TOOLS', str(Path.home() / '.cache/frametop-formal/tla2tools-1.7.4.jar'))
JAVA = os.environ.get('JAVA', 'java')
MODELS = {'Desktop': 'G2Desktop'}


def configs(model):
    return sorted(p.name[len('G2-desktop-'):-4] for p in HERE.glob('G2-desktop-*.cfg'))


def rules(cfg):
    return re.findall(r'^(INVARIANT|PROPERTY) (\w+)', cfg, re.M)


def run(model, name, kind, rule, work):
    cfg = Path(HERE, f'G2-desktop-{name}.cfg').read_text()
    cfg = re.sub(r'^(INVARIANT|PROPERTY) .*\n', '', cfg, flags=re.M) + f'{kind} {rule}\n'
    Path(work, 'run.cfg').write_text(cfg)
    out = subprocess.run([JAVA, '-XX:+UseParallelGC', '-cp', JAR, 'tlc2.TLC', '-workers', '1', '-deadlock',
                          '-metadir', str(Path(work, 'meta')), '-config', 'run.cfg', f'{MODELS[model]}.tla'],
                         cwd=work, capture_output=True, text=True, timeout=900).stdout
    states = re.search(r'([\d,]+) distinct states found', out)
    states = states.group(1) if states else '?'
    broke = f'Invariant {rule} is violated' in out or 'Temporal properties were violated' in out
    if broke:
        steps = [s for s in re.findall(r'^State \d+: <(\w+)', out, re.M) if s != 'Initial']
        stutter = 'Stuttering' in out
        back = re.search(r'Back to state (\d+)', out)
        tail = ' (then stutters)' if stutter else f' (then back to state {back.group(1)})' if back else ''
        Path(HERE, 'G2-desktop-traces').mkdir(exist_ok=True)
        Path(HERE, 'G2-desktop-traces', f'{name}-{rule}.txt').write_text(out)
        return 'breaks', ' -> '.join(steps) + tail, states
    if 'No error has been found' in out:
        return 'holds', '', states
    sys.stderr.write(out[-3000:])
    sys.exit(f'TLC failed on {model}/{name}/{rule}')


def main():
    picks = sys.argv[1:] or list(MODELS)
    with tempfile.TemporaryDirectory(prefix='g2-desktop-') as work:
        for pick in picks:
            model, _, only = pick.partition(':')
            shutil.copy(Path(HERE, 'G2-desktop.tla'), Path(work, f'{MODELS[model]}.tla'))
            for name in [only] if only else configs(model):
                for kind, rule in rules(Path(HERE, f'G2-desktop-{name}.cfg').read_text()):
                    got, path, states = run(model, name, kind, rule, work)
                    print(f'{name:13} {rule:16} {got:6} ({states} states)', flush=True)
                    if path:
                        print('      path: ' + path, flush=True)


if __name__ == '__main__':
    main()
