#!/bin/sh
# OpenXR and OpenVR programs find SteamVR through two files under the config folder:
# openxr/1/active_runtime.json (the OpenXR loader) and openvr/openvrpaths.vrpath (SteamVR's
# path registry, which its OpenXR runtime reads too). The Frametop desktop has a config
# folder of its own, ~/.config/frametop, so programs started in it found neither: the
# loader said "failed to determine active runtime file path" (issue 78), and OpenVR
# programs failed with VRInitError_Init_PathRegistryNotFound. This links both folders in
# the desktop's config folder to the user's, one level up. The loader needs both: with
# openxr alone it finds the runtime, which then can't find SteamVR.
#
# A link is made even when the user's folder isn't there yet, so it works once SteamVR
# writes it. Anything already under that name is left alone, with a line saying so.
#
#   vr-links.sh ~/.config/frametop
config=${1:?usage: vr-links.sh CONFIG_FOLDER}
for name in openxr openvr; do
  link=$config/$name
  if [ -L "$link" ]; then
    continue
  elif [ -e "$link" ]; then
    echo "frametop: $link is not a link to the user's ${config%/*}/$name, so OpenXR and OpenVR programs in the desktop may not find SteamVR (move it away, then restart the desktop)" >&2
  else
    ln -s "../$name" "$link"
  fi
done
