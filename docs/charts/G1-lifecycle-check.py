#!/usr/bin/env python3
"""Model-check G1-lifecycle.tla (module G1Lifecycle, copied under that name for TLC) with TLC, one run per config and rule, the way the verify-* skills'
scripts/check do (TLC 1.7.4 from ~/.cache/frametop-formal, one worker, -deadlock, so the same model
gives the same shortest path). Prints each result, the state count, and each break's shortest path.

  check.py [CONFIG ...]     (default: every config below)"""
import re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
JAR = Path.home() / '.cache/frametop-formal/tla2tools-1.7.4.jar'
CONFIGS = {
    'PauseHide': ['MouseFreeWhilePaused', 'PointerFreeWhilePaused', 'GamepadOutsideGames', 'GazeOffWhilePaused',
                  'HandsOffWhilePaused', 'HandsFollowDesktop', 'DesktopMatchesPause', 'GazeBackAfterResume',
                  'ClosedDesktopComesBack', 'NoStuckGamePause'],
    'PauseClose': None, 'PauseCloseNoAuto': None, 'PauseNoCrash': None, 'PauseLossy': None,
    'Presence': ['WornAgreesSoon', 'DarkHasOwner', 'AwayReleasesPointer'],
    'Joint': None,
}
ALL = CONFIGS['PauseHide'] + CONFIGS['Presence']
for k, v in CONFIGS.items():
    if v is None:
        CONFIGS[k] = CONFIGS['PauseHide'] if k.startswith('Pause') else ALL


def java():
    out = subprocess.run(['mise', 'which', 'java', '--tool', 'java@temurin-25'], capture_output=True, text=True)
    return out.stdout.strip() or 'java'


def run(jvm, config, rule, work):
    cfg = (HERE / f'G1-lifecycle-{config}.cfg').read_text()
    cfg = re.sub(r'^INVARIANT .*\n', '', cfg, flags=re.M) + f'INVARIANT TypeOK\nINVARIANT {rule}\n'
    Path(work, 'run.cfg').write_text(cfg)
    out = subprocess.run([jvm, '-XX:+UseParallelGC', '-cp', str(JAR), 'tlc2.TLC', '-workers', '1', '-deadlock',
                          '-metadir', str(Path(work, 'meta')), '-config', 'run.cfg', 'G1Lifecycle.tla'],
                         cwd=work, capture_output=True, text=True, timeout=1800).stdout
    states = re.search(r'([\d,]+) distinct states found', out)
    states = states.group(1) if states else '?'
    if f'Invariant {rule} is violated' in out:
        steps = re.findall(r'^State \d+: <(\w+)', out, re.M)
        return 'breaks', states, ' '.join(s for s in steps if s != 'Initial'), out
    if 'No error has been found' in out:
        return 'holds', states, '', out
    sys.stderr.write(out[-3000:])
    sys.exit(f'check: TLC failed on {config}/{rule}')


def main():
    jvm = java()
    picked = sys.argv[1:] or list(CONFIGS)
    traces = HERE / 'G1-lifecycle-traces'
    traces.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='g1-check-') as work:
        shutil.copy(HERE / 'G1-lifecycle.tla', Path(work, 'G1Lifecycle.tla'))
        for config in picked:
            for rule in CONFIGS[config]:
                got, states, steps, out = run(jvm, config, rule, work)
                print(f'{config:17} {rule:23} {got:6} ({states} states)', flush=True)
                if steps:
                    print('    shortest path: ' + steps.replace(' ', ' -> '), flush=True)
                    (traces / f'{config}-{rule}.out').write_text(out)


if __name__ == '__main__':
    main()
