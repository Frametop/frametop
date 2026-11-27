#!/usr/bin/env bash
# Uninstall Frametop from the Steam Frame. In a terminal on the headset (Konsole in the desktop,
# or over SSH):
#
#   curl -fsSL https://frametop.github.io/frametop/uninstall.sh | bash
#
# or ~/frametop/uninstall.sh. It doesn't use the rest of the repo, so it also works when
# ~/frametop is gone or broken.
#
# It never stops what you're using now: the input relay carries the keyboard and mouse, and the
# desktop runs from the repo. So it goes in two steps:
#   1. Frametop stops starting. The launcher's Desktop entry goes back to the stock desktop, and
#      Frametop's Native Desktop entry, services, SteamVR driver, menu entries, and system files
#      (our eye tracker's frame grabber, NPU hand tracking's ft-npud, which it stops, and the Bluetooth
#      fixes, with sudo) are removed, and so
#      are the file capabilities of hand tracking's camera broker (ft-camd, with sudo). The
#      tracking cameras go back to SteamVR's own mode if 60 fps hand tracking changed it. What
#      runs now keeps running until you restart the headset.
#   2. After the restart, run it again. It deletes the code (~/frametop, or the releases in
#      ~/.local/share/frametop/releases with their containers and images) and, if you want, your
#      settings, voice dictation's speech models, the avatar and hand models you imported, and
#      the build container.
# When nothing of Frametop is running, one run does both.
#
# Options (piped, they go after "bash -s --"):
#   --dir DIR    where the repo is, if not ~/frametop
#   --dry-run    show what it would do, and change nothing
#   --yes        ask nothing, and need no terminal: each question takes its default, or what
#                --delete and --keep say. sudo asks SUDO_ASKPASS (sudo -A) when it's set. It ends
#                with exit status 3 when step 1 is done and the headset has to restart before
#                step 2, and 5 when something that needs sudo couldn't be removed.
#   --delete KEY, --keep KEY   with --yes, the answer to one question: releases, code, settings,
#                voice, avatars, recordings, container or distrobox
#   --plan       change nothing and print only what there is to do, one line each (the Frametop
#                Installer's window reads it): "step1 yes|no", "sudo yes|no", "running NAMES", and
#                "ask KEY DEFAULT SIZE" for each question step 2 would ask
#   --from-installer   run by the Frametop Installer: leave its cache (~/.cache/frametop-installer),
#                which holds this script, its log and its result
set -euo pipefail
shopt -s nullglob

usage() {
  cat <<'EOF'
usage: uninstall.sh [--dir DIR] [--dry-run] [--yes [--delete KEY]... [--keep KEY]...] [--plan] [--from-installer]
piped: curl -fsSL https://frametop.github.io/frametop/uninstall.sh | bash -s -- [options]
EOF
}

dry=0 yes=0 plan=0 from_installer=0
declare -A choice=()  # --delete and --keep: KEY -> y or n
apps=$HOME/.local/share/applications
override=$apps/deckard-nested-desktop.desktop
native_copy=$apps/native-deckard-nested-desktop.desktop
relay_unit=$HOME/.config/systemd/user/frametop-input-relay.service
driver=$HOME/.local/share/frametop/ft_pointer
releases=$HOME/.local/share/frametop/releases  # pack/install-release.sh
vrpathreg=/opt/steamvr/bin/linuxarm64/vrpathreg
handsctl=$HOME/.local/bin/ft-handsctl
cmd_link=$HOME/.local/bin/ft-cmd  # commands/ft_commands.py: the relay links it, and your commands' aliases call it
ondesktop_link=$HOME/.local/bin/ft-ondesktop  # the same: Steam games' launch options call it (steam/ft-ondesktop)
bashrc_mark='# Frametop commands as shell aliases'  # ...and ~/.bashrc's two lines that read those aliases
eye_rate=$HOME/.local/state/frametop/gaze/steamvr-eye-rate  # gaze/eye-rate.py turned the setting on
dash_debugger=$HOME/.local/state/frametop/pointer/dashboard-debugger  # pointer/dashpatch/ft-dashpatch.py turned it on
xr_settings=$HOME/.config/openvr/config/cv/xrservice/XRServiceSettings.json  # SteamVR's; hands/handsrate.py sets the cameras' mode in it
hands_rate=  # 1 when that mode is on (the 60 fps switch: the controllers don't track) and handsrate.py can take it out
eyegrab_files=(/etc/systemd/system/frametop-eyegrab.service /etc/frametop/ft-eyegrab
               /etc/atomic-update.conf.d/frametop-eyegrab.conf)
