#!/usr/bin/env bash
# Install, start, stop, or inspect Frametop Voice on the Frame (ft-voiced runs in the container).
# Usage: voice/run.sh install     # fetch the models (voice/models.sh), install and start the service
#        voice/run.sh uninstall   # stop it and remove the service (the models stay: models.sh's folder)
#        voice/run.sh start|stop|restart|status|log [lines]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
unit=frametop-voice.service
whisper=frametop-voice-whisper.service
case ${1:-status} in
  install)
    "$root/scripts/sync.sh" >/dev/null
    "$root/voice/models.sh"
    for u in $unit $whisper; do
      fill_template "$root/voice/$u" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$u"
    done
    "$frame" --host "set -e
python3 voice/voice_settings.py install
# The standalone frame-voice, from before it was part of Frametop, would dictate a second time.
for old in frame-voice-vr-input frame-voice frame-voice-whisper; do
  if systemctl --user cat \$old.service >/dev/null 2>&1; then
    systemctl --user disable --now \$old.service >/dev/null 2>&1 || true
    echo \"voice: turned off the standalone \$old.service (Frametop Voice replaces it; its folder stays)\"
  fi
done
systemctl --user daemon-reload
systemctl --user enable $unit 2>/dev/null
systemctl --user restart $unit
for i in \$(seq 30); do [ -S /run/user/\$(id -u)/frametop-voice/control.sock ] && break; sleep 1; done
echo \"$unit: \$(systemctl --user is-active $unit)\"; journalctl --user -u $unit --no-pager -o cat -n 3" ;;
  uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; systemctl --user stop $whisper 2>/dev/null
rm -f ~/.config/systemd/user/$unit ~/.config/systemd/user/$whisper; systemctl --user daemon-reload; echo removed" ;;
  start|stop|restart) "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit" ;;
  status) "$frame" --host "echo \"$unit: \$(systemctl --user is-active $unit), $whisper: \$(systemctl --user is-active $whisper)\"
voice/ft-voice status || true" ;;
  log) "$frame" --host "journalctl --user -u $unit -u $whisper --no-pager -o cat -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|start|stop|restart|status|log" >&2; exit 2 ;;
esac
