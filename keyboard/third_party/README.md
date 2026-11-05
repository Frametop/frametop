# Third-party sources

## libei client

`libei/` contains the client and shared utility sources from libei 1.6.0,
commit `8a46bf3d4b6af7f25a61be53386cf618218114ef`, under the adjacent MIT
`COPYING`. See `libei/README.md` and `provenance.json` for the official archive,
checksum, selected files and protocol regeneration commands. The fixed client
is compiled into Full Keyboard; SteamOS libraries are unchanged. This avoids
libei 1.4.1's null connection cleanup when a peer aborts negotiation. No libeis
server or portal helper is bundled.

## Overlay texture and OpenVR

`overlay_texture/vk_texture.{h,cpp}` comes from
https://github.com/sasaken1102r/frame-perf-overlay at commit
`61c597c4f57624339c222094582f15da32016716`, under its MIT license.
It handles OpenVR-compatible persistent Vulkan textures. Local changes are noted
in comments and Git history. The transport also accepts an explicit Vulkan pixel format, defaulting to its original RGBA format. Native-BGRA selection and fallback are owned by `include/framekeyboard/vr_texture.hpp`.

`openvr/openvr.h` is the Valve OpenVR 2.15.6 header from that same pinned tree.
The adjacent BSD license was retrieved from Valve's OpenVR repository.
The SDK library is supplied by the installed SteamVR runtime, not redistributed.

## Gamescope input-method protocol

`gamescope/gamescope-input-method.xml` comes from ValveSoftware/gamescope commit `6867f509874f9bc52e12d6f4c4596cdf0d5be6b4`, under the MIT license included in the XML. Wayland client stubs are generated at build time. Only version 1 text submission is used, feature-checked at runtime.

Japanese conversion dynamically uses the system Anthy public C API (`anthy.h`, 9100h). Anthy and its dictionary are not vendored. No learning/commit API is called and composition is not written to a user dictionary.

Chinese conversion links the installed PyZy public C++ API, under LGPL-2.1-or-later.
Korean composition links the installed libhangul public C API, under LGPL-2.1-or-later.
Neither library nor dictionary is redistributed. PyZy engine interaction was checked
against https://github.com/pyzy/pyzy commit `6d9c3cdff364e0da75e1c26222240f26370ebf73` and the installed
Frame headers. No commit/selectCandidate learning APIs are called. The user-cache
path is `/dev/null`, forcing the library's in-memory fallback and preventing history
writes. Candidate selection reads conversion/rest text and rebuilds the remaining
reading. libhangul API reference: https://libhangul.github.io/libhangul-doc/git/group__hangulic.html.