# NPU hand tracking (npu/root.sh's ETCUNIT and TREE; npu/tests/test_root.py checks they match): its unit, and its
# tree on the home partition, which the unit binds at /var/lib/ft-npud
npu_unit=/etc/systemd/system/ft-npud.service
npu_tree=/home/.steamos/offload/var/lib/ft-npud
bt_files=(/etc/systemd/system/steamframe-bt-fixups.service /etc/systemd/system/bluetooth.service.d/steamframe.conf
          /etc/steamframe/bt-fixups.sh /etc/atomic-update.conf.d/frametop-bluetooth.conf)

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
run() {  # run a command, or with --dry-run, show it
  if [ "$dry" = 1 ]; then printf '  would run: %s\n' "$*"; else "$@"; fi
}
ask() {  # ask KEY "question" default(y|n) [SIZE]
  local key=$1 q=$2 def=$3 hint answer
  if [ "$plan" = 1 ]; then printf 'ask %s %s %s\n' "$key" "$def" "${4:--}" >&3; return 0; fi
  if [ "$yes" = 1 ]; then
    answer=${choice[$key]:-$def}
    echo "$q [$([[ $answer = y ]] && echo yes || echo no)]"
  elif [ "$dry" = 1 ]; then
    echo "$q [dry run: yes]"; return 0
  else
    hint=$([ "$def" = y ] && echo "Y/n" || echo "y/N")
    read -r -p "$q [$hint] " answer </dev/tty || answer=
    answer=${answer:-$def}
  fi
  [[ $answer =~ ^[Yy] ]]
}
said() { [ "$plan" = 1 ] && printf '%s\n' "$*" >&3; return 0; }  # a --plan line
sudo_() { if [ -n "${SUDO_ASKPASS:-}" ]; then run sudo -A "$@"; else run sudo "$@"; fi; }
exists() { local f; for f in "$@"; do [ -e "$f" ] && return 0; done; return 1; }
size() { du -shc "$@" 2>/dev/null | tail -1 | cut -f1; }

# Is this folder Frametop's code? Only then is it deleted.
is_repo() { [ "$1" != "$HOME" ] && [ -f "$1/desktops.sh" ] && [ -f "$1/session/frametop-session.sh" ]; }
find_repo() {
  local p
  [ -n "$dir" ] && { echo "$dir"; return; }
  # The launcher entry and the relay's unit point into the repo, until step 1 removes them.
  p=$(sed -n 's#^Exec=\(.*\)/session/frametop-session\.sh.*#\1#p' "$override" 2>/dev/null | head -1)
  [ -z "$p" ] && p=$(sed -n 's#^ExecStart=/usr/bin/python3 \(.*\)/input/input-relay\.py.*#\1#p' "$relay_unit" 2>/dev/null | head -1)
  [ -z "$p" ] && [ -f "${BASH_SOURCE[0]:-}" ] && p=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
  echo "${p:-$HOME/frametop}"
}

# Frametop's menu entries in DIR, one a line: every ft-*.desktop and frametop-*.desktop (the apps,
# the recorders, the gaze bench, and one frametop-profile-* per layout profile), and nothing else.
desktop_entries() {
  local f
  for f in "$1"/ft-*.desktop "$1"/frametop-*.desktop; do printf '%s\n' "$f"; done
}

# 60 fps hand tracking's camera mode, in SteamVR's settings: "1" when it's on and REPO's
# handsrate.py can take it out, "manual" when a camera mode is set and that code is gone or
# doesn't run (a broken checkout).
hands_rate_state() {  # hands_rate_state REPO
  local st
  if [ -f "$1/hands/handsrate.py" ] && st=$(python3 "$1/hands/handsrate.py" status --json 2>/dev/null); then
    grep -q '"on": true' <<<"$st" && echo 1
  elif grep -qs '"slamFrameFPSMode"' "$xr_settings"; then
    echo manual
  fi
  return 0
}

