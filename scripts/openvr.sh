# Sourced by renderer/build.sh on ARM64 Linux: pinned OpenVR overlay API.
#
# Headers: the pinned public SDK, fetched once into build/include and checked by sha256.
# The headers SteamVR ships on the Frame (in its samples) are 2.1.0, which predates
# interfaces the hand overlay uses (IVRIPCResourceManagerClient) and has no
# public tag to pin; the Frame's runtime supports this SDK's interface versions.
# Library: SteamVR's own libopenvr_api by default. OPENVR_LIB names another folder with a
# libopenvr_api.so to link (CI uses the pinned SDK copy); the binary looks in
# SteamVR's folder first on the Frame.
openvr_tag=v2.15.6
openvr_commit=0924064316de3effbcd1acf1e309182a2deb1c05   # the commit v2.15.6 names
openvr_steamvr=/opt/steamvr/bin/linuxarm64

openvr_fetch() {  # openvr_fetch <header> <sha256>
  local f=build/include/$1
  echo "$2  $f" | sha256sum -c --status 2>/dev/null && return 0
  mkdir -p build/include
  curl -fsSL "https://raw.githubusercontent.com/ValveSoftware/openvr/$openvr_commit/headers/$1" -o "$f.part" || return
  if ! echo "$2  $f.part" | sha256sum -c --status; then
    echo "$1 from openvr $openvr_tag doesn't match its pinned sha256" >&2
    rm -f "$f.part"
    return 1
  fi
  mv "$f.part" "$f"
}
openvr_fetch openvr.h 1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd

openvr_lib=${OPENVR_LIB:-$openvr_steamvr}
openvr_rpath=$openvr_steamvr
[ "$openvr_lib" = "$openvr_steamvr" ] || openvr_rpath=$openvr_steamvr:$openvr_lib
OPENVR_CFLAGS=-Ibuild/include
OPENVR_LIBS="-L$openvr_lib -lopenvr_api -Wl,-rpath,$openvr_rpath"
