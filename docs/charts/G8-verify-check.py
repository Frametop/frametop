#!/usr/bin/env python3
"""G8: run TLC on the G8 models, one run per config and rule, and print each result with TLC's
shortest path to a break. Traces go to G8-verify-traces/. Read-only: copies each model into a
temp dir.

  G8-verify-check.py                   every model and config
  G8-verify-check.py Guard             one model
  G8-verify-check.py Verify:Overrides  one config

  Verify  G8-verify.tla (module G8Verify): one verify run's lease, the testbench's proxy
          overrides and fake trackers, and whether they end once nobody drives the run.
          Configs G8-verify-NAME.cfg
  Guard   G8-verify-Guard.tla (module G8Guard): verify-fake-worn-guard's FakeWornGuard.tla,
          used as it is from docs/charts/fake-worn-guard, with fairness for the
          system's own steps and the rule that the guard always restores the real wearer.
          Configs G8-verify-Guard-NAME.cfg

TLC from TLA2TOOLS or ~/.cache/frametop-formal/tla2tools-1.7.4.jar, Java from JAVA or PATH."""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
JAR = os.environ.get('TLA2TOOLS', str(Path.home() / '.cache/frametop-formal/tla2tools-1.7.4.jar'))
JAVA = os.environ.get('JAVA', 'java')
MODELS = {'Verify': 'G8Verify', 'Guard': 'G8Guard'}
PREFIX = {'Verify': 'G8-verify-', 'Guard': 'G8-verify-Guard-'}
GUARD = HERE / 'fake-worn-guard/FakeWornGuard.tla'


def configs(model):
    names = (p.name[len(PREFIX[model]):-4] for p in HERE.glob(f'{PREFIX[model]}*.cfg'))
    return sorted(n for n in names if model == 'Guard' or not n.startswith('Guard-'))


def rules(cfg):
    return re.findall(r'^(INVARIANT|PROPERTY) (\w+)', cfg, re.M)


def run(model, name, kind, rule, work):
    cfg = Path(HERE, f'{PREFIX[model]}{name}.cfg').read_text()
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
        Path(HERE, 'G8-verify-traces').mkdir(exist_ok=True)
        Path(HERE, 'G8-verify-traces', f'{model}-{name}-{rule}.txt').write_text(out)
        return 'breaks', ' -> '.join(steps) + tail, states
    if 'No error has been found' in out:
        return 'holds', '', states
    sys.stderr.write(out[-3000:])
    sys.exit(f'TLC failed on {model}/{name}/{rule}')


def main():
    picks = sys.argv[1:] or list(MODELS)
    with tempfile.TemporaryDirectory(prefix='g8-verify-') as work:
        for pick in picks:
            model, _, only = pick.partition(':')
            src = 'G8-verify.tla' if model == 'Verify' else 'G8-verify-Guard.tla'
            shutil.copy(Path(HERE, src), Path(work, f'{MODELS[model]}.tla'))
            if model == 'Guard':
                shutil.copy(GUARD, Path(work, 'FakeWornGuard.tla'))
            for name in [only] if only else configs(model):
                for kind, rule in rules(Path(HERE, f'{PREFIX[model]}{name}.cfg').read_text()):
                    got, path, states = run(model, name, kind, rule, work)
                    print(f'{model:6} {name:13} {rule:17} {got:6} ({states} states)', flush=True)
                    if path:
                        print('      path: ' + path, flush=True)


if __name__ == '__main__':
    main()
