# Native compositor compatibility

This implementation targets two observed Steam Frame runtimes. It does not infer
compatibility from a SteamVR version number. `compatibility.h` owns the executable
SHA-256 profiles, native entrypoint names, patch addresses, original instructions and
trampoline instructions. Both native libraries use its executable check.
`native-shaders.json` owns the eight shader hashes, filenames, native position
IDs and allowed stage pairs; `build.py` generates the layer's pair table from it.

## Installation evidence

The original installation below passed live rectangle, hand, stereo and overlay
ordering tests. Metadata was read from the development headset on 2026-10-12:

| Item | Observed value | Source |
|---|---|---|
| SteamOS | 0.3.0, build `20260922.6101926`, variant `vr` | `/etc/os-release` |
| Kernel | `6.18.0-gfbdbca41fd45` | `uname -r` |
| SteamVR | `2.17.10`, runtime `/opt/steamvr`, `linuxarm64` | compositor startup log |
| SteamVR package | `deckard-steamvr-rel r25358740+28b72a4f-1` | `pacman -Qi deckard-steamvr-rel` |
| Runtime build stamp | `1789606310` | `/opt/steamvr/bin/version.txt` |
| Compositor ELF build ID | `ba0d798c2694e05985421e848b67ced8984fe2d3` | `readelf -n` |
| Compositor SHA-256 | `9b1bd5fe4463592cbc71b2517b9ceb0a0fcd66cd6885a19dca4be0784bf6c265` | `sha256sum /opt/steamvr/bin/linuxarm64/vrcompositor` |
| Graphics packages | `deckard-mesa-linux-aarch64` and `deckard-mesa-android-aarch64`, both `26.3.0_devel+git8aa73b4b-1` | `pacman -Q` |

The owner reports this is not a beta installation. The installed package is
named `-rel`; the Steam channel selection was not independently inspected.
The package build date is 2026-09-16 and install date is 2026-09-22. The version
and build metadata above describe the tested installation, not a support range.

After the owner's headset update, the following installation was inspected on
2026-10-12. Its executable control flow and all shader inputs were checked;
generated shaders passed validation and both libraries built with strict warnings.
Live verification on 2026-10-12 confirmed rectangle camera coverage in both
eyes, above/below Steam overlay ordering, and Off suppression. An eight-capsule
synthetic hand publisher confirmed hand-only stereo camera coverage and removal
after tracking expired. The real hand tracker was restored and publishing, but
reported no hands during this check; real-hand alignment on this build remains
to be checked while wearing the headset. The user's rectangle placement and
behind-menu preference were restored after the temporary test placement.

At the time of that update test, the earlier boot-only override disappeared on
reboot. The current installer replaces it with a persistent user SteamVR override
and bridge service. The bridge follows SteamVR startup/restart and restarts on
failure. Desktop startup remains the launcher's responsibility: a settings window
can open without `ft-screens`, in which case its abstract socket is unavailable.
The backend waits for the desktop rather than recreating a session using an old
saved process environment.

Persistence checks on the 0.4.5 installation passed a bridge restart, compositor
restart followed by a fresh desktop launch, and the game's pause/resume command
path. Both saved toggles restored. Stereo capture confirmed camera coverage
returned after a forced RoomView Off without cycling either setting. Persistent
unit installation and migration also passed offline tests.

A full reboot on 2026-10-12 changed the kernel boot ID and automatically started
the native backend. Starting the desktop then restored both enabled toggles and
the exact saved rectangle coordinates. Stereo capture confirmed camera rendering.
Steam's standing-origin Y translation changed from 1.600000 to 0.723369 metres,
however, so identical saved coordinates did not retain the rectangle's physical
height relative to the headset. Placement across origin changes remains unresolved.

The actual Steam dashboard's "Toggle Room View" button was exercised through its
live DOM click handler. Its active state cleared at 100 ms and returned by two
seconds, and stereo capture confirmed masked camera rendering returned. This
demonstrates recovery but also a conflict: the bridge's periodic On request
overrides the user's Room View Off choice. Independent full-room/Aurora switching
is not supported correctly while masks are enabled. Another OS update and actual
game launch/exit remain untested with this lifecycle change.

