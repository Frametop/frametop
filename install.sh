#!/usr/bin/env bash
# Install everything on the Steam Frame: the build container, Frametop (multi-screen
# desktop, input relay, universal 3D mouse, settings app), and optionally voice dictation,
# hand tracking, gaze mode, our own eye tracker for it, and the Bluetooth fixes. Run it on the
# headset in a terminal, from this repo. It's safe to re-run, for example after `git pull`.
# (It also works from a PC over SSH; see "Developing from a PC" in the README.)
# In a release (pack/install-release.sh), the programs come built from its image: this installs the
# distrobox the release brings, makes the release's container from its image, and builds nothing.
# It asks every question first, then your password once if a part you picked needs it (each
# question that needs it says so), and then installs without asking anything else.
#
# Usage: ./install.sh [--yes] [--no-voice] [--no-hands] [--no-hand-recorder] [--no-eye-tracker]
#                    [--no-eye-recorder] [--no-bluetooth | --bluetooth]
#   --yes           don't ask; installs voice dictation, gaze mode and the Eye Recorder, and hand
#                   tracking, the Hand Recorder and our eye tracker if sudo can run without a
#                   password prompt; skips the Bluetooth fixes and the SteamVR restart
#   --no-voice      don't offer voice dictation (Frametop Voice: it downloads about 320 MB)
#   --no-hands      don't offer hand tracking or the Hand Recorder
#   --no-hand-recorder  don't offer the Hand Recorder (a release's; a clone builds it with
#                   hands/rec/install.sh)
#   --no-eye-tracker  don't offer our own eye tracker (or the Eye Recorder, which needs it)
#   --no-eye-recorder  don't offer the Eye Recorder (a release's; a clone builds it with
#                   gaze/rec/install.sh)
#   --no-bluetooth  don't offer the Bluetooth fixes
#   --bluetooth     install the Bluetooth fixes without asking (with --yes: if sudo can run
#                   without a password prompt)
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$root/scripts/_env.sh"

assume_yes=0 bluetooth=1 eye_tracker=1 eye_recorder=1 hands=1 hand_recorder=1 voice=1
for arg in "$@"; do
  case $arg in
    --yes) assume_yes=1 ;;
    --no-bluetooth) bluetooth=0 ;;
    --bluetooth) bluetooth=2 ;;
    --no-eye-tracker) eye_tracker=0 eye_recorder=0 ;;
    --no-eye-recorder) eye_recorder=0 ;;
    --no-voice) voice=0 ;;
    --no-hands) hands=0 hand_recorder=0 ;;
    --no-hand-recorder) hand_recorder=0 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

