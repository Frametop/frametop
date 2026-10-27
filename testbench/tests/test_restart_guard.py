"""install.sh's SteamVR restart guard against a stubbed systemd, clock and health check."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Each sleep advances the fake clock by one tick. The health check's tracker is a real file.
HARNESS = r'''
. "$INSTALL_SH"
tracker=$T/tracker dropin=$T/dropin log=$T/log
n=0 base=$(command date +%s)
: >"$dropin"
case $SCEN in notracker) ;; frog) printf frog:glasses: >"$tracker" ;; *) : >"$tracker" ;; esac
[ "$SCEN" = fresh ] || [ ! -f "$tracker" ] || touch -d '1 hour ago' "$tracker"
date() { echo $((base + n)); }
sleep() {
  n=$((n + 1))
  if [ "$SCEN" = latefrog ] && [ $n -eq 8 ]; then printf frog:glasses: >>"$tracker"; fi
  if [ "$SCEN" = waitfrog ] && [ $n -eq 2 ]; then printf frog:glasses: >>"$tracker"; fi
  return 0
}
python3() { echo rollback >>"$log"; }
pgrep() { [ "$SCEN" != novrserver ]; }
systemctl() {
  case "$*" in
    *restart*)
      echo "restart $n" >>"$log"
      if [ "$SCEN" = stopfail ]; then printf frog:glasses: >>"$tracker"; fi
      return 0 ;;
    *ActiveState*)
      case $SCEN in
        failed) [ $n -ge 3 ] && echo failed || echo active ;;
        activating|waitfrog) [ $n -lt 3 ] && echo activating || echo active ;;
        stuck) echo activating ;;
        *) echo active ;;
      esac ;;
    *InvocationID*) [ "$SCEN" = crash ] && [ $n -ge 5 ] && echo B || echo A ;;
  esac
}
restart_steamvr "$MODE"
'''

CASES = [
    # scenario, mode, exit status, log
    ("healthy", "install", 0, ["restart 0"]),
    ("healthy", "uninstall", 0, ["restart 0"]),
    ("notracker", "install", 0, ["restart 0"]),
    ("activating", "install", 0, ["restart 3"]),
    ("crash", "install", 1, ["restart 0", "rollback"]),
    ("latefrog", "install", 1, ["restart 0", "rollback"]),
    ("failed", "install", 1, ["restart 0", "rollback"]),
    ("stopfail", "install", 1, ["restart 0", "rollback"]),
    ("novrserver", "install", 1, ["restart 0", "rollback"]),
    ("frog", "install", 1, []),
    ("frog", "uninstall", 0, []),
    ("waitfrog", "install", 1, []),
    ("stuck", "install", 1, []),
]


class RestartGuardTests(unittest.TestCase):
    def run_guard(self, scenario, mode):
        temp = tempfile.TemporaryDirectory(prefix="rg-")
        self.addCleanup(temp.cleanup)
        env = dict(os.environ, INSTALL_SH=str(ROOT / "install.sh"), T=temp.name,
                   SCEN=scenario, MODE=mode, FRAME_LOCAL="1")
        r = subprocess.run(["bash", "-c", HARNESS], env=env, text=True, capture_output=True, timeout=30)
        log = Path(temp.name, "log")
        return r, log.read_text().split("\n")[:-1] if log.exists() else []

    def test_scenarios(self):
        for scenario, mode, status, expected in CASES:
            with self.subTest(scenario=scenario, mode=mode):
                r, log = self.run_guard(scenario, mode)
                self.assertEqual(r.returncode, status, r.stderr)
                self.assertEqual(log, expected, r.stderr)

    def test_waits_out_a_session_younger_than_the_health_checks_limit(self):
        r, log = self.run_guard("fresh", "install")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(log), 1)
        self.assertIn(log[0], ("restart 15", "restart 16"))


if __name__ == "__main__":
    unittest.main()