Desk and hand On/Off intent is saved independently in global layout settings and
reapplied during layout restore. Pause, tracking expiry or backend unavailability
suppresses coverage without clearing that intent. A supported compositor can
resume it after restart; a new, unrecognized executable cannot. Unknown images
are rejected by the native libraries and produce no ready initialization evidence,
so the bridge leaves coverage empty and does not enable RoomView. Persistent
installation survives an OS update, but adding support still requires the binary
and runtime validation described below.

| Item | Observed value | Source |
|---|---|---|
| SteamOS | 0.4.5, build `20261007.6125817`, variant `vr` | `/etc/os-release` |
| Kernel | `6.18.0-ge66bc2ca6f8c` | `uname -r` |
| SteamVR | `2.18.2`, runtime `/opt/steamvr`, `linuxarm64` | compositor startup log |
| SteamVR package | `deckard-steamvr-rel r25765468+0ab5c24d-1` | `pacman -Q` |
| Runtime build stamp | `1791331621` | `/opt/steamvr/bin/version.txt` |
| Compositor ELF build ID | `f608305627cd1579a5bbd714d4209922c892da0a` | `readelf -n` |
| Compositor SHA-256 | `0bb148eda6b5e54e2fbed09d475a43671f5ff6fcb6c2b4f48345b1a9fe8d6f96` | `sha256sum` |
| Graphics package | `deckard-mesa-linux-aarch64 26.3.0_devel+gitabc426bb-1` | `pacman -Q` |

All eight shader files are byte-identical across these installations, so they
share the manifest and generated shader transformations. Executable addresses
are selected only by full-image SHA-256; an unknown image is refused.

## Executable patches

Addresses below are ELF virtual addresses relative to the main executable's load
bias, not file offsets or absolute process addresses. The preload constructor
only considers a process named `vrcompositor`, with the exact executable hash
and one thread. It requires the sites to lie in executable `PT_LOAD` memory and
on the same writable patch page. All checked words must match before any edit.

| Site (SteamOS 0.3.0 / 0.4.5) | Original | Change and meaning |
|---|---|---|
| `0x9f404` / `0xa246c` | `0x34003577`, `cbz w23,...` | Replace with AArch64 NOP `0xd503201f`; permits the native Aurora scene path while RoomView is active. |
| `0x9fab0` / `0xa2b18` | `0xb9410be0`, `ldr w0,[sp,#0x108]` | Branch to the selector trampoline. The next two words must be `0x7100001f` (`cmp w0,#0`) and `0xb940d3e0` (`ldr w0,[sp,#0xd0]`). |

At the selector site, stack slot `0x108` chooses the existing pre/post-overlay
camera path. Stack slot `0xd0` denotes the native stencil mode: when nonzero,
the trampoline leaves its selector unchanged. Otherwise it writes the current
Frametop above-overlay flag to `0x108`, executes the displaced load, and returns
at selector + 4. Native camera and overlay call sites are:

| Call | SteamOS 0.3.0 | SteamOS 0.4.5 |
|---|---|---|
| Camera before overlays | `0xa0dbc` to `0x160be8` | `0xa3e30` to `0x164e18` |
| Overlay processing | `0xa0034` to `0x98c60` | `0xa30a0` to `0x9bb98` |
| Camera after overlays | `0xa03f8` to `0x160be8` | `0xa3464` to `0x164e18` |

Those addresses describe control flow, not additional patched sites. In 0.4.5,
the before-overlay camera block returns to `0xa2b34`; the after-overlay block
still reads stack slot `0x108` at `0xa3358`. The gated Aurora call is at
`0xa2730` to `0x72568`. REA/Ghidra inspected the full containing function
`0xa1880..0xa4917`; its provider addresses add `0x100000` to these ELF addresses.
The retained instruction evidence ID is
`ev_f6d608c5d2615e59e35e51dc2a42cd9738314eee8e38a632eaa858475eef381a`;
decompilation is `ev_2e041ee6e96969a5c81f146d5e668d4db757959bea2f4f684cfc145822638d38`.

