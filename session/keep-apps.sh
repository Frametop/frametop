#!/bin/bash
# Runs on the Frame host just before the desktop stops (desktops.sh stop). Moves the
# programs started inside the desktop out of its systemd unit (frametop-desktop) into a
# scope of their own, so stopping the unit (which kills everything left in it) only ends
# the desktop itself. Windows still close: a GUI app exits when its compositor goes.
# Background work started from the desktop keeps running: servers, agents, tmux, builds.
unit=frametop-desktop.service
cg=$(systemctl --user show -p ControlGroup --value "$unit" 2>/dev/null)
[ -n "$cg" ] && [ -r "/sys/fs/cgroup$cg/cgroup.procs" ] || exit 0

# The desktop's own processes stay in the unit and stop with it: the session, KWin,
# Plasma, and the session services it started (portals, input methods, kded, wallet...).
own='^(frametop-sessi|dbus-|startplasma|plasma|kwin|Xwayland|ksmserver|krdpserver|Xvnc|xfreerdp|ft-layout|ft-atspi|'
own+='kded|kactivitymanage|kaccess|kglobalaccel|kscreen|kwalletd|ksecretd|polkit-kde|org_kde_|baloo|'
own+='xembedsniproxy|gmenudbusmenu|DiscoverNotifie|kimpanel|ibus|xdg-|at-spi|dconf-service|fusermount|agent)'
keep=()
# So does anything run from Frametop's own files: the session's helpers (ft-dashroute,
# Frunner, ft-floatd...) mostly run as python3, and kept ones would hold their sockets
# against the next desktop's copies, from a release that may be gone by then.
root=$(cd "$(dirname "$0")/.." && pwd -P)/
while read -r pid; do
  comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
  [[ $comm =~ $own ]] && continue
  # ft-screens' launcher (distrobox enter, podman exec) stays too.
  tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -F -e 'ft-screens' -e 'ft-layout' -e 'frametop-session' -e "$root" && continue
  keep+=("$pid")
done < "/sys/fs/cgroup$cg/cgroup.procs"
[ ${#keep[@]} -gt 0 ] || exit 0

scope=frametop-apps-$(date +%s).scope
if busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager \
    StartTransientUnit 'ssa(sv)a(sa(sv))' "$scope" fail 3 \
    PIDs au "${#keep[@]}" "${keep[@]}" \
    Description s "Programs started in the Frametop desktop, kept past its restart" \
    CollectMode s inactive-or-failed 0 >/dev/null; then
  echo "kept ${#keep[@]} process(es) in $scope"
else
  echo "couldn't move the desktop's programs out of $unit; they stop with it" >&2
fi