# Only the questions read from the terminal (or whatever stdin is); the build steps get
# no input, so they can't swallow typed-ahead or piped answers.
exec 3<&0 </dev/null

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ask() {  # ask "question" default(y|n)
  [ "$assume_yes" = 1 ] && [ "$2" = y ] && return 0
  [ "$assume_yes" = 1 ] && return 1
  local hint answer
  hint=$([ "$2" = y ] && echo "Y/n" || echo "y/N")
  read -r -p "$1 [$hint] " answer <&3 || answer=
  answer=${answer:-$2}
  [[ $answer =~ ^[Yy] ]]
}
# Can sudo run here without a password prompt (SUDO_ASKPASS, the repo's .env, a recent sudo)?
# See frame_sudo in scripts/_env.sh.
sudo_quiet() {
  { [ "$FRAME_LOCAL" = 1 ] && [ -n "${SUDO_ASKPASS:-}" ]; } && return 0
  grep -q '^steamos_root_pwd=.' "$REPO_ROOT/.env" 2>/dev/null && return 0
  on_frame 'sudo -n true' 2>/dev/null
}
# A release never builds (FRAME_RELEASE, scripts/_env.sh): its programs come in its image.
build() { [ "$FRAME_RELEASE" = 1 ] || "$@"; }
# A part this install skips (or fails to install) can still have units from another Frametop
# folder: the release before, or a clone. They'd go on running that folder's programs, and fail
# once install-release.sh removes it two releases on. They're stopped and removed, and
# the message says how to install the part for this folder.
# drop_other_folder WHAT COMMAND UNIT...
drop_other_folder() {
  local what=$1 how=$2 found
  shift 2
  found=$(on_frame "for u in $*; do f=~/.config/systemd/user/\$u; [ -f \$f ] && ! grep -qF -- $(printf %q "$FRAME_REPO/") \$f && echo \$u; done") || found=
  [ -n "$found" ] || return 0
  on_frame "systemctl --user disable --now $(echo $found) 2>/dev/null; for u in $(echo $found); do rm -f ~/.config/systemd/user/\$u; done; systemctl --user daemon-reload" || true
  echo "$what from another Frametop folder: stopped and removed, so it doesn't run a release that goes later. Install it for this one with: $how"
}
# Does hand tracking's camera broker have its file capabilities? A release brings ft-camd built,
# so a reinstall of the same release doesn't set them again; a clone's build clears them.
camd_has_caps() {
  on_frame "getcap $(printf %q "$FRAME_REPO/hands/build/ft-camd") 2>/dev/null | grep -q cap_sys_ptrace"
}
eyegrab_enabled() { on_frame 'systemctl is-enabled -q frametop-eyegrab.service 2>/dev/null'; }
# Does this user have a password sudo can take? SteamOS starts without one (passwd -S: P has one,
# NP none, L locked). Unknown counts as yes, and sudo says if it isn't.
password_set() {
  local s
  s=$(passwd -S "$(id -un)" 2>/dev/null | awk '{ print $2 }') || s=
  [ "$s" != NP ] && [ "$s" != L ]
}

# sudo_once: ask for the password once, after every question, for all the parts that need it,
# so nothing waits for you later and you can take the headset off. sudo keeps it (its timestamp,
# for this terminal) while a keep-alive refreshes it every FRAMETOP_SUDO_REFRESH seconds; the
# keep-alive ends with this script (the EXIT trap), and sudo -k then forgets it. The installers
# still run their own sudo (frame_sudo in scripts/_env.sh), which then doesn't ask, so each one
# also works on its own. Only on the Frame, with a terminal and without SUDO_ASKPASS: FrameDrop's
# window answers sudo itself (framedrop/installer), and from a PC each sudo has its own ssh -t.
keepalive=
sudo_once() {
  if [ "$FRAME_LOCAL" = 0 ]; then
    [ "$assume_yes" = 1 ] || echo "From a PC each one asks for it over SSH, unless it's in the repo's .env (README)."
    return 0
  fi
  [ -z "${SUDO_ASKPASS:-}" ] || return 0
  { : </dev/tty; } 2>/dev/null || return 0
  if [ "$assume_yes" = 1 ]; then
    sudo -n -v 2>/dev/null || return 0   # --yes never asks: only keep a password sudo has now
  elif ! password_set; then
    echo "Your user has no password, so sudo can't run. Set one in a terminal with passwd, then run this again to add them."
    ask "Go on without them now?" y || exit 1
    drop_sudo_parts "your user has no password (passwd sets one)"
    return 0
  else
    echo "It's asked once, now. sudo keeps it until the install ends, then forgets it."
    if ! sudo -v -p "Your password (sudo): "; then
      ask "sudo didn't take the password. Go on without them?" y || exit 1
      drop_sudo_parts "sudo didn't take the password"
      return 0
    fi
  fi
  local parent=$$ every=${FRAMETOP_SUDO_REFRESH:-50}
  ( while sleep "$every"; do kill -0 "$parent" 2>/dev/null && sudo -n -v 2>/dev/null || exit 0; done ) \
    </dev/null >/dev/null 2>&1 &
  keepalive=$!
  sudo -n true 2>/dev/null ||
    echo "This sudo doesn't keep a password between commands (timestamp_timeout 0), so each part asks for it again."
}
# drop_sudo_parts WHY: skip every picked part that needs sudo, saying why at its step.
drop_sudo_parts() {
  local why=$1
  if [ "$do_hands" = 1 ]; then
    do_hands=0 hands_skip="skipped: $why. Install it later with: hands/run.sh install"
  fi
  if [ "$rec_hands" != 0 ] && ! camd_has_caps; then   # hand tracking was going to set them
    rec_hands=0 rec_hands_skip="$recorder: skipped: $why. Add it later with: hands/rec/install.sh"
  fi
  if [ "$do_tracker" = 1 ]; then
    do_tracker=0 tracker_ours=0
    tracker_skip="skipped: $why, so gaze mode uses SteamVR's eye tracker. Install ours later with: gaze/tracker/install.sh"
  fi
  if [ "$do_bt" = 1 ]; then
    do_bt=0 bt_skip="skipped: $why. Install later with: setup/bluetooth/install.sh install"
  fi
}
# If a step fails, set -e stops here with the steps before it done and the rest still from the
# install before: run the same command again, which does every step again. A signal (Ctrl-C, a
# closed terminal, FrameDrop stopping it) would end bash without the EXIT trap, so each one
# exits instead, and the trap says so too.
# interrupted-install trap: begin
# It also ends the sudo keep-alive (sudo_once, below) and has sudo forget the password.
trap 'rc=$?
if [ -n "${keepalive:-}" ]; then kill "$keepalive" 2>/dev/null || true; sudo -k 2>/dev/null || true; fi
[ "$rc" = 0 ] || echo "install.sh stopped before it finished (exit $rc). Run the same command again to finish the install." >&2' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
# interrupted-install trap: end

