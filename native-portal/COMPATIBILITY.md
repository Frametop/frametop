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

Independent camera demand replaced the bridge's periodic Room View requests.
Live tests on 0.4.5 exercised the actual Steam dashboard's "Toggle Room View"
button through its DOM click handler. Off remained Off while stereo masks
rendered in Aurora; On showed full-room stereo passthrough; returning to Off
restored masks. Stopping the bridge cleared masks without changing Steam's
toggle or interrupting full-room passthrough, and restarting restored masks.
Hand-only synthetic capsules also rendered with Room View Off and expired
normally; real tracking was restored afterward. Another OS update and actual
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
bias. The preload constructor requires a process named `vrcompositor`, an exact
executable hash and one thread. Every site must lie inside executable `PT_LOAD`
memory and match its original instruction before any edit. The independent camera
contracts were statically checked for both recorded images. Independent-mode live
tests used SteamOS 0.4.5 / SteamVR 2.18.2; the earlier 0.3.0 live results concern
the former shared-RoomView implementation, so its independent mode still needs a
live regression test when that runtime is available.

| Site (0.4.5) | Original | Change |
|---|---|---|
| `0xa1ecc` | `0x94073da9`, `bl 0x271570` | Request trampoline combines native demand with fresh Frametop demand, then calls the original singleton getter. |
| `0xa2b18` | `0xb9410be0`, `ldr w0,[sp,#0x108]` | Selector trampoline changes order only when stock camera demand is absent. Next words must be `0x7100001f` and `0xb940d3e0`. |
| `0xa3e30` | `0x940303fa`, `bl 0x164e18` | Camera call before overlays passes through the mask-only argument wrapper. |
| `0xa3464` | `0x9403066d`, `bl 0x164e18` | Camera call after overlays uses the same wrapper. |
| `0xa246c` | `0x34003577`, `cbz w23,...` | Checked but left unchanged. Native Aurora/full-room scene policy is preserved. |

The equivalent 0.3.0 sites were read from the retained executable with matching
SHA-256, not calculated from an assumed version delta:

| Site (0.3.0) | Original | Role |
|---|---|---|
| `0x9ee6c` | `0x94072d43`, `bl 0x26a378` | Completed camera-demand hook |
| `0x9fab0` | Same three selector words as 0.4.5 | Before/after selector |
| `0xa0dbc` | `0x9402ff8b`, `bl 0x160be8` | Camera before overlays |
| `0xa03f8` | `0x940301fc`, `bl 0x160be8` | Camera after overlays |
| `0x9f404` | `0x34003577` | Aurora condition retained |

At the request site, x0 still contains the low-byte camera availability result
from `0x162238`; w27 is the completed native camera-demand predicate. It includes
shared system state byte `0xd6d`, per-render camera flag `0x62c`, and the native
boundary/proximity condition involving byte `0xd6a` and float `0x648`. The helper
records that unmodified predicate as stock/full-room demand, then returns native
OR (available AND requested mask). It never writes the shared RoomView flag.
The existing code uses w27 for native acquisition/update and draw selection,
including calls to `0x163e88` and acquisition setup `0x163c10`.
In 0.3.0 the availability call is `0x15e008`, shared demand/boundary bytes are
`0xd5d`/`0xd5a`, and acquisition update is `0x15fc58`. The fallback branches leave
x0 intact and merge at `0x9ee6c` with the same w27 predicate. Neither version's
shared byte offsets are directly accessed by the hooks.

The request trampoline replaces an existing BL. It saves/restores x29 and x30 on
an aligned 16-byte stack frame, calls the helper, updates w27 deliberately, then
executes the original singleton getter so x0 has its original return value.
Other callee-saved registers follow AAPCS64; caller-saved registers and flags are
already clobberable at the displaced native call. No native object is replaced.

Stack slot `0x108` selects pre/post-overlay camera rendering; `0xd0` is the native
stencil mode. The selector trampoline preserves the original selector whenever
stock camera demand or stencil mode is active. Otherwise it writes Frametop's
above-overlay flag to `0x108`. Only x0 is scratch, matching the displaced load;
NZCV, SP, LR and SIMD registers are preserved. The before-overlay block returns
to `0xa2b34`, overlay rendering is called at `0xa30a0` to `0x9bb98`, and the
post-overlay path reads `0x108` at `0xa3358` before its camera call.

