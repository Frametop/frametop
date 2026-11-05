"""Parse real generated units without registering or starting a service."""
from pathlib import Path
import subprocess
import sys
import tempfile

generator, analyzer = sys.argv[1:]
source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text()
guard = source.index('fk::require_steamvr();')
assert guard < source.index('fk::load_profiles('), 'SteamVR check must precede profile parsing'
assert guard < source.index('std::make_unique<fk::EiSink>'), 'SteamVR check must precede input connect'
with tempfile.TemporaryDirectory(prefix='keyboard-unit-') as directory:
    # Conditions consume a raw path; ExecStart consumes quoted command arguments.
    base = Path(directory) / 'Full Keyboard 100%'
    subprocess.run([generator, str(base)], check=True, capture_output=True)
    unit = base / 'units/framekeyboard-autostart.service'
    assert 'RestartSteps=5\nRestartMaxDelaySec=60\n' in unit.read_text()
    expected = str(base / 'install/current/bin/framekeyboard').replace('%', '%%')
    line = next(l for l in unit.read_text().splitlines() if l.startswith('ConditionPathExists='))
    assert line == 'ConditionPathExists=' + expected, line
    parsed = subprocess.run([analyzer, '--user', 'verify', str(unit)], text=True,
                            capture_output=True)
    # systemd can report malformed conditions as warnings while returning 0.
    assert parsed.returncode == 0 and not parsed.stderr.strip(), parsed.stderr
print('Generated unit parses without diagnostics, including spaces and literal percent.')