if [ "$FRAME_RELEASE" = 1 ]; then
  echo "Installing Frametop $(sed -n 's/^VERSION=//p' "$root/.frametop-release") on this Steam Frame from $FRAME_REPO"
elif [ "$FRAME_LOCAL" = 1 ]; then
  echo "Installing on this Steam Frame from $FRAME_REPO"
else
  echo "Installing on $FRAME_HOST over SSH (repo copy at $FRAME_REPO)"
  "$root/scripts/sync.sh" >/dev/null
fi
if [ "$FRAME_LOCAL" = 0 ] || [ -n "${SSH_CONNECTION:-}" ]; then
  echo "Over SSH: if the connection drops, run this again (it keeps what it downloaded). Services"
  echo "that need SteamVR start with it if it isn't running now."
fi

# Every question comes here, before any step runs, and then the password, once (sudo_once): the
# rest of the install asks nothing, so you can take the headset off while it runs. The questions
# for parts that need your password (sudo) say so. do_PART=1 installs it; PART_skip is what its
# step says instead.
[ "$assume_yes" = 1 ] || step "What to install (every question comes now; the rest runs without asking)"

do_voice=0 voice_skip="skipped. Install later with: voice/run.sh install"
if [ "$voice" = 0 ]; then
  voice_skip="skipped (--no-voice). Install later with: voice/run.sh install"
elif ask "Install voice dictation? A Right Ctrl tap or holding the left d-pad down starts it; it downloads about 320 MB of speech models." y; then
  do_voice=1
fi

# Hand tracking needs sudo once per build of ft-camd, its camera broker (file capabilities to
# borrow XRService's camera buffers).
do_hands=0 hands_skip="skipped. Install later with: hands/run.sh install"
if [ "$hands" = 0 ]; then
  hands_skip="skipped (--no-hands). Install later with: hands/run.sh install"
elif [ "$assume_yes" = 1 ] && ! sudo_quiet; then
  hands_skip="skipped: it needs your password (sudo), and --yes doesn't ask. Install it later with: hands/run.sh install"
elif ask "Install hand tracking? Poke, pinch and grab the screens with your hands. It runs while the Frametop desktop does (change that on the Hands page of Frametop Input Settings). Needs your password (sudo): its camera broker gets file capabilities." y; then
  do_hands=1
fi