The trampoline assumes AArch64 little-endian instructions and 64-bit pointers.
Its eight instruction words precede the selector pointer at byte 32. Only x0 is
used as scratch, matching the displaced w0 load's clobber; NZCV, SP, LR and SIMD
registers are preserved. Direct branches must fit the signed 26-bit word offset
(128 MiB reach). Allocation uses `MAP_FIXED_NOREPLACE` near the site; no existing
mapping is replaced. Code is flushed and made RX before patching. The original
page is temporarily RW and restored RX; failure to restore execute protection
terminates startup. There is no live unpatch: disabling restarts stock SteamVR.

These control-flow meanings were checked against native REA/Ghidra disassembly
for both images and runtime captures on 0.3.0. The full executable hash is essential: three matching
instruction words alone would not establish stack-slot or control-flow meaning.

## Shader transformation

Generation reads `/opt/steamvr/resources/shaders/vulkan` by default, verifies all
input hashes first, and assembles/validates SPIR-V for Vulkan 1.0. Numeric IDs
are preserved. Every original instruction must survive in order after assembly
and disassembly; only the entrypoint interface list may grow. Native camera
sampling, sharpening, colorspace processing and position output are retained.

The pinned variants are VS 57 with PS 125/126/127 and sharpen VS 58 with PS
128/129/130. Each has one entrypoint, `VS_MAIN` or `PS_MAIN`, and a GLSL.std.450
import. The fragment entry begins with a label and no function-local variables.
There are no existing push constants or `EarlyFragmentTests`; the added varying
locations 3, 4 and 5 must be free. These structural conditions are checked by
the generator in addition to the input hashes.

The vertex camera UBO is the unique binding 0 descriptor, at set 0, with 26
members. It remains 528 bytes: its last member begins at byte 464. Member 1 (M1)
is a float4x4 at byte 64, row-major with matrix stride 16. The native M0 result
is position ID `%371` in VS 57 and `%385` in VS 58. These IDs must remain vec4.
Native fragment descriptors stay unchanged, including their camera UBO at
set 0/binding 4 in the pinned variants.

For this runtime, inverse(M1) times homogeneous origin yields the eye in raw
tracking coordinates. The xyz of the native reference position supplies the
other point on the viewing ray. Live pose-translation and yaw captures confirmed
this coordinate interpretation, including separate eye origins and IPD. This is
a measured native convention, not a general guarantee about camera matrices.
Frametop converts its standing-space plane and predicted hands through the
inverse public raw-to-standing transform before publication.

Added interfaces are location 3: smooth vec3 homogeneous plane coordinates;
location 4: smooth vec3 raw eye ray; location 5: flat vec4 eye origin plus capsule
count. The vertex push block is four vec4 values at offsets 0/16/32/48 (64 bytes).
The fragment hand UBO is set 1/binding 0, 128 vec4 values with stride 16 (2048
bytes). These added layouts are Frametop's own contract, not native UBO fields.

## Vulkan interception and lifetime

The layer uses loader interface version 2 and standard dispatchable-handle
dispatch keys. Shader replacement requires exact original bytes and size.
Camera pipeline recognition requires exactly two stages, the pinned entrypoint
names and an allowed pair from the manifest. It does not recognize other native
camera pipeline families or indirect/non-indexed camera draws.

All native layouts with zero push ranges receive the same 64-byte vertex range
to preserve push-range compatibility across native descriptor binds. For native
layouts with exactly one descriptor set, the layer appends its hand set at index
1; the original set 0 handle is retained. Camera pipelines using other layout
shapes are rejected. The observed camera draw uses one color attachment with
blending already enabled, ADD color/alpha operations and no logic operation.
Its factors become ONE / ONE_MINUS_SRC_ALPHA for source-over. An unexpected
blend shape is not rewritten; the executable/shader pins and live validation
are what establish this tested shape.

The pinned compositor records camera `vkCmdDrawIndexed` calls anew during
rendering. A command recording samples the mask once and pushes the same plane
and count for its camera draws. Its hand buffer/descriptor are private to that
command buffer and immutable until native begin/reset; native synchronization
must have completed execution before reuse. Host-visible, host-coherent memory
is required. Free/reset/destroy hooks track the native command/pool lifecycle.
No native camera descriptors or packets are copied. The tested runtime retains
camera pipeline layouts while pipelines use them; destroying one early drops
tracking and emits a diagnostic instead of using the destroyed handle.

