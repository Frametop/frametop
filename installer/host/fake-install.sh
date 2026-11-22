#!/usr/bin/env bash
# A test run's install (frametop-installer --dry-run): it prints what get.sh --release and
# install.sh print, slowly, asks askpass once if it has one, and installs nothing.
# FAKE_RC: the exit status to end with (default 0).
set -u
echo "Downloading https://github.com/Frametop/frametop/releases/download/v0.0.0-test/Frametop.zip (test run: nothing downloads)"
printf '  %% Total    %% Received %% Xferd  Average Speed   Time    Time     Time  Current\n'
printf '                                 Dload  Upload   Total   Spent    Left  Speed\n'
for p in 0 10 25 40 55 70 85 100; do
  printf '\r%3d 1208M  %3d %4dM    0     0  60.0M      0  0:00:20  0:00:0%d  0:00:%02d 60.0M' \
    "$p" "$p" $((p * 1208 / 100)) $((p / 15)) $(((100 - p) / 5))
  sleep "${FAKE_SLEEP:-0.6}"
done
printf '\n'
echo "Checked Frametop.zip against the release's SHA256SUMS"
echo "Unpacking (test run)"
printf '\n\033[1m== 0/13 loading Frametop 0.0.0-test (a minute or two)\033[0m\n'
sleep "${FAKE_SLEEP:-1}"
steps=("distrobox" "Frametop's container" "input relay" "3D mouse: SteamVR driver" "3D mouse: pointer helper"
  "power service" "multi-screen desktop" "voice dictation" "hand tracking" "gaze mode" "our own eye tracker"
  "Frametop Together" "Bluetooth fixes")
for i in "${!steps[@]}"; do
  printf '\n\033[1m== %d/13 %s (test run)\033[0m\n' $((i + 1)) "${steps[$i]}"
  if [ "$i" = 8 ] && [ -n "${SUDO_ASKPASS:-}" ]; then
    if "$SUDO_ASKPASS" >/dev/null; then echo "askpass answered (test run: the password isn't used)"
    else echo "askpass got no password: this part would be skipped"; fi
  fi
  sleep "${FAKE_SLEEP:-1}"
done
printf '\n\033[1m== Done\033[0m\n'
echo "Test run: nothing was installed."
exit "${FAKE_RC:-0}"