# The Hand Recorder (hands/rec): a release brings it built, so it's only ft-camd's capabilities
# (set already if hand tracking installs) and the menu entry. An entry from the release before
# points into that release's folder, which goes two releases on: it's renewed without asking.
# A clone builds it with hands/rec/install.sh (a few minutes), so it isn't offered here.
# rec_hands: 1 add it, 2 renew it.
recorder="the Hand Recorder (records your hands for Frametop's open hand dataset)"
rec_hands=0 rec_hands_skip=
if [ "$FRAME_RELEASE" = 1 ]; then
  rec_hands_skip="$recorder: skipped. Add it later with: hands/rec/install.sh"
  if [ "$hand_recorder" = 0 ]; then
    :
  elif on_frame '[ -e ~/.local/share/applications/frametop-handrec.desktop ]'; then
    rec_hands=2
  elif [ "$assume_yes" = 1 ] && [ "$do_hands" = 0 ] && ! sudo_quiet; then
    rec_hands_skip="$recorder: skipped: it needs your password (sudo), and --yes doesn't ask. Add it later with: hands/rec/install.sh"
  elif ask "Add the Hand Recorder to the app menu? It records your hands for Frametop's open hand dataset, which trains hand tracking; nothing leaves the headset until you press Upload. Needs your password (sudo), as hand tracking does: its camera broker gets file capabilities." y; then
    rec_hands=1
  fi
fi

do_gaze=0
if ask "Install gaze mode? The pointer goes where you look; you turn it on and calibrate it in Frametop Input Settings, on the Gaze page." y; then
  do_gaze=1
fi

# Ours is gaze mode's default once it's installed (GAZE_TRACKER=auto). Without it, gaze mode
# uses SteamVR's eye tracker. Configs made before GAZE_TRACKER=auto say steam, which keeps
# SteamVR's: tracker_ours=1 changes that once ours is in.
do_tracker=0 tracker_ours=0
tracker_skip="skipped: gaze mode uses SteamVR's eye tracker. Install ours later with: gaze/tracker/install.sh"
if [ "$do_gaze" = 0 ]; then
  tracker_skip="skipped: gaze mode isn't installed. Install it later with: gaze/tracker/install.sh"
elif [ "$eye_tracker" = 0 ]; then
  tracker_skip="skipped. Install it later with: gaze/tracker/install.sh"
elif [ "$assume_yes" = 1 ] && ! sudo_quiet; then
  tracker_skip="skipped: it needs your password (sudo), and --yes doesn't ask. Install it later with: gaze/tracker/install.sh"
elif ask "Install our own eye tracker for gaze mode? It's more accurate than SteamVR's$([ "$FRAME_RELEASE" = 1 ] || echo ", and downloads about 165 MB (numpy, OpenCV)"). Needs your password (sudo): its frame grabber is a small system service." y; then
  do_tracker=1
  tracker=$(on_frame "sed -n 's/^GAZE_TRACKER=\([a-z]*\).*/\1/p' ~/.config/frametop.conf 2>/dev/null | tail -1") || tracker=
  if [ "$tracker" = steam ] &&
     ask "Your settings pick SteamVR's eye tracker (GAZE_TRACKER=steam, the old default). Use ours instead, once it's installed?" y; then
    tracker_ours=1
  fi
fi

# The Eye Recorder (gaze/rec), as the Hand Recorder: a release brings it built, so it's only the
# menu entry, which needs our eye tracker's frame grabber. An entry from the release before
# points into that release's folder: it's renewed without asking. A clone builds it with
# gaze/rec/install.sh (a few minutes), so it isn't offered here. rec_eyes: 1 add it, 2 renew it.
eye_rec="the Eye Recorder (records your eyes for Frametop's eye tracking dataset)"
rec_eyes=0 rec_eyes_skip=
if [ "$FRAME_RELEASE" = 1 ]; then
  rec_eyes_skip="$eye_rec: skipped. Add it later with: gaze/rec/install.sh"
  if [ "$eye_recorder" = 0 ]; then
    :
  elif on_frame '[ -e ~/.local/share/applications/frametop-eyerec.desktop ]'; then
    rec_eyes=2
  elif [ "$do_tracker" = 0 ] && ! eyegrab_enabled; then
    rec_eyes_skip="$eye_rec: skipped: it needs our eye tracker. Install both later with: gaze/tracker/install.sh, then gaze/rec/install.sh"
  elif ask "Add the Eye Recorder to the app menu? It records your eyes for Frametop's eye tracking dataset, which trains our eye tracker; nothing leaves the headset until you press Upload, and uploads are encrypted." y; then
    rec_eyes=1
  fi