# Frametop's programs that are running now (step 1 leaves them running until the restart).
running() {
  local n out=()
  for n in ft-screens ft-pointer ft-powerd ft-eyegrab ft-camd ft-hands ft-voiced whisper-server framekeyboard ft-together ft-roomvoice; do
    pgrep -x "$n" >/dev/null && out+=("$n")
  done
  pgrep -f '[i]nput/input-relay\.py' >/dev/null && out+=("input relay")
  pgrep -f '[k]eyboard/host/ft-kbhost' >/dev/null && out+=("ft-kbhost")
  pgrep -f '[g]aze/ft-gazed' >/dev/null && out+=("gaze service")
  pgrep -f '[f]rametop-session\.sh' >/dev/null && out+=("desktop session")
  echo "${out[*]}"
}

# distrobox's containers besides Frametop's: its releases' (frametop-*) unless KEEP_RELEASES is 1,
# and the build container (dev) unless KEEP_DEV is 1. One a line.
other_boxes() {  # other_boxes KEEP_RELEASES KEEP_DEV
  local n
  command -v podman >/dev/null || return 0
  while read -r n; do
    [ -n "$n" ] || continue
    [[ $n == frametop-* && $1 != 1 ]] && continue
    [[ $n == dev && $2 != 1 ]] && continue
    echo "$n"
  done < <(podman ps -a --filter label=manager=distrobox --format '{{.Names}}' 2>/dev/null)
}

