# What debug-report.sh and debug-features.sh both use (sourced).

NOISE='container (exec|exec_died|init|start|died|attach|cleanup)'   # podman's lines for each command run

section() { printf '\n===== %s\n' "$*"; }

# t SECONDS COMMAND...: a command that can't hang the report
# (one that isn't there returns 127 quietly, for the caller's fallback)
t() { local s=$1; shift; command -v "$1" >/dev/null 2>&1 || return 127; timeout "$s" "$@" 2>&1; }

# units UNIT...: user services' enabled / active; sunits: system services'
units() {
  for u in "$@"; do
    echo "$u: $(systemctl --user is-enabled "$u" 2>/dev/null || true) / $(systemctl --user is-active "$u" 2>/dev/null || true)"
  done
}
sunits() {
  for u in "$@"; do
    echo "$u (system): $(systemctl is-enabled "$u" 2>/dev/null || true) / $(systemctl is-active "$u" 2>/dev/null || true)"
  done
}