fi

do_bt=0 bt_skip="skipped. Install later with: setup/bluetooth/install.sh install"
if [ "$bluetooth" = 2 ] && [ "$assume_yes" = 1 ] && ! sudo_quiet; then
  bt_skip="skipped: they need your password (sudo), and --yes doesn't ask. Install later with: setup/bluetooth/install.sh install"
elif [ "$bluetooth" = 2 ] || { [ "$bluetooth" = 1 ] && ask "Install the Bluetooth fixes? They let LE mice and keyboards like the Swiftpoint Z3 reconnect. Needs your password (sudo): they're system files and a system service." n; }; then
  do_bt=1
fi

do_restart=0
if ask "Restart SteamVR when the install finishes? It has to restart once, to load the 3D mouse driver; that closes everything open in VR, this terminal too if it's in a VR desktop." n; then
  do_restart=1
fi

# The picked parts that need sudo, then the password, once.
sudo_parts=()
if [ "$do_hands" = 1 ] && { [ "$FRAME_RELEASE" = 0 ] || ! camd_has_caps; }; then
  sudo_parts+=("hand tracking: file capabilities for its camera broker (setcap)")
fi
if [ "$rec_hands" != 0 ] && [ "$do_hands" = 0 ] && ! camd_has_caps; then
  sudo_parts+=("the Hand Recorder: file capabilities for hand tracking's camera broker (setcap)")
fi
if [ "$do_tracker" = 1 ]; then
  sudo_parts+=("our eye tracker: its frame grabber, a system service (/etc/frametop, /etc/systemd/system)")
fi
if [ "$do_bt" = 1 ]; then
  sudo_parts+=("the Bluetooth fixes: system files and a system service (/etc/steamframe, /etc/systemd/system)")
fi
if [ "${#sudo_parts[@]}" -gt 0 ]; then
  if [ "$assume_yes" = 0 ]; then
    echo
    echo "These need your password (sudo):"
    printf '  - %s\n' "${sudo_parts[@]}"
  fi
  sudo_once
fi

step "1/12 distrobox (container tool, installed in your home folder)"
if on_frame 'test -x ~/.local/bin/distrobox'; then
  echo "already installed: $(on_frame '~/.local/bin/distrobox version | head -1')"
elif [ "$FRAME_RELEASE" = 1 ]; then
  # The release's image brings the distrobox it was tested with (pack/Containerfile).
  on_frame 'cd pack/build/distrobox && ./install --prefix ~/.local'
else
  on_frame 'set -e; mkdir -p ~/dev/src
# A tested release, so upstream changes cannot break new installs: 1.8.2.5, by its commit (the
# one pack/Containerfile fetches).
if [ ! -d ~/dev/src/distrobox ]; then
  d=~/dev/src/distrobox.part.$$
  git init -q $d
  git -C $d fetch -q --depth 1 https://github.com/89luca89/distrobox.git 40c3cd724faa434aeb0a23e28776665b92de68bd
  git -C $d checkout -q FETCH_HEAD
  mv $d ~/dev/src/distrobox
fi
cd ~/dev/src/distrobox && ./install --prefix ~/.local'
fi

if [ "$FRAME_RELEASE" = 1 ]; then
  step "2/12 Frametop's container (from this release's image)"
  "$root/pack/release-box.sh"
else
  step "2/12 build container (Fedora 44 'dev', about 1-2 GB the first time)"
  "$root/setup/dev-container.sh"
fi

step "3/12 input relay (keeps Bluetooth mice working in SteamVR, device roles, button maps)"
"$root/desktops.sh" relay install

step "4/12 3D mouse: SteamVR driver"
build "$root/pointer/driver/build.sh"
"$root/pointer/driver/install.sh" install

step "5/12 3D mouse: pointer helper service, and the dashboard patch that lets it move SteamVR's windows"
build "$root/pointer/helper/build.sh"
"$root/pointer/helper/run.sh" install
"$root/pointer/dashpatch/run.sh" install

step "6/12 power service (turns the displays off while the headset isn't used, even on a stand)"
build "$root/power/build.sh"
"$root/power/run.sh" install