main() {
  local units=() entries=() names=() sys=0 bashrc_lines=0 repo now f sz left=0 kept_releases=0 kept_dev=0
  while [ $# -gt 0 ]; do
    case $1 in
      --dir) dir=${2:?--dir needs a folder}; shift ;;
      --dry-run) dry=1 ;;
      --yes) yes=1 ;;
      --delete|--keep)
        [[ ${2:-} =~ ^(releases|code|settings|voice|avatars|recordings|container|distrobox)$ ]] ||
          { echo "$1 takes releases, code, settings, voice, avatars, recordings, container or distrobox" >&2; return 2; }
        choice[$2]=$([ "$1" = --delete ] && echo y || echo n); shift ;;
      --plan) plan=1 dry=1 yes=1 ;;
      --from-installer) from_installer=1 ;;
      -h|--help) usage; return 0 ;;
      *) echo "unknown option: $1" >&2; usage >&2; return 2 ;;
    esac
    shift
  done

  if ! { grep -qx 'ID=steamos' /etc/os-release && grep -qE '^VARIANT_ID="?vr"?$' /etc/os-release; } 2>/dev/null; then
    echo "This uninstalls Frametop from a Steam Frame (SteamOS, VR variant). Run it in a terminal on the headset." >&2
    return 1
  fi
  if [ "$dry" = 0 ] && [ "$yes" = 0 ] && ! { : </dev/tty; } 2>/dev/null; then
    echo "This asks questions, and there's no terminal to ask in. Run it in one (over SSH: ssh -t), or add --yes." >&2
    return 1
  fi
  # --plan: only its own lines, on what was stdout (fd 3)
  [ "$plan" = 1 ] && exec 3>&1 >/dev/null 2>&1
  [ "$dry" = 1 ] && echo "Dry run: nothing changes."
  repo=$(find_repo)
  camd=$repo/hands/build/ft-camd
  camd_caps=$(getcap "$camd" 2>/dev/null || true)

  # Step 1: what makes Frametop start. Nothing here stops a running program.
  [ -f "$override" ] && grep -q 'Frametop' "$override" || override=
  [ -f "$native_copy" ] || native_copy=
  units=("$HOME"/.config/systemd/user/frametop-*.service)
  mapfile -t entries < <(desktop_entries "$apps")
  [ -L "$handsctl" ] || handsctl=
  [ -L "$cmd_link" ] || cmd_link=
  [ -L "$ondesktop_link" ] || ondesktop_link=
  grep -qF "$bashrc_mark" "$HOME/.bashrc" 2>/dev/null && bashrc_lines=1
  [ -f "$eye_rate" ] && [ -f "$repo/gaze/eye-rate.py" ] || eye_rate=
  [ -f "$dash_debugger" ] && [ -f "$repo/pointer/dashpatch/ft-dashpatch.py" ] || dash_debugger=
  hands_rate=$(hands_rate_state "$repo")
  if [ "$hands_rate" = manual ]; then
    hands_rate=
    echo "note: SteamVR's tracking cameras have a mode set in $xr_settings (slamFrameFPSMode), as 60 fps hand" >&2
    echo "  tracking sets it, and Frametop's code in $repo is gone or doesn't run, so it stays. If the controllers don't track," >&2
    echo "  take streamingMode and slamFrameFPSMode out of captureSessionSettings.trackingCameras in that file," >&2
    echo "  then restart SteamVR." >&2
  fi
  exists "${eyegrab_files[@]}" "${bt_files[@]}" "$npu_unit" "$npu_tree" && sys=1
  said sudo "$([ "$sys" = 1 ] || [ -n "$camd_caps" ] && echo yes || echo no)"

  if [ -n "$override" ] || [ -n "$native_copy" ] || [ ${#units[@]} -gt 0 ] || [ ${#entries[@]} -gt 0 ] ||
     [ -d "$driver" ] || [ -n "$handsctl" ] || [ -n "$cmd_link" ] || [ -n "$ondesktop_link" ] || [ "$bashrc_lines" = 1 ] || [ "$sys" = 1 ] || [ -n "$camd_caps" ] || [ -n "$eye_rate" ] || [ -n "$dash_debugger" ] || [ -n "$hands_rate" ]; then
    said step1 yes
    step "Step 1 of 2: stop Frametop from starting"
    echo "This removes:"
    [ -n "$override" ] && echo "  - the launcher's Desktop entry (Launch a program -> Desktop opens the stock desktop again)"
    [ -n "$native_copy" ] && echo "  - the launcher's Native Desktop entry (Desktop opens the same stock desktop then)"
    for f in "${units[@]}"; do echo "  - the service $(basename "$f")"; done
    [ -d "$driver" ] && echo "  - the 3D mouse's SteamVR driver (ft_pointer)"
    [ ${#entries[@]} -gt 0 ] && echo "  - ${#entries[@]} menu entries (Frametop Settings, Reset Screen Layout, ...)"
    [ -n "$handsctl" ] && echo "  - $handsctl"
    [ -n "$cmd_link" ] && echo "  - $cmd_link (your commands' aliases stop working)"
    [ -n "$ondesktop_link" ] && echo "  - $ondesktop_link (take it out of your Steam games' launch options)"
    [ "$bashrc_lines" = 1 ] && echo "  - the two lines in ~/.bashrc that read your commands' aliases"
    [ -n "$eye_rate" ] && echo "  - gaze mode's SteamVR setting for 90 fps eye cameras (steamvr.gazeDepEyeToHeadEnable2, or gazeDepEyeToHeadEnable before SteamOS 0.5)"
    [ -n "$dash_debugger" ] && echo "  - SteamVR's dashboard debugger, which the 3D mouse's window moves use (VRWebHelper.DebuggerEnabled)"
    [ -n "$hands_rate" ] && echo "  - 60 fps hand tracking's camera mode in SteamVR's XRServiceSettings.json (the controllers track again after the restart)"
    [ -n "$camd_caps" ] && echo "  - the hand camera broker's file capabilities (needs your password)"
    exists "${eyegrab_files[@]}" && echo "  - our eye tracker's frame grabber (a system service: needs your password)"
    exists "${bt_files[@]}" && echo "  - the Bluetooth fixes (system files: needs your password)"
    exists "$npu_unit" "$npu_tree" && echo "  - NPU hand tracking (ft-npud, a system service: it stops now, and hand tracking goes on with the CPU; needs your password)"
    echo "What runs now keeps running until you restart the headset, so your keyboard, mouse, and"
    echo "this terminal keep working. Your settings and the code stay for now."
    if [ "$yes" = 1 ]; then echo "Uninstalling Frametop."
    else ask frametop "Uninstall Frametop?" n || { echo "Nothing changed."; return 0; }; fi

    [ -n "$override" ] && run rm -f "$override"
    [ -n "$native_copy" ] && run rm -f "$native_copy"
    if [ ${#units[@]} -gt 0 ]; then
      for f in "${units[@]}"; do names+=("$(basename "$f")"); done
      run systemctl --user disable "${names[@]}" 2>/dev/null || true
      run rm -f "${units[@]}"
      run systemctl --user daemon-reload
    fi
    if [ -d "$driver" ]; then
      if [ -x "$vrpathreg" ]; then
        LD_LIBRARY_PATH=$(dirname "$vrpathreg") run "$vrpathreg" removedriver "$driver" ||
          echo "warning: SteamVR's vrpathreg couldn't unregister the driver; SteamVR may log that it's missing" >&2
      fi
      run rm -rf "$driver"
    fi
    [ ${#entries[@]} -gt 0 ] && run rm -f "${entries[@]}"
    [ -n "$handsctl" ] && run rm -f "$handsctl"
    [ -n "$cmd_link" ] && run rm -f "$cmd_link"
    [ -n "$ondesktop_link" ] && run rm -f "$ondesktop_link"
    [ "$bashrc_lines" = 1 ] && run sed -i "/^$bashrc_mark/{N;/aliases\.sh\$/d}" "$HOME/.bashrc"
    [ -n "$eye_rate" ] && { run python3 "$repo/gaze/eye-rate.py" off ||
      echo "warning: SteamVR's eye camera setting stays on. Turn it off with: python3 $repo/gaze/eye-rate.py off" >&2; }
    [ -n "$dash_debugger" ] && { run python3 -I "$repo/pointer/dashpatch/ft-dashpatch.py" off ||
      echo "warning: SteamVR's dashboard debugger stays on. Turn it off with: python3 -I $repo/pointer/dashpatch/ft-dashpatch.py off" >&2; }
    [ -n "$hands_rate" ] && { run python3 "$repo/hands/handsrate.py" off ||
      echo "warning: the tracking cameras stay in 60 fps hand tracking's mode, where the controllers don't track. Put SteamVR's own back with: python3 $repo/hands/handsrate.py off" >&2; }
    if [ "$sys" = 1 ]; then
      echo "The system files need your password (sudo)."
      # $1 is ft-camd when it has capabilities to remove, else empty; $2 ft-npud's tree; the system files follow.
      if ! sudo_ bash -c '
        camd=$1 npu_tree=$2; shift 2
        for u in frametop-eyegrab steamframe-bt-fixups; do systemctl disable $u.service 2>/dev/null; done
        systemctl disable --now ft-npud.service 2>/dev/null
        rm -f "$@"
        rm -rf "$npu_tree" "$npu_tree.prev" "$npu_tree.new"
        rmdir /var/lib/ft-npud /var/lib/ft-npud.prev /var/lib/ft-npud.new 2>/dev/null
        rmdir /etc/frametop /etc/steamframe /etc/systemd/system/bluetooth.service.d 2>/dev/null
        [ -n "$camd" ] && [ -x "$camd" ] && setcap -r "$camd" 2>/dev/null
        systemctl daemon-reload; true' sys "${camd_caps:+$camd}" "$npu_tree" "${eyegrab_files[@]}" "${bt_files[@]}" "$npu_unit"; then
        echo "warning: the system files weren't removed (no password?). Run this again to retry." >&2
        left=1
      fi
    elif [ -n "$camd_caps" ]; then
      echo "ft-camd's file capabilities need your password (sudo) to remove."
      sudo_ setcap -r "$camd" ||
        { echo "warning: ft-camd keeps its capabilities (no password?). Run this again to retry." >&2; left=1; }
    fi
    [ "$dry" = 1 ] || echo "Frametop no longer starts."
  else
    said step1 no
  fi

  # Step 2: delete what's left, once nothing of Frametop runs.
  now=$(running)
  [ -n "$now" ] && said running "$now"
  if [ -n "$now" ]; then
    step "Restart the headset to finish"
    echo "Still running from before: $now."
    echo "They stop when the headset restarts. After that, run this again to delete the code"
    echo "($repo) and, if you want, your settings and the build container:"
    echo
    if [ "$repo" = "$HOME/frametop" ]; then
      echo "  curl -fsSL https://frametop.github.io/frametop/uninstall.sh | bash"
    else
      echo "  curl -fsSL https://frametop.github.io/frametop/uninstall.sh | bash -s -- --dir $(printf %q "$repo")"
    fi
    echo
    if [ "$yes" = 1 ]; then
      [ "$dry" = 1 ] || return 3   # the window offers the restart
    elif ask restart "Restart the headset now? This closes everything open, in VR and on the desktop." n; then
      run systemctl reboot || echo "Couldn't restart it from here: restart the headset from Steam's power menu." >&2
    fi
    [ "$dry" = 1 ] || return 0
    echo "Dry run: after the restart, step 2 would go like this."
  fi

  step "Step 2 of 2: delete what's left"
  if [ -d "$releases" ]; then
    local boxes=() images=() b
    for f in "$releases"/*/.frametop-release; do
      boxes+=("$(sed -n 's/^BOX=//p' "$f")")
      images+=("$(sed -n 's/^IMAGE=//p' "$f")")
    done
    echo "Frametop's releases: their files in $releases ($(size "$releases")), and the container and"
    echo "image of each (${#boxes[@]}; an image is about 3 GB). A downloaded Frametop.zip stays where it is."
    if ask releases "Delete the releases, their containers, and their images?" y "$(size "$releases")"; then
      for b in "${boxes[@]}"; do
        [[ $b =~ ^frametop-[A-Za-z0-9_.-]+$ ]] && { run podman rm -f "$b" 2>/dev/null || true; }
      done
      for b in "${images[@]}"; do
        [[ $b == localhost/frametop:* ]] && { run podman rmi "$b" 2>/dev/null || true; }
      done
      run rm -rf "$releases"
    else
      kept_releases=1
    fi
  fi
  if [[ $repo == "$releases"/* ]]; then
    :  # a release: above
  elif [ -d "$repo" ] && is_repo "$repo"; then
    if [ -f "$repo/.git" ]; then
      echo "Leaving $repo: it's a git worktree. Remove it with git worktree remove."
    else
      local def=y
      if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ] ||
         [ -n "$(git -C "$repo" log --branches --not --remotes --oneline 2>/dev/null | head -1)" ]; then
        echo "$repo has changes of its own (git -C $repo status)."
        def=n
      fi
      sz=$(size "$repo")
      if ask code "Delete the Frametop code in $repo ($sz)?" "$def" "$sz"; then
        cd "$HOME"
        run rm -rf "$repo"
      fi
    fi
  elif [ -e "$repo" ]; then
    echo "Leaving $repo: it doesn't look like Frametop's code."
  fi

  # Rebuilt by the desktop at each start, so nothing to keep.
  run rm -rf "$HOME/.local/share/frametop/apps" "$HOME/.local/share/kwin/decorations/kwin4_decoration_qml_frametop" \
    "$HOME/.cache/frametop" "$HOME/.cache/frametop-framedrop"
  # The Frametop Installer's scripts, log and state; its window, running this, removes them itself.
  [ "$from_installer" = 1 ] || run rm -rf "$HOME/.cache/frametop-installer"

  local settings=("$HOME"/.config/frametop.conf* "$HOME"/.config/frametop-*.json* "$HOME/.config/frametop-tiles.txt" "$HOME"/.config/frametop-palm.txt* "$HOME/.config/frametop-remote"
                  "$HOME/.config/frametop-keyboard" "$HOME/.config/frametop" "$HOME/.local/state/frametop" "$HOME/.local/share/frametop-stream"
                  "$HOME/.local/state/frametop-trial" "$HOME/.local/share/frametop/wallpaper")
  local kept=()
  for f in "${settings[@]}"; do [ -e "$f" ] && kept+=("$f"); done
  if [ ${#kept[@]} -gt 0 ]; then
    echo "Your settings: the screen layout and profiles, the dashboard tiles, the palm menu, button maps, the keyboard's, gaze calibration, the remote"
    echo "desktop password, the remote displays' pairings and sign-ins (their computers keep Frametop's"
    echo "clients and token until you remove them in their Web UI), the stereo wallpaper, and the Frametop"
    echo "desktop's own Plasma setup (${kept[*]/#$HOME/\~})."
    ask settings "Delete your settings too? Keep them to pick up where you left off if you reinstall." n \
      "$(size "${kept[@]}")" &&
      run rm -rf "${kept[@]}"
  fi

  # Frametop Voice's speech models (voice/models.sh): downloaded again by an install.
  if [ -d "$HOME/.local/share/frametop/voice" ]; then
    sz=$(size "$HOME/.local/share/frametop/voice")
    ask voice "Delete voice dictation's speech models ($sz; an install downloads them again)?" y "$sz" &&
      run rm -rf "$HOME/.local/share/frametop/voice"
  fi

  # Imported avatar and hand models (avatar/README.md): the person's own files, kept unless asked.
  if [ -d "$HOME/.local/share/frametop/avatars" ]; then
    sz=$(size "$HOME/.local/share/frametop/avatars")
    ask avatars "Delete the avatar and hand models you imported ($sz)?" n "$sz" &&
      run rm -rf "$HOME/.local/share/frametop/avatars"
  fi

  local recs=()
  for f in "$HOME/.local/share/frametop/eyes" "$HOME/.local/share/frametop/hands" "$HOME/.local/share/frametop/gaze/bench"; do
    [ -e "$f" ] && recs+=("$f")
  done
  if [ ${#recs[@]} -gt 0 ]; then
    sz=$(size "${recs[@]}")
    ask recordings "Delete your eye and hand recordings and gaze bench runs in ~/.local/share/frametop ($sz)?" n "$sz" &&
      run rm -rf "${recs[@]}"
  fi
  [ "$dry" = 1 ] || rmdir "$HOME/.local/share/frametop/gaze" "$HOME/.local/share/frametop" 2>/dev/null || true

  if command -v podman >/dev/null && podman container exists dev 2>/dev/null; then
    echo "The build container (dev) holds Frametop's compilers and libraries, 1-2 GB. If you put"
    echo "anything else in it, that goes too."
    if ask container "Delete the build container?" n; then
      run "$HOME/.local/bin/distrobox" rm --force dev </dev/null
      run podman image rm registry.fedoraproject.org/fedora-toolbox:44 >/dev/null 2>&1 || true
    else
      kept_dev=1
    fi
  fi

  # distrobox, which the install put in ~/.local (install.sh's step 1), with what its own uninstall
  # removes and its icons, while no other container needs it.
  local dbx=("$HOME"/.local/bin/distrobox "$HOME"/.local/bin/distrobox-* "$HOME"/.local/share/man/man1/distrobox*
             "$HOME"/.local/share/bash-completion/completions/distrobox* "$HOME"/.local/share/icons/distrobox
             "$HOME"/.local/share/icons/terminal-distrobox-icon.svg
             "$HOME"/.local/share/icons/hicolor/*/apps/terminal-distrobox-icon.*) others
  if [ -e "$HOME/.local/bin/distrobox" ]; then
    others=$(other_boxes "$kept_releases" "$kept_dev" | tr '\n' ' ')
    if [ -n "$others" ]; then
      echo "distrobox stays in ~/.local/bin: these containers use it: $others"
    elif ask distrobox "Remove distrobox (the container tool the install put in ~/.local/bin)?" y; then
      run rm -rf "${dbx[@]}"
    fi
  fi

  if [ "$from_installer" = 0 ] && command -v flatpak >/dev/null && flatpak info io.github.frametop.Installer >/dev/null 2>&1; then
    echo "The Frametop Installer app stays, so you can install Frametop again. To remove it too:"
    echo "  flatpak uninstall --delete-data io.github.frametop.Installer"
  fi

  step "Done"
  if [ "$left" = 1 ]; then
    echo "Frametop is uninstalled, but the parts above that need your password stay. Run this again to retry."
    [ "$yes" = 1 ] && [ "$dry" = 0 ] && return 5
  else
    echo "Frametop is uninstalled."
  fi
  return 0
}

dir=
# Sourced (scripts/tests/test_uninstall.py), it only defines the functions: return works only then.
(return 0 2>/dev/null) && return 0
main "$@"
