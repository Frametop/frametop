#!/usr/bin/env bash
# Update the lines of ~/.config/frametop.conf that still read exactly as an older
# frametop.conf.example wrote them. A line you changed stays as it is. install.sh and
# hands/rec/install.sh run this on every install and update.
#   HANDS_SWAP_SIDES=0: the example's value until 2026-10-05. It made ft-hands keep ft-camd's side
#   camera names even when they're backwards (some SteamVR restarts swap them), so hands landed
#   beside their cutouts and the hand recorder labelled the side cameras wrong. auto tells from
#   the hands.
#   SHARE_KEYS: gone on 2026-10-10. It sent the keys of keyboards grabbed for the desktop to
#   @frametop_keys for frame-voice, which is now Frametop Voice and gets its keys from the relay.
#   The example's line goes; a changed one stays, unread, and gets a note.
#   HANDS_NEAR_REACH=0.30: the example's value from the hands-reach branch (2026-10-10, a foot). The
#   live test that day asked for about 3 inches: 0.075.
#   HANDS_POKE_TRAVEL=0.025: the example's value from the hands-touch branch (2026-10-10). The second
#   live test that day refused taps that came 1.7-2.0 cm in: 0.017.
# Then input/rules-migrate.py makes its one-time changes to ~/.config/frametop-input.json: a
# key_bindings list of your own gets Meta+Shift+H (DEE-217), which was a KWin shortcut before.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"

on_frame_script <<'EOF'
f=~/.config/frametop.conf
[ -f "$f" ] || exit 0
old="HANDS_SWAP_SIDES=0         # hand tracking (hands/run.sh install): 1 = the side cameras' names are swapped, which some SteamVR restarts cause (hands/tools/check_sides.py --ring tells)"
new="HANDS_SWAP_SIDES=auto      # hand tracking: which side camera is which. auto = ft-hands tells from the hands and fixes ft-camd's names, which some SteamVR restarts swap | 0 = keep ft-camd's | 1 = exchange them"
if grep -qxF "$old" "$f"; then
  tmp=$(mktemp)
  awk -v old="$old" -v new="$new" '$0 == old { print new; next } { print }' "$f" > "$tmp"
  cat "$tmp" > "$f"   # in place: the file keeps its owner and mode
  rm -f "$tmp"
  echo "settings: HANDS_SWAP_SIDES=0 (the old default) is now auto: hand tracking tells the side cameras apart itself"
fi
old="HANDS_NEAR_REACH=0.30      # Frametop panels: a bare hand this close (m) in front of one touches it: its ray hides and takes no pinches, the index pokes (0.15 = the old reach; a hand resting low by the body: 0.15 at most)"
new="HANDS_NEAR_REACH=0.075     # Frametop panels: a bare hand this close (m) in front of one touches it (about 3 inches): its ray hides, the index pokes, its pinches and fists are the panel's (beyond it they're the ray's or the gaze's; a hand resting low by the body: 0.04 at most)"
if grep -qxF "$old" "$f"; then
  tmp=$(mktemp)
  awk -v old="$old" -v new="$new" '$0 == old { print new; next } { print }' "$f" > "$tmp"
  cat "$tmp" > "$f"
  rm -f "$tmp"
  echo "settings: HANDS_NEAR_REACH=0.30 (a foot, the old default) is now 0.075: hands touch Frametop's panels within about 3 inches"
fi
old="HANDS_POKE_TRAVEL=0.025    # ... and presses on its first frame past the surface when it came this far (m) in, decisively; slower ones need two frames (0 = always two)"
new="HANDS_POKE_TRAVEL=0.017    # ... and presses on its first frame past the surface when it came this far (m) in, decisively (under 2.5 cm: steadily, each of the last 3 frames nearer); slower ones need two frames (0 = always two)"
if grep -qxF "$old" "$f"; then
  tmp=$(mktemp)
  awk -v old="$old" -v new="$new" '$0 == old { print new; next } { print }' "$f" > "$tmp"
  cat "$tmp" > "$f"
  rm -f "$tmp"
  echo "settings: HANDS_POKE_TRAVEL=0.025 (the old default) is now 0.017: a quick short tap presses"
fi
old="SHARE_KEYS=0       # 1 = keys of keyboards grabbed for the desktop also go to @frametop_keys, for hotkey tools (any local process can listen)"
if grep -qxF "$old" "$f"; then
  tmp=$(mktemp)
  awk -v old="$old" '$0 != old { print }' "$f" > "$tmp"
  cat "$tmp" > "$f"
  rm -f "$tmp"
elif grep -q '^SHARE_KEYS=1' "$f"; then
  echo "settings: SHARE_KEYS=1 does nothing now: nothing sends keys to @frametop_keys (Frametop Voice gets them from the input relay)"
fi
EOF
on_frame "python3 -I input/rules-migrate.py" ||
  echo "key combinations: the one-time update didn't finish (above); the next install tries again"