The gate is preloaded; the Vulkan layer is enabled explicitly, never preloaded.
Both use the same whole-executable pin. Pin rejection skips their native edits;
it is not a claim that stock RoomView will provide Frametop masks. Shader/layout
mismatch requires investigation before enabling a trial on a changed runtime.

## Desktop composition

The native selector brackets all overlays. To place passthrough above Frametop
desktops but below Steam menus, `handcut::Renderer` removes desktop alpha with
the same per-eye plane and tapered-capsule coverage. Panel texture coordinates
are reconstructed on the flat or curved panel in standing space. Camera rays
continue through the panel; physical hand cutouts keep their separate depth rule.
The native renderer needs no additional patch. Above-menu mode uses the native
after-overlay pass and does not need these desktop openings.

Live checks on SteamOS 0.4.5 / SteamVR 2.18.2 confirmed rectangle and synthetic
eight-capsule hand coverage above a curved desktop and below an open Steam menu
in both eyes. Portal Off and expired hand data restored desktop pixels. The
above-menu mode also passed. GLES shader compilation and the existing CPU
geometry/input ownership suite passed. Real tracking was restored afterward.
Floating windows and their cropped popups retain the upstream stereo-cutout
limitation. Desktop openings require a fresh bridge heartbeat and clear when
the mask is disabled, tracking expires, or the bridge stops publishing.

## Control and mask transport

`control.py` assumes the local SteamVR WebSocket at 127.0.0.1:27062 accepts an
HTTP Upgrade with the same host/origin. It validates the WebSocket accept key
and sends a masked text frame containing `mailbox_send vrcompositor_systemlayer`
and JSON `type=set_camera_roomview_enabled, enabled=<bool>`. This private setter
has no acknowledgement; delivery is not rendering verification.

`bridge.py` uses Frametop's abstract Unix datagram socket `ft_screens`, command
`passthrough mask`, and its JSON plane/intent/capsule snapshot. This is our own
interface. It atomically publishes 2112 little-endian bytes: the 64-byte plane
prefix plus 64 tapered capsules, each `(A.xyz,ra,B.xyz,rb)`. Prefix floats 12/13/14
mean desk enabled / above overlays / capsule count. The layer requires a regular,
current-user-owned file, exact length, finite floats, count 0..64 and positive
radii below 1 metre; data older than 500 ms is rejected. Hands separately inherit
upstream `handcut::Hands` prediction and 300-ms tracking expiry. Rectangle and
capsule coverage are unioned; one native draw order applies to both masks.

Capsule testing uses swept spheres with linearly interpolated radii and excludes
centres within 12 cm of the eye. It is geometric, hard-edged coverage, not an
image segmentation mask. The current plane/capsule wire format is internal;
future image or contour masks should replace coverage without taking over
native camera ownership or assuming this payload is a permanent external API.

## Rechecking a changed installation

Record the OS/package/runtime metadata above, executable hash/build ID and all
eight shader hashes. Reinspect native patch context, stack semantics, matrix
producer and render order before changing pins. Regenerate and validate shaders,
build to a separate directory, and verify both eyes: rectangle, hand-only and
combined coverage; scene outside masks; motion/origin changes; stale tracking
and bridge loss; both menu orders; repeated On/Off; then stock restoration.
Do not overwrite libraries mapped by the compositor. Steam startup/update or
scene transitions can temporarily obscure rendering; inspect a settled scene
before concluding a shader regression. See also `scripts/update-check.py`.

The bridge validates native status against the compositor PID, process start time
and executable identity before sending camera requests or acknowledging its
heartbeat. Errors clear readiness and desktop alpha openings. RoomView delivery
runs in a worker so transport waits do not stop mask publication. Enabled intent
is reasserted once per second, and compositor changes invalidate cached delivery;
manual Off/On cycling is not required after native state resets. Off is sent on
transitions or retry, preserving stock RoomView controls while Frametop is off.
These checks establish initialization and transport readiness, not rendered frames.