step "7/12 multi-screen desktop (ft-screens), its keyboard (Full Keyboard), Frametop Input Settings, Display Settings, Remote Displays, and the Tour"
build "$root/screens/build.sh"
build "$root/layout/countdown/build.sh"  # visible three-second reset countdown
build "$root/stream/build.sh"  # ft-stream: remote displays (Frametop Remote Displays)
build "$root/keyboard/build.sh"  # Frametop's keyboard: Full Keyboard, by TaiKeid (keyboard/FRAMETOP.md)
"$root/desktops.sh" install >/dev/null
"$root/input-settings/install.sh"
"$root/display-settings/install.sh"
"$root/tour/install.sh"
"$root/remote-displays/install.sh"
"$root/remote/install.sh"
"$root/keyboard/run.sh" install
on_frame "sed -i 's/^POINTER=0/POINTER=1/' ~/.config/frametop.conf; grep -q '^POINTER=' ~/.config/frametop.conf || echo 'POINTER=1' >> ~/.config/frametop.conf"
echo "the launcher's Desktop entry now opens the multi-screen desktop; 3D mouse on (POINTER=1 in ~/.config/frametop.conf)"
"$root/scripts/conf-migrate.sh"

step "8/12 voice dictation (optional: speak, and it's typed where you type; nothing leaves the headset)"
voice_units="frametop-voice.service frametop-voice-whisper.service"
if [ "$do_voice" = 1 ]; then
  build "$root/voice/build.sh"
  if ! "$root/voice/run.sh" install; then
    echo "voice dictation: not installed (above). Try again later with: voice/run.sh install"
    drop_other_folder "voice dictation" "voice/run.sh install" $voice_units
  fi
else
  echo "$voice_skip"
  drop_other_folder "voice dictation" "voice/run.sh install" $voice_units
fi

# Installed, hand tracking runs with the Frametop desktop (HANDS_MODE=frametop), unless the
# settings already pick always.
step "9/12 hand tracking (optional, experimental: your hands work the screens; it uses a lot of the headset's CPU)"
hand_units="frametop-camd.service frametop-hands.service frametop-handview.service"
hands_done=0
if [ "$do_hands" = 1 ]; then
  if "$root/hands/run.sh" install; then
    hands_done=1
    mode=$(on_frame "sed -n 's/^HANDS_MODE=\([a-z]*\).*/\1/p' ~/.config/frametop.conf | tail -1") || mode=
    if [ "$mode" = always ]; then
      echo "hand tracking runs whenever SteamVR does (HANDS_MODE=always, kept from your settings)"
    else
      if on_frame "python3 $(printf %q "$FRAME_REPO/hands/handsmode.py") set frametop" >/dev/null; then
        echo "hand tracking runs while the Frametop desktop does (HANDS_MODE=frametop)"
      else
        echo "hand tracking is installed, but HANDS_MODE didn't change: choose With Frametop on the Hands page"
      fi
    fi
  else
    echo "hand tracking: not installed (above). Try again later with: hands/run.sh install"
  fi
else
  echo "$hands_skip"
fi
if [ "$hands_done" = 0 ]; then
  drop_other_folder "hand tracking" "hands/run.sh install" $hand_units
  # its ft-handsctl link too, as hands/run.sh uninstall does
  on_frame "l=~/.local/bin/ft-handsctl; [ -e ~/.config/systemd/user/frametop-hands.service ] || [ ! -L \$l ] ||