The native camera function independently checks flags and opacity, so combining
demand alone cannot produce masked camera pixels with stock RoomView off. Its
mask-only wrapper copies 0x18 bytes of per-call controls and 0x90 bytes of draw
arguments onto an aligned stack. Those bounds cover all direct reads in the
pinned renderer: controls byte 0 and float +0x14; draw arguments through byte
+0x8c. The wrapper changes controls byte 0 to 1 and draw-argument float +0x0c to
1.0. This selects an explicit camera draw with opaque coverage while retaining
native matrices, handles, capture and reprojection. Copies live through the
synchronous native call, which reads their values without retaining their
addresses. Stock-demand calls receive their original pointers unchanged.
No shared setting, persistent native packet or native camera resource is copied.

The camera-call trampoline tail-branches to the C++ wrapper, retaining the BL
return address. All trampoline instructions are little-endian AArch64 with
64-bit pointers. Direct branches must fit the signed 26-bit word displacement
(128 MiB reach). One nearby page is allocated using `MAP_FIXED_NOREPLACE`, filled,
cache-flushed and made RX. All original pages are made RW before writing any
instruction, then cache-flushed and restored RX. Failure to restore execute
permission terminates startup. Disabling restarts stock SteamVR; no live unpatch
is attempted.

REA/Ghidra inspected the full containing function `0xa1880..0xa4917`; its provider
addresses add `0x100000` to ELF addresses. Retained instruction evidence is
`ev_f6d608c5d2615e59e35e51dc2a42cd9738314eee8e38a632eaa858475eef381a`;
SystemLayer decompilation is
`ev_2e041ee6e96969a5c81f146d5e668d4db757959bea2f4f684cfc145822638d38`.
Focused availability, acquisition and camera renderer decompilation is
`ev_5f2cdd3514a8e247f076df234d01f4a10fe131bf68397db5be04a8087e344ba7`.
For 0.3.0, complete SystemLayer instruction evidence is
`ev_6b6fff062186d3503ed3bd53240ce1b753619ab8dcd27acf0f2cbeff3c0fac13`;
SystemLayer and camera-renderer decompilation is
`ev_a163a0ed82eadc5e47c54d1c7d637c0394d100f2ace6a634b577f84717eb26fd`.
The older renderer has the same direct control/argument read bounds, opacity
offset, explicit camera flag and synchronous pointer lifetimes described above.
The full executable pin is essential: matching isolated instruction words would
not establish these register, argument or lifetime contracts.

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

The optional development helper `control.py` assumes the local SteamVR WebSocket at 127.0.0.1:27062 accepts an
HTTP Upgrade with the same host/origin. It validates the WebSocket accept key
and sends a masked text frame containing `mailbox_send vrcompositor_systemlayer`
and JSON `type=set_camera_roomview_enabled, enabled=<bool>`. This private setter
has no acknowledgement; delivery is not rendering verification. Neither the
bridge nor installer uses this setter. Normal operation leaves Steam's Room View
state alone.

`bridge.py` uses Frametop's abstract Unix datagram socket `ft_screens`, command
`passthrough mask`, and its JSON plane/intent/capsule snapshot. This is our own
interface. It atomically publishes 2112 little-endian bytes: the 64-byte plane
prefix plus 64 tapered capsules, each `(A.xyz,ra,B.xyz,rb)`. Prefix floats 12/13/14
mean desk enabled / above overlays / capsule count. The layer overrides float 15
with native full-room demand before drawing; external data cannot enable this
bypass. Flat shader location 6 carries it to the fragment mask. The layer requires a regular,
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

The bridge validates native status against compositor PID, process start time
and executable identity before publishing masks and acknowledging its heartbeat.
Errors clear readiness and desktop alpha openings. Native scene command recording
feeds fresh mask demand to the request hook even when the camera is off, avoiding
a dependency on an existing camera draw. Expired data clears that demand.
These checks establish initialization and publication readiness, not rendered frames.
