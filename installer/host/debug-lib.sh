# What debug-report.sh and debug-features.sh both use (sourced).

NOISE='container (exec|exec_died|init|start|died|attach|cleanup)'   # podman's lines for each command run

# A journal line's program ("Nov 25 17:56:30 HOST PROGRAM[PID]: ..."), and the programs whose lines
# a report keeps from the user's session: the desktop's, SteamVR's, audio's and Frametop's own. Not
# apps', which can name the user's files, sites and windows.
FROM='^[A-Za-z.]+ +[0-9]+ [0-9:]+ [^ ]+'
OURS="$FROM (systemd[a-z-]*|kwin_wayland|plasmashell|plasma_session|ksmserver|kded6|org_kde_powerdevil"
OURS+='|xdg-desktop-portal[a-z-]*|kscreen-doctor|vrserver|vrcompositor|vrclient_[^ []*|vrwebhelper[^ []*|XRService'
OURS+='|console_log|pipewire[a-z-]*|wireplumber|dbus-broker[a-z-]*|in-box|podman|conmon|ft-[a-z-]+|frametop[a-z.-]*)\['

# The desktop session's log (/tmp/frametop-session.log) has all its programs' stderr, apps' too: of
# it, only Frametop's own lines, D-Bus starting services (without who asked) and the portals'.
SESSION_OURS='^(frametop|Frametop|ft-[a-z-]+|remote desktop|window drag|Steam PIN|Error EACCES|dbus-daemon\['
SESSION_OURS+='|\((/usr/lib/)?(xdg-desktop-portal|flatpak-portal)[a-z-]*:)'
# sessionlog FILE LINES: its last lines of those
sessionlog() {
  [ -f "$1" ] || { echo "not there"; return; }
  grep -a -E "$SESSION_OURS" "$1" | sed -E 's/comm="[^"]*"?/comm="<hidden>"/' | tail -n "$2"
}

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
