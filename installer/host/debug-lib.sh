# The debug report's helpers, sourced by debug-report.sh and debug-features.sh.

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

# show FILE [BYTES]: a file, or why it isn't there
show() {
  if [ -f "$1" ]; then
    echo "--- $1 ($(stat -c '%s bytes, changed %y' "$1" 2>/dev/null | cut -d. -f1))"
    head -c "${2:-30000}" "$1"
    echo
  else
    echo "--- $1: not there"
  fi
}

# tailf FILE LINES: a log file's last lines, or that it isn't there
tailf() {
  if [ -f "$1" ]; then
    echo "--- $1 (last $2 lines; changed $(date -r "$1" '+%F %T'))"
    tail -n "$2" "$1" | sed 's/.*\r//'
  else
    echo "--- $1: not there"
  fi
}

# ulog LINES UNIT...: user services' last journal lines; slog: system services'
# (a unit can be a pattern, such as 'frametop-room-*': journalctl matches it, the shell doesn't)
ulog() {
  local n=$1 u args=(); shift
  for u in "$@"; do args+=(-u "$u"); done
  echo "--- journal: $* (last $n lines)"
  t 15 journalctl --user "${args[@]}" -n "$((n * 2))" --no-pager -o short | grep -a -v -E "$NOISE" | tail -n "$n"
}
slog() {
  local n=$1 u args=(); shift
  for u in "$@"; do args+=(-u "$u"); done
  echo "--- journal: $* (system; last $n lines)"
  t 15 journalctl "${args[@]}" -n "$n" --no-pager -o short
}