case \$(readlink \$l) in $(printf %q "$FRAME_REPO")/*) ;; *) rm -f \$l ;; esac" || true
fi
if [ "$rec_hands" = 2 ]; then
  "$root/hands/rec/install.sh" install >/dev/null && echo "$recorder: updated" ||
    echo "$recorder: couldn't update it (above). Try again with: hands/rec/install.sh"
elif [ "$rec_hands" = 1 ]; then
  "$root/hands/rec/install.sh" install ||
    echo "$recorder: not installed (above). Try again later with: hands/rec/install.sh"
elif [ -n "$rec_hands_skip" ]; then
  echo "$rec_hands_skip"
fi

step "10/12 gaze mode (optional, experimental: the pointer goes where you look)"
if [ "$do_gaze" = 1 ]; then
  "$root/gaze/run.sh" install
else
  echo "skipped. Install later with: gaze/run.sh install"
fi

step "11/12 our own eye tracker for gaze mode (recommended: more accurate than SteamVR's)"
if [ "$do_tracker" = 1 ]; then
  if "$root/gaze/tracker/install.sh" install; then
    if [ "$tracker_ours" = 1 ]; then
      on_frame "sed -i 's~^GAZE_TRACKER=steam\b.*~GAZE_TRACKER=auto          # gaze service: auto = our own eye tracker when installed, else SteamVR | own | steam~' ~/.config/frametop.conf"
    fi
    tracker=$(on_frame "sed -n 's/^GAZE_TRACKER=\([a-z]*\).*/\1/p' ~/.config/frametop.conf | tail -1")
    if [ "$tracker" = steam ]; then
      echo "installed; your settings keep SteamVR's eye tracker (pick Own tracker on the Gaze page to use ours)"
    else
      echo "gaze mode uses our eye tracker: calibrate it once, with Calibrate on the Gaze page"
    fi
  else
    echo "our eye tracker didn't install, so gaze mode uses SteamVR's. Try again with: gaze/tracker/install.sh"
  fi
else
  echo "$tracker_skip"
fi
if [ "$rec_eyes" = 2 ]; then
  "$root/gaze/rec/install.sh" install >/dev/null && echo "$eye_rec: updated" ||
    echo "$eye_rec: couldn't update it (above). Try again with: gaze/rec/install.sh"
elif [ "$rec_eyes" = 1 ] && ! eyegrab_enabled; then
  echo "$eye_rec: skipped: it needs our eye tracker, which isn't installed. Install both later with: gaze/tracker/install.sh, then gaze/rec/install.sh"
elif [ "$rec_eyes" = 1 ]; then
  "$root/gaze/rec/install.sh" install ||
    echo "$eye_rec: not installed (above). Try again later with: gaze/rec/install.sh"
elif [ -n "$rec_eyes_skip" ]; then
  echo "$rec_eyes_skip"
fi

step "12/12 Bluetooth fixes (optional; they let LE mice and keyboards like the Swiftpoint Z3 reconnect)"
if [ "$do_bt" = 1 ]; then
  "$root/setup/bluetooth/install.sh" install
else
  echo "$bt_skip"
fi

# A release's install.sh is also how you go back to it (pack/README.md), so it makes its own
# folder "current": fast-install, get.sh and the next install-release.sh go by that link.
# install-release.sh sets it too, after this.
if [ "$FRAME_RELEASE" = 1 ] && [ "$FRAME_LOCAL" = 1 ]; then
  version=$(sed -n 's/^VERSION=//p' "$root/.frametop-release")
  here=$(cd "$root" && pwd -P)
  if [ -n "$version" ] && [ "${here##*/}" = "$version" ]; then
    ln -sfn "$version" "${here%/*}/current"
    echo "Frametop $version is the current release (${here%/*}/current)"
  fi
fi

step "Done"
cat <<'EOF'
SteamVR has to restart once, to load the 3D mouse driver and to start the input relay
before it. Restarting SteamVR closes everything open in VR, including this terminal if
it's in a VR desktop. Rebooting the headset works too.

Recommended: in Frametop Display Settings > Power, choose when the displays turn off
while the headset isn't used (for a stand or mount that covers its proximity sensor), and
turn on Stay awake while plugged in, so Steam doesn't put the headset to sleep while it
charges. The displays still turn off when you take the headset off.

If an install or update is ever interrupted, run the same command again: it finishes it.
EOF
if [ "$do_restart" = 1 ]; then
  echo "Restarting SteamVR, as you picked at the start."
  # sudo forgets the password first: the restart can close this terminal.
  if [ -n "$keepalive" ]; then kill "$keepalive" 2>/dev/null || true; sudo -k 2>/dev/null || true; keepalive=; fi
  on_frame 'systemctl --user restart steamvr.service'
fi
